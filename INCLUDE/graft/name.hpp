#pragma once
#include <algorithm>
#include <cstddef>

// Имя типа Enforce как значение шаблонного параметра. Позволяет писать
// graft::ref<"PlayerBase"> и собирать "array<int>" на этапе компиляции.
namespace graft {

template <std::size_t N>
struct name_t {
    char value[N]{};
    consteval name_t(const char (&s)[N]) { std::ranges::copy(s, value); }
};
template <std::size_t N>
name_t(const char (&)[N]) -> name_t<N>;

template <std::size_t A, std::size_t B>
consteval name_t<A + B - 1> operator+(name_t<A> a, name_t<B> b) {
    char out[A + B - 1]{};
    // У первого имени хвостовой ноль отбрасывается, у второго — остаётся.
    const auto tail = std::ranges::copy_n(a.value, A - 1, out).out;
    std::ranges::copy(b.value, tail);
    return name_t<A + B - 1>{out};
}

}  // namespace graft
