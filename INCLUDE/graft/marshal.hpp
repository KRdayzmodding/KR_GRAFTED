// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "graft/guard.hpp"
#include "graft/thunk.hpp"

// Маршалируемый путь (`proto`): движок передаёт блок аргументов, у каждого — тег типа.
// Отсюда свои шаблонные классы (один импл на любые K и V) и out-аргументы у значений.
namespace graft::detail {

// ── Маршалируемый вызов: один импл на ВСЕ инстанциации шаблона ──────────────
// Движок зовёт такой метод иначе: rcx = объект, rdx = блок указателей на скриптовые
// переменные (args[0] — уже первый настоящий аргумент), r8 — переменная возврата.
// У каждой переменной есть ТЕГ ТИПА (+16) — по нему и узнаём, что приехало. Так
// устроены ванильные array<T>.Insert / map<K,V>.Get; так же теперь и свои шаблоны.
// Проверено на живом сервере: <string,int> дал теги 0x50000003 / 0x20000000,
// <int,string> — 0x20000000 / 0x50000003.
//
// Тег есть не у всякого аргумента. ПОЛЕ объекта движок отдаёт указателем прямо на ячейку
// поля: значение там на месте, а за ним вместо тега — соседние поля (снято зондом на 1.30:
// у строкового поля «тег» e0a9da8e, у int-поля — кусок указателя; out во float-поле по
// такому «тегу» обрушил кучу). Поэтому тип, известный из объявления, читается и пишется ПО
// ОБЪЯВЛЕНИЮ, а тег переменной спрашивается только у `void` — и проверяется на правдоподобие.

// Тег встроенного типа по его имени в объявлении; 0 — тип не встроенный (шаблонный K, класс).
constexpr std::uint32_t builtin_tag(std::string_view name) {
    if (name == "bool") {
        return script::tag_bool;
    }
    if (name == "int") {
        return script::tag_int;
    }
    if (name == "float") {
        return script::tag_float;
    }
    if (name == "vector") {
        return script::tag_vector;
    }
    if (name == "string") {
        return script::type_string;
    }
    if (name == "typename") {
        return script::type_typename;
    }
    return 0;
}

// Тег, под которым аргумент объявлен: param<"float"> — float, голый value — 0 («любой»).
template <class T>
inline constexpr std::uint32_t declared_tag = 0;
template <name_t N>
inline constexpr std::uint32_t declared_tag<param<N>> = builtin_tag(N.value);

// Тег настоящей переменной: известное семейство и небольшой индекс типа в таблице
// контекста. Соседние поля объекта сюда почти никогда не проходят.
inline bool plausible_tag(std::uint32_t tag) {
    switch (tag & script::type_family) {
        case script::type_int:
        case script::type_float:
        case script::type_vector:
        case script::type_string:
        case script::type_class:
        case script::type_typename:
            return (tag & 0x0FFF0000u) == 0;
        default:
            return false;
    }
}

// Значение переменной, тип которой уже известен.
inline value read_as(const void* var, std::uint32_t tag) {
    if (!var) {
        return {};
    }
    const auto raw = *reinterpret_cast<const std::uint64_t*>(var);
    if (tag == script::type_bool) {
        return value{(raw & 0xFF) != 0}; // младший байт: у поля за ним уже сосед
    }
    switch (tag & script::type_family) {
        case script::type_int:
            return value{static_cast<i32>(static_cast<std::uint32_t>(raw))};
        case script::type_float: {
            return value{std::bit_cast<f32>(static_cast<std::uint32_t>(raw))};
        }
        case script::type_vector: {
            // vector в 8 байт значения не влезает — там указатель на сами 12 байт.
            const auto* p = reinterpret_cast<const vector*>(raw);
            return p ? value{*p} : value{};
        }
        case script::type_string:
            return value{str{reinterpret_cast<const char*>(raw)}};
        case script::type_class:
            // Ссылка может лежать и обёрткой {vtable, счётчик, объект} — разворачиваем
            // тем же способом, что и элементы контейнеров.
            return value{obj{script::deref_object(reinterpret_cast<void*>(raw))}};
        case script::type_typename:
            return value{type{reinterpret_cast<void*>(raw)}};
        default:
            return {};
    }
}

// Значение переменной по её собственному тегу — для `void`, где объявление типа не знает.
inline value read_var(const void* var) {
    if (!var) {
        return {};
    }
    const auto tag = *reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(var) + 16);
    if (!plausible_tag(tag)) {
        log("graft: у аргумента `void` нет тега — похоже, это поле объекта; передай его копией "
            "в локальной переменной");
        return {};
    }
    return read_as(var, tag);
}

