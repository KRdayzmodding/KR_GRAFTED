// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <tuple>
#include <utility>

#include "graft/guard.hpp"
#include "graft/thunk.hpp"

// Маршалируемый путь (`proto`): движок передаёт блок аргументов, у каждого — тег типа.
// Отсюда свои шаблонные классы (один импл на любые K и V) и out-аргументы у значений.
namespace graft::detail {

// ── Маршалируемый вызов: один импл на ВСЕ инстанциации шаблона ──────────────
// Движок зовёт такой метод иначе: rcx = объект, rdx = блок аргументов, r8 — переменная
// возврата. Блок — два массива (script::arg_block): по +0 указатели на ЗНАЧЕНИЯ, по +8 —
// на ПЕРЕМЕННЫЕ, а у переменной есть тег типа (+16) и флаги (+20). У локальной переменной
// это один и тот же адрес, у ПОЛЯ объекта — нет: значение лежит в слоте поля, тег и флаги
// — у переменной, описывающей поле. Ванильные имплы так и читают: тег и флаги из второго
// массива, значение из первого (RESEARCH/out/diag/pseudo/sub_1403057E0.c). Так устроены
// и ванильные array<T>.Insert / map<K,V>.Get, и свои шаблоны.
// Проверено на живом сервере: <string,int> дал теги 0x50000003 / 0x20000000,
// <int,string> — 0x20000000 / 0x50000003.

inline std::uint32_t tag_of_var(const void* var) {
    return var ? *reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(var) + layout::var_type)
               : 0;
}

inline std::uint32_t flags_of_var(const void* var) {
    return var ? *reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(var) + layout::var_flags)
               : 0;
}

