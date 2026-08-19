// Нативы этого проекта. Один файл — единственный источник истины: отсюда берётся и
// адрес impl для движка, и текст `proto native` для PBO (его пишет graft.exe на каждой сборке).
//
// Заодно это матрица покрытия ABI: по одному нативу на каждый поддерживаемый тип.
// Скриптовая сьюта seraph::graft проверяет их все — после патча игры она же скажет,
// не поехал ли ABI.
//
// Что где: скриптовые классы отражены классами C++ (script_object<"Имя">), их методы —
// обычные методы, свободные функции остаются свободными. Связывается всё в блоках
// GRAFT_BINDINGS в конце файла.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "graft/engine.hpp"
#include "graft/native.hpp"
#include "ping.hpp"

// Зеркало движкового API генерится из скриптов игры (`graft apigen`) в каталог сборки.
// Его может не быть (у чужого проекта нет PDrive) — тогда целевой сценарий ниже честно
// говорит об этом, а не исчезает: пропавший натив уронил бы линковку всего мода.
#if __has_include("graft/dayz/3_Game.hpp")
#include "graft/dayz/3_Game.hpp"
#define SERAPH_HAVE_DAYZ_API 1
#endif

using graft::f32;
using graft::i32;
using graft::str;
using graft::text;
using graft::vector;

namespace {

// Поток, на котором плагин загрузился, — для проверки гипотезы про фибры: скриптовые
// co_routine могут исполняться на своём стеке, а раскрутка по чужому стеку работает не
// так, как по обычному.
unsigned g_load_thread = GetCurrentThreadId();

// ── Скриптовый класс SeraphGraft ─────────────────────────────────────────────
struct Graft : graft::script_object<"SeraphGraft"> {
    // Статический метод: объекта нет.
    static i32 Magic() { return seraph::kMagic; }

    // Обычный member-метод, как у ванильных коллекций: в скрипте объявляется без static
    // и без self, а объект приезжает сюда сам — это и есть this.
    i32 SelfTag() const { return *this ? seraph::kMagic + 1 : -1; }
};

// ── Скриптовый класс SeraphNode: поля объекта, вложенность ───────────────────
// Имя поля известно на компиляции, поэтому пишется параметром шаблона: номер слота
// тогда ищется один раз на класс, а не перебором имён на каждом обращении.
struct Node : graft::script_object<"SeraphNode"> {
    int Id() const { return field<int, "m_id">(); }
    std::string Label() const { return std::string{field<str, "m_label">().view()}; }
    float Weight() const { return field<float, "m_weight">(); }

    // Спуск по вложенным объектам: сумма id по всей цепочке m_child.
    i32 ChainSum() const {
        i32 sum = 0;
        for (Node at = *this; at; at = at.Child()) {
            sum += at.Id();
        }
        return sum;
    }

    // Поле-контейнер: массив внутри объекта читается той же вьюхой.
    int ValuesSum() const {
        int sum = 0;
        for (int v : field<graft::array<int>, "m_values">()) {
            sum += v;
        }
        return sum;
    }

    // Запись в поле объекта.
    int SetId(int id) { return set_field<"m_id">(id) ? Id() : -1; }

    // Обход незнакомой структуры: сколько полей и как называется каждое.
    i32 FieldCount() const { return field_count(); }
    text FieldName(i32 index) const { return text::from(field_name(index)); }

    // Отсутствующее поле — пустой optional, а не тихий ноль.
    text TryField(str name) const {
        const auto value = try_field<i32>(name);
        return value ? text::of("{}", *value) : text::from("нет поля");
    }

private:
    // Своим классом можно читать поле напрямую — он такой же скриптовый тип, как ref<...>.
    Node Child() const { return field<Node, "m_child">(); }
};

// ── Свой шаблонный тип с proto-реализацией ───────────────────────────────────
// Скриптовое объявление написано руками (scripts/1_Core/seraph_graft_class.c), потому
// что шаблонный синтаксис генератор не выражает. Методы не зависят от T, поэтому
// компилируются в обычные нативы и привязываются как методы класса SeraphBox.
struct Box : graft::script_object<"SeraphBox"> {
    // Обычное поле: живёт столько же, сколько скриптовый объект. Что this доезжает и
    // различает экземпляры, видно по независимости счётчиков у разных объектов.
    i32 counter = 0;

    i32 Bump(i32 by) { return counter += by; }
};

// Методом можно привязать и свободную функцию — объект приходит первым параметром.
// Так цепляются классы, которые править нельзя, и уже написанные функции.
i32 BoxTag(const Box& box) {
    return box ? seraph::kMagic : 0;
}

// Сколько SeraphBox сейчас держит C++. Показывает, дошла ли смерть скриптового объекта
// до нашей стороны: движок разрушил объект — библиотека забыла его экземпляр.
i32 SeraphGraftLiveBoxes() {
    return static_cast<i32>(graft::live_instances<Box>());
}

// ── Полный спектр типов, out-аргументы и вложенность ─────────────────────────
// Маршалируемый путь отдаёт значение ЛЮБОГО скриптового типа как graft::value: тип
// приезжает тегом в рантайме. Неконстантная ссылка в сигнатуре — это out-аргумент,
// движок забирает записанное обратно.
struct Any : graft::script_object<"SeraphGraft"> {
    // Что именно приехало — по одному ответу на тип.
    graft::param<"string"> Describe(graft::value v) const {
        if (v.is<bool>()) {
            return std::string{"bool:"} + v.to_string();
        }
        if (v.is<i32>()) {
            return std::string{"int:"} + v.to_string();
        }
        if (v.is<f32>()) {
            return std::string{"float:"} + v.to_string();
        }
        if (v.is<vector>()) {
            return std::string{"vector:"} + v.to_string();
        }
        if (v.is<graft::type>()) {
            return std::string{"typename:"} + v.to_string();
        }
        if (v.is<std::string>()) {
            return std::string{"string:"} + v.to_string();
        }
        if (v.is<graft::obj>()) {
            // Настоящее имя типа объекта: так отличается SeraphNode от array<int>.
            return std::string{"object:"} + std::string{v.type_name().view()};
        }
        return std::string{"пусто"};
    }

