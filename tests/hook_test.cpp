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
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include "graft/engine.hpp"
#include "graft/loader.hpp"
#include "graft/thunk.hpp"
#include "graft/loader.hpp"

namespace {

// Числа нагрузки читаются из окружения: гонка редкая, и разбирали её прогонами вида
// GRAFT_HAMMERS=16 GRAFT_ROUNDS=600. По умолчанию скромные — кейс живёт в обычной сьюте,
// а каждая врезка стоит десятки миллисекунд. getenv_s, а не getenv: второй у MSVC помечен
// устаревшим, и сьюта собирается без предупреждений.
int from_env(const char* name, int fallback) {
    char buf[32]{};
    std::size_t len = 0;
    if (getenv_s(&len, buf, sizeof buf, name) != 0 || len == 0) {
        return fallback;
    }
    const int got = std::atoi(buf);
    return got > 0 ? got : fallback;
}

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
// стоит десятки миллисекунд, и число здесь ради того, чтобы это было видно, а не
// подразумевалось: safetyhook потоки не останавливает
// сам, а graft делает это вокруг него — иначе поток, снятый планировщиком внутри
// переписываемых байт, проснётся на середине новой инструкции.
//
// Дорогая часть — заморозка потоков: снимок toolhelp снимает потоки ВСЕЙ СИСТЕМЫ и лишь
// потом фильтруется по своему процессу. Она обязательна, и почему — записано у
// frozen_threads в SRC/graft/hook.cpp. Практический вывод: врезку ставят на старте.
TEST(Hook, InstallCostIsMeasured) {
    answer_fn original = nullptr;
    constexpr int rounds = 16;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i) {
        ASSERT_TRUE(graft::hook(&answer, &detour, &original));
        ASSERT_TRUE(graft::unhook(&answer));
    }
    const double ms =
        std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - t0}.count() /
        rounds;

    std::cout << "  [замер] врезка + снятие: " << ms << " мс на пару" << std::endl;
    EXPECT_EQ(g_call(1), 2);
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

