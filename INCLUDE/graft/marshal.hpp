// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <cstdint>
#include <string>
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
inline value read_var(const void* var) {
    if (!var) {
        return {};
    }
    const auto* at = static_cast<const char*>(var);
    const auto tag = *reinterpret_cast<const std::uint32_t*>(at + 16);
    const auto raw = *reinterpret_cast<const std::uint64_t*>(at);
    if (tag == script::type_bool) {
        return value{raw != 0};
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

// Возврат кладём в переменную, которую движок уже подготовил под тип инстанциации:
// тег там стоит правильный, поэтому приводим значение к нему, а тег не трогаем.
// out — пишем в аргумент, а не в возврат. Разница принципиальная для строк: возврат
// движок копирует себе сразу, а строкой в out-переменной он ВЛАДЕЕТ и потом освободит —
// свой указатель туда класть нельзя (порча кучи 0xC0000374, проверено падением).
// Это ограничение движка, то же самое, что запрет писать char* в array<string>.
enum class var_kind { returned, out };

inline void write_var(void* var, const value& v, var_kind kind = var_kind::returned) {
    if (!var) {
        return;
    }
    auto* at = static_cast<char*>(var);
    const auto tag = *reinterpret_cast<const std::uint32_t*>(at + 16);
    auto* slot = reinterpret_cast<std::uint64_t*>(at);
    if (kind == var_kind::out && (tag & script::type_family) == script::type_string) {
        log("graft: строку через out-аргумент вернуть нельзя — строкой владеет движок; "
            "отдавай её возвратом");
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
// param<"K"> — своим именем.
template <class T>
struct marshal_name {
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
    static_assert((is_value_type<A> && ... && true),
                  "у маршалируемого метода аргументы — graft::value (по значению или out-ссылкой)");
    static_assert(std::is_void_v<R> || std::is_constructible_v<value, R>,
                  "возврат маршалируемого метода должен приводиться к graft::value");
    static constexpr const char* ret = marshal_ret<R>::value;
    // out-аргументы объявляются с модификатором: движок забирает записанное обратно.
    static constexpr const char* const args[] = {marshal_arg<A>::value..., nullptr};
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

template <class C, auto F>
struct marshal_thunk;

// Один трамплин на все инстанциации: значения читаются по тегам, out-аргументы
// записываются обратно после вызова.
template <class R, class... A, class C, class M, std::size_t... I>
std::int64_t marshal_run(C&& object, M method, void** block, void* ret,
                         std::index_sequence<I...>) {
    // Значения живут до конца вызова: out-ссылки смотрят именно на них.
    std::tuple<std::remove_cvref_t<A>...> slots{read_var(block ? block[I] : nullptr)...};
    const auto write_back = [&] {
        ((is_out_param<A> ? write_var(block ? block[I] : nullptr, std::get<I>(slots), var_kind::out)
                          : void()),
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

}  // namespace graft::detail
