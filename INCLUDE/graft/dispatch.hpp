// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>
#include <vector>

#include "graft/world.hpp"

// Настоящий шаблон C++ вместо фасада: одна привязка обслуживает ЛЮБЫЕ параметры
// скриптового шаблонного класса. Какая инстанциация приехала, говорит имя класса
// объекта — оно разбирается один раз и запоминается.
namespace graft::detail {

// ── Настоящий шаблон C++ вместо фасада ──────────────────────────────────────
// Инстанциация скриптового шаблона — отдельный класс с полным именем
// ("CppHashMap<string,int>"), и это имя доступно у любого объекта. Значит по объекту
// видно, какие типы подставлены, — и вызов можно отправить в НАСТОЯЩИЙ Table<K,V>.
// Тогда в C++ пишется обычный шаблон обычными типами:
//
//   template <class K, class V>
//   struct Table {
//       std::unordered_map<K, V> data;
//       void Set(K key, V value) { data.insert_or_assign(std::move(key), std::move(value)); }
//       V Get(K key) const { … }
//   };
//
// Библиотека разворачивает шаблон по всем поддерживаемым типам заранее, а на вызове
// выбирает нужную инстанциацию по имени класса объекта.

// Типы, по которым разворачивается пользовательский шаблон, и их имена в скрипте.
// «Class» — любой объект: конкретный класс подставлять незачем, ссылка одна и та же.
template <class... T>
struct type_list {
    static constexpr std::size_t size = sizeof...(T);
};
using script_param_types = type_list<i32, f32, bool, std::string, vector, obj>;

inline constexpr std::size_t kParamCount = script_param_types::size;

// Имена скриптовых типов из САМОГО списка, а не второй копией рядом. Раньше здесь была
// цепочка `if (name == "int") return 0;` с руками проставленными индексами — то есть
// параллельный список, который молча разъехался бы с script_param_types при первой же
// правке. Теперь таблица собирается из него на этапе компиляции, и разъехаться нечему.
inline constexpr auto kParamNames = []<class... T>(type_list<T...>) {
    return std::array<std::string_view, sizeof...(T)>{
        enf_type<typename arg_of<T>::abi>::name...};
}(script_param_types{});

// Индекс типа obj — он же ответ по умолчанию: незнакомый параметр обслуживается как
// объект. Тоже вычисляется, а не пишется числом.
inline constexpr std::size_t kParamObject = [] {
    const auto it = std::ranges::find(kParamNames, enf_type<obj>::name);
    return static_cast<std::size_t>(it - kParamNames.begin());
}();
static_assert(kParamObject < kParamCount, "в script_param_types обязан быть obj");

constexpr std::size_t param_index(std::string_view name) {
    const auto it = std::ranges::find(kParamNames, name);
    return it == kParamNames.end() ? kParamObject : static_cast<std::size_t>(it - kParamNames.begin());
}

inline void split_params(std::string_view full, std::string_view* out, std::size_t max,
                         std::size_t& count) {
    count = 0;
    const auto open = full.find('<');
    if (open == full.npos || full.back() != '>') {
        return;
    }
    const std::string_view inner = full.substr(open + 1, full.size() - open - 2);
    std::size_t depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= inner.size(); ++i) {
        const char c = i < inner.size() ? inner[i] : ',';
        if (c == '<') {
            ++depth;
        } else if (c == '>') {
            --depth;
        } else if (c == ',' && depth == 0) {
            if (count < max) {
                std::string_view part = inner.substr(start, i - start);
                while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
                while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
                out[count++] = part;
            }
            start = i + 1;
        }
    }
}

// Индексы параметров инстанциации по самому объекту. Считаются один раз на дескриптор
// класса: у всех объектов одной инстанциации он общий.
template <std::size_t N>
struct param_slots {
    std::size_t at[N]{};
};

template <std::size_t N>
param_slots<N> parse_slots(void* self) {
    param_slots<N> slots{};
    for (std::size_t i = 0; i < N; ++i) {
        slots.at[i] = kParamObject;
    }
    const char* name = script::class_name(script::class_of(self));
    if (!name) {
        return slots;
    }
    std::string_view parts[N]{};
    std::size_t count = 0;
    split_params(name, parts, N, count);
    for (std::size_t i = 0; i < count && i < N; ++i) {
        slots.at[i] = param_index(parts[i]);
    }
    return slots;
}

