#pragma once
#include <array>
#include <bit>
#include <concepts>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "graft/engine.hpp"
#include "graft/name.hpp"
#include "graft/script.hpp"

// Типы Enforce в терминах C++: то, что пишется в сигнатуре натива.
//
// Скалярам своих типов не нужно — int, float, bool, std::string и std::string_view
// переводятся сами (см. convert.hpp). Здесь живёт то, чему нужен свой тип: строка в
// движковом представлении, vector, ссылка на скриптовый объект и значение с тегом.
namespace graft {

// ── Типы Enforce в терминах C++ ──────────────────────────────────────────────
using i32 = std::int32_t;  // Enforce int — 32-битный знаковый
using f32 = float;         // Enforce float

// Enforce string как аргумент. Внутри — тот самый char*, который кладёт движок, но
// наружу это обычная строка: сравнивается, режется, приводится к string_view.
// Размер и тривиальность обязаны совпадать с const char*, иначе поедет ABI.
// ВАЖНО: агрегат без пользовательских конструкторов. По MS x64 ABI 8-байтная структура
// возвращается в регистре и передаётся в регистре только пока она такая; стоит завести
// свой конструктор — компилятор перейдёт на скрытый указатель, и ABI разъедется с
// движком (проверено падением).
struct str {
    const char* raw = nullptr;

    constexpr const char* c_str() const { return raw ? raw : ""; }
    constexpr explicit operator bool() const { return raw != nullptr; }
    constexpr operator std::string_view() const { return view(); }
    explicit operator std::string() const { return std::string{view()}; }
    constexpr std::string_view view() const {
        return raw ? std::string_view{raw} : std::string_view{};
    }
    constexpr bool empty() const { return view().empty(); }
    constexpr std::size_t size() const { return view().size(); }
    constexpr bool starts_with(std::string_view p) const { return view().starts_with(p); }
    constexpr bool contains(std::string_view p) const { return view().find(p) != view().npos; }

    friend constexpr bool operator==(str a, str b) { return a.view() == b.view(); }
    friend constexpr bool operator==(str a, std::string_view b) { return a.view() == b; }
    friend constexpr auto operator<=>(str a, str b) { return a.view() <=> b.view(); }
};
static_assert(sizeof(str) == sizeof(const char*) && std::is_trivially_copyable_v<str>);

// Enforce vector — три float подряд, как их кладёт движок. Имя такое же, как в скрипте:
// в сигнатуре натива читается ровно как объявление.
//
// ВНИМАНИЕ внутри библиотеки: рядом живёт std::vector, поэтому в namespace graft писать
// `vector` без квалификации для стандартного контейнера нельзя — только `std::vector`.
struct vector {
    float x = 0, y = 0, z = 0;

