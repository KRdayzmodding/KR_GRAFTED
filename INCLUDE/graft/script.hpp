#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "graft/abi.h"

// Доступ к движковым реализациям скриптовых методов. Нужен там, где своей раскладкой
// не обойтись: методы контейнеров (array.Insert, map.GetKey, ...) движок не кладёт в
// таблицу нативов, но кладёт в сам класс — оттуда их и берём и зовём его же кодом.
// Так не приходится повторять ни аллокатор, ни хеш-функцию: и то и другое поехало бы
// на патче молча.
//
// Как найдено (re/out/diag/pseudo/sub_140352DB0.c — линковщик `proto`):
//   idx  = FindFunctionIndex(class, "Имя")   // хеш имени, с обходом базовых классов
//   desc = ((void**)((char*)class + 104))[idx]
//   impl = *(void**)((char*)desc + 8);  flags = *(uint*)((char*)desc + 80)

namespace graft {

// ── Раскладка движковых структур ──────────────────────────────────────────────
// ЕДИНСТВЕННОЕ место, где записаны смещения: движковые структуры трогают и заголовок,
// и врезка, и регистрация — держать три копии значит однажды поправить две из трёх.
//
// Снято зондом на живом сервере (скрипт кладёт известные значения — C++ дампит память)
// и закреплено сьютой seraph::graft: если движок раскладку поменяет, тесты покраснеют.
//
// version меняется вместе с любым числом ниже. Смещения запекаются в машинный код тех,
// кто их читает, поэтому патч движка чинится не заменой одного модуля, а пересборкой
// всех: версия — то, по чему такое расхождение будет видно, а не угадываться.
namespace layout {
// Одно число на две стороны: хост сверяет его с тем, что принёс плагин, и отказывает
// при расхождении. Значение живёт в abi.h, потому что его читает и чистый C.
inline constexpr std::uint32_t version = GRAFT_LAYOUT_VERSION;

// Объект
inline constexpr std::size_t object_class = 8;      // дескриптор класса объекта
inline constexpr std::size_t wrapper_object = 16;   // объект внутри обёртки
inline constexpr std::size_t object_back_ref = 32;  // обратная ссылка объекта на обёртку

// Дескриптор класса
inline constexpr std::size_t class_name = 16;        // const char*
inline constexpr std::size_t class_var_table = 88;   // указатель на массив записей переменных
inline constexpr std::size_t class_var_count = 100;  // uint32
inline constexpr std::size_t class_functions = 104;  // массив указателей на дескрипторы
inline constexpr std::size_t class_var_base = 184;   // uint32: где в объекте начинаются поля
inline constexpr std::size_t var_entry_name = 8;     // const char*
inline constexpr std::size_t var_entry_slot = 40;    // uint16: номер слота, смещение = slot*4

// Дескриптор функции
inline constexpr std::size_t desc_impl = 8;
inline constexpr std::size_t desc_flags = 80;
// Нужно, чтобы звать движковые `proto` В ДРУГУЮ СТОРОНУ. Разобрано по подготовке кадра
// вызова sub_1403663B0 (re/README.md): движок не конструирует скриптовую переменную, а
// КОПИРУЕТ 40-байтный шаблон из дескриптора вызываемой функции. Значит и нам сочинять
// нечего — шаблон несёт сама функция.
inline constexpr std::size_t desc_params = 32;       // void**: сперва параметры, потом локали
inline constexpr std::size_t desc_var_count = 44;    // uint32: всего переменных
inline constexpr std::size_t desc_param_count = 88;  // uint8: сколько из них параметров

// Скриптовая переменная в рантайме
inline constexpr std::size_t var_size = 40;
inline constexpr std::size_t var_value = 0;
inline constexpr std::size_t var_type = 16;    // тег типа
inline constexpr std::size_t var_flags = 20;   // 0x10 const, 0x40 no-write, 0x80 out, 0x800 owned
inline constexpr std::size_t var_extent = 32;  // два uint16: размерность массива и счётчик
// По этому полю движок находит ДЕСКРИПТОР ТИПА переменной:
//   type = (*(void**)(var+24))[104][tag & 0xFFFFFFF]      (sub_140367280)
// Ради него шаблон и копируется целиком: с нулём здесь падает разрешение типа.
inline constexpr std::size_t var_context = 24;

// Флаги дескриптора функции. Выведены из компилятора (re/README.md) и сверены с
// ванильными дескрипторами на живом сервере: у member-натива бита static нет
// (string.Length = 0x4a28, map.Count = 0xc08).
inline constexpr std::uint32_t flag_static = 0x4;
inline constexpr std::uint32_t flag_native = 0x20;
inline constexpr std::uint32_t flag_marshalled = 0x40;
inline constexpr std::uint32_t flag_external = 0x4000;

// Контейнеры и typename
inline constexpr std::size_t array_data = 40;      // T* — элементы лежат подряд
inline constexpr std::size_t array_capacity = 48;  // int
inline constexpr std::size_t array_count = 52;     // int
inline constexpr std::size_t type_name = 16;       // const char* у typename
inline constexpr std::size_t map_nodes = 48;       // блок узлов
inline constexpr std::size_t map_count = 64;       // int
}  // namespace layout

}  // namespace graft