// Значение скриптовой переменной как КОНКРЕТНЫЙ тип C++ (а не value). Путь через value
// стоил бы на каждый аргумент сборки и разрушения варианта (внутри него std::string, то
// есть нетривиальный деструктор), а для строки — ещё и лишней копии: одна в вариант,
// вторая из него. Тип известен из объявления, поэтому тег переменной не нужен.
template <class T>
T as_typed(const void* var) {
    // Наследник value (param<"K"> и прочие имена для объявления) — это тот же value:
    // встроенный тип читается по объявлению, остальное — по тегу переменной. Без этой
    // ветки param<> в сигнатуре не собирался вовсе.
    if constexpr (std::is_base_of_v<value, T>) {
        constexpr std::uint32_t tag = declared_tag<T>;
        return tag ? T{read_as(var, tag)} : T{read_var(var)};
    } else {
        if (!var) {
            return T{};
        }
        const auto raw = *reinterpret_cast<const std::uint64_t*>(var);
        if constexpr (std::is_same_v<T, bool>) {
            return (raw & 0xFF) != 0;
        } else if constexpr (std::is_same_v<T, i32>) {
            return static_cast<i32>(static_cast<std::uint32_t>(raw));
        } else if constexpr (std::is_same_v<T, f32>) {
            return std::bit_cast<f32>(static_cast<std::uint32_t>(raw));
        } else if constexpr (std::is_same_v<T, str>) {
            // Строка движка как есть: указатель живёт до конца вызова, копии нет.
            // Без этой ветки graft::str в маршалируемой сигнатуре не собирался —
            // разбор уходил в value::as<str>(), а в варианте лежит std::string.
            return str{reinterpret_cast<const char*>(raw)};
        } else if constexpr (std::is_same_v<T, std::string>) {
            const auto* text = reinterpret_cast<const char*>(raw);
            return text ? std::string{text} : std::string{};
        } else if constexpr (std::is_same_v<T, vector>) {
            // vector в 8 байт не влезает — в переменной указатель на сами 12 байт.
            const auto* p = reinterpret_cast<const vector*>(raw);
            return p ? *p : vector{};
        } else if constexpr (std::is_same_v<T, obj>) {
            return obj{script::deref_object(reinterpret_cast<void*>(raw))};
        } else if constexpr (std::is_same_v<T, type>) {
            return type{reinterpret_cast<void*>(raw)};
        } else {
            return read_var(var).template as<T>();
        }
    }
}

// Возврат кладём в переменную, которую движок уже подготовил под тип инстанциации:
// тег там стоит правильный, поэтому приводим значение к нему, а тег не трогаем.
// out — пишем в аргумент, а не в возврат. Разница принципиальная для строк: возврат
// движок копирует себе сразу, а строкой в out-переменной он ВЛАДЕЕТ и потом освободит —
// свой указатель туда класть нельзя (порча кучи 0xC0000374, проверено падением).
// Это ограничение движка, то же самое, что запрет писать char* в array<string>.
//
// out пишется только в настоящую переменную — ту, чей тег совпал с объявлением (declared,
// для `void` — любой правдоподобный). Поле объекта движок отдаёт указателем на ячейку без
// тега, и запись туда не доживает до скрипта целой: два float-поля подряд обрушили кучу
// (0xC0000374, снято на 1.30), одно — прошло. Такой out не пишется, а попадает в журнал.
enum class var_kind { returned, out };

