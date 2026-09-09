// Врезка в чужой код — сервис хоста, а не библиотека в плагине.
//
// Проверяется здесь, а не только в игре: MinHook лежит в отдельной цели (graft::hook),
// и сьюта линкует её напрямую. Значит «детур сработал, оригинал остался достижим»
// краснеет обычным прогоном, а не выездом на стенд.
#include <gtest/gtest.h>

#include <chrono>
#include <iostream>

#include "graft/engine.hpp"

namespace {

using answer_fn = int(__fastcall*)(int);

// Пролог обязан быть длиннее пяти байт: короче — и переходу некуда лечь, не задев
// соседнюю функцию. volatile держит тело от схлопывания в одну инструкцию.
int __fastcall answer(int x) {
    volatile int a = x;
    a += 1;
    return a;
}

int __fastcall twice(int x) {
    volatile int a = x;
    a += a;
    return a;
}

int __fastcall thrice(int x) {
    volatile int a = x;
    a += a + a;
    return a;
}

answer_fn g_original = nullptr;
answer_fn g_twice = nullptr;
answer_fn g_thrice = nullptr;

int __fastcall detour(int x) {
    return g_original(x) * 10;
}

int __fastcall detour_twice(int x) {
    return g_twice(x) * 100;
}

int __fastcall detour_thrice(int x) {
    return g_thrice(x) * 1000;
}

// Через указатель, а не по имени: иначе компилятор подставит тело на месте вызова и
// врезка окажется ни при чём.
answer_fn volatile g_call = &answer;
answer_fn volatile g_call_twice = &twice;
answer_fn volatile g_call_thrice = &thrice;

}  // namespace

TEST(Hook, DetourRunsAndOriginalStaysReachable) {
    ASSERT_EQ(g_call(1), 2);

    ASSERT_TRUE(graft::hook(&answer, &detour, &g_original));
    EXPECT_EQ(g_call(1), 20);      // пошло через детур
    EXPECT_EQ(g_original(1), 2);   // а оригинал достижим трамплином

    EXPECT_TRUE(graft::unhook(&answer));
    EXPECT_EQ(g_call(1), 2);
}

// Повторная врезка в ту же цель — отказ, а не молча испорченный пролог. Ради этого
// копия MinHook в процессе и должна быть одна.
TEST(Hook, RefusesToHookTheSameTargetTwice) {
    answer_fn first = nullptr;
    answer_fn again = nullptr;
    ASSERT_TRUE(graft::hook(&answer, &detour, &first));
    EXPECT_FALSE(graft::hook(&answer, &detour, &again));
    EXPECT_TRUE(graft::unhook(&answer));
}

TEST(Hook, NullArgumentsAreRefused) {
    void* orig = nullptr;
    EXPECT_FALSE(graft::hook(nullptr, reinterpret_cast<void*>(&detour), &orig));
    EXPECT_FALSE(graft::hook(reinterpret_cast<void*>(&answer), nullptr, &orig));
    EXPECT_FALSE(graft::unhook(nullptr));
}

// Снимать нечего — это не ошибка вызывающей стороны, но и не успех.
TEST(Hook, UnhookOfUnknownTargetIsFalse) {
    EXPECT_FALSE(graft::unhook(&answer));
}

// ── Пачкой ───────────────────────────────────────────────────────────────────
TEST(Hook, BatchInstallsEveryTarget) {
    ASSERT_TRUE(graft::hook_all({graft::hooked(&answer, &detour, &g_original),
                                 graft::hooked(&twice, &detour_twice, &g_twice),
                                 graft::hooked(&thrice, &detour_thrice, &g_thrice)}));

    EXPECT_EQ(g_call(1), 20);
    EXPECT_EQ(g_call_twice(1), 200);
    EXPECT_EQ(g_call_thrice(1), 3000);

    EXPECT_TRUE(graft::unhook(&answer));
    EXPECT_TRUE(graft::unhook(&twice));
    EXPECT_TRUE(graft::unhook(&thrice));
}

// Пачка либо встаёт целиком, либо не встаёт вовсе. Половина врезок — это состояние, в
// котором мод уже сломан, но ещё думает, что жив.
TEST(Hook, BatchIsAllOrNothing) {
    answer_fn first = nullptr;
    ASSERT_FALSE(graft::hook_all({graft::hooked(&answer, &detour, &first),
                                  graft::hooked<answer_fn>(nullptr, &detour, &first)}));
    // Первая цель обязана остаться нетронутой — иначе откат не откат.
    EXPECT_EQ(g_call(1), 2);
    EXPECT_FALSE(graft::unhook(&answer));
}

TEST(Hook, EmptyBatchIsHarmless) {
    EXPECT_TRUE(graft::hook_all({}));
}

// Цена. Сам ВЫЗОВ после врезки стоит один переход — мерить там нечего. А вот УСТАНОВКА
// стоит дорого и по неочевидной причине: MinHook останавливает все потоки процесса
// (снимок через toolhelp) и проверяет, не стоит ли чей-то rip внутри патчимого пролога.
// Это десятки миллисекунд на каждую врезку, и потому она делается на старте, а не по
// ходу игры. Замер печатает число, чтобы это было видно, а не подразумевалось.
TEST(Hook, InstallCostIsMeasured) {
    answer_fn original = nullptr;
    constexpr int rounds = 8;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i) {
        ASSERT_TRUE(graft::hook(&answer, &detour, &original));
        ASSERT_TRUE(graft::unhook(&answer));
    }
    const double ms =
        std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - t0}.count() /
        rounds;

    std::cout << "  [замер] врезка + снятие: " << ms << " мс на пару (потоков в процессе — "
              << "все, их и морозят)" << std::endl;
    EXPECT_EQ(g_call(1), 2);
}

// Ради чего пачка и заведена: заморозка одна на всю пачку, а не на каждую цель.
TEST(Hook, BatchIsCheaperThanOneByOne) {
    const auto unhook_three = [] {
        graft::unhook(&answer);
        graft::unhook(&twice);
        graft::unhook(&thrice);
    };

    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(graft::hook(&answer, &detour, &g_original));
    ASSERT_TRUE(graft::hook(&twice, &detour_twice, &g_twice));
    ASSERT_TRUE(graft::hook(&thrice, &detour_thrice, &g_thrice));
    const double one_by_one =
        std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - t0}.count();
    unhook_three();

    const auto t1 = std::chrono::steady_clock::now();
    ASSERT_TRUE(graft::hook_all({graft::hooked(&answer, &detour, &g_original),
                                 graft::hooked(&twice, &detour_twice, &g_twice),
                                 graft::hooked(&thrice, &detour_thrice, &g_thrice)}));
    const double batched =
        std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - t1}.count();
    unhook_three();

    std::cout << "  [замер] три врезки: по одной " << one_by_one << " мс -> пачкой " << batched
              << " мс" << std::endl;
    EXPECT_LT(batched, one_by_one);
}