namespace graft::script {

struct method {
    void* impl = nullptr;
    std::uint32_t flags = 0;
    // Сам дескриптор. Нужен только обратному направлению (звать движковый `proto`):
    // в нём лежат шаблоны переменных-параметров и их количество. Прямому вызову хватает
    // impl, поэтому на горячем пути это поле никто не читает.
    void* desc = nullptr;
    explicit operator bool() const { return impl != nullptr; }

    // Годен ли для прямого вызова из C++. Проверяем не флаг, а саму память: у натива
    // в impl машинный код (исполняемая страница образа), у скриптового метода —
    // БАЙТКОД в куче, и вызов такого как C-функции гарантированно роняет процесс.
    // Флаг 0x20 (native) для этого не годится: у методов шаблонных классов он не
    // выставлен (у map.Count флаги 0xc08), хотя импл — обычный машинный код.
    bool executable = false;
    bool callable() const { return impl != nullptr && executable; }
};

// Теги типов. Тип задаёт СТАРШИЙ ниббл, младшие биты — идентификатор конкретного типа
// (у Class там номер класса, поэтому array<int> и map<string,int> оба 0x6000xxxx).
// Снято зондом на живом сервере: int 0x20000000, bool 0x20000007, float 0x30000001,
// vector 0x40000002, string 0x50000003, объект 0x60000287, array/map 0x60000005,
// typename 0xb0000008.
inline constexpr std::uint32_t type_family = 0xF0000000;
inline constexpr std::uint32_t type_int = 0x20000000;
// Тег = (семейство << 28) | ИНДЕКС типа в таблице контекста (разобрано по конструктору
// enf::BaseVarType и регистратору типов). Поэтому сравнивать можно только по семейству —
// кроме bool: он делит семейство с int и отличается лишь индексом. Индекс встроенных
// типов задан порядком их регистрации в движке и держится от версии к версии; если
// когда-нибудь съедет, bool будет читаться как int, а не ломаться.
inline constexpr std::uint32_t type_bool = 0x20000007;
inline constexpr std::uint32_t type_float = 0x30000000;
inline constexpr std::uint32_t type_vector = 0x40000000;  // в переменной — УКАЗАТЕЛЬ на 12 байт
inline constexpr std::uint32_t type_string = 0x50000000;
inline constexpr std::uint32_t type_class = 0x60000000;
inline constexpr std::uint32_t type_typename = 0xB0000000;
// `void` в объявлении параметра значит «любой тип»: настоящую переменную движок
// собирает на месте вызова по фактическому аргументу. Снято с дескриптора
// EnScript.GetClassVar: p0=0x60000005 p1=0x50000003 p2=0x20000000 p3=0x70000004.
inline constexpr std::uint32_t type_any = 0x70000000;

// Полные теги встроенных типов — те самые значения, что снял зонд. Семейством
// СРАВНИВАЮТ (выше), а вот СИНТЕЗИРОВАТЬ переменную приходится полным тегом: движок
// хранит в младших битах индекс типа в таблице контекста. Нужно там, где шаблон брать
// неоткуда, — у переменной возврата (её дескриптор функция не носит).
inline constexpr std::uint32_t tag_int = 0x20000000;
inline constexpr std::uint32_t tag_bool = 0x20000007;
inline constexpr std::uint32_t tag_float = 0x30000001;
inline constexpr std::uint32_t tag_vector = 0x40000002;
inline constexpr std::uint32_t tag_string = 0x50000003;
inline constexpr std::uint32_t tag_class = 0x60000005;
inline constexpr std::uint32_t tag_typename = 0xB0000008;

// ── Состояние библиотеки: живёт в script.cpp ─────────────────────────────────
// Всё, чему нужен script-контекст движка или найденные сканом адреса.

// Вызывается движковой врезкой (engine.cpp), когда известны контекст и точки поиска.
void bind(void* ctx, void* find_class, void* find_index);
void set_register_method(void* fn);

// Регистрация метода на классе, которого в момент врезки ещё не было. Так цепляются
// ИНСТАНЦИАЦИИ шаблонных классов мода: `CppHashMap<string,int>` — отдельный скриптовый
// класс (проверено), и появляется он там, где его впервые используют, то есть позже
// 1_Core. Флаги дескриптора приводятся к member-виду, как при обычной регистрации.
bool register_method_late(const char* class_name, const char* name, void* impl, bool is_static,
                          bool marshalled);
bool available();  // всё найдено и самопроверка на ванильном методе прошла
// Корень объектного графа игры (CGame). Приносит его мод вместе с тиком: движок отдать
// глобальную переменную отказывается — см. graft::try_global и кейс сьюты
// Out_GlobalVariableIsRefusedByEngine, где то же самое не работает и из скрипта.
void* root();
// Ищет класс во всех известных script-контекстах. Контекст у каждого модуля свой:
// в основном (1_Core) игровых классов ещё нет, Object/EntityAI появляются в поздних,
// поэтому контексты мы копим по ходу регистрации.
void* find_class(const char* name);
void note_context(void* ctx);  // зовёт врезка на каждой регистрации метода

// Дескриптор метода по классу: нужен адрес FindFunctionIndex, найденный сканом.
// Вызывающая сторона (detail::engine_method) кэширует результат на пару <класс, имя>,
// поэтому здесь и VirtualQuery внутри is_code() ничего не стоят.
method find_method(const char* class_name, const char* name);
method find_method(void* class_desc, const char* name);

// Почему вызов не состоялся — пишется в журнал.
void note_call_miss(const char* class_name, const char* name, void* self, const struct method& fn);

// ── Чистые чтения памяти: inline ─────────────────────────────────────────────
// Ни одному из них не нужно ни контекста, ни состояния — это разыменования по раскладке
// выше. Поэтому им место в заголовке, а не за границей TU: диспетчер инстанциаций зовёт
// class_of на КАЖДЫЙ маршалируемый вызов, deref_object — на каждый объектный аргумент,
// а find_variable/variable_address — на каждое обращение к полю. LTO в сборке нет, так
// что вызов через TU здесь был настоящим вызовом с барьером для оптимизатора.
//
// Заодно это готовый водораздел для разделения на хост и плагины: наружу (в таблицу
// сервисов) обязано уйти только то, что выше — состояние движка. Всё, что ниже,
// компилируется прямо в вызывающего.

// Дескриптор класса объект носит при себе (+8), у дескриптора есть таблица переменных.
// Разобрано по движковым typename.GetVariableCount/GetVariableName и вычислителю
// адреса поля (re/out/diag/pseudo/sub_140306520.c, sub_140306530.c, sub_140348EF0.c).
inline void* class_of(void* instance) {
    return instance
               ? *reinterpret_cast<void**>(static_cast<char*>(instance) + layout::object_class)
               : nullptr;
}

inline const char* class_name(void* class_desc) {
    return class_desc ? *reinterpret_cast<const char* const*>(static_cast<char*>(class_desc) +
                                                              layout::class_name)
                      : nullptr;
}

namespace detail {

// Похоже ли это вообще на указатель. Отсекает мусор (мелкие числа, невыровненные и
// неканонические значения) в несколько инструкций — до того, как звать VirtualQuery.
// Ради этого проверка и заведена: в поле +16 у НЕ-обёртки лежит что угодно.
inline bool plausible(const void* p) {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    constexpr std::uintptr_t user_space_end = 0x0000'8000'0000'0000ull;
    return v >= 0x10000 && v < user_space_end && (v & 7) == 0;
}

// Единственное, ради чего здесь остаётся вызов наружу: спрашивает у системы, отображена
// ли страница. Пробовать чтение под SEH нельзя — движок ставит свой фильтр исключений и
// рапортует о падении раньше, чем сработал бы наш __except (проверено — вылет
// 0xC0000005 с адресом внутри dwmapi.dll).
bool readable(const void* p, std::size_t n);

inline char* var_entry(void* class_desc, std::int32_t index) {
    void** table = class_desc ? *reinterpret_cast<void***>(static_cast<char*>(class_desc) +
                                                           layout::class_var_table)
                              : nullptr;
    return table ? static_cast<char*>(table[index]) : nullptr;
}

inline const char* var_name(char* entry) {
    return entry ? *reinterpret_cast<const char**>(entry + layout::var_entry_name) : nullptr;
}

inline std::int32_t var_count(void* class_desc) {
    return class_desc ? static_cast<std::int32_t>(*reinterpret_cast<std::uint32_t*>(
                            static_cast<char*>(class_desc) + layout::class_var_count))
                      : 0;
}

}  // namespace detail

// Ссылка может лежать двумя способами: прямым указателем на объект (поле класса) или
// через обёртку {vtable, счётчик, объект} (элемент контейнера). Отличаем по обратной
// ссылке: у объекта из обёртки в +32 записан адрес самой обёртки.
inline void* deref_object(void* maybe_wrapper) {
    // Сюда приходит значение переменной типа Class или элемент контейнера — то есть
    // живой объект либо живая обёртка, и в обоих +16 своё содержимое. Спрашивать про
    // него систему незачем: хватает того, что он похож на указатель. Опасно другое —
    // ТО, ЧТО ПО +16: у не-обёртки там данные объекта, то есть что угодно. Вот его и
    // проверяем. Так на объектный аргумент остаётся один VirtualQuery вместо двух.
    if (!detail::plausible(maybe_wrapper)) {
        return maybe_wrapper;
    }
    void* inner =
        *reinterpret_cast<void**>(static_cast<char*>(maybe_wrapper) + layout::wrapper_object);
    if (!detail::readable(inner, layout::object_back_ref + sizeof(void*))) {
        return maybe_wrapper;
    }
    void* back =
        *reinterpret_cast<void**>(static_cast<char*>(inner) + layout::object_back_ref);
    return back == maybe_wrapper ? inner : maybe_wrapper;
}

inline std::int32_t variable_count(void* instance) {
    return detail::var_count(class_of(instance));
}

inline const char* variable_name(void* instance, std::int32_t index) {
    void* class_desc = class_of(instance);
    if (index < 0 || index >= detail::var_count(class_desc)) {
        return nullptr;
    }
    return detail::var_name(detail::var_entry(class_desc, index));
}

// Адрес поля: obj + база полей класса + 4 * номер слота.
inline void* variable_address(void* instance, std::int32_t index) {
    void* class_desc = class_of(instance);
    if (index < 0 || index >= detail::var_count(class_desc)) {
        return nullptr;
    }
    char* entry = detail::var_entry(class_desc, index);
    if (!entry) {
        return nullptr;
    }
    const std::uint32_t base =
        *reinterpret_cast<std::uint32_t*>(static_cast<char*>(class_desc) + layout::class_var_base);
    const std::uint16_t slot = *reinterpret_cast<std::uint16_t*>(entry + layout::var_entry_slot);
    return static_cast<char*>(instance) + base + 4u * slot;
}

// Ищем перебором по именам: хеш-таблица класса устроена сложнее, а полей у класса
// десятки — цена перебора незаметна рядом с вызовом натива. Дескриптор класса берём
// один раз на весь перебор, а не на каждое имя.
inline std::int32_t find_variable(void* instance, const char* name) {
    if (!name) {
        return -1;
    }
    void* class_desc = class_of(instance);
    const std::int32_t count = detail::var_count(class_desc);
    for (std::int32_t i = 0; i < count; ++i) {
        const char* have = detail::var_name(detail::var_entry(class_desc, i));
        if (have && std::strcmp(have, name) == 0) {
            return i;
        }
    }
    return -1;
}

// ── Обратное направление: звать движковые `proto` ────────────────────────────
// Скриптовая переменная в рантайме — 40 байт. Движок её НЕ конструирует: подготовка
// кадра (sub_1403663B0) льёт готовый шаблон из дескриптора вызываемой функции. Мы
// делаем ровно то же, поэтому и не гадаем, что лежит в полях, которых не разобрали.
struct var {
    // Ровно 8, а не «побольше на всякий случай»: движок кладёт эти переменные в кадр
    // подряд с шагом 40, и выравнивание пошире сделало бы структуру 48 — блок разъехался
    // бы с тем, что читает вызываемая функция. Сторожит static_assert ниже.
    alignas(8) unsigned char raw[layout::var_size]{};