// Значение из ячейки по тегу её переменной.
inline value read_as(const void* at, std::uint32_t tag) {
    if (!at) {
        return {};
    }
    const auto raw = *reinterpret_cast<const std::uint64_t*>(at);
    if (tag == script::type_bool) {
        return value{(raw & 0xFF) != 0}; // младший байт: у поля за ним уже сосед
    }
    switch (tag & script::type_family) {
        case script::type_int:
            return value{static_cast<i32>(static_cast<std::uint32_t>(raw))};
        case script::type_float:
            return value{std::bit_cast<f32>(static_cast<std::uint32_t>(raw))};
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

// Переменная целиком: её значение лежит в ней самой, по +0.
inline value read_var(const void* var) {
    return read_as(var, tag_of_var(var));
}

template <class>
inline constexpr bool unsupported = false;

// Значение как КОНКРЕТНЫЙ ABI-тип, без промежуточного value: тип гарантирует объявление
// (компилятор скрипта не пустит туда чужое), поэтому тег не нужен. Путь через value стоил
// бы на каждый аргумент сборки и разрушения варианта, а строке — ещё и копии.
template <class T>
T as_typed(const void* at) {
    if (!at) {
        return T{};
    }
    const auto raw = *reinterpret_cast<const std::uint64_t*>(at);
    if constexpr (std::is_same_v<T, bool>) {
        return (raw & 0xFF) != 0;
    } else if constexpr (std::is_same_v<T, i32>) {
        return static_cast<i32>(static_cast<std::uint32_t>(raw));
    } else if constexpr (std::is_same_v<T, f32>) {
        return std::bit_cast<f32>(static_cast<std::uint32_t>(raw));
    } else if constexpr (std::is_same_v<T, str>) {
        // Строка движка как есть: указатель живёт до конца вызова, копии нет.
        return str{reinterpret_cast<const char*>(raw)};
    } else if constexpr (std::is_same_v<T, vector>) {
        const auto* p = reinterpret_cast<const vector*>(raw);
        return p ? *p : vector{};
    } else if constexpr (std::is_same_v<T, type>) {
        return type{reinterpret_cast<void*>(raw)};
    } else if constexpr (requires { T::script_class; }) {
        // obj, ref<"Имя">, свой класс-обёртка: ссылка, возможно, обёрткой.
        T out{};
        out.ptr = script::deref_object(reinterpret_cast<void*>(raw));
        return out;
    } else {
        static_assert(unsupported<T>, "этот тип маршалируемый аргумент не принимает");
    }
}

// Возврат кладём в переменную, которую движок уже подготовил под тип инстанциации: тег там
// стоит правильный, поэтому приводим значение к нему, а тег не трогаем. out — пишем в
// аргумент. Разница принципиальная для строк: возврат движок копирует себе сразу, а строкой
// в out-переменной он ВЛАДЕЕТ и потом освободит — свой указатель туда класть нельзя (порча
// кучи 0xC0000374, проверено падением). То же ограничение, что и запрет писать char* в
// array<string>.
enum class var_kind { returned, out };

// at — ячейка значения, var — переменная с тегом и флагами (у своей переменной это одно и
// то же). У поля объекта int/float/bool занимают 4 байта слота, а за ними соседнее поле:
// пишем ровно 4, как ванильные имплы. Восемь байт в последнее поле объекта — это запись за
// его конец и порча кучи (0xC0000374, снято на 1.30). Переменная возврата — целая 8-байтная
// ячейка, её дочищаем нулём.
inline void store(void* at, const void* var, const value& v, var_kind kind) {
    if (!at || !var) {
        return;
    }
    const std::uint32_t tag = tag_of_var(var);
    if (kind == var_kind::out) {
        // Запрет записи (0x40) ванильный импл уважает молча — и мы так же.
        if ((flags_of_var(var) & 0x40) != 0) {
            return;
        }
        if ((tag & script::type_family) == script::type_string) {
            log("graft: a string cannot be returned through an out argument - the engine owns the string; "
                "return it instead");
            return;
        }
    }
    const auto put32 = [&](std::uint32_t bits) {
        if (kind == var_kind::returned) {
            const std::uint64_t wide = bits;
            std::memcpy(at, &wide, sizeof wide);
        } else {
            std::memcpy(at, &bits, sizeof bits);
        }
    };
    const auto put_ptr = [&](const void* p) { std::memcpy(at, &p, sizeof p); };
    const auto as_int  = [&] { return v.is<bool>() ? (v.as<bool>() ? 1u : 0u) : static_cast<std::uint32_t>(v.as<i32>()); };
    if (tag == script::type_bool) {
        put32(as_int());
        return;
    }
    switch (tag & script::type_family) {
        case script::type_int:
            put32(as_int());
            return;
        case script::type_float:
            put32(std::bit_cast<std::uint32_t>(v.as<f32>()));
            return;
        case script::type_vector: {
            // Движок уже выделил 12 байт и положил сюда указатель — пишем на месте.
            vector* p = nullptr;
            std::memcpy(&p, at, sizeof p);
            if (p) {
                *p = v.as<vector>();
            }
            return;
        }
        case script::type_string:
            // Строка обязана дожить до конца вызова — кладём её в ту же арену, что и text.
            put_ptr(stash(v.to_string()));
            return;
        case script::type_class:
            put_ptr(v.as<obj>().ptr);
            return;
        case script::type_typename:
            put_ptr(v.as<type>().ptr);
            return;
        default:
            return;
    }
}

inline void write_var(void* var, const value& v, var_kind kind = var_kind::returned) {
    store(var, var, v, kind);
}

// ── Аргумент маршалируемого вызова ───────────────────────────────────────────
// graft::value в сигнатуре — признак того, что метод обслуживает шаблонный класс и
// зовётся маршалируемым путём. Библиотека переключается на него сама.
template <class T>
inline constexpr bool is_value_type = std::is_base_of_v<value, std::remove_cvref_t<T>>;

// Неконстантная ССЫЛКА — out: движок помечает такие переменные флагом 0x80 и забирает
// записанное обратно в скрипт (проверено: запись по +0 дошла до скрипта для int, float и
// string).
template <class T>
inline constexpr bool is_out_param = std::is_lvalue_reference_v<T> &&
                                     !std::is_const_v<std::remove_reference_t<T>>;

template <class T, class... U>
inline constexpr bool is_one_of = (std::is_same_v<std::remove_cvref_t<T>, U> || ...);

// out у значений движок даёт только маршалируемому вызову, поэтому `f32& ratio` в
// сигнатуре переключает привязку на него сама — как и graft::value. Строка сюда тоже
// попадает, но лишь затем, чтобы marshal_check отказал ей внятно.
template <class T>
inline constexpr bool is_string_out = is_out_param<T> && is_one_of<T, std::string, str>;
template <class T>
inline constexpr bool is_scalar_out = is_out_param<T> && is_one_of<T, bool, i32, f32, vector>;

// Как аргумент называется в объявлении скрипта: голый value — «любой» (void), param<"K"> —
// своим именем, обычный тип — как у `proto native`.
//
// Смешивать можно: маршалируемый вызов передаёт ВСЕ аргументы блоком с тегами, и
// объявленный тип аргумента на форму вызова не влияет. Поэтому сигнатура вроде
// `bool Emit(i32 schema, obj player, vector pos, value data)` объявляется как
// `proto bool Emit(int p0, Class p1, vector p2, void p3)` — точная типизация там,
// где она есть, и «любой» только там, где он нужен.
template <class T>
struct value_name;

template <>
struct value_name<value> {
    static constexpr name_t id{"void"};
};

template <name_t N>
struct value_name<param<N>> {
    static constexpr name_t id = N;
};

template <class A>
struct marshal_name {
    static constexpr name_t id = enf_type<arg_abi<A>>::id;
};

template <class A>
    requires is_value_type<A>
struct marshal_name<A> {
    static constexpr name_t id = value_name<std::remove_cvref_t<A>>::id;
};

template <class R>
struct marshal_ret {
    static constexpr const char* value = marshal_name<R>::id.value;
};
template <>
struct marshal_ret<void> {
    static constexpr const char* value = "void";
};

// Аргумент в объявлении: out-аргументы — с модификатором, движок забирает записанное обратно.
template <class A>
inline constexpr auto marshal_decl = [] {
    if constexpr (is_out_param<A>) {
        return name_t{"out "} + marshal_name<A>::id;
    } else {
        return marshal_name<A>::id;
    }
}();

// Обычный тип: читается по объявлению в свой ABI-тип и переводится тем же arg_of, что и у
// `proto native` — std::string, std::string_view, объект класса с полями (const C&).
// Неконстантная ссылка (bool/int/float/vector) — out: после вызова пишется обратно.
template <class A>
struct marshal_arg {
    using slot = arg_abi<A>;

    static slot load(const void* at, const void*) { return as_typed<slot>(at); }

    static decltype(auto) pass(slot& s) { return arg_of<A>::in(s); }

    static void store(slot& s, void* at, const void* var) {
        if constexpr (is_out_param<A>) {
            detail::store(at, var, value{s}, var_kind::out);
        }
    }
};

// graft::value и param<"K">: тип приезжает тегом переменной.
template <class A>
    requires is_value_type<A>
struct marshal_arg<A> {
    using slot = std::remove_cvref_t<A>;

    static slot load(const void* at, const void* var) { return slot{read_as(at, tag_of_var(var))}; }

    static A pass(slot& s) { return static_cast<A>(s); }

    static void store(slot& s, void* at, const void* var) {
        if constexpr (is_out_param<A>) {
            detail::store(at, var, s, var_kind::out);
        }
    }
};

template <class R, class... A>
inline constexpr bool needs_marshal = is_value_type<R> || ((is_value_type<A> || is_scalar_out<A> || is_string_out<A>) || ... || false);

// Нужен ли маршалируемый путь свободной функции и методу (у всех трёх форм метода — по
// одной сигнатуре).
template <auto F>
inline constexpr bool marshalled_free = false;
template <class R, class... A, R (*F)(A...)>
inline constexpr bool marshalled_free<F> = needs_marshal<R, A...>;

template <class R, class Args>
inline constexpr bool needs_marshal_of = false;
template <class R, class... A>
inline constexpr bool needs_marshal_of<R, type_list<A...>> = needs_marshal<R, A...>;
template <auto F>
inline constexpr bool marshalled_member = needs_marshal_of<typename method_sig<F>::ret, typename method_sig<F>::args>;

template <class R, class... A>
struct marshal_check {
    static_assert(((!is_out_param<A> || is_value_type<A> || is_scalar_out<A> || is_string_out<A>) && ... && true),
                  "out-аргумент маршалируемого метода — graft::value, param<> или bool/int/float/vector: "
                  "обратно движку значение отдаётся по тегу");
    static_assert((!is_string_out<A> && ... && true),
                  "строку через out не вернуть: ею владеет движок и освободит сам — верни её результатом");
    static_assert(std::is_void_v<R> || std::is_constructible_v<value, R>,
                  "возврат маршалируемого метода должен приводиться к graft::value");
    static constexpr const char* ret = marshal_ret<R>::value;
    static constexpr const char* const args[] = {marshal_decl<A>.value..., nullptr};
};

// Результат маршалируемого метода: graft::value или что-то, из чего он строится.
template <class T>
value to_value(T&& v) {
    if constexpr (is_value_type<T>) {
        return std::forward<T>(v);
    } else {
        return value{std::forward<T>(v)};
    }
}

// ── Один исполнитель на все формы ────────────────────────────────────────────
// Метод объекта, глобальный и статический натив, метод инстанциации шаблона — разница у
// них только в том, кого звать (call). Значения живут в slots до конца вызова: out-ссылки
// смотрят именно на них, и после вызова они пишутся обратно.
template <class R, class... A, class Call, std::size_t... I>
std::int64_t marshal_run(Call&& call, const script::arg_block* block, void* ret, std::index_sequence<I...>) {
    [[maybe_unused]] arena_scope<R> alive;

    [[maybe_unused]] const auto at = [block](std::size_t i) -> void* {
        return block && block->values ? block->values[i] : nullptr;
    };
    // Где движок переменную не дал, значение лежит в переменной само — читаем его так.
    [[maybe_unused]] const auto var = [&](std::size_t i) -> void* {
        return block && block->vars && block->vars[i] ? block->vars[i] : at(i);
    };
    std::tuple<typename marshal_arg<A>::slot...> slots{marshal_arg<A>::load(at(I), var(I))...};

    const auto write_back = [&] { (marshal_arg<A>::store(std::get<I>(slots), at(I), var(I)), ...); };
    if constexpr (std::is_void_v<R>) {
        call(marshal_arg<A>::pass(std::get<I>(slots))...);
        write_back();
    } else {
        value result = to_value(call(marshal_arg<A>::pass(std::get<I>(slots))...));
        write_back();
        write_var(ret, result);
    }
    return 0;
}

// Метод объекта: функция-член, константная или свободная с объектом первым параметром.
template <class C, auto F, class R, class Args>
struct marshal_method;

template <class C, auto F, class R, class... A>
struct marshal_method<C, F, R, type_list<A...>> : marshal_check<R, A...> {
    static std::int64_t __fastcall call(void* self, script::arg_block* args, void** ret) {
        return guarded<std::int64_t>(reinterpret_cast<void*>(&call), [&] { return body(self, args, ret); });
    }

    static std::int64_t body(void* self, script::arg_block* args, void** ret) {
        C& object = instance_of<C>(self);
        return marshal_run<R, A...>(
            [&](auto&&... a) -> decltype(auto) { return std::invoke(F, object, std::forward<decltype(a)>(a)...); },
            args,
            ret ? *ret : nullptr,
            std::index_sequence_for<A...>{});
    }
};

template <class C, auto F>
struct marshal_thunk : marshal_method<C, F, typename method_sig<F>::ret, typename method_sig<F>::args> {};

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
template <auto F>
struct marshal_free_thunk;

template <class R, class... A, R (*F)(A...)>
struct marshal_free_thunk<F> : marshal_check<R, A...> {
    static std::int64_t __fastcall call(script::arg_block* args, void** ret) {
        // Тело — отдельной функцией: у x64-SEH таблица областей описывает диапазоны
        // адресов, и защита работает, только если сбой случился ВНУТРИ вызова.
        return guarded<std::int64_t>(reinterpret_cast<void*>(&call), [&] { return body(args, ret); });
    }

    static std::int64_t body(script::arg_block* args, void** ret) {
        return marshal_run<R, A...>(F, args, ret ? *ret : nullptr, std::index_sequence_for<A...>{});
    }
};

}  // namespace graft::detail