inline void write_var(void* var, const value& v, var_kind kind = var_kind::returned, std::uint32_t declared = 0) {
    if (!var) {
        return;
    }
    auto*      at   = static_cast<char*>(var);
    const auto tag  = *reinterpret_cast<const std::uint32_t*>(at + 16);
    auto*      slot = reinterpret_cast<std::uint64_t*>(at);
    const auto same = declared ? (tag & script::type_family) == (declared & script::type_family) : true;
    if (kind == var_kind::out && (!same || !plausible_tag(tag))) {
        log("graft: out-аргумент — не переменная (поле объекта?), значение не записано; передай "
            "локальную переменную");
        return;
    }
    if (kind == var_kind::out && (tag & script::type_family) == script::type_string) {
        log("graft: a string cannot be returned through an out argument - the engine owns the string; "
            "return it instead");
        return;
    }
    if (tag == script::type_bool) {
        *slot = v.is<bool>() ? (v.as<bool>() ? 1u : 0u) : static_cast<std::uint32_t>(v.as<i32>());
        return;
    }
    switch (tag & script::type_family) {
        case script::type_int:
            *slot = v.is<bool>() ? (v.as<bool>() ? 1u : 0u) : static_cast<std::uint32_t>(v.as<i32>());
            return;
        case script::type_float: {
            *slot = std::bit_cast<std::uint32_t>(v.as<f32>());
            return;
        }
        case script::type_vector: {
            // Движок уже выделил 12 байт и положил сюда указатель — пишем на месте.
            if (auto* p = reinterpret_cast<vector*>(*slot)) {
                *p = v.as<vector>();
            }
            return;
        }
        case script::type_string:
            // Строка обязана дожить до конца вызова — кладём её в ту же арену, что и text.
            *slot = reinterpret_cast<std::uint64_t>(stash(v.to_string()));
            return;
        case script::type_class:
            *slot = reinterpret_cast<std::uint64_t>(v.as<obj>().ptr);
            return;
        case script::type_typename:
            *slot = reinterpret_cast<std::uint64_t>(v.as<type>().ptr);
            return;
        default:
            return;
    }
}

// graft::value в сигнатуре — признак того, что метод обслуживает шаблонный класс и
// зовётся маршалируемым путём. Библиотека переключается на него сама.
template <class T>
inline constexpr bool is_value_type = std::is_base_of_v<value, std::remove_cvref_t<T>>;

// Аргумент маршалируемого метода: по значению — вход, по неконстантной ССЫЛКЕ — out.
// Движок помечает такие переменные флагом 0x80 и забирает записанное обратно в скрипт
// (проверено: запись по +0 дошла до скрипта для int, float и string).
template <class T>
inline constexpr bool is_out_param = std::is_lvalue_reference_v<T> &&
                                     !std::is_const_v<std::remove_reference_t<T>>;

// Как этот тип называется в объявлении скрипта: голый value — «любой» (void),
// param<"K"> — своим именем, обычный тип — как везде.
//
// Смешивать можно: маршалируемый вызов передаёт ВСЕ аргументы блоком с тегами, и
// объявленный тип аргумента на форму вызова не влияет. Поэтому сигнатура вроде
// `bool Emit(i32 schema, obj player, vector pos, value data)` объявляется как
// `proto bool Emit(int p0, Class p1, vector p2, void p3)` — точная типизация там,
// где она есть, и «любой» только там, где он нужен.
template <class T>
struct marshal_name {
    static constexpr name_t id = enf_type<std::remove_cvref_t<T>>::id;
};

template <>
struct marshal_name<value> {
    static constexpr name_t id{"void"};
};
template <name_t N>
struct marshal_name<param<N>> {
    static constexpr name_t id = N;
};