    // out-аргументы: заполняем несколько разом, ничего не возвращая. Строку через out
    // отдать нельзя — ею владеет движок, для строк есть возврат.
    void Measure(graft::value source, graft::param<"int">& bytes, graft::param<"int">& words,
                 graft::param<"float">& ratio) const {
        const std::string_view text = source.view();
        i32 count = text.empty() ? 0 : 1;
        for (const char c : text) {
            count += (c == ' ') ? 1 : 0;
        }
        bytes = static_cast<i32>(text.size());
        words = count;
        ratio = count ? static_cast<f32>(text.size()) / static_cast<f32>(count) : 0.0f;
    }

    // Вложенность и ООП: объект приезжает значением, дальше — обычные поля и спуск.
    graft::param<"int"> ChainOf(graft::value node) const {
        i32 sum = 0;
        for (graft::ref<"SeraphNode"> at{node.object().ptr}; at;
             at = at.field<graft::ref<"SeraphNode">, "m_child">()) {
            sum += at.field<int, "m_id">();
        }
        return sum;
    }

    // Вектор туда и обратно через маршалируемый путь.
    graft::param<"vector"> Scale(graft::value v, graft::value k) const {
        return v.as<vector>() * k.as<f32>();
    }
};

// ── Свободные функции: матрица типов ─────────────────────────────────────────

// int -> int, плюс запись наружу процесса: доказательство, что C++ реально исполнился.
// Нативы ниже намеренно написаны ОБЫЧНЫМ C++ — int, std::string_view, std::string,
// std::vector, std::array, неконстантная ссылка вместо out. Так проверяется, что
// перевод типов работает не только в юнит-тесте на именах, но и на живом вызове:
// эти же кейсы сьюты гоняются через них.
int SeraphGraftPing(int token) {
    const int reply = seraph::ping_reply(token);
    graft::log(seraph::proof_line(token, reply));
    return reply;
}

// float+int вперемешку: проверяет раскладку по xmm/целочисленным регистрам.
f32 SeraphGraftMix(f32 a, f32 b, i32 k) {
    return a * b + static_cast<f32>(k);
}

// string как аргумент: движок отдаёт обычный nul-terminated char*.
int SeraphGraftStrLen(std::string_view s) {
    return static_cast<int>(s.size());
}

// два string-аргумента + bool-возврат.
bool SeraphGraftHasPrefix(std::string_view s, std::string_view prefix) {
    return s.starts_with(prefix);
}

// vector и на вход, и на возврат (12-байтная структура — компилятор сам разложит её
// так же, как это делают движковые нативы).
// Скриптовый vector — три float: обычный std::array, без своего типа.
std::array<float, 3> SeraphGraftVecScale(std::array<float, 3> v, float k) {
    return {v[0] * k, v[1] * k, v[2] * k};
}

// Возврат строки: graft::text принимает std::string/string_view и сам держит память до
// конца вызова — ни статических буферов, ни snprintf.
std::string SeraphGraftEcho(std::string_view s) {
    return std::format("echo:{}", s);
}

// То же самое, но возвратом graft::text: форматирование идёт СРАЗУ в арену, временной
// std::string не возникает и куча не тревожится вовсе. Разницу меряет Perf_HotPathBudget
// (textLong против textArena) — она и есть цена одной аллокации.
graft::text SeraphGraftEchoArena(std::string_view s) {
    return graft::text::of("echo:{}", s);
}

// ── Цена поиска метода: имя в рантайме против имени на компиляции ────────────
// Оба зовут ОДИН И ТОТ ЖЕ метод одного и того же класса. Разница только в том, где
// известно его имя. Первый ищет дескриптор на каждом вызове (контексты, хеш в движке,
// VirtualQuery), второй — один раз за процесс. Замеряется в Perf_HotPathBudget.
int SeraphGraftCallByRuntimeName(graft::ref<"SeraphGraft"> o) {
    return o.call<int>("SelfTag");
}
int SeraphGraftCallByCompileName(graft::ref<"SeraphGraft"> o) {
    return o.call<int, "SelfTag">();
}

// Приватная память процесса в килобайтах. Ею проверяется главное свойство перехода на
// движковый аллокатор: строка, которую скрипт не потребил, должна быть освобождена
// движком, а не утечь. Своё кольцо такой вопрос не задавало — оно переиспользовалось.
int SeraphGraftPrivateKB() {
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = sizeof info;
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof info)) {
        return -1;
    }
    return static_cast<int>(info.PrivateUsage / 1024);
}

// Строки выдаёт наше кольцо. Натив остался от эксперимента с движковым аллокатором:
// он позволял сьюте проверять свойства памяти по фактическому режиму, а не по вере.
// Аллокатор отвергнут замером (см. SRC/graft/text.cpp), но кейсы через него написаны —
// и это правильно: они спрашивают у кода, а не предполагают.
bool SeraphGraftEngineStrings() {
    return false;
}

// Сколько байт максимум понадобилось на возвращаемые строки между двумя внешними
// вызовами, и какого размера кольцо. Живёт В ПЛАГИНЕ, а не в хосте: память под строки
// у каждого модуля своя, и чужого расхода ей не видно.
int SeraphGraftTextPeak() {
    return static_cast<int>(graft::detail::peak_demand());
}

int SeraphGraftTextRing() {
    return static_cast<int>(graft::detail::ring_size());
}

// Затирает всё, что арена возвращаемых строк сейчас держит, и говорит сколько байт.
// Нужен одному вопросу: сколько на самом деле обязана жить возвращённая строка.
int SeraphGraftPoisonArena() {
    return static_cast<int>(graft::detail::poison_arena());
}

// Прокручивает кольцо на bytes байт и возвращает переданное число. Ставится ВТОРЫМ
// аргументом, чтобы прокрутка случилась между возвратом первой строки и потреблением:
// так проверяется, чем именно оборачивается переполнение окна.
int SeraphGraftChurnAndPass(int bytes, int value) {
    graft::detail::churn_arena(static_cast<std::size_t>(bytes));
    return value;
}

// То же, но возвращает переданное число — чтобы затирание случилось МЕЖДУ возвратом
// строки и её потреблением: f(Echo("x"), PoisonAndPass(7)).
int SeraphGraftPoisonAndPass(int value) {
    graft::detail::poison_arena();
    return value;
}

// Диагностика дескрипторов: какие флаги у движковых методов и у наших.
// Нужна, чтобы наши регистрации выглядели для компилятора так же, как ванильные.
i32 SeraphGraftMethodFlags(str class_name, str method) {
    const graft::script::method fn = graft::method_of(class_name, method);
    graft::log(std::format("flags {}.{} = {:#x} native={}", class_name.view(), method.view(), fn.flags,
                           fn.callable()));
    return fn.impl ? static_cast<i32>(fn.flags) : -1;
}

