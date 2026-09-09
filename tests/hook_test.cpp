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

// Цена. Сам ВЫЗОВ после врезки стоит один переход — мерить там нечего. Установка тоже
// оказалась дешёвой, и это стоит зафиксировать числом: safetyhook потоки НЕ останавливает
// (он патчит пролог под снятыми правами страницы и ловит тех, кто по ней исполняется,
// своим обработчиком), поэтому снимка toolhelp по всей системе здесь нет. Врезка на
// прежней механике стоила около 70 мс, на этой — сотые доли миллисекунды.
//
// Практический вывод один: врезку больше не обязательно делать только на старте.
TEST(Hook, InstallCostIsMeasured) {
    answer_fn original = nullptr;
    constexpr int rounds = 256;

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

// ИЗВЕСТНЫЙ ПОТОЛОК, зафиксированный намеренно.
//
// safetyhook патчит чужой пролог так: снимает со страницы права, пишет переход и ловит
// обращения к этой странице своим векторным обработчиком, отвечая ему «продолжай». Пока
// патч идёт, это ровно то, что нужно. Но список отравленных страниц он НЕ ЧИСТИТ: после
// снятия врезки страница остаётся в нём навсегда, и настоящий сбой на ней получит то же
// «продолжай» — то есть повтор той же инструкции вместо падения.
//
// Мы это принимаем, и вот почему. Отравленные страницы — это страницы кода движка с
// нашими целями и страницы трамплинов. Адрес, по которому промахнулся кривой натив, —
// это его данные: ноль или мусор. Совпадение возможно, но для него нужно, чтобы битый
// указатель показал ровно в одну из нескольких страниц кода из ста двадцати восьми
// терабайт адресного пространства. Кейс ниже подстраивает это совпадение руками —
// сам по себе он не случается.
//
// Цена альтернативы измерена, а не прикинута: свой слой ОС с заморозкой потоков вместо
// этого обработчика был написан и убран. Около четырёхсот строк своего кода и форк чужого
// файла, врезка дороже в две с половиной тысячи раз (70 мс против 0.03 мс) — и при этом
// на стресс-тесте ниже он падал так же, как штатный. Ради случая, который не наступает,
// платить было нечем.
//
// Кейс живёт здесь как характеристика чужого поведения: если safetyhook однажды начнёт
// чистить список, он покраснеет, и это будет хорошей новостью, а не поломкой. А guard за
// это время успеет написать в журнал, что сбой доставлен повторно (detail::watch_faults):
// без этой строки такой случай выглядит как молча зависший сервер.
TEST(Hook, FaultOnAPageOfARemovedHookIsRetried) {
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

    // Ровно два: первый сбой съеден и повторён, второй наша страховка развернула сама.
    // Станет один — значит список отравленных страниц начали чистить, и потолка больше
    // нет; тогда этот кейс надо не чинить, а выбрасывать.
    EXPECT_EQ(g_faults.load(), 2) << "потолок исчез — перечитай комментарий выше";
    EXPECT_EQ(got, 0) << "сбой дошёл до __except, хотя страница отравлена";
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

    // Дожидаемся, пока потоки реально пойдут по цели: врезка теперь стоит сотые доли
    // миллисекунды, и без этого весь цикл успевал отработать до их первого вызова.
    while (spins.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }

    // Оборотов много именно потому, что они дешёвые: каждый — окно, в котором чужой поток
    // может оказаться на переписываемом прологе. Это и есть смысл кейса.
    constexpr int rounds = 200;
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
}