    std::uint32_t tag() const {
        std::uint32_t out = 0;
        std::memcpy(&out, raw + layout::var_type, sizeof out);
        return out;
    }
    void set_tag(std::uint32_t value) { std::memcpy(raw + layout::var_type, &value, sizeof value); }

    // Значение переменной — первые 8 байт. Отдельным методом, чтобы место записи было
    // названо, а не считалось «ну, начало структуры».
    void* value_slot() { return raw; }

    // Шаблон из дескриптора: копируем целиком, ничего не трогая.
    static var from_template(const void* desc) {
        var out;
        if (desc) {
            std::memcpy(out.raw, desc, layout::var_size);
        }
        return out;
    }
    // Шаблона нет (возврат) — собираем минимально достаточную. Размерности берём такими,
    // какие движок пишет скаляру сам: младшее слово 0, старшее 1 (в кадре это одна
    // запись 0x10000 по +32).
    static var of_tag(std::uint32_t tag) {
        var out;
        out.set_tag(tag);
        const std::uint32_t scalar_extent = 0x10000;
        std::memcpy(out.raw + layout::var_extent, &scalar_extent, sizeof scalar_extent);
        return out;
    }
};
static_assert(sizeof(var) == layout::var_size);

// Больше в блок не поместится. Не «на всякий случай», а граница, за которой отказ
// лучше молчаливо кривого кадра: у ванильных `proto` аргументов заметно меньше.
inline constexpr std::size_t max_proto_args = 12;

inline std::size_t param_count(const method& fn) {
    return fn.desc ? *reinterpret_cast<const std::uint8_t*>(static_cast<const char*>(fn.desc) +
                                                            layout::desc_param_count)
                   : 0;
}

// Дескриптор i-го параметра — он и есть шаблон его переменной.
inline const void* param_template(const method& fn, std::size_t index) {
    if (!fn.desc || index >= param_count(fn)) {
        return nullptr;
    }
    auto* table = *reinterpret_cast<void* const* const*>(static_cast<const char*>(fn.desc) +
                                                         layout::desc_params);
    return table ? table[index] : nullptr;
}

}  // namespace graft::script