// ── Современный стиль: ranges, expected, format ──────────────────────────────

// Обход скриптового массива стандартными алгоритмами: вьюха — полноценный range.
i32 SeraphGraftCountAbove(graft::array<i32> values, i32 threshold) {
    return static_cast<i32>(std::ranges::count_if(values, [&](i32 v) { return v > threshold; }));
}

// std::views поверх скриптовых данных.
i32 SeraphGraftSumEvenDoubled(graft::array<i32> values) {
    i32 sum = 0;
    for (i32 v : values | std::views::filter([](i32 x) { return x % 2 == 0; })
                     | std::views::transform([](i32 x) { return x * 2; })) {
        sum += v;
    }
    return sum;
}

// Промах виден в типе: try_call возвращает expected, поэтому «метод не нативный»
// отличается от «объекта нет», и ничего не падает.
text SeraphGraftProbeCall(graft::ref<"Object"> o, str method) {
    const auto result = o.try_call<i32>(method);
    if (result) {
        return text::of("ok:{}", *result);
    }
    switch (result.error()) {
        case graft::miss::null_object:
            return text::from("null");
        case graft::miss::not_found:
            return text::from("not-found");
        case graft::miss::not_native:
            return text::from("not-native");
        // Дальше — причины, которые бывают только у обратного направления (звать
        // движковый `proto`); у try_call их не случается, но перечислить обязаны.
        case graft::miss::wrong_arity:
            return text::from("arity");
        case graft::miss::too_many_args:
            return text::from("too-many");
        case graft::miss::unsafe_arg:
            return text::from("unsafe");
        case graft::miss::wrong_type:
            return text::from("wrong-type");
        case graft::miss::no_template:
            return text::from("no-template");
    }
    return text::from("?");
}

// ── Свой примитив: тип C++, который скрипт видит как обычный int ─────────────
// Система типов открыта: достаточно специализировать enf_type (имя для объявления) и,
// если тип должен лежать в контейнерах, element_access (как читать из слота).
enum class Team : graft::i32 { none = 0, red = 1, blue = 2 };

}  // namespace

template <>
struct graft::enf_type<Team> {
    static constexpr graft::name_t id{"int"};
    static constexpr const char* name = id.value;
};
template <>
inline constexpr bool graft::is_element<Team> = true;