// Имя возврата: значение называет себя само, обычный тип — как везде.
template <class R>
struct marshal_ret {
    static constexpr const char* value = enf_type<R>::name;
};
template <>
struct marshal_ret<void> {
    static constexpr const char* value = "void";
};
template <class R>
    requires std::is_base_of_v<value, R>
struct marshal_ret<R> {
    static constexpr const char* value = marshal_name<R>::id.value;
};

// Имя аргумента с модификатором: out-аргумент объявляется как `out T`.
template <class A>
struct marshal_arg {
    static constexpr auto id = [] {
        if constexpr (is_out_param<A>) {
            return name_t{"out "} + marshal_name<std::remove_cvref_t<A>>::id;
        } else {
            return marshal_name<std::remove_cvref_t<A>>::id;
        }
    }();
    static constexpr const char* value = id.value;
};

template <class R, class... A>
inline constexpr bool needs_marshal = is_value_type<R> || (is_value_type<A> || ... || false);

template <class R, class... A>
struct marshal_check {
    static_assert(((!is_out_param<A> || is_value_type<A>) && ... && true),
                  "out-аргумент маршалируемого метода обязан быть graft::value: обратно "
                  "движку значение отдаётся по тегу");
    static_assert(std::is_void_v<R> || std::is_constructible_v<value, R>,
                  "возврат маршалируемого метода должен приводиться к graft::value");
    static_assert(((!script_class<std::remove_cvref_t<A>> || !has_fields<std::remove_cvref_t<A>>) && ... && true),
                  "объект класса с полями аргументом — только у немаршалируемого натива (proto "
                  "native); здесь его экземпляр не найти");
    static constexpr const char* ret = marshal_ret<R>::value;
    // out-аргументы объявляются с модификатором: движок забирает записанное обратно.
    static constexpr const char* const args[] = {marshal_arg<A>::value..., nullptr};
};

// Аргумент маршалируемого вызова. graft::value отдаётся как есть, обычный тип —
// приведением по тегу приехавшей переменной, БЕЗ промежуточного варианта: не тот тип
// на входе даёт пустое значение (0 / "" / null), а не порчу памяти.
template <class T>
std::remove_cvref_t<T> from_var(const void* var) {
    return as_typed<std::remove_cvref_t<T>>(var);
}

// Результат маршалируемого метода: graft::value или что-то, из чего он строится.
template <class T>
value to_value(T&& v) {
    if constexpr (is_value_type<T>) {
        return std::forward<T>(v);
    } else {
        return value{std::forward<T>(v)};
    }
}

template <class C, auto F>
struct marshal_thunk;

// Один трамплин на все инстанциации: значения читаются по тегам, out-аргументы
// записываются обратно после вызова.
template <class R, class... A, class C, class M, std::size_t... I>
std::int64_t marshal_run(C&& object, M method, void** block, void* ret,
                         std::index_sequence<I...>) {
    // Значения живут до конца вызова: out-ссылки смотрят именно на них.
    std::tuple<std::remove_cvref_t<A>...> slots{from_var<A>(block ? block[I] : nullptr)...};
    const auto                            write_back = [&] {
        ([&] {
            if constexpr (is_out_param<A> && is_value_type<A>) {
                write_var(block ? block[I] : nullptr, std::get<I>(slots), var_kind::out, declared_tag<std::remove_cvref_t<A>>);
            }
        }(),
         ...);
    };
    if constexpr (std::is_void_v<R>) {
        (object.*method)(static_cast<A>(std::get<I>(slots))...);
        write_back();
    } else {
        value result = to_value((object.*method)(static_cast<A>(std::get<I>(slots))...));
        write_back();
        write_var(ret, result);
    }
    return 0;
}

