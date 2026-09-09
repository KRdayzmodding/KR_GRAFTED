// Врезка в чужой код — сервис хоста, а не библиотека в плагине.
//
// Проверяется здесь, а не только в игре: механика врезки лежит в отдельной цели
// (graft::hook), и сьюта линкует её напрямую. Значит «детур сработал, оригинал остался
// достижим» краснеет обычным прогоном, а не выездом на стенд.
#include <gtest/gtest.h>

// Часть кейсов ниже проверяет, что врезка не мешает обработке сбоев: для этого нужны и
// SEH, и векторный обработчик, и права на страницу — то есть Win32 целиком.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

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

// Детур, которому оригинал не нужен вовсе: для кейсов, где важен сам факт патча, а не
// достижимость оригинала. Значение выбрано так, чтобы его нельзя было спутать ни с чем.
int __fastcall detour_flat(int) {
    return 777;
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

// Повторная врезка в ту же цель — отказ, а не молча испорченный пролог. Держит его
// список целей хоста: у safetyhook такого списка нет, он принял бы вторую и построил
// цепочку, которую снятие в неверном порядке порвало бы молча.
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

// Две заявки на ОДНУ цель в одной пачке — отказ. Словарь занятых целей их не поймает:
// цели в нём ещё нет, а safetyhook под ним принял бы обе и построил цепочку.
TEST(Hook, BatchRefusesTheSameTargetTwice) {
    answer_fn first = nullptr;
    answer_fn again = nullptr;
    EXPECT_FALSE(graft::hook_all({graft::hooked(&answer, &detour_flat, &first),
                                  graft::hooked(&answer, &detour_flat, &again)}));
    EXPECT_EQ(g_call(1), 2) << "цель тронули, хотя пачку отклонили";
    EXPECT_FALSE(graft::unhook(&answer));
}

TEST(Hook, EmptyBatchIsHarmless) {
    EXPECT_TRUE(graft::hook_all({}));
}

// Цена. Сам ВЫЗОВ после врезки стоит один переход — мерить там нечего. А вот УСТАНОВКА
// стоит дорого и по неочевидной причине: патч останавливает все потоки процесса (снимок
// через toolhelp) и проверяет, не стоит ли чей-то rip внутри патчимого пролога. Это
// десятки миллисекунд на каждую врезку, и потому она делается на старте, а не по ходу
// игры. Замер печатает число, чтобы это было видно, а не подразумевалось.
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

// ── Врезка и обработка сбоев: они обязаны не мешать друг другу ────────────────
// Библиотека забирает падение натива себе (guard.hpp, __try/__except) и на этом держится
// обещание «кривой плагин не уносит сервер». Врезка стоит НИЖЕ по стеку обработчиков:
// она не имеет права ни съесть чужой сбой, ни оставить после себя что-то, что съест его
// потом. Кейсы ниже проверяют оба конца этого обещания фактом, а не рассуждением.
//
// Почему это тесты именно ЗДЕСЬ, а не в runtime_test: они краснеют только от смены
// механики врезки — и обязаны покраснеть, если она однажды сменится на такую, которая
// ставит свой обработчик исключений процесса.

namespace {

// Сбой обязан лежать в ОТДЕЛЬНОЙ noinline функции: у x64-SEH таблица областей описывает
// ДИАПАЗОНЫ АДРЕСОВ, и обращение по нулю, написанное прямо в защищённой функции,
// компилятор выносит за её пределы (RESEARCH/theory/crashes.md).
__declspec(noinline) int read_at(const volatile int* where) {
    return *where;
}

// `try` и `__try` в одной функции стоять не могут — отсюда два слоя и здесь тоже.
int guarded_read(const volatile int* where) {
    __try {
        return read_at(where);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

}  // namespace

// Врезка стоит — сбой в чужом коде по-прежнему доходит до нашего __except. Если этот
// кейс покраснел, значит механика врезки завела в процессе свой обработчик и встала
// ВЫШЕ кадрового: с этого момента guard.hpp больше не защита, а видимость.
TEST(Hook, FaultStillReachesSehWhileHooked) {
    answer_fn original = nullptr;
    ASSERT_TRUE(graft::hook(&answer, &detour, &original));

    EXPECT_EQ(guarded_read(nullptr), -1);

    EXPECT_TRUE(graft::unhook(&answer));
    // И после снятия — тоже: снятие обязано возвращать процесс в исходное состояние.
    EXPECT_EQ(guarded_read(nullptr), -1);
}

// Снятая врезка не оставляет за собой ничего, что съест ЧУЖОЙ сбой потом.
//
// ЗАЧЕМ ЭТОТ КЕЙС. Механику врезки можно построить двумя способами. Первый — заморозить
// потоки на время патча и переставить rip тем, кто стоит внутри переписываемых байт;
// после этого в процессе не остаётся ничего. Второй — снять со страницы бит исполнения
// и ловить обращения СВОИМ векторным обработчиком, отвечая ему «продолжай». Второй
// способ дешевле в реализации и ровно поэтому опасен: обработчик остаётся зарегистрирован
// на весь процесс, список отравленных страниц не чистится, и однажды настоящий сбой на
// такой странице получит «продолжай» — то есть вечный цикл вместо падения. Для
// библиотеки, которая обещает ЗАБРАТЬ падение и записать, кто упал, это подмена обещания.
//
// Проверяем поведением, а не заглядыванием внутрь: врезаемся в свою страницу, снимаем
// врезку, отравляем страницу и смотрим, СКОЛЬКО РАЗ сбой будет доставлен. Ровно один —
// значит его отдали нашему __except. Два — значит кто-то вернул «продолжай».
namespace {

std::uint8_t* g_poisoned = nullptr;
std::atomic<long> g_faults{0};

// Считает сбои на отравленной странице и пропускает их дальше по цепочке — к чужим
// обработчикам и к нашему __except. Со второго раза снимает отраву сам: если цепочку
// замкнуло в цикл, кейс обязан покраснеть, а не подвесить прогон.
long CALLBACK count_faults(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        ep->ExceptionRecord->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto at = reinterpret_cast<std::uint8_t*>(ep->ExceptionRecord->ExceptionInformation[1]);
    if (g_poisoned == nullptr || at < g_poisoned || at >= g_poisoned + 0x1000) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_faults.fetch_add(1) + 1 >= 2) {
        DWORD was = 0;
        VirtualProtect(g_poisoned, 0x1000, PAGE_EXECUTE_READWRITE, &was);
        return EXCEPTION_CONTINUE_EXECUTION;  // страховка от вечного цикла
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int __fastcall detour_on_page(int x) {
    return x;
}

}  // namespace

TEST(Hook, RemovedHookLeavesNothingThatSwallowsFaults) {
    auto* page = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(page, nullptr);

    // Обычный пролог MSVC и возврат: десять байт под переход и корректный разбор любым
    // дизассемблером. Тело не зовём — важно только то, что во врезку оно годится.
    const std::uint8_t body[] = {0x48, 0x89, 0x5C, 0x24, 0x08,   // mov [rsp+8], rbx
                                 0x48, 0x89, 0x6C, 0x24, 0x10,   // mov [rsp+10h], rbp
                                 0xC3};                          // ret
    memcpy(page, body, sizeof body);
    FlushInstructionCache(GetCurrentProcess(), page, sizeof body);

    void* original = nullptr;
    ASSERT_TRUE(graft::hook(page, reinterpret_cast<void*>(&detour_on_page), &original));
    ASSERT_TRUE(graft::unhook(page));

    g_poisoned = page;
    g_faults.store(0);
    void* const veh = AddVectoredExceptionHandler(1, &count_faults);
    ASSERT_NE(veh, nullptr);

    DWORD was = 0;
    ASSERT_NE(VirtualProtect(page, 0x1000, PAGE_NOACCESS, &was), 0);
    // Читаем в стороне от переписанных байт, но на той же странице: именно так выглядит
    // случайный сбой рядом с целью, а не попадание ровно в патч.
    const int got = guarded_read(reinterpret_cast<const volatile int*>(page + 0x100));

    RemoveVectoredExceptionHandler(veh);
    VirtualProtect(page, 0x1000, PAGE_EXECUTE_READWRITE, &was);
    g_poisoned = nullptr;
    VirtualFree(page, 0, MEM_RELEASE);

    EXPECT_EQ(g_faults.load(), 1) << "сбой доставлен дважды — кто-то ответил «продолжай»";
    EXPECT_EQ(got, -1) << "сбой не дошёл до __except: врезка забрала его себе";
}

// Врезка под нагрузкой: цель зовут с других потоков, пока пролог переписывают.
//
// Это и есть тот случай, ради которого патч обязан останавливать потоки: поток, снятый
// планировщиком внутри переписываемых байт, после возобновления продолжит с середины
// новой инструкции. Кейс не ловит эту гонку прицельно (она редкая), но ловит всё, чем
// такая механика ломается заметно: незамороженный поток на пропавшем прологе, потерянный
// ResumeThread, взаимная блокировка на своём же замке.
TEST(Hook, TargetUnderTrafficSurvivesInstallAndRemoval) {
    std::atomic<bool> stop{false};
    std::atomic<long long> spins{0};
    std::atomic<long> wrong{0};

    // Детур НЕ зовёт оригинал: иначе кейс проверял бы ещё и время жизни трамплина, а
    // после снятия врезки поток может оказаться внутри уже освобождённого.
    static answer_fn s_unused = nullptr;

    std::vector<std::jthread> hammer;
    for (int i = 0; i < 4; ++i) {
        hammer.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const int got = g_call(1);
                if (got != 2 && got != 777) {
                    wrong.fetch_add(1, std::memory_order_relaxed);
                }
                spins.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int round = 0; round < 10; ++round) {
        ASSERT_TRUE(graft::hook(&answer, &detour_flat, &s_unused));
        ASSERT_TRUE(graft::unhook(&answer));
    }

    stop.store(true);
    hammer.clear();  // jthread: join на разрушении

    EXPECT_EQ(wrong.load(), 0) << "цель вернула то, чего не возвращают ни оригинал, ни детур";
    EXPECT_GT(spins.load(), 0) << "потоки не крутились — их не разморозили";
    EXPECT_EQ(g_call(1), 2) << "после снятия цель обязана быть исходной";
}