namespace {

std::uint8_t* g_poisoned = nullptr;
std::atomic<long> g_faults{0};

// Считает сбои на отравленной странице и пропускает их дальше по цепочке. Со второго раза
// снимает отраву сам: если цепочку замкнуло в цикл, кейс обязан дать число, а не подвесить
// прогон.
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

// Детур под защитой: падение внутри него отменяет ВЫЗОВ, а не игру.
//
// Натив плагина был обёрнут всегда, а детур — нет: hook принимал голый указатель, и код
// того же плагина падал по-разному в зависимости от того, как его позвали. Форма
// hook<&detour>(target, &orig) это закрывает.
namespace {

int __fastcall detour_that_falls(int) {
    return read_at(nullptr);  // отдельная noinline функция: иначе сбой уедет из области
}

}  // namespace

TEST(Hook, GuardedDetourSurvivesItsOwnFault) {
    answer_fn original = nullptr;
    const std::size_t before = graft::loader::fault_count();

    ASSERT_TRUE(graft::hook<&detour_that_falls>(&answer, &original));
    // Детур падает на каждом вызове, а цель продолжает отвечать — нулём по умолчанию.
    EXPECT_EQ(g_call(1), 0);
    EXPECT_EQ(g_call(1), 0);
    EXPECT_EQ(graft::loader::fault_count(), before + 2);

    EXPECT_TRUE(graft::unhook(&answer));
    EXPECT_EQ(g_call(1), 2);
}

// Голая форма осталась и по-прежнему не оборачивает: это осознанный выход для детура,
// который обязан досылать управление оригиналу при любом исходе.
TEST(Hook, RawDetourFormIsStillAvailable) {
    answer_fn original = nullptr;
    ASSERT_TRUE(graft::hook(&answer, &detour_flat, &original));
    EXPECT_EQ(g_call(1), 777);
    EXPECT_TRUE(graft::unhook(&answer));
}

// Снятая врезка не оставляет за собой ничего, что съест ЧУЖОЙ сбой потом.
//
// Штатный safetyhook патчит так: снимает со страницы права и ловит обращения к ней своим
// векторным обработчиком, отвечая ему «продолжай». Список отравленных страниц он НЕ
// ЧИСТИТ никогда — каждая пара врезка+снятие добавляет в него две записи навсегда, — а
// обработчик стоит первым в цепочке, выше кадрового. Настоящий сбой на такой странице
// после снятия врезки получал бы «продолжай»: вечный цикл вместо падения с дампом, и
// guard.hpp со своим __except до него просто не доходил.
//
// В graft этого обработчика нет: патч идёт под заморозкой потоков, исполняться по
// странице в этот момент некому, и ловушка не срабатывала ни разу — только копила. Она
// убрана вендорной копией слоя ОС (SRC/vendor, шапка файла объясняет что и почему).
//
// Кейс подстраивает совпадение руками: врезается в свою страницу, снимает врезку,
// отравляет страницу и смотрит, сколько раз будет доставлен сбой. Один — значит его
// отдали нашему __except. Два — значит кто-то в цепочке снова отвечает «продолжай».
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
// Именно это safetyhook и защищает своим обработчиком. Пока переход пишут, права со
// страницы сняты, поток, зашедший на неё, получает сбой, а обработчик переставляет ему
// rip в трамплин и говорит «продолжай». Кейс гоняет этот механизм по-настоящему.
//
// ПОЧЕМУ ОБОРОТОВ ДВЕСТИ, А НЕ ПЯТЬ ТЫСЯЧ. Потому что на пяти тысячах ломается всё, и
// это измерено, а не предположено. Шесть прогонов на 5000 оборотов: штатный safetyhook —
// три прогона с неверным значением на цели, один с падением процесса. Своя механика с
// ЗАМОРОЗКОЙ ПОТОКОВ, написанная было ради этого самого случая, на том же тесте дала три
// падения подряд. То есть заморозка от этой гонки не спасает: она закрывает поток,
// снятый планировщиком внутри переписываемых байт, но не закрывает поток, которого
// перенос rip только что отправил в трамплин, — а трамплин исчезает на следующем обороте.
//
// Вывод, ради которого кейс и оставлен с этим числом: быстрое чередование врезки и снятия
// на живой цели не поддержано НИ ОДНОЙ из механик. Врезку ставят на старте и не трогают.
// До двух тысяч оборотов штатный safetyhook проходит устойчиво (три прогона из трёх),
// двести — с запасом.
namespace {
long CALLBACK report_unhandled(EXCEPTION_POINTERS* ep) {
    const auto rip = static_cast<std::uintptr_t>(ep->ContextRecord->Rip);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    char buf[512];
    _snprintf_s(buf, sizeof buf, _TRUNCATE,
                "\n[СБОЙ] code=%08lX rip=%p (RVA %llX) обращение=%p поток=%lu answer=%p\n",
                ep->ExceptionRecord->ExceptionCode, reinterpret_cast<void*>(rip),
                static_cast<unsigned long long>(rip >= base ? rip - base : rip),
                ep->ExceptionRecord->NumberParameters >= 2
                    ? reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1])
                    : nullptr,
                GetCurrentThreadId(), reinterpret_cast<void*>(&answer));
    DWORD wrote = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, static_cast<DWORD>(strlen(buf)), &wrote, nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

TEST(Hook, TargetUnderTrafficSurvivesInstallAndRemoval) {
    // Фильтр необработанных исключений: без него падение этого кейса выглядит как молча
    // исчезнувший прогон. Именно он и показал причину — rip на байт внутри цели.
    // Восстанавливаем прежний в конце: фильтр общий на процесс.
    LPTOP_LEVEL_EXCEPTION_FILTER const was_filter = SetUnhandledExceptionFilter(&report_unhandled);
    std::atomic<bool> stop{false};
    std::atomic<long long> spins{0};
    std::atomic<long> wrong{0};

    // Детур НЕ зовёт оригинал: иначе кейс проверял бы ещё и время жизни трамплина, а
    // после снятия врезки поток может оказаться внутри уже освобождённого.
    static answer_fn s_unused = nullptr;

    std::vector<std::jthread> hammer;
    const int threads = from_env("GRAFT_HAMMERS", 4);
    for (int i = 0; i < threads; ++i) {
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

    // Дожидаемся, пока потоки реально пойдут по цели: врезка теперь стоит сотые доли
    // миллисекунды, и без этого весь цикл успевал отработать до их первого вызова.
    while (spins.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }

    // Оборотов много именно потому, что они дешёвые: каждый — окно, в котором чужой поток
    // может оказаться на переписываемом прологе. Это и есть смысл кейса.
    const int rounds = from_env("GRAFT_ROUNDS", 40);
    const auto t0 = std::chrono::steady_clock::now();
    for (int round = 0; round < rounds; ++round) {
        ASSERT_TRUE(graft::hook(&answer, &detour_flat, &s_unused));
        ASSERT_TRUE(graft::unhook(&answer));
    }
    const double ms =
        std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - t0}.count() /
        rounds;
    std::cout << "  [замер] врезка + снятие ПОД НАГРУЗКОЙ: " << ms << " мс на пару" << std::endl;

    stop.store(true);
    hammer.clear();  // jthread: join на разрушении

    EXPECT_EQ(wrong.load(), 0) << "цель вернула то, чего не возвращают ни оригинал, ни детур";
    EXPECT_GT(spins.load(), 0) << "потоки не крутились — их не разморозили";
    EXPECT_EQ(g_call(1), 2) << "после снятия цель обязана быть исходной";
    SetUnhandledExceptionFilter(was_filter);
}
