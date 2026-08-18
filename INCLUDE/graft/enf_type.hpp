#pragma once
#include <concepts>

#include "graft/types.hpp"

// Имя скриптового типа по типу C++ и чтение элемента из слота памяти.
//
// enf_type<T> намеренно не определён в общем виде: неподдерживаемый тип обязан ловиться
// компилятором, а не превращаться в мусорное объявление.
namespace graft {

template <class T>
struct enf_type;  // намеренно не определён: неподдерживаемый тип ловится компилятором

#define GRAFT_MAP_TYPE(cpp, enf)                        \
    template <>                                         \
    struct enf_type<cpp> {                              \
        static constexpr name_t id{enf};                \
        static constexpr const char* name = id.value;   \
    }
GRAFT_MAP_TYPE(void, "void");
GRAFT_MAP_TYPE(bool, "bool");
GRAFT_MAP_TYPE(i32, "int");
GRAFT_MAP_TYPE(f32, "float");
GRAFT_MAP_TYPE(str, "string");
GRAFT_MAP_TYPE(vector, "vector");
GRAFT_MAP_TYPE(owned, "owned string");
#undef GRAFT_MAP_TYPE

// Скриптовая ссылка — и сама ref<...>, и любой пользовательский класс, отражающий
// скриптовый (наследник script_object). Благодаря этому свой класс пишется в сигнатуре
// натива как есть: `i32 SumIds(graft::array<Node> nodes)` — но только пока он остаётся
// голой обёрткой: класс со своими полями в сигнатуру не пролезет (сторож ABI ниже).
template <class T>
concept script_class = requires { T::script_class; } && std::derived_from<T, ref<T::script_class>>;

template <class T>
    requires script_class<T>
struct enf_type<T> {
    static constexpr name_t id = T::script_class;
    static constexpr const char* name = id.value;
};


// Как достать элемент из слота контейнера: значения лежат как есть, ссылки — через
// обёртку.
template <class T>
struct element_access {
    static T load(const void* slot) {
        T value{};
        std::memcpy(&value, slot, sizeof value);
        return value;
    }
};
template <class T>
    requires script_class<T> && (sizeof(T) == sizeof(void*))
struct element_access<T> {
    static T load(const void* slot) {
        void* stored = nullptr;
        std::memcpy(&stored, slot, sizeof stored);
        // В контейнере лежит обёртка, в поле класса — сам объект: различаем по обратной
        // ссылке, чтобы одна и та же вьюха работала и там, и там.
        T value{};
        value.ptr = script::deref_object(stored);
        return value;
    }
};

template <class T>
T load_slot(const void* slot) {
    return element_access<T>::load(slot);
}

template <>
struct enf_type<type> {
    static constexpr name_t id{"typename"};
    static constexpr const char* name = id.value;
};

// Элементы контейнеров лежат в памяти как есть, поэтому поддерживаем только те типы,
// чей размер в скрипте нам достоверно известен (int/float — 4, string/ссылка — 8).
template <class T>
inline constexpr bool is_element = false;
template <>
inline constexpr bool is_element<i32> = true;
template <>
inline constexpr bool is_element<f32> = true;
template <>
inline constexpr bool is_element<str> = true;
template <class T>
    requires script_class<T> && (sizeof(T) == sizeof(void*))
inline constexpr bool is_element<T> = true;

// Элементы, за временем жизни которых движок не следит: их можно писать напрямую.
template <class T>
concept plain_element = std::same_as<T, i32> || std::same_as<T, f32>;


}  // namespace graft