    constexpr vector operator+(vector o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr vector operator-(vector o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr vector operator*(float k) const { return {x * k, y * k, z * k}; }
    constexpr float dot(vector o) const { return x * o.x + y * o.y + z * o.z; }
    float length() const { return std::sqrt(dot(*this)); }
    friend constexpr bool operator==(vector, vector) = default;

    // Взаимозаменяемость с обычным std::array<float, 3>: раскладка та же, поэтому
    // переход в обе стороны бесплатный, и в сигнатуре натива можно писать любой из двух.
    constexpr explicit operator std::array<float, 3>() const { return {x, y, z}; }
    static constexpr vector from(std::array<float, 3> a) { return {a[0], a[1], a[2]}; }
};
static_assert(sizeof(vector) == sizeof(std::array<float, 3>));

using vec3 = vector;  // прежнее имя

// Возврат `owned string`. Текст складывается в арену на поток — скользящее окно: внутри
// вызова строки копятся без ограничения, хвост обрезается только на входе в следующий
// ВНЕШНИЙ вызов. Освобождать в конце вызова нельзя: движок читает результат уже после
// возврата, а скрипт держит строку между нативами (см. SRC/graft/text.cpp — там оба
// случая описаны падениями). Пользовательский код при этом обычный: std::string, format.
namespace detail {
const char* stash(std::string_view text);
// Место под строку прямо в арене: форматирование пишет туда сразу, без временной
// std::string и без обращения к куче.
char* reserve_text(std::size_t bytes);
// Диагностика: сколько байт максимум понадобилось между двумя внешними вызовами и
// каков размер кольца. По ним видно, что запас не подобран, а измерен.
std::size_t peak_demand();
std::size_t ring_size();
// Затирает всё выданное — ею измерено требование движка ко времени жизни. См. text.cpp.
std::size_t poison_arena();
// Прокручивает кольцо: так проверяется, что переполнение окна даёт неверный текст,
// а не падение.
std::size_t churn_arena(std::size_t bytes);

// Глубину вложенности вызовов считает call_scope, который заводит каждый трамплин: по
// ней арена понимает, где начинается новый внешний вызов.
void enter_call();
void leave_call();

struct call_scope {
    call_scope() { enter_call(); }
    ~call_scope() { leave_call(); }
    call_scope(const call_scope&) = delete;
    call_scope& operator=(const call_scope&) = delete;
};
}

struct text {
    const char* raw = "";

    // Фабрики, а не конструкторы — по той же причине, что и у str: агрегат обязан
    // остаться агрегатом, иначе возврат уедет на скрытый указатель.
    static text from(std::string_view s) { return {detail::stash(s)}; }
    static text from(str s) { return {s.c_str()}; }

    // graft::text::of("{}:{}", name, count) — форматирование без ручных буферов.
    //
    // Пишет СРАЗУ в арену: временной std::string не возникает, поэтому на строку до 255
    // байт куча не тревожится вовсе. Длиннее — обычный std::format с одной аллокацией.
    template <class... A>
    static text of(std::format_string<A...> fmt, A&&... args) {
        // `small` тут писать нельзя: windows.h тащит rpcndr.h, где это макрос (#define
        // small char). Заголовок библиотеки включают вместе с ним, поэтому имя другое.
        char stack[256];
        // forward, а не args...: format_to_n выводит Args из аргументов, и с lvalue
        // получилось бы format_string<A&...> вместо нашего format_string<A...>.
        // Форматтеры аргументы только читают, поэтому второй проход по ним безопасен.
        const auto done = std::format_to_n(stack, sizeof stack - 1, fmt, std::forward<A>(args)...);
        const auto size = static_cast<std::size_t>(done.size);
        if (size < sizeof stack) {
            char* dst = detail::reserve_text(size);
            std::memcpy(dst, stack, size);
            dst[size] = '\0';
            return {dst};
        }
        return from(std::format(fmt, std::forward<A>(args)...));
    }

    constexpr std::string_view view() const {
        return raw ? std::string_view{raw} : std::string_view{};
    }
};
static_assert(sizeof(text) == sizeof(const char*) && std::is_trivially_copyable_v<text>);

using owned = text;  // прежнее имя типа возврата

// Чтение значения из слота памяти по правилам скриптового типа (объявление здесь,
// определение — в enf_type.hpp, после element_access).
template <class T>
T load_slot(const void* slot);

// Что можно писать в поле объекта напрямую. Строками и ссылками владеет движок: он
// считает ссылки и освобождает память, поэтому запись мимо него их ломает.
template <class T>
concept owned_by_script = std::same_as<T, i32> || std::same_as<T, f32> || std::same_as<T, bool>;

// Значение любого скриптового типа — определение ниже, но упомянуть его надо раньше:
// маршалируемый вызов принимает аргументы именно им.
class value;

// Почему обращение не состоялось. Возвращается в std::expected/std::optional, чтобы
// промах было видно в типе, а не только по нулю.
enum class miss {
    null_object,
    not_found,
    not_native,
    // Дальше — только про обратное направление (звать движковый `proto`): блок
    // аргументов собирается по дескриптору, и расхождение с ним это не «вернём ноль»,
    // а кривой кадр вызова.
    wrong_arity,
    too_many_args,
    unsafe_arg,
    wrong_type,    // тип аргумента разошёлся с дескриптором движка
    no_template,   // не нашлось шаблона переменной этого типа — собирать её самим нельзя
};

namespace detail {
// Дескриптор движкового метода ищется ОДИН РАЗ на пару <класс, имя>. Обе части известны
// на компиляции, поэтому и память под ответ своя у каждой пары — искать заново незачем.
// Промах НЕ кэшируется: первый вызов может случиться раньше, чем движок отдал контекст,
// и запомненный ноль остался бы навсегда.
template <name_t Klass, name_t Name>
script::method engine_method() {
    static script::method cached;
    // Промах не кэшируем сразу: первый вызов может случиться раньше, чем движок отдал
    // контекст, и запомненный ноль остался бы навсегда. Но и искать вечно нельзя.
    //
    // Зеркало движкового API объявляет ВСЕ ветки препроцессора сразу — какие дефайны были
    // у запущенной игры, узнать неоткуда. Значит часть объявлений в конкретной сборке не
    // существует, и без потолка каждый их вызов стоил бы полного поиска: обход контекстов,
    // хеш имени в движке, VirtualQuery — 21 «пустой вызов», измерено.
    //
    // ponytail: потолок в ПОПЫТКАХ, а не сигнал «регистрация закончена». Такого сигнала
    // нет, а восемь промахов подряд означают, что метод зовут из уже работающего мода.
    static int attempts = 0;
    constexpr int kGiveUpAfter = 8;
    if (!cached.callable() && attempts < kGiveUpAfter) {
        cached = script::find_method(Klass.value, Name.value);
        if (!cached.callable()) {
            ++attempts;
        }
    }
    return cached;
}

// Промах в журнал — но не на каждый вызов. Причина та же: метода может не быть в этой
// сборке игры, и цикл по нему залил бы graft.log целиком, да ещё и через std::format.
template <name_t Klass, name_t Name>
void report_miss(void* self, const script::method& fn) {
    static int said = 0;
    constexpr int kSayAtMost = 3;
    if (said < kSayAtMost) {
        ++said;
        script::note_call_miss(Klass.value, Name.value, self, fn);
    }
}

// Маршалируемый вызов движкового `proto`. Определение — в callout.hpp: ему нужен разбор
// значений по тегам, а тот стоит ниже по цепочке заголовков. Здесь только объявление,
// чтобы ref ниже умел и этот путь тоже.
template <class R, name_t Klass, name_t Method, class... A>
std::expected<R, miss> proto_invoke(void* self, const A&... args);
// Форма для методов типов-значений (typename, string): приёмник у них едет первым в
// блоке аргументов, а не в rcx. См. комментарий у определения.
template <class R, name_t Klass, name_t Method, class... A>
std::expected<R, miss> proto_invoke_on_value(const graft::value& receiver, const A&... args);
}  // namespace detail

namespace detail {
// Номер слота поля с именем Name у КЛАССА ЭТОГО ОБЪЕКТА. Кэш свой у каждой пары
// <Tag, Name>, то есть фактически у каждого места в коде: оба — параметры шаблона.
// Номер зависит от класса объекта (наследник может быть шире), поэтому помним последний
// виденный дескриптор — как и диспетчер инстанциаций.
template <name_t Name, class Tag>
i32 slot_of(void* instance) {
    static thread_local void* seen = nullptr;
    static thread_local i32 index = -1;
    void* const class_desc = script::class_of(instance);
    if (class_desc != seen) {
        index = script::find_variable(instance, Name.value);
        seen = class_desc;
    }
    return index;
}
}  // namespace detail

// ── Поле объекта как lvalue ──────────────────────────────────────────────────
// Прокси, за который `player["m_Health"_f] = 100` читается так же, как в скрипте.
// Имя при этом живёт в ТИПЕ, поэтому номер слота ищется один раз на класс, а не на
// обращение: динамический язык такого не может, а статический обязан.
//
// Ловушка ровно та же, что у sol2 и у выражений Eigen: `auto v = obj["x"_f]` даёт
// прокси, а не значение. Пишите тип или зовите .get<T>().
template <class Owner, name_t Name>
class field_proxy {
public:
    explicit field_proxy(void* instance) : instance_(instance) {}

    // Значение поля; пусто, если поля нет.
    template <class T>
    std::optional<T> get() const {
        void* at = script::variable_address(instance_, detail::slot_of<Name, Owner>(instance_));
        if (!at) {
            return std::nullopt;
        }
        return load_slot<T>(at);
    }

    template <class T>
        requires(!std::same_as<std::remove_cvref_t<T>, field_proxy>)
    operator T() const {
        return get<T>().value_or(T{});
    }

    // Писать можно только то, чем не владеет движок: за строками и ссылками он считает
    // ссылки, и запись мимо него их ломает. Это не осторожность, а проверенная падением
    // порча кучи — прокси её не ослабляет, а делает нарушение ошибкой КОМПИЛЯЦИИ.
    template <class T>
    const field_proxy& operator=(T value) const {
        static_assert(owned_by_script<T>,
                      "прямо писать можно только int/float/bool: строками и ссылками "
                      "владеет движок, их отдают возвратом натива или пишут в скрипте");
        void* at = script::variable_address(instance_, detail::slot_of<Name, Owner>(instance_));
        if (at) {
            std::memcpy(at, &value, sizeof value);
        }
        return *this;
    }

    bool exists() const {
        return script::variable_address(instance_, detail::slot_of<Name, Owner>(instance_)) !=
               nullptr;
    }

private:
    void* instance_;
};

// Имя поля как значение параметра шаблона: `"m_Health"_f`. Тот же приём, что у ctre.
template <name_t N>
struct field_name {};

namespace literals {
template <name_t N>
consteval field_name<N> operator""_f() {
    return {};
}
}  // namespace literals

// Ссылка на любой скриптовый объект: имя класса задаётся прямо в типе.
//   graft::ref<"PlayerBase"> p   ->  PlayerBase p0
// Внутри — тот самый указатель, который движок кладёт в регистр.
//
// Наследник этой обёртки — обычный класс C++, отражающий скриптовый: его методы
// становятся нативами, а его ПОЛЯ живут ровно столько же, сколько скриптовый объект
// (библиотека держит по одному экземпляру на объект, см. detail::instance_of).
template <name_t N>
struct ref {
    // Имя скриптового класса доступно наследникам: по нему bindings::class_name<C>() узнаёт,
    // куда вешать методы, и дублировать строку в привязке не нужно.
    static constexpr name_t script_class = N;

    void* ptr = nullptr;
    explicit operator bool() const { return ptr != nullptr; }

    // Вызов движкового метода этого класса по имени. Работает для любого `proto native`,
    // не зависящего от шаблонного параметра — а это почти весь API игровых объектов
    // (IEntity.GetOrigin/GetID/GetName, Object.*, ...). Поиск идёт с обходом базовых
    // классов, поэтому на Object находятся и методы IEntity.
    //   graft::vector pos = o.call<graft::vector>("GetOrigin");
    //   o.call<void>("SetOrigin", graft::vector{1, 2, 3});
    template <class R, class... A>
    R call(const char* method, A... args) const {
        const script::method fn = lookup(method);
        // callable() отсекает скриптовые методы: у них в impl байткод, и вызов как
        // C-функции роняет процесс. Промах — это ноль и запись в graft.log, не падение.
        if (!ptr || !fn.callable()) {
            script::note_call_miss(N.value, method, ptr, fn);
            if constexpr (std::is_void_v<R>) {
                return;
            } else {
                return R{};
            }
        }
        return reinterpret_cast<R(__fastcall*)(void*, A...)>(fn.impl)(ptr, args...);
    }

    // То же, но промах виден в типе, а не превращается в ноль.
    template <class R, class... A>
    std::expected<R, miss> try_call(const char* method, A... args) const {
        const script::method fn = lookup(method);
        if (!ptr) {
            return std::unexpected(miss::null_object);
        }
        if (!fn.impl) {
            return std::unexpected(miss::not_found);
        }
        if (!fn.callable()) {
            return std::unexpected(miss::not_native);
        }
        if constexpr (std::is_void_v<R>) {
            reinterpret_cast<void(__fastcall*)(void*, A...)>(fn.impl)(ptr, args...);
            return R{};
        } else {
            return reinterpret_cast<R(__fastcall*)(void*, A...)>(fn.impl)(ptr, args...);
        }
    }
    // Можно ли звать этот метод напрямую (существует и нативный).
    bool has(const char* method) const { return lookup(method).callable(); }

    // ── То же самое, но имя метода известно на компиляции ────────────────────
    // А оно известно почти всегда: `o.call<vector, "GetOrigin">()`.
    //
    // Разница не косметическая. Форма с `const char*` ищет дескриптор НА КАЖДОМ
    // ВЫЗОВЕ: обход script-контекстов, хеш имени в движке и VirtualQuery внутри
    // is_code(). Имя в типе даёт каждой паре <класс, метод> свою статическую ячейку,
    // и вся эта работа случается один раз за процесс — ровно так уже работают методы
    // контейнеров (array.Resize, map.Count).
    template <class R, name_t Method, class... A>
    R call(A... args) const {
        const script::method fn = detail::engine_method<N, Method>();
        if (!ptr || !fn.callable()) {
            detail::report_miss<N, Method>(ptr, fn);
            if constexpr (std::is_void_v<R>) {
                return;
            } else {
                return R{};
            }
        }
        return reinterpret_cast<R(__fastcall*)(void*, A...)>(fn.impl)(ptr, args...);
    }

    template <class R, name_t Method, class... A>
    std::expected<R, miss> try_call(A... args) const {
        const script::method fn = detail::engine_method<N, Method>();
        if (!ptr) {
            return std::unexpected(miss::null_object);
        }
        if (!fn.impl) {
            return std::unexpected(miss::not_found);
        }
        if (!fn.callable()) {
            return std::unexpected(miss::not_native);
        }
        if constexpr (std::is_void_v<R>) {
            reinterpret_cast<void(__fastcall*)(void*, A...)>(fn.impl)(ptr, args...);
            return R{};
        } else {
            return reinterpret_cast<R(__fastcall*)(void*, A...)>(fn.impl)(ptr, args...);
        }
    }

    template <name_t Method>
    bool has() const {
        return detail::engine_method<N, Method>().callable();
    }

    // ── Маршалируемый `proto` движка ─────────────────────────────────────────
    // Второй этаж скриптового API объявлен без `native`: typename.Spawn,
    // EnScript.GetClassVar, ScriptModule.CallFunction, PlayerIdentity.GetPlainId и ещё
    // восемь сотен. Движок зовёт такие иначе — блоком переменных с тегами, — поэтому и
    // здесь путь свой. Снаружи разницы нет:
    //   graft::str id = identity.proto<graft::str, "GetPlainId">();
    template <class R, name_t Method, class... A>
    R proto(A... args) const {
        const std::expected<R, miss> r = detail::proto_invoke<R, N, Method>(ptr, args...);
        if (!r) {
            detail::report_miss<N, Method>(ptr, detail::engine_method<N, Method>());
            if constexpr (std::is_void_v<R>) {
                return;
            } else {
                return R{};
            }
        }
        if constexpr (!std::is_void_v<R>) {
            return *r;
        }
    }

    // То же, но промах виден в типе: не найден, не нативный, разошлась арность.
    template <class R, name_t Method, class... A>
    std::expected<R, miss> try_proto(A... args) const {
        return detail::proto_invoke<R, N, Method>(ptr, args...);
    }

    // ── Поля объекта по имени ────────────────────────────────────────────────
    // Работает для любого скриптового класса и любой вложенности: поле-объект
    // читается как ref<...>, у него снова есть поля и методы.
    //   auto child = node.field<graft::ref<"SeraphNode">>("m_child");
    //   graft::i32 id = child.field<graft::i32>("m_id");
    template <class T>
    T field(const char* name) const {
        void* addr = script::variable_address(ptr, script::find_variable(ptr, name));
        if (!addr) {
            return T{};
        }
        return load_slot<T>(addr);
    }
    // Промах виден в типе: поля нет — пусто, а не тихий ноль.
    template <class T>
    std::optional<T> try_field(const char* name) const {
        void* addr = script::variable_address(ptr, script::find_variable(ptr, name));
        if (!addr) {
            return std::nullopt;
        }
        return load_slot<T>(addr);
    }
    template <owned_by_script T>
    bool set_field(const char* name, T value) const {
        void* addr = script::variable_address(ptr, script::find_variable(ptr, name));
        if (!addr) {
            return false;
        }
        std::memcpy(addr, &value, sizeof value);
        return true;
    }
    // ── Поле, имя которого известно на компиляции ────────────────────────────
    //   graft::i32 id = node.field<graft::i32, "m_id">();
    //
    // Форма с `const char*` ищет поле ПЕРЕБОРОМ имён на каждом обращении — у класса с
    // сотней полей это сотня strcmp. Здесь номер слота ищется один раз на класс: сам
    // номер зависит от класса объекта (наследник может быть шире), поэтому помним
    // последний виденный дескриптор — как и диспетчер инстанциаций.
    template <class T, name_t Name>
    T field() const {
        void* addr = script::variable_address(ptr, slot_of<Name>());
        if (!addr) {
            return T{};
        }
        return load_slot<T>(addr);
    }
    template <class T, name_t Name>
    std::optional<T> try_field() const {
        void* addr = script::variable_address(ptr, slot_of<Name>());
        if (!addr) {
            return std::nullopt;
        }
        return load_slot<T>(addr);
    }
    template <name_t Name, owned_by_script T>
    bool set_field(T value) const {
        void* addr = script::variable_address(ptr, slot_of<Name>());
        if (!addr) {
            return false;
        }
        std::memcpy(addr, &value, sizeof value);
        return true;
    }

    // ── То же в стиле sol2: поле как lvalue ──────────────────────────────────
    //   player["m_Health"_f] = 100.0f;
    //   float hp = player["m_Health"_f];
    //   auto maybe = player["m_Nope"_f].template get<graft::i32>();
    //
    // Тип в обращении не повторяется — его выводит присваивание или конверсия. При этом
    // имя остаётся параметром шаблона, поэтому обходится это ровно столько же, сколько
    // field<T, "имя">(): сторожит метрика fieldProxy в Perf_HotPathBudget.
    template <name_t Name>
    field_proxy<ref, Name> operator[](field_name<Name>) const {
        return field_proxy<ref, Name>{ptr};
    }

    // Сколько полей и как они называются — для обхода незнакомой структуры.
    i32 field_count() const { return script::variable_count(ptr); }
    str field_name(i32 index) const { return str{script::variable_name(ptr, index)}; }

    // Имя может приехать и аргументом натива: `node.field<i32>(name)` вместо
    // `node.field<i32>(name.c_str())`. str по построению nul-terminated — это тот же
    // указатель, что дал движок.
    template <class R, class... A>
    R call(str method, A... args) const {
        return call<R>(method.c_str(), args...);
    }
    template <class R, class... A>
    std::expected<R, miss> try_call(str method, A... args) const {
        return try_call<R>(method.c_str(), args...);
    }
    bool has(str method) const { return has(method.c_str()); }
    template <class T>
    T field(str name) const {
        return field<T>(name.c_str());
    }
    template <class T>
    std::optional<T> try_field(str name) const {
        return try_field<T>(name.c_str());
    }
    template <owned_by_script T>
    bool set_field(str name, T value) const {
        return set_field<T>(name.c_str(), value);
    }

    // Ссылки сравниваются и кладутся в контейнеры как обычные ключи — указатель
    // наружу доставать не нужно.
    friend bool operator==(ref a, ref b) { return a.ptr == b.ptr; }
    // Тип объекта на самом деле (может быть наследником N).
    str type_name() const { return str{script::class_name(script::class_of(ptr))}; }

private:
    // Ищем по имени класса: script::find_class перебирает контексты всех модулей,
    // поэтому находятся и игровые классы, объявленные позже основного.
    script::method lookup(const char* method) const {
        return script::find_method(N.value, method);
    }

    // Кэш номера слота один на пару <ref<N>, Name> — тот же, которым пользуется прокси
    // поля, поэтому два синтаксиса стоят ровно одинаково.
    template <name_t Name>
    i32 slot_of() const {
        return detail::slot_of<Name, ref>(ptr);
    }
};

using obj = ref<"Class">;  // любой скриптовый объект

// Дескриптор движкового метода по именам класса и метода — для диагностики (флаги,
// исполняемость). Обычный вызов делается через ref::call, объект знает свой класс сам.
inline script::method method_of(str class_name, str name) {
    return script::find_method(class_name.c_str(), name.c_str());
}

// Раскладка скриптовых структур (graft::layout) объявлена в script.hpp: смещения
// нужны и здесь, и врезке, и регистрации — держим их одним блоком в одном месте.

// Enforce typename: указатель на дескриптор класса.
struct type {
    void* ptr = nullptr;
    const char* name() const {
        return ptr ? *reinterpret_cast<const char* const*>(static_cast<const char*>(ptr) +
                                                           layout::type_name)
                   : nullptr;
    }
    explicit operator bool() const { return ptr != nullptr; }
    friend bool operator==(type a, type b) { return a.ptr == b.ptr; }

    // У typename в движке есть свои методы, и они настоящие: Spawn создаёт объект этого
    // класса (тот же `new`, что в скрипте), GetModule говорит, в каком модуле класс
    // объявлен, ToString даёт имя. Зовутся они НА ДЕСКРИПТОРЕ КЛАССА — он и есть
    // значение typename, то есть ровно то, что лежит в ptr.
    //
    //   graft::obj made = t.try_proto<graft::obj, "Spawn">().value_or(graft::obj{});
    // Определения — ниже, после graft::value: тело упоминает его по имени, а не через
    // параметр шаблона, и потому требует полного типа уже здесь.
    template <class R, name_t Method, class... A>
    std::expected<R, miss> try_proto(A... args) const;
    template <class R, name_t Method, class... A>
    R proto(A... args) const;
};

// ── Значение любого скриптового типа ─────────────────────────────────────────
// Нужно там, где тип известен только в рантайме: у СВОЕГО шаблонного класса
// (`CppHashMap<K,V>`) методы, упоминающие K или V, обслуживаются одним имплом на все
// инстанциации — какой тип приехал, говорит тег скриптовой переменной.
//
//   struct Table {
//       std::unordered_map<graft::value, graft::value> data;
//       void Set(graft::value key, graft::value v) { data.insert_or_assign(key, v); }
//   };
//
// Значение владеет своим содержимым (строка копируется), поэтому его можно хранить,
// класть ключом в контейнер и сравнивать. Появление graft::value в сигнатуре само
// переключает привязку на маршалируемый вызов — писать ничего не нужно.
class value {
public:
    value() = default;
    value(bool v) : data_(v) {}
    value(i32 v) : data_(v) {}
    value(f32 v) : data_(v) {}
    value(vector v) : data_(v) {}
    value(std::string v) : data_(std::move(v)) {}
    value(str v) : data_(std::string{v.view()}) {}
    value(obj v) : data_(v) {}
    value(type v) : data_(v) {}

    bool empty() const { return data_.index() == 0; }
    template <class T>
    bool is() const {
        return std::holds_alternative<T>(data_);
    }
    // Значение нужного типа; если приехал другой — пусто (0 / "" / null).
    template <class T>
    T as() const {
        const T* p = std::get_if<T>(&data_);
        return p ? *p : T{};
    }
    std::string_view view() const {
        const auto* p = std::get_if<std::string>(&data_);
        return p ? std::string_view{*p} : std::string_view{};
    }
    // Скриптовый объект целиком: у него снова есть поля, методы и настоящее имя типа —
    // отсюда работа с вложенностью и ООП на любую глубину.
    //   node.field<graft::value>("m_child").object().field<i32>("m_id")
    obj object() const { return as<obj>(); }
    // Чем объект оказался на самом деле (SeraphNode, array<int>, map<string,int>, ...).
    str type_name() const {
        const obj o = as<obj>();
        return o ? o.type_name() : str{};
    }

    // Текстовое представление — для журналов и ключей «по тексту».
    std::string to_string() const {
        return std::visit(
            []<class T>(const T& v) -> std::string {
                if constexpr (std::is_same_v<T, std::monostate>) {
                    return {};
                } else if constexpr (std::is_same_v<T, std::string>) {
                    return v;
                } else if constexpr (std::is_same_v<T, bool>) {
                    return v ? "1" : "0";
                } else if constexpr (std::is_same_v<T, vector>) {
                    return std::format("{} {} {}", v.x, v.y, v.z);
                } else if constexpr (std::is_same_v<T, obj>) {
                    return std::format("{}", static_cast<const void*>(v.ptr));
                } else if constexpr (std::is_same_v<T, type>) {
                    return v.name() ? v.name() : "";
                } else {
                    return std::format("{}", v);
                }
            },
            data_);
    }

    friend bool operator==(const value&, const value&) = default;

private:
    std::variant<std::monostate, bool, i32, f32, vector, std::string, obj, type> data_;
};

// Методы typename — только теперь, когда value полный.
template <class R, name_t Method, class... A>
std::expected<R, miss> type::try_proto(A... args) const {
    // typename — тип-ЗНАЧЕНИЕ: сам дескриптор класса едет первым в блоке, а rcx у такого
    // вызова нет вовсе.
    if (!ptr) {
        return std::unexpected(miss::null_object);
    }
    return detail::proto_invoke_on_value<R, "typename", Method>(value{*this}, args...);
}

template <class R, name_t Method, class... A>
R type::proto(A... args) const {
    const std::expected<R, miss> r = try_proto<R, Method>(args...);
    if (!r) {
        if constexpr (std::is_void_v<R>) {
            return;
        } else {
            return R{};
        }
    }
    if constexpr (!std::is_void_v<R>) {
        return *r;
    }
}

// То же значение, но с именем скриптового типа для объявления: `param<"K">` печатается
// как `K`, `param<"string">` — как `string`, а голый value — как `void` (тип-«любой»,
// так объявлен и ванильный string.ToString(void var, ...)). Имя — любое: параметр
// шаблона, обычный тип, вложенный шаблон вроде `array<K>`.
//
//   graft::param<"V"> Get(graft::param<"K"> key);   -> proto V Get(K p0);
//   graft::param<"string"> Name(graft::value v);    -> proto string Name(void p0);
template <name_t N>
struct param : value {
    using value::value;
    param() = default;
    param(value v) : value(std::move(v)) {}
};

}  // namespace graft

// vector и typename тоже кладутся ключом — это нужно и само по себе, и чтобы
// пользовательский шаблон разворачивался по всем поддерживаемым типам.
template <>
struct std::hash<graft::vector> {
    std::size_t operator()(graft::vector v) const noexcept {
        const std::hash<float> h;
        return h(v.x) ^ (h(v.y) << 1) ^ (h(v.z) << 2);
    }
};
template <>
struct std::hash<graft::type> {
    std::size_t operator()(graft::type t) const noexcept {
        return std::hash<const void*>{}(t.ptr);
    }
};

// Значение любого скриптового типа тоже кладётся ключом.
template <>
struct std::hash<graft::value> {
    std::size_t operator()(const graft::value& v) const noexcept {
        if (v.is<bool>()) {
            return std::hash<bool>{}(v.as<bool>());
        }
        if (v.is<graft::i32>()) {
            return std::hash<graft::i32>{}(v.as<graft::i32>());
        }
        if (v.is<graft::f32>()) {
            return std::hash<graft::f32>{}(v.as<graft::f32>());
        }
        if (v.is<graft::obj>()) {
            return std::hash<const void*>{}(v.as<graft::obj>().ptr);
        }
        if (v.is<graft::type>()) {
            return std::hash<const void*>{}(v.as<graft::type>().ptr);
        }
        if (v.is<graft::vector>()) {
            const graft::vector p = v.as<graft::vector>();
            return std::hash<float>{}(p.x) ^ (std::hash<float>{}(p.y) << 1) ^
                   (std::hash<float>{}(p.z) << 2);
        }
        return std::hash<std::string_view>{}(v.view());
    }
};

// Ссылку можно класть ключом в std::unordered_map — без возни с void*.
template <graft::name_t N>
struct std::hash<graft::ref<N>> {
    std::size_t operator()(graft::ref<N> r) const noexcept {
        return std::hash<const void*>{}(r.ptr);
    }
};