// Разбор имени — на вызов заметная работа, а ответ зависит только от дескриптора
// класса. Помним последний: цикл в скрипте обычно молотит одну инстанциацию, а промах
// стоит ровно того же разбора, что и раньше.
// ponytail: одна ячейка вместо таблицы — чередование инстанциаций в горячем цикле
// сведёт её на нет, тогда сюда просится unordered_map по дескриптору.
template <std::size_t N>
param_slots<N> slots_of(void* self) {
    void* const class_name = script::class_of(self);
    thread_local void* seen = nullptr;
    thread_local param_slots<N> cached{};
    if (class_name != seen) {
        cached = parse_slots<N>(self);
        seen = class_name;
    }
    return cached;
}

// Значение скриптовой переменной как КОНКРЕТНЫЙ тип C++ (а не value). Путь через value
// стоил бы на каждый аргумент сборки и разрушения варианта (внутри него std::string, то
// есть нетривиальный деструктор), а для строки — ещё и лишней копии: одна в вариант,
// вторая из него. Решения по тегу здесь ровно те же, что и в read_var.
template <class T>
T as_typed(const void* var) {
    if constexpr (std::is_same_v<T, value>) {
        return read_var(var);
    } else {
        if (!var) {
            return T{};
        }
        const auto* at = static_cast<const char*>(var);
        const auto tag = *reinterpret_cast<const std::uint32_t*>(at + 16);
        const auto raw = *reinterpret_cast<const std::uint64_t*>(at);
        const auto family = tag & script::type_family;
        if constexpr (std::is_same_v<T, bool>) {
            return tag == script::type_bool && raw != 0;
        } else if constexpr (std::is_same_v<T, i32>) {
            return family == script::type_int && tag != script::type_bool
                       ? static_cast<i32>(static_cast<std::uint32_t>(raw))
                       : i32{};
        } else if constexpr (std::is_same_v<T, f32>) {
            if (family != script::type_float) {
                return f32{};
            }
            return std::bit_cast<f32>(static_cast<std::uint32_t>(raw));
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (family != script::type_string) {
                return read_var(var).to_string();  // не строка — приводим к тексту
            }
            const auto* text = reinterpret_cast<const char*>(raw);
            return text ? std::string{text} : std::string{};
        } else if constexpr (std::is_same_v<T, vector>) {
            // vector в 8 байт не влезает — в переменной указатель на сами 12 байт.
            const auto* p = reinterpret_cast<const vector*>(raw);
            return family == script::type_vector && p ? *p : vector{};
        } else if constexpr (std::is_same_v<T, obj>) {
            return family == script::type_class
                       ? obj{script::deref_object(reinterpret_cast<void*>(raw))}
                       : obj{};
        } else if constexpr (std::is_same_v<T, type>) {
            return family == script::type_typename ? type{reinterpret_cast<void*>(raw)} : type{};
        } else {
            return read_var(var).template as<T>();
        }
    }
}

// Трамплин для одной инстанциации: аргументы приводятся к настоящим типам метода.
template <class Inst, auto F>
struct typed_thunk;

template <class Inst, class Own, class R, class... A, R (Own::*F)(A...)>
struct typed_thunk<Inst, F> {
    static std::int64_t run(void* self, void** block, void* ret) {
        [[maybe_unused]] arena_scope<R> alive;
        return call(instance_of<Inst>(self), block, ret, std::index_sequence_for<A...>{});
    }

private:
    template <std::size_t... I>
    static std::int64_t call(Inst& object, void** block, void* ret, std::index_sequence<I...>) {
        if constexpr (std::is_void_v<R>) {
            (object.*F)(as_typed<std::remove_cvref_t<A>>(block ? block[I] : nullptr)...);
        } else {
            write_var(ret, value{(object.*F)(
                              as_typed<std::remove_cvref_t<A>>(block ? block[I] : nullptr)...)});
        }
        return 0;
    }
};

template <class Inst, class Own, class R, class... A, R (Own::*F)(A...) const>
struct typed_thunk<Inst, F> {
    static std::int64_t run(void* self, void** block, void* ret) {
        [[maybe_unused]] arena_scope<R> alive;
        return call(instance_of<Inst>(self), block, ret, std::index_sequence_for<A...>{});
    }

private:
    template <std::size_t... I>
    static std::int64_t call(const Inst& object, void** block, void* ret,
                             std::index_sequence<I...>) {
        if constexpr (std::is_void_v<R>) {
            (object.*F)(as_typed<std::remove_cvref_t<A>>(block ? block[I] : nullptr)...);
        } else {
            write_var(ret, value{(object.*F)(
                              as_typed<std::remove_cvref_t<A>>(block ? block[I] : nullptr)...)});
        }
        return 0;
    }
};

