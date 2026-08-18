// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "graft/registry.hpp"

// Перевод обычных типов C++ через границу и сторож ABI.
//
// В движок пролезает только то, что влезает в регистр, но ПИСАТЬ в этих типах не нужно:
// функция объявляется обычным C++, а трамплин переводит аргументы и результат сам.
// Свой тип добавляется специализацией arg_of/ret_of — как enf_type.
namespace graft::detail {

// ── Перевод обычных типов C++ через границу ──────────────────────────────────
// В движок пролезает только то, что влезает в регистр, но писать в этих типах не
// обязательно: привязываемая функция объявляется в обычном C++, а трамплин переводит
// её аргументы и результат сам. Свой тип добавляется специализацией — как enf_type.
template <class T>
struct arg_of {  // по умолчанию тип уже пригоден для ABI (i32/f32/bool/str/vector/...)
    using abi = T;
    static T in(T value) { return value; }
};
template <>
struct arg_of<std::string_view> {
    using abi = str;
    static std::string_view in(str s) { return s.view(); }
};
template <>
struct arg_of<std::string> {
    using abi = str;
    static std::string in(str s) { return std::string{s.view()}; }
};
template <>
struct arg_of<const std::string&> {
    using abi = str;
    static std::string in(str s) { return std::string{s.view()}; }
};

// Неконстантная ссылка — это out-аргумент, как в самом Enforce
// (`proto native void GetPlayers(out array<Man> players)`). Отдельного маркера не нужно:
// параметры трамплина — именованные, то есть lvalue, поэтому ссылку можно привязать
// прямо к ним и ничего копировать не приходится.
template <class T>
    requires(!std::is_const_v<T>)
struct arg_of<T&> {
    using abi = T;
    static T& in(T& value) { return value; }
};

// Вектор — обычный std::vector: движковый массив копируется в него на входе. Копия
// честная и видна в сигнатуре; кому она не нужна, берёт graft::array<T> — ту же память
// без копирования.
template <class T>
struct arg_of<std::vector<T>> {
    using abi = array<typename arg_of<T>::abi>;
    static std::vector<T> in(abi view) {
        std::vector<T> out;
        out.reserve(static_cast<std::size_t>(view.size()));
        for (auto element : view) {
            out.push_back(arg_of<T>::in(element));
        }
        return out;
    }
};

// Массив чисел БЕЗ КОПИИ. int и float лежат в движковом буфере подряд и ровно так же,
// как в C++, поэтому span смотрит прямо на них: ни аллокации, ни прохода по элементам.
// Для строк и ссылок так нельзя — там в слоте лежит не значение, а указатель/обёртка,
// и разворачивать её всё равно придётся поэлементно (для них есть std::vector).
template <class T>
    requires plain_element<T>
struct arg_of<std::span<const T>> {
    using abi = array<T>;
    static std::span<const T> in(abi view) {
        const T* data = view.data();
        return data ? std::span<const T>{data, static_cast<std::size_t>(view.size())}
                    : std::span<const T>{};
    }
};

// Скриптовый vector — три float подряд; кому не нужны .x/.y/.z и арифметика graft::vector,
// пишет обычный std::array.
template <>
struct arg_of<std::array<float, 3>> {
    using abi = vector;
    static std::array<float, 3> in(vector v) { return static_cast<std::array<float, 3>>(v); }
};

template <class T>
struct ret_of {
    using abi = T;
    static T out(T value) { return value; }
};
template <>
struct ret_of<void> {
    using abi = void;
};
template <>
struct ret_of<std::string> {  // в скрипте это `owned string`: движок копирует себе
    using abi = text;
    // Строка копируется в арену байтами: своей аллокации арена под неё не делает, а
    // временная std::string натива умирает тут же. Кому и эта аллокация лишняя — пишет
    // возврат через graft::text::of, тот форматирует прямо в арену.
    static text out(const std::string& value) { return text::from(value); }
};
template <>
struct ret_of<std::string_view> {
    using abi = text;
    static text out(std::string_view value) { return text::from(value); }
};
template <>
struct ret_of<std::array<float, 3>> {
    using abi = vector;
    static vector out(std::array<float, 3> v) { return vector::from(v); }
};

// Имя типа для ОБЪЯВЛЕНИЯ в скрипте. Отличается от enf_type<arg_abi<A>> одним: у
// неконстантной ссылки добавляется `out`, а сам ABI-тип у неё тот же, что у значения.
template <class A>
struct script_arg {
    static constexpr const char* name = enf_type<typename arg_of<A>::abi>::name;
};
template <class T>
    requires(!std::is_const_v<T>)
struct script_arg<T&> {
    static constexpr auto id = name_t{"out "} + enf_type<typename arg_of<T>::abi>::id;
    static constexpr const char* name = id.value;
};

template <class T>
using arg_abi = typename arg_of<T>::abi;
template <class T>
using ret_abi = typename ret_of<T>::abi;

// ── Сторож ABI ───────────────────────────────────────────────────────────────
// Движок зовёт натив напрямую, поэтому соглашение обязано совпасть бит в бит.
// По MS x64 структура возвращается в RAX, только пока она «C++03 POD»: появился
// пользовательский конструктор — компилятор молча переходит на скрытый указатель,
// и первый аргумент уезжает в RDX (проверено дизассемблером; ровно на этом мы уже
// один раз упали, добавив конструктор в str). is_aggregate ловит конструкторы;
// базовый класс трейтами не ловится вовсе — не наследуйся от возвращаемых типов.
template <class T>
concept abi_value = std::is_trivially_copyable_v<T> && !std::is_reference_v<T> &&
                    (sizeof(T) <= sizeof(void*) || std::is_same_v<T, vector>);
template <class T>
concept abi_ret = std::is_void_v<T> || std::is_scalar_v<T> ||
                  (abi_value<T> && std::is_aggregate_v<T> && !std::is_polymorphic_v<T>);

// Проверяется то, что реально поедет в движок, — типы ПОСЛЕ перевода.
template <class R, class... A>
struct abi_check {
    static_assert(abi_ret<ret_abi<R>>,
                  "возврат натива не проходит по ABI движка: нужен агрегат без "
                  "конструкторов и баз, либо своя специализация ret_of<T>");
    static_assert((abi_value<arg_abi<A>> && ...),
                  "аргумент натива не проходит по ABI движка: нужна своя arg_of<T>");
    static_assert((!std::is_same_v<A, owned> && ...), "owned string бывает только возвратом");
    static constexpr const char* ret = enf_type<ret_abi<R>>::name;
    static constexpr const char* const args[] = {script_arg<A>::name..., nullptr};
};

// Арена под возвращаемые строки нужна не всякому вызову, а тип возврата известен на
// компиляции: числу, вектору, объекту и typename строке взяться неоткуда (строковый
// out-аргумент движок не отдаёт — write_var такое отклоняет). У таких трамплинов
// call_scope превращается в пустую структуру и вызовов enter/leave не остаётся вовсе.
template <class R>
inline constexpr bool needs_text_arena =
    !(std::is_void_v<R> || std::is_arithmetic_v<R> || std::is_enum_v<R> ||
      std::is_same_v<std::remove_cvref_t<R>, vector> ||
      std::is_same_v<std::remove_cvref_t<R>, obj> ||
      std::is_same_v<std::remove_cvref_t<R>, type>);

struct no_scope {};
template <class R>
using arena_scope = std::conditional_t<needs_text_arena<R>, call_scope, no_scope>;

}  // namespace graft::detail