namespace {

// Дальше Team ведёт себя как полноценный тип: и аргументом, и возвратом, и элементом
// массива — при этом в скрипте это просто int.
Team SeraphGraftOpposite(Team team) {
    switch (team) {
        case Team::red:
            return Team::blue;
        case Team::blue:
            return Team::red;
        default:
            return Team::none;
    }
}

i32 SeraphGraftCountReds(graft::array<Team> teams) {
    i32 reds = 0;
    for (Team t : teams) {
        reds += (t == Team::red) ? 1 : 0;
    }
    return reds;
}

// ── Игровые объекты: движковые методы прямо из C++ ───────────────────────────
// Object наследует IEntity, а поиск метода идёт с обходом базовых классов, поэтому
// GetOrigin/GetID/GetName находятся на объекте любого игрового типа. Методами класса
// это сделать нельзя: к моменту регистрации класс Object ещё не разобран (он в 3_Game),
// поэтому нативы — свободные функции с объектом в аргументе.

// graft::vector — тот же скриптовый vector, что и std::array<float,3>, но с .x/.y/.z и
// арифметикой. Для геометрии читается лучше, поэтому игровые нативы написаны им.
vector SeraphGraftEntityOrigin(graft::ref<"Object"> o) {
    return o.call<vector>("GetOrigin");
}

// Запись: двигаем объект из C++ и возвращаем то, что после этого отдаёт движок.
vector SeraphGraftEntityMove(graft::ref<"Object"> o, vector to) {
    o.call<void>("SetOrigin", to);
    return o.call<vector>("GetOrigin");
}

// owned string от движкового метода, отданный дальше как свой: движок копирует его
// себе на возврате, поэтому владеть нечем.
// Внимание: Object.GetType — НЕ натив, а скриптовый метод (флаги 0x500a18, impl не на
// исполняемой странице), и call() его правильно отказывается звать. GetName — натив
// (0x4a28, как string.Length), хотя у многих объектов он пуст.
text SeraphGraftEntityName(graft::ref<"Object"> o) {
    return o.call<graft::owned>("GetName");
}

// Проверка наличия метода — страховка на случай, если движок переименует API.
bool SeraphGraftEntityHas(graft::ref<"Object"> o, str method) {
    return o.has(method);
}

// Вложенность: контейнер -> игровой объект -> движковый метод -> vector. Считаем сумму
// высот всех объектов массива, ничего не зная о них заранее.
i32 SeraphGraftSumHeights(graft::array<graft::ref<"Object">> objects) {
    i32 total = 0;
    for (graft::ref<"Object"> o : objects) {
        total += static_cast<i32>(o.call<vector>("GetOrigin").y);
    }
    return total;
}

// Обратный ход: C++ двигает каждый объект из массива вверх на dy.
i32 SeraphGraftRaiseAll(graft::array<graft::ref<"Object">> objects, f32 dy) {
    i32 moved = 0;
    for (graft::ref<"Object"> o : objects) {
        vector p = o.call<vector>("GetOrigin");
        p.y += dy;
        o.call<void>("SetOrigin", p);
        ++moved;
    }
    return moved;
}

// ── Контейнеры и произвольные типы ───────────────────────────────────────────

// array<int> на чтение: обычный range-for по скриптовому массиву.
// std::vector — копия движкового массива. Кому копия не нужна, берёт std::span (числа
// лежат подряд, смотрим прямо на них) или graft::array<T> (ещё и писать можно).
int SeraphGraftSumArray(std::vector<int> a) {
    return std::ranges::fold_left(a, 0, std::plus{});
}

// То же самое без копии: span смотрит прямо в буфер движка. Разницу меряет
// Perf_HotPathBudget — строки arrVec против arrSpan.
int SeraphGraftSumSpan(std::span<const int> a) {
    return std::ranges::fold_left(a, 0, std::plus{});
}

// array<string>: элементы приходят как char*.
i32 SeraphGraftStrArrayLen(graft::array<str> a) {
    i32 total = 0;
    for (str s : a) {
        total += static_cast<i32>(s.size());
    }
    return total;
}

// Запись: правим уже существующие элементы (память принадлежит движку, размер не трогаем).
i32 SeraphGraftDoubleArray(graft::array<i32> a) {
    for (i32 i = 0; i < a.size(); ++i) {
        a.set(i, a[i] * 2);
    }
    return a.size();
}

// Остальные методы движка, объявленные как proto native: сортировка и удаление.
// Reserve/Swap звать нельзя — они маршалируемые (см. container_view).
i32 SeraphGraftSortArray(graft::array<i32> a, bool reverse) {
    if (!a.sort(reverse)) {
        return -1;
    }
    return a.size() > 0 ? a[0] : 0;
}

i32 SeraphGraftRemoveAt(graft::array<i32> a, i32 index) {
    return a.remove(index) ? a.size() : -1;
}

// set<T> устроен так же, как array<T>.
i32 SeraphGraftSetSum(graft::set<i32> s) {
    i32 sum = 0;
    for (i32 v : s) {
        sum += v;
    }
    return sum;
}

// typename: у дескриптора класса есть имя.
text SeraphGraftTypeName(graft::type t) {
    return text::from(str{t.name()});
}

// Произвольный скриптовый класс: имя типа задаётся прямо в C++ сигнатуре.
bool SeraphGraftRefAlive(graft::ref<"SeraphGraft"> r) {
    return static_cast<bool>(r);
}

// Полный цикл над массивом из C++: движковый Resize сам выделяет память, дальше пишем
// элементы. Скрипту массив можно отдать пустым.
// Неконстантная ссылка в сигнатуре = `out` в объявлении для скрипта.
int SeraphGraftFillSquares(graft::array<int>& a, int n) {
    if (!a.resize(n)) {
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        a.set(i, i * i);
    }
    return a.size();
}

// Resize/Clear законны и для array<string>: их делает движок, он же освобождает
// элементы (в отличие от прямой записи строк, которая порвала бы кучу).
i32 SeraphGraftClearArray(graft::array<str> a) {
    return a.clear() ? a.size() : -1;
}

// map: размер и очистка — движковыми Count/Clear, содержимое приходит мостом.
i32 SeraphGraftMapSize(graft::map<str, i32> m) {
    return m.size();
}

i32 SeraphGraftMapClear(graft::map<str, i32> m) {
    return m.clear() ? m.size() : -1;
}

// Обход map прямо из C++: итератор берём у движка (он сам пропускает пустые слоты),
// ключ со значением читаем по раскладке узла. Работает для любых типов ключа/значения.
i32 SeraphGraftMapChecksum(graft::map<str, i32> m) {
    i32 sum = 0;
    for (const auto& [key, value] : m) {
        sum += value * static_cast<i32>(key.size());
    }
    return sum;
}

i32 SeraphGraftMapIntSum(graft::map<i32, i32> m) {
    i32 sum = 0;
    for (const auto& [key, value] : m) {
        sum += key * value;
    }
    return sum;
}

// map скрипта -> своя хеш-мапа C++ одним вызовом, без скриптового моста.
i32 SeraphGraftMapToTable(graft::map<str, i32> m) {
    std::unordered_map<std::string, i32> copy;
    for (const auto& [key, value] : m) {
        copy.emplace(key.view(), value);
    }
    i32 sum = 0;
    for (const auto& [key, value] : copy) {
        sum += value * static_cast<i32>(key.size());
    }
    return sum;
}

// Третья раскладка узла: два 4-байтных поля РАЗНЫХ типов — шаг 16, used +4, key +8,
// value +12 (у <int,int> шаг 12, у пары со строкой — 24). Возвращаем целым, чтобы
// сравнение в скрипте не зависело от точности float.
i32 SeraphGraftMapIntFloat(graft::map<i32, f32> m) {
    f32 sum = 0;
    for (const auto& [key, value] : m) {
        sum += static_cast<f32>(key) * value;
    }
    return static_cast<i32>(sum * 10.0f);
}

// ── Мост между хеш-мапой C++ и map скрипта ───────────────────────────────────
// Хранилище на стороне C++ — обычный std::unordered_map.
std::unordered_map<std::string, i32>& table() {
    static std::unordered_map<std::string, i32> t{{"alpha", 1}, {"beta", 22}, {"gamma", 333}};
    return t;
}

// Порядок у хеш-таблицы не определён, но между вызовами не меняется, поэтому Key(i) и
// Value(i) идут одним и тем же обходом и всегда согласованы.
// ponytail: std::next вместо параллельного вектора порядка — в таблице три записи;
// понадобится O(1) — снимай снимок обхода один раз и инвалидируй его на мутации.
auto table_at(i32 index) {
    return std::next(table().begin(), index);
}
bool in_table(i32 index) {
    return index >= 0 && std::cmp_less(index, table().size());
}

// C++ -> DayZ. Строки отдаём возвратом `owned string`: движок копирует их себе, и
// вопрос владения не встаёт (писать char* прямо в array<string> нельзя — см. set()).
i32 SeraphGraftTableCount() {
    return static_cast<i32>(table().size());
}

text SeraphGraftTableKey(i32 index) {
    return in_table(index) ? text::from(table_at(index)->first) : text{};
}

i32 SeraphGraftTableValue(i32 index) {
    return in_table(index) ? table_at(index)->second : 0;
}

// out-массив чисел заполняется прямо из C++: размер ставит движковый Resize, а за
// int'ами движок не следит, поэтому запись безопасна.
i32 SeraphGraftExportValues(graft::out<graft::array<i32>> values) {
    if (!values.resize(static_cast<i32>(table().size()))) {
        return -1;
    }
    for (const auto [i, v] : std::views::enumerate(std::views::values(table()))) {
        values.set(static_cast<i32>(i), v);
    }
    return values.size();
}

// DayZ -> C++: ключи и значения приезжают массивами и ложатся в обычный std-контейнер.
i32 SeraphGraftImportTable(graft::array<str> keys, graft::array<i32> values) {
    std::unordered_map<std::string, i32> imported;
    for (const auto [key, value] : std::views::zip(keys, values)) {
        imported.emplace(key.view(), value);
    }
    i32 sum = 0;
    for (const auto& [key, value] : imported) {
        sum += value * static_cast<i32>(key.size());
    }
    return sum;
}

// Потребитель моста: ключи и значения карты, разложенные скриптом в массивы. Две вьюхи
// проходятся парой — zip сам обрежет по короткой, ручного индекса не нужно.
i32 SeraphGraftSumPairs(graft::array<str> keys, graft::array<i32> values) {
    i32 sum = 0;
    for (const auto [key, value] : std::views::zip(keys, values)) {
        sum += value * static_cast<i32>(key.size());
    }
    return sum;
}

// ── Вызов В ДРУГУЮ СТОРОНУ: движковые `proto` из C++ ─────────────────────────
// Всё, что ниже, проверяется на ЖИВОМ движке: заранее известно только то, что мы
// разобрали по дизассемблеру, а верно оно или нет — говорит сьюта.

// Сколько параметров у движковой функции ПО ЕЁ ДЕСКРИПТОРУ. Это первое, что должно
// сойтись: не сойдётся — блок аргументов собирать не по чему.
i32 SeraphGraftProtoArity(str class_name, str method) {
    const graft::script::method fn = graft::script::find_method(class_name.c_str(), method.c_str());
    if (!fn.desc) {
        return -1;
    }
    return static_cast<i32>(graft::script::param_count(fn));
}

// Тег типа i-го параметра — по нему видно, что шаблон переменной настоящий, а не
// прочитанный мимо.
i32 SeraphGraftProtoParamTag(str class_name, str method, i32 index) {
    const graft::script::method fn = graft::script::find_method(class_name.c_str(), method.c_str());
    const void* tpl = graft::script::param_template(fn, static_cast<std::size_t>(index));
    if (!tpl) {
        return 0;
    }
    return static_cast<i32>(*reinterpret_cast<const std::uint32_t*>(
        static_cast<const char*>(tpl) + graft::layout::var_type));
}

// Первые байты импла движковой функции. Нужны, когда соглашение вызова приходится
// выяснять на живом бинаре: RVA после патча игры уезжает, а пролог — нет.
text SeraphGraftProtoImplBytes(str class_name, str method, i32 count) {
    const graft::script::method fn = graft::script::find_method(class_name.c_str(), method.c_str());
    if (!fn.impl) {
        return text::from("нет");
    }
    const i32 want = count > 0 ? (count > 48 ? 48 : count) : 32;
    const auto* at = static_cast<const unsigned char*>(fn.impl);
    std::string hex;
    for (i32 i = 0; i < want; ++i) {
        hex += std::format("{:02x}", at[i]);
    }
    return text::of("{}", hex);
}

// Первый настоящий маршалируемый вызов наружу: `typename.GetModule()` — метод класса
// typename, то есть зовётся на дескрипторе класса. Ответ проверяемый: "1_Core", "3_Game".
text SeraphGraftTypeModule(str class_name) {
    void* found = graft::script::find_class(class_name.c_str());
    if (!found) {
        return text::from("нет класса");
    }
    const auto module =
        graft::type{found}.proto<std::string, "GetModule">();
    return module.empty() ? text::from("пусто") : text::of("{}", module);
}

// Создание скриптового объекта из C++: typename.Spawn(). Отвечает именем класса того,
// что получилось, — так видно и что объект создан, и что он нужного типа.
text SeraphGraftSpawnClass(str class_name) {
    void* found = graft::script::find_class(class_name.c_str());
    if (!found) {
        return text::from("нет класса");
    }
    const auto made = graft::type{found}.try_proto<graft::obj, "Spawn">();
    if (!made) {
        return text::of("отказ:{}", static_cast<int>(made.error()));
    }
    if (!*made) {
        return text::from("null");
    }
    return text::of("{}", made->type_name().view());
}

// ── Можно ли забрать падение у движка ───────────────────────────────────────
// В проекте записано «SEH тут не работает: движок ставит свой фильтр и валит процесс
// раньше нашего __except». Разбор это ОПРОВЕРГАЕТ:
//
//   sub_14037ED60:  AddVectoredExceptionHandler(0, Handler)   // 0 — в КОНЕЦ списка
//                   SetUnhandledExceptionFilter(TopLevelExceptionFilter)
//   Handler:        порча кучи -> верхний фильтр; ВСЁ ОСТАЛЬНОЕ -> return 0
//
// `return 0` это EXCEPTION_CONTINUE_SEARCH: движок на первом проходе НИЧЕГО не делает,
// а верхний фильтр зовётся только когда кадрового обработчика не нашлось. Значит наш
// __except обязан получать управление первым. Кейс это и проверяет — фактом, а не
// рассуждением.
//
// Отдельная функция без объектов C++: __try нельзя ставить туда, где что-то требует
// раскрутки, и это ограничение компилятора, а не стиль.
// Сбой ОБЯЗАН случиться внутри диапазона __try, а не в заинлайненном куске: таблица
// областей у x64-SEH описывает диапазоны адресов, и если компилятор вынес обращение
// наружу, обработчик не найдётся. Поэтому падаем в отдельной функции без инлайна.
__declspec(noinline) i32 fault_now() {
    return *reinterpret_cast<volatile int*>(0);
}

i32 SeraphGraftGuardedFault() {
    __try {
        return fault_now();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0x5E4;  // дожили — падение поймано нами
    }
}

// Тот же натив, но БЕЗ падения: сколько стоит сама защита, когда ничего не случилось.
// На x64 SEH табличный — записи раскрутки лежат в .pdata, и в рантайме не исполняется
// ничего. Метрика это подтверждает или опровергает.
__declspec(noinline) i32 work_now(i32 token) {
    return token ^ 0x5E1AF;
}

i32 SeraphGraftGuardedNoop(i32 token) {
    __try {
        return work_now(token);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Строка из скрипта в СИСТЕМНЫЙ журнал (graft_<метка>.log). Нужна ровно затем, что
// журнал сьюты эфемерный и удаляется вместе с профилем, а журнал библиотеки живёт:
// замеры Perf_HotPathBudget иначе видно только когда они УПАЛИ, а числа в README нужны
// и когда всё зелено.
void SeraphGraftNote(std::string_view line) {
    graft::log(std::string{line});
}

// Пользовательский канал: строка уходит в журналы САМОЙ ИГРЫ — Print в script-лог,
// Error в crash-лог. Возвращают, дошло ли до движка: до первого тика корня ещё нет.
bool SeraphGraftSay(std::string_view line) {
    return graft::print(line);
}

bool SeraphGraftCry(std::string_view line) {
    return graft::error(line);
}

// Проба: нашлась ли по имени глобаль движка и позвалась ли она вслепую. Ею видно, что
// путь «имя с регистрации -> импл -> кадр по донорам» не отвалился после патча игры.
i32 SeraphGraftSayVia(std::string_view fn, std::string_view line) {
    void* impl = graft::script::find_global(std::string{fn}.c_str());
    if (!impl) {
        return -1;
    }
    const graft::value args[] = {graft::value{std::string{line}}};
    return graft::call_proto(graft::as_global(impl), nullptr, args).has_value() ? 1 : 0;
}

// ── Защита самой библиотеки ─────────────────────────────────────────────────
// Два натива выше защищают себя САМИ — ими измерялась цена и проверялось, что кадровый
// обработчик вообще получает управление. Эти три не защищают себя ничем: их роняет
// трамплин библиотеки. Если защита работает, скрипт получит нулевое значение и живой
// сервер; если нет — сервер умрёт прямо на этой строке, и это будет видно.
//
// Обращение по нулю написано ПРЯМО В ТЕЛЕ, без noinline: требование «сбой в отдельной
// функции» относится к устройству защиты (тело вызова там и так отделено от __try), а не
// к коду пишущего натив. Кейс это и сторожит — на живом сервере, а не на словах.
i32 SeraphGraftFaultsHere() {
    volatile int* nowhere = nullptr;
    return *nowhere;
}

i32 SeraphGraftThrowsHere() {
    throw std::runtime_error("натив передумал на середине");
}

// То же, но с возвратом строки: движок копирует её сам и nullptr не проверяет, поэтому
// отказ обязан выглядеть пустой строкой, а не падением уже В ДВИЖКЕ.
std::string SeraphGraftThrowsText() {
    throw std::runtime_error("строковый натив передумал");
}

// На каком потоке нас зовут. Гипотеза, которую это проверяет: скриптовые co_routine
// могут исполняться на своём стеке (фибре), а раскрутка исключений по чужому стеку
// работает не так. Если поток тот же, что при загрузке, гипотеза отпадает.
i32 SeraphGraftThreadHere() {
    return static_cast<i32>(GetCurrentThreadId());
}
i32 SeraphGraftThreadAtLoad() {
    return static_cast<i32>(g_load_thread);
}

// Маршалируемый метод НАСТОЯЩЕГО КЛАССА — форма с объектом в rcx. Остальные кейсы
// проверяют две другие формы (статическую и метод типа-значения), а эта до сих пор
// выполнялась только у PlayerIdentity.GetPlainId, то есть при живом игроке. Здесь она
// проверяется на пустом сервере: ScriptModule.CallFunction зовёт СКРИПТОВУЮ функцию
// GetGame(), и её ответ обязан совпасть с корнем, который принёс тик.
text SeraphGraftCallScriptFunction() {
    using namespace graft::literals;
    const graft::ref<"DayZGame"> game = graft::game();
    if (!game) {
        return text::from("нет корня");
    }
    const auto holder = game["GameScript"_f].get<graft::obj>();
    if (!holder || !*holder) {
        return text::from("нет GameScript");
    }
    // proto volatile int CallFunction(Class inst, string function, out void returnVal, void parm)
    const graft::script::method fn =
        graft::script::find_method("ScriptModule", "CallFunction");
    const graft::value args[] = {graft::value{graft::obj{}}, graft::value{std::string{"GetGame"}},
                                 graft::value{graft::obj{}}, graft::value{graft::obj{}}};
    const auto r = graft::call_proto(fn, holder->ptr, args, graft::script::tag_int);
    if (!r) {
        return text::of("отказ:{}", static_cast<int>(r.error()));
    }
    const graft::obj returned = r->arg(2).as<graft::obj>();
    if (!returned) {
        return text::from("вернулось пусто");
    }
    return text::of("{}", returned.type_name().view());
}

// Вызов объявления, которого в ЭТОЙ сборке игры нет. Зеркало печатает объединение всех
// веток препроцессора, поэтому такие вызовы законны и должны стоить КОПЕЙКИ: поиск
// прекращается после нескольких промахов, дальше это чтение статической ячейки.
i32 SeraphGraftCallMissing(graft::ref<"SeraphGraft"> inst) {
    return inst.call<i32, "NoSuchMethodInAnyBuildOfTheGame">();
}

// То же поле, что читает Node::Id, но синтаксисом sol2. Существует ради метрики: два
// синтаксиса обязаны стоить одинаково, иначе сахар оказался не бесплатным.
i32 SeraphGraftNodeIdProxy(graft::ref<"SeraphNode"> node) {
    using namespace graft::literals;
    return node["m_id"_f];
}

// Поле объекта РУКАМИ ДВИЖКА, а не по нашей раскладке: маршалируемый вызов с
// out-аргументом целиком, включая обратное чтение. Сверяется с прямым чтением поля —
// если разойдутся, виноват один из двух путей, и это будет видно.
i32 SeraphGraftClassVarInt(graft::obj node, str name) {
    const auto got = graft::try_script_var<i32>(node, name.c_str());
    return got ? *got : -1;
}

// Корень объектного графа игры: приезжает вместе с тиком.
text SeraphGraftGameClass() {
    const auto found = graft::try_game();
    if (!found) {
        return text::of("отказ:{}", static_cast<int>(found.error()));
    }
    return found->ptr ? text::of("{}", found->type_name().view()) : text::from("null");
}

// ── Тик ──────────────────────────────────────────────────────────────────────
i32 g_ticks = 0;
f32 g_last_dt = 0;

// Тик приходит из ДВИЖКОВОЙ точки входа кадра, то есть СНАРУЖИ скриптового вызова.
// Законен ли отсюда вызов движкового `proto` — вопрос, ради которого всё затевалось.
// Замер (кейс Tick_LoopProbe, числа уходят в системный журнал строкой [tick]):
//
//   числовой результат (EnScript.GetClassVar)   — 8 из 8 кадров, ни одного промаха
//   объектный результат (typename.Spawn)        — 8 из 8 кадров
//   СТРОКОВЫЙ результат (typename.GetModule)    — 0 из 8: вызов проходит, строка ПУСТАЯ
//
// То есть дело не в форме вызова и не в отсутствии скриптового кадра как такового, а
// именно в пути возврата строки. Проба оставлена в тике сторожем: починим — станет
// зелёной сама.
i32 g_tick_proto_ok = 0;
i32 g_tick_proto_fail = 0;
i32 g_tick_proto_why = -1;   // код miss последнего отказа, -1 — отказов не было
i32 g_tick_class_found = 0;
i32 g_tick_var_ok = 0;      // маршалируемый вызов с ЧИСЛОВЫМ результатом
i32 g_tick_var_why = -1;
i32 g_tick_spawn_ok = 0;    // typename.Spawn из тика: своё дело замер сделал, см. README

GRAFT_ON_TICK(dt) {
    ++g_ticks;
    g_last_dt = dt;
    // typename.GetModule() — метод типа-значения, самая требовательная из трёх форм
    // маршалируемого вызова: приёмник едет первым в блоке переменных.
    void* found = graft::script::find_class("SeraphGraft");
    g_tick_class_found = found ? 1 : 0;
    const auto module =
        found ? graft::type{found}.try_proto<std::string, "GetModule">()
              : std::expected<std::string, graft::miss>{std::unexpected(graft::miss::not_found)};
    // Второй пробой, но с числовым результатом: если строка пустая, а число приходит,
    // значит дело не в самом вызове, а в пути возврата строки.
    if (const auto flag = graft::try_script_var<i32>(graft::obj{graft::script::root()}, "m_DebugMonitorEnabled");
        flag) {
        ++g_tick_var_ok;
    } else {
        g_tick_var_why = static_cast<i32>(flag.error());
    }
    if (module && !module->empty()) {
        ++g_tick_proto_ok;
    } else {
        ++g_tick_proto_fail;
        g_tick_proto_why = module ? 100 : static_cast<i32>(module.error());
    }
}

i32 SeraphGraftTickProtoOk() {
    return g_tick_proto_ok;
}
i32 SeraphGraftTickProtoFail() {
    return g_tick_proto_fail;
}
i32 SeraphGraftTickProtoWhy() {
    return g_tick_proto_why;
}
i32 SeraphGraftTickClassFound() {
    return g_tick_class_found;
}
i32 SeraphGraftTickVarOk() {
    return g_tick_var_ok;
}
i32 SeraphGraftTickVarWhy() {
    return g_tick_var_why;
}
i32 SeraphGraftTickSpawnOk() {
    return g_tick_spawn_ok;
}

i32 SeraphGraftTicks() {
    return g_ticks;
}
f32 SeraphGraftLastDt() {
    return g_last_dt;
}

// ── Целевой сценарий ─────────────────────────────────────────────────────────
// Собрать игроков и узнать их steam id — целиком из C++, ни строчки скрипта. Ровно то,
// ради чего затевалось обратное направление.
//
// Возврат ОБЪЕКТА из движкового натива: тот ли это объект, что видит скрипт.
//
// Кейс сторожит правило x64 ABI, на котором библиотека однажды уже обожглась: тип зеркала
// это struct с базовым классом, и MSVC такие возвращает через скрытый буфер, а движок
// кладёт указатель в rax. Пока сверка идёт с указателем, пришедшим ИЗ СКРИПТА, никакая
// перестановка регистров мимо не проедет.
bool SeraphGraftObjectReturnMatches(graft::obj from_script) {
#ifdef SERAPH_HAVE_DAYZ_API
    const auto game = graft::cast<graft::dayz::CGame>(graft::game());
    if (!game || !from_script.ptr) {
        return false;
    }
    // Через ЗЕРКАЛО — ровно тем путём, каким мод зовёт движок.
    return game.GetMission().ptr == from_script.ptr;
#else
    return false;
#endif
}

text SeraphGraftPlayerIds() {
#ifdef SERAPH_HAVE_DAYZ_API
    const auto game = graft::cast<graft::dayz::CGame>(graft::game());
    if (!game) {
        return text::from("нет g_Game");
    }
    // Буфер под out-аргумент: один на процесс, движок им владеет сам.
    const auto players = graft::scratch<graft::array<graft::dayz::Man>>();
    if (!players) {
        return text::from("нет array<Man>");
    }
    players.clear();
    game.GetPlayers(players);

    std::string ids;
    for (const graft::dayz::Man man : players) {
        const graft::dayz::PlayerIdentity id = man.GetIdentity();
        if (!id) {
            continue;
        }
        if (!ids.empty()) {
            ids += ',';
        }
        ids += id.GetPlainId();
    }
    return text::of("{}:{}", players.size(), ids);
#else
    return text::from("зеркало не сгенерено");
#endif
}

// ── Что регистрировать и что объявлять ───────────────────────────────────────

GRAFT_BINDINGS("1_Core") {
    bind.class_<Any>()
        .method<&Any::Describe>("Describe")
        .method<&Any::Measure>("Measure")
        .method<&Any::ChainOf>("ChainOf")
        .method<&Any::Scale>("Scale");

    bind.class_<Graft>()
        .static_method<&Graft::Magic>("Magic")
        .method<&Graft::SelfTag>("SelfTag");

    bind.class_<Node>()
        .method<&Node::Id>("Id")
        .method<&Node::Label>("Label")
        .method<&Node::Weight>("Weight")
        .method<&Node::ChainSum>("ChainSum")
        .method<&Node::ValuesSum>("ValuesSum")
        .method<&Node::SetId>("SetId")
        .method<&Node::FieldCount>("FieldCount")
        .method<&Node::FieldName>("FieldName")
        .method<&Node::TryField>("TryField");

    // Шаблонный класс: объявление в скрипте написано руками.

    bind.class_<Box>(graft::declared)
        .method<&BoxTag>("Tag")
        .method<&Box::Bump>("Bump");

    bind.global<&SeraphGraftPing>("SeraphGraftPing")
        .global<&SeraphGraftMix>("SeraphGraftMix")
        .global<&SeraphGraftStrLen>("SeraphGraftStrLen")
        .global<&SeraphGraftHasPrefix>("SeraphGraftHasPrefix")
        .global<&SeraphGraftVecScale>("SeraphGraftVecScale")
        .global<&SeraphGraftEcho>("SeraphGraftEcho")
        .global<&SeraphGraftEchoArena>("SeraphGraftEchoArena")
        .global<&SeraphGraftMethodFlags>("SeraphGraftMethodFlags")
        .global<&SeraphGraftEngineStrings>("SeraphGraftEngineStrings")
        .global<&SeraphGraftPrivateKB>("SeraphGraftPrivateKB")
        .global<&SeraphGraftTextPeak>("SeraphGraftTextPeak")
        .global<&SeraphGraftTextRing>("SeraphGraftTextRing")
        .global<&SeraphGraftChurnAndPass>("SeraphGraftChurnAndPass")
        .global<&SeraphGraftPoisonArena>("SeraphGraftPoisonArena")
        .global<&SeraphGraftPoisonAndPass>("SeraphGraftPoisonAndPass")
        .global<&SeraphGraftCallByRuntimeName>("SeraphGraftCallByRuntimeName")
        .global<&SeraphGraftCallByCompileName>("SeraphGraftCallByCompileName")
        .global<&SeraphGraftCountAbove>("SeraphGraftCountAbove")
        .global<&SeraphGraftSumEvenDoubled>("SeraphGraftSumEvenDoubled")
        .global<&SeraphGraftOpposite>("SeraphGraftOpposite")
        .global<&SeraphGraftCountReds>("SeraphGraftCountReds")
        .global<&SeraphGraftLiveBoxes>("SeraphGraftLiveBoxes");

    bind.global<&SeraphGraftSumArray>("SeraphGraftSumArray")
        .global<&SeraphGraftSumSpan>("SeraphGraftSumSpan")
        .global<&SeraphGraftStrArrayLen>("SeraphGraftStrArrayLen")
        .global<&SeraphGraftDoubleArray>("SeraphGraftDoubleArray")
        .global<&SeraphGraftSortArray>("SeraphGraftSortArray")
        .global<&SeraphGraftRemoveAt>("SeraphGraftRemoveAt")
        .global<&SeraphGraftSetSum>("SeraphGraftSetSum")
        .global<&SeraphGraftTypeName>("SeraphGraftTypeName")
        .global<&SeraphGraftRefAlive>("SeraphGraftRefAlive")
        .global<&SeraphGraftFillSquares>("SeraphGraftFillSquares")
        .global<&SeraphGraftClearArray>("SeraphGraftClearArray");

    // Обратное направление: движковые `proto` из C++.
    bind.global<&SeraphGraftProtoArity>("SeraphGraftProtoArity")
        .global<&SeraphGraftProtoParamTag>("SeraphGraftProtoParamTag")
        .global<&SeraphGraftProtoImplBytes>("SeraphGraftProtoImplBytes")
        .global<&SeraphGraftTypeModule>("SeraphGraftTypeModule")
        .global<&SeraphGraftSpawnClass>("SeraphGraftSpawnClass")
        .global<&SeraphGraftClassVarInt>("SeraphGraftClassVarInt")
        .global<&SeraphGraftNodeIdProxy>("SeraphGraftNodeIdProxy")
        .global<&SeraphGraftCallMissing>("SeraphGraftCallMissing")
        .global<&SeraphGraftNote>("SeraphGraftNote")
        .global<&SeraphGraftSay>("SeraphGraftSay")
        .global<&SeraphGraftCry>("SeraphGraftCry")
        .global<&SeraphGraftSayVia>("SeraphGraftSayVia")
        .global<&SeraphGraftFaultsHere>("SeraphGraftFaultsHere")
        .global<&SeraphGraftThrowsHere>("SeraphGraftThrowsHere")
        .global<&SeraphGraftThrowsText>("SeraphGraftThrowsText")
        .global<&SeraphGraftGuardedFault>("SeraphGraftGuardedFault")
        .global<&SeraphGraftGuardedNoop>("SeraphGraftGuardedNoop")
        .global<&SeraphGraftThreadHere>("SeraphGraftThreadHere")
        .global<&SeraphGraftThreadAtLoad>("SeraphGraftThreadAtLoad")
        .global<&SeraphGraftTickProtoOk>("SeraphGraftTickProtoOk")
        .global<&SeraphGraftTickProtoWhy>("SeraphGraftTickProtoWhy")
        .global<&SeraphGraftTickClassFound>("SeraphGraftTickClassFound")
        .global<&SeraphGraftTickVarOk>("SeraphGraftTickVarOk")
        .global<&SeraphGraftTickVarWhy>("SeraphGraftTickVarWhy")
        .global<&SeraphGraftTickSpawnOk>("SeraphGraftTickSpawnOk")
        .global<&SeraphGraftTickProtoFail>("SeraphGraftTickProtoFail")
        .global<&SeraphGraftTicks>("SeraphGraftTicks")
        .global<&SeraphGraftLastDt>("SeraphGraftLastDt");

    bind.global<&SeraphGraftMapSize>("SeraphGraftMapSize")
        .global<&SeraphGraftMapClear>("SeraphGraftMapClear")
        .global<&SeraphGraftMapChecksum>("SeraphGraftMapChecksum")
        .global<&SeraphGraftMapIntSum>("SeraphGraftMapIntSum")
        .global<&SeraphGraftMapToTable>("SeraphGraftMapToTable")
        .global<&SeraphGraftMapIntFloat>("SeraphGraftMapIntFloat");

    bind.global<&SeraphGraftTableCount>("SeraphGraftTableCount")
        .global<&SeraphGraftTableKey>("SeraphGraftTableKey")
        .global<&SeraphGraftTableValue>("SeraphGraftTableValue")
        .global<&SeraphGraftExportValues>("SeraphGraftExportValues")
        .global<&SeraphGraftImportTable>("SeraphGraftImportTable")
        .global<&SeraphGraftSumPairs>("SeraphGraftSumPairs");
}

// Игровые типы в 1_Core ещё не объявлены — этим нативам нужен 3_Game.
GRAFT_BINDINGS("3_Game") {
    bind.global<&SeraphGraftProbeCall>("SeraphGraftProbeCall")
        .global<&SeraphGraftEntityOrigin>("SeraphGraftEntityOrigin")
        .global<&SeraphGraftEntityMove>("SeraphGraftEntityMove")
        .global<&SeraphGraftEntityName>("SeraphGraftEntityName")
        .global<&SeraphGraftEntityHas>("SeraphGraftEntityHas")
        .global<&SeraphGraftSumHeights>("SeraphGraftSumHeights")
        .global<&SeraphGraftRaiseAll>("SeraphGraftRaiseAll")
        // Игровые классы (CGame, Man) появляются только здесь.
        .global<&SeraphGraftGameClass>("SeraphGraftGameClass")
        .global<&SeraphGraftPlayerIds>("SeraphGraftPlayerIds")
        .global<&SeraphGraftObjectReturnMatches>("SeraphGraftObjectReturnMatches")
        .global<&SeraphGraftCallScriptFunction>("SeraphGraftCallScriptFunction");
}

}  // namespace