// Имена для объявления. Метод шаблонного класса пишется обычными типами, поэтому какой
// аргумент — параметр шаблона, а какой обычный тип, выясняем сравнением ДВУХ пробных
// инстанциаций: C<int,float> и C<float,int>. Аргумент, который поменялся вместе с
// позицией i, — это i-й параметр шаблона; который не поменялся — обычный тип.
template <class TA, class TB>
constexpr int param_slot() {
    if constexpr (std::is_same_v<TA, i32> && std::is_same_v<TB, f32>) {
        return 0;
    } else if constexpr (std::is_same_v<TA, f32> && std::is_same_v<TB, i32>) {
        return 1;
    } else {
        return -1;
    }
}

// Имя типа для объявления: либо имя параметра шаблона из заголовка класса, либо
// обычное имя типа Enforce.
template <class TA, class TB>
const char* slot_name(const std::string_view* params, std::size_t count) {
    constexpr int slot = param_slot<TA, TB>();
    if constexpr (slot >= 0) {
        return static_cast<std::size_t>(slot) < count
                   ? intern(std::string{params[static_cast<std::size_t>(slot)]})
                   : "void";
    } else {
        return enf_type<std::remove_cvref_t<TA>>::name;
    }
}

// Список имён аргументов, собранный на этапе привязки (nullptr-terminated).
inline const char* const* intern_args(std::vector<const char*> names) {
    static std::deque<std::vector<const char*>> kept;
    names.push_back(nullptr);
    return kept.emplace_back(std::move(names)).data();
}

template <class MA, class MB>
struct template_sig;

template <class OA, class RA, class... AA, class OB, class RB, class... AB>
struct template_sig<RA (OA::*)(AA...), RB (OB::*)(AB...)> {
    static const char* ret(const std::string_view* params, std::size_t count) {
        if constexpr (std::is_void_v<RA>) {
            return "void";
        } else {
            return slot_name<RA, RB>(params, count);
        }
    }
    static const char* const* args(const std::string_view* params, std::size_t count) {
        return intern_args({slot_name<AA, AB>(params, count)...});
    }
};

template <class OA, class RA, class... AA, class OB, class RB, class... AB>
struct template_sig<RA (OA::*)(AA...) const, RB (OB::*)(AB...) const>
    : template_sig<RA (OA::*)(AA...), RB (OB::*)(AB...)> {};

// Таблица инстанциаций: разворачиваем шаблон пользователя по всем парам типов и
// складываем указатели на трамплины. Выбор — по индексам из имени класса объекта.
template <template <class...> class C, class Pick, class KL, class VL>
struct dispatch2;

template <template <class...> class C, class Pick, class... K, class... V>
struct dispatch2<C, Pick, type_list<K...>, type_list<V...>> {
    using slot = std::int64_t (*)(void*, void**, void*);

    template <class Kt>
    static constexpr auto row() {
        return std::array<slot, sizeof...(V)>{
            &typed_thunk<C<Kt, V>, Pick{}.template operator()<C<Kt, V>>()>::run...};
    }
    static constexpr auto table = std::array<std::array<slot, sizeof...(V)>, sizeof...(K)>{
        row<K>()...};

    // Арену заводит уже сам трамплин инстанциации: тип возврата у каждой свой, и
    // отсюда его не видно.
    static std::int64_t __fastcall call(void* self, void*** args, void** ret) {
        const auto slots = slots_of<2>(self);
        return table[slots.at[0]][slots.at[1]](self, args ? *args : nullptr,
                                               ret ? *ret : nullptr);
    }
};

// Нужен ли этому указателю на член маршалируемый путь.
template <auto F>
inline constexpr bool marshalled_member = false;
template <class Own, class R, class... A, R (Own::*F)(A...)>
inline constexpr bool marshalled_member<F> = needs_marshal<R, A...>;
template <class Own, class R, class... A, R (Own::*F)(A...) const>
inline constexpr bool marshalled_member<F> = needs_marshal<R, A...>;

// Трамплин метода, у которого класс выводится из самого указателя на член.
template <auto Mfn>
struct member_thunk;
template <class C, class R, class... A, R (C::*Mfn)(A...)>
struct member_thunk<Mfn> : method_thunk<C, Mfn> {};
template <class C, class R, class... A, R (C::*Mfn)(A...) const>
struct member_thunk<Mfn> : method_thunk<C, Mfn> {};

}  // namespace graft::detail