template <class C, class Own, class R, class... A, R (Own::*F)(A...)>
struct marshal_thunk<C, F> : marshal_check<R, A...> {
    static std::int64_t __fastcall call(void* self, void*** args, void** ret) {
        return guarded<std::int64_t>(reinterpret_cast<void*>(&call),
                                     [&] { return body(self, args, ret); });
    }
    static std::int64_t body(void* self, void*** args, void** ret) {
        [[maybe_unused]] arena_scope<R> alive;
        return marshal_run<R, A...>(instance_of<C>(self), F, args ? *args : nullptr,
                                    ret ? *ret : nullptr, std::index_sequence_for<A...>{});
    }
};

template <class C, class Own, class R, class... A, R (Own::*F)(A...) const>
struct marshal_thunk<C, F> : marshal_check<R, A...> {
    static std::int64_t __fastcall call(void* self, void*** args, void** ret) {
        return guarded<std::int64_t>(reinterpret_cast<void*>(&call),
                                     [&] { return body(self, args, ret); });
    }
    static std::int64_t body(void* self, void*** args, void** ret) {
        [[maybe_unused]] arena_scope<R> alive;
        const C& object = instance_of<C>(self);
        return marshal_run<R, A...>(object, F, args ? *args : nullptr, ret ? *ret : nullptr,
                                    std::index_sequence_for<A...>{});
    }
};

// ── Маршалируемый вызов БЕЗ ПРИЁМНИКА: глобальные и статические нативы ──────
//
// RESEARCH/theory/abi.md, «Форм вызова ДВЕ»: у метода настоящего класса приёмник
// лежит в rcx (`impl(self, args, ret)`), а у статического его нет вовсе
// (`impl(args, ret)`) — так зовутся EnScript.GetClassVar и Class.CastTo. Глобальный
// `proto` (ванильный Print(void var)) — та же форма: получателя у него тоже нет.
//
// Флаг маршалируемости глобальному нативу ставить не нужно: движок линкует его по
// имени с УЖЕ РАЗОБРАННЫМ прототипом, и `proto` против `proto native` он берёт из
// объявления в скрипте. Поэтому здесь достаточно правильной формы вызова и правильно
// напечатанного объявления.
template <class R, class... A, class Fn, std::size_t... I>
std::int64_t marshal_run_free(Fn fn, void** block, void* ret, std::index_sequence<I...>) {
    // Значения живут до конца вызова: out-ссылки смотрят именно на них.
    std::tuple<std::remove_cvref_t<A>...> slots{from_var<A>(block ? block[I] : nullptr)...};
    const auto                            write_back = [&] {
        ([&] {
            if constexpr (is_out_param<A> && is_value_type<A>) {
                write_var(block ? block[I] : nullptr, std::get<I>(slots), var_kind::out, declared_tag<std::remove_cvref_t<A>>);
            }
        }(),
         ...);
    };
    if constexpr (std::is_void_v<R>) {
        fn(static_cast<A>(std::get<I>(slots))...);
        write_back();
    } else {
        value result = to_value(fn(static_cast<A>(std::get<I>(slots))...));
        write_back();
        write_var(ret, result);
    }
    return 0;
}

template <auto F>
struct marshal_free_thunk;

template <class R, class... A, R (*F)(A...)>
struct marshal_free_thunk<F> : marshal_check<R, A...> {
    static std::int64_t __fastcall call(void*** args, void** ret) {
        // Тело — отдельной функцией: у x64-SEH таблица областей описывает диапазоны
        // адресов, и защита работает, только если сбой случился ВНУТРИ вызова.
        return guarded<std::int64_t>(reinterpret_cast<void*>(&call),
                                     [&] { return body(args, ret); });
    }

    static std::int64_t body(void*** args, void** ret) {
        [[maybe_unused]] arena_scope<R> alive;
        return marshal_run_free<R, A...>(F, args ? *args : nullptr, ret ? *ret : nullptr, std::index_sequence_for<A...>{});
    }
};

}  // namespace graft::detail
