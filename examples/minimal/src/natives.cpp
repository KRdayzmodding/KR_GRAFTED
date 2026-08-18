// Единственный файл, который правишь под свой мод: здесь объявляются нативы.
// Из каждой записи получается и адрес impl для движка, и строка `proto native`
// для PBO (её пишет graft.exe на каждой сборке) — разъехаться они не могут.
//
// ПИШЕТСЯ ЭТО ОБЫЧНЫМ C++. Никаких своих типов в сигнатурах не нужно: трамплин
// переводит их сам.
//
//   int / float / bool          -> int / float / bool
//   string_view            -> string          (аргумент, без копии)
//   string                 -> string          (аргумент, с копией)
//   string                 -> owned string    (возврат)
//   vector<T>              -> array<T>        (аргумент, копия)
//   array<float, 3>        -> vector
//   T&  (неконстантная ссылка)  -> out T
//
// Свои типы остаются только там, где обычным C++ не выразить: `graft::array<T>` —
// ВЬЮХА на движковую память (тот же array, но без копирования), `graft::ref<"Класс">` —
// ссылка на скриптовый объект. Полный список — в README рядом (examples/minimal/README.md).
#include <algorithm>
#include <array>
#include <format>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "graft/native.hpp"

using namespace std;

namespace {

// Скриптовый класс отражается классом C++: наследуешься от script_object<"Имя"> и
// пишешь обычные методы. Объект приезжает сам (это this), в объявление скрипта он не
// попадает — ровно как у ванильных коллекций.
//   modded class ExampleGraft { proto native int Version(); }
// Класс должен быть объявлен в 1_Core мода: движок связывает нативы, когда скрипты
// уже разобраны.
struct ExampleGraft : graft::script_object<"ExampleGraft"> {
    int Version() const { return *this ? 1 : 0; }
};

// Глобальная функция скрипта — обычная функция C++.
//   proto native int ExampleAdd(int p0, int p1);
int ExampleAdd(int a, int b) {
    return a + b;
}

// Возврат строки: обычный string. Память до конца вызова держит библиотека —
// движок копирует строку себе, статических буферов и snprintf не нужно.
//   proto native owned string ExampleGreet(string p0);
string ExampleGreet(string_view who) {
    return format("hello, {}", who.empty() ? "world" : who);
}

// Скриптовый array<int> приезжает обычным вектором — копией, и дальше это обычный C++
// со всей стандартной библиотекой.
//   proto native int ExampleSum(array<int> p0);
int ExampleSum(vector<int> values) {
    return accumulate(values.begin(), values.end(), 0);
}

// А это тот же массив БЕЗ копии: graft::array<T> — вьюха на движковую память, и она
// полноценный range, так что std::ranges по ней работают.
//   proto native int ExampleCountAbove(array<int> p0, int p1);
int ExampleCountAbove(graft::array<int> values, int threshold) {
    return static_cast<int>(ranges::count_if(values, [&](int v) { return v > threshold; }));
}

// Массив можно и заполнить: неконстантная ссылка в сигнатуре — это `out` в объявлении.
// Размер ставит движковый Resize, писать числа безопасно.
//   proto native int ExampleFillSquares(out array<int> p0, int p1);
int ExampleFillSquares(graft::array<int>& values, int count) {
    if (!values.resize(count)) {
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        values.set(i, i * i);
    }
    return values.size();
}

// Скриптовый vector — три float: обычный std::array.
//   proto native vector ExampleScale(vector p0, float p1);
array<float, 3> ExampleScale(array<float, 3> v, float k) {
    return {v[0] * k, v[1] * k, v[2] * k};
}

// ── Что регистрировать и что объявлять ───────────────────────────────────────
// Аргумент — скриптовый модуль, в который генератор напечатает объявления. Нативу с
// игровым типом в сигнатуре (Object, EntityAI) нужен отдельный блок GRAFT_BINDINGS("3_Game"):
// в 1_Core таких типов ещё нет.
GRAFT_BINDINGS("1_Core") {
    bind.class_<ExampleGraft>().method<&ExampleGraft::Version>("Version");

    bind.global<&ExampleAdd>("ExampleAdd")
        .global<&ExampleGreet>("ExampleGreet")
        .global<&ExampleSum>("ExampleSum")
        .global<&ExampleCountAbove>("ExampleCountAbove")
        .global<&ExampleFillSquares>("ExampleFillSquares")
        .global<&ExampleScale>("ExampleScale");
}

}  // namespace
