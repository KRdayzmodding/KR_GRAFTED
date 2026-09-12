// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once

// КТО ПИШЕТ В ЭТУ ПАМЯТЬ. Аппаратная точка останова на запись — и обратные адреса того,
// кто записал.
//
// ЭТО ИНСТРУМЕНТ РАЗРАБОТКИ, А НЕ ЧАСТЬ БИБЛИОТЕКИ. Заголовок целиком inline, `native.hpp`
// его не тянет, ни одна цель сборки его не компилирует: в `hid.dll` он не попадает ни
// байтом. Включается в плагин руками на время разбора и убирается, когда ответ получен.
// Снаружи разбора взведённые DR0..DR3 не нужны никому: на каждую запись по адресу
// процессор поднимает исключение, и это стоит кадру.
//
// ЗАЧЕМ ИМЕННО ТАК. Вопрос «кто затирает наше поле» по-другому не закрывается. Врезка
// отвечает только там, где уже знаешь функцию; поллинг из тика говорит «стало другим», но
// не говорит кем, и между двумя опросами теряет всё. Процессор ловит запись сам, в момент
// записи, без цены для остальной памяти — ценой ровно четырёх адресов (их столько у
// железа) и одного исключения на срабатывание.
//
// Обработчик ничего не печатает и не выделяет: кладёт в кольцо адрес, новое значение и
// пять кадров вызова. Печатает уже кадр игры — `drain()`.
//
// ПЯТЬ ПРАВИЛ, И КАЖДОЕ СТОИЛО ПРОГОНА:
//
//   1. Регистры ставит ЧУЖОЙ поток. Свой себе их не ставит: `SetThreadContext` на
//      собственный поток молча не доезжает. Отсюда поток-помощник внутри `arm()`.
//   2. Адрес обязан быть ВЫРОВНЕН по длине (8 байт — на границе восьми). Невыровненная
//      точка не срабатывает вовсе и молчит об этом, поэтому `arm()` такой отказывает вслух.
//   3. Точка на запись — ЛОВУШКА, а не сбой: она поднимается ПОСЛЕ записи. Значит в кольце
//      лежит уже новое значение, и это то, что нужно.
//   4. Обработчик обязан выходить на ПЕРВОЙ строке по коду исключения. Он встанет выше
//      кадрового и увидит каждое исключение процесса, а обращение к недоступной памяти
//      здесь событие штатное — так работает окно врезки (`graft/guard.hpp`, там же почему).
//   5. Печатать надо ЦЕПОЧКАМИ со счётчиком, а не срабатываниями. Движок пишет в свои поля
//      каждый кадр; журнал с одной строкой на запись тонет за секунду.
//
// КАК ПОЛЬЗОВАТЬСЯ. Взводить — с того самого потока, за которым смотрим (обычно скриптовый:
// тик и трамплин натива идут по нему), сливать — оттуда же:
//
//   graft::watch::arm({{&result->count, "count", 4}, {&obj->dest, "dest", 8}});
//   ...
//   graft::watch::drain("после команды");   // каждый кадр или по событию
//   graft::watch::report();                 // итог: кто сколько раз
//   graft::watch::disarm();
//
// Свои записи от чужих отделяются счётчиком: `graft::watch::own mark;` на время своего
// вызова — и в журнале он будет «СВОЯ запись».
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>

#include "graft/engine.hpp"

namespace graft::watch {

// За чем смотреть. bytes — 1, 2, 4 или 8, и адрес должен быть кратен им.
struct spot {
    void* at = nullptr;
    const char* name = "?";
    std::uint8_t bytes = 8;
};

constexpr int k_spots = 4;   // столько точек у железа, и больше не будет
constexpr int k_frames = 5;  // столько кадров снимается с места записи
constexpr int k_ring = 128;
constexpr int k_chains = 64;

// Свой вызов или чужой. Счётчик, а не флаг: свои вызовы вкладываются друг в друга.
inline thread_local int ours = 0;

struct own {
    own() { ++ours; }
    ~own() { --ours; }
    own(const own&) = delete;
    own& operator=(const own&) = delete;
};

struct hit {
    std::uintptr_t frames[k_frames];
    std::uint64_t value;  // что лежит по адресу СРАЗУ ПОСЛЕ записи
    int which;
    bool ours;
};

inline spot spots[k_spots]{};
inline DWORD watched = 0;
inline PVOID veh = nullptr;
inline hit ring[k_ring]{};
inline std::atomic<unsigned> head{0};
inline unsigned tail = 0;

inline LONG CALLBACK on_exception(EXCEPTION_POINTERS* e) {
    // Правило 4: выход по коду исключения первой же строкой.
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* c = e->ContextRecord;
    const DWORD64 dr6 = c->Dr6;
    if (!(dr6 & 0xF)) {  // пошаговый режим чужого отладчика — не наше дело
        return EXCEPTION_CONTINUE_SEARCH;
    }
    c->Dr6 = 0;
    hit h{};
    h.which = (dr6 & 0x1) ? 0 : (dr6 & 0x2) ? 1 : (dr6 & 0x4) ? 2 : 3;
    h.ours = ours != 0;
    if (spots[h.which].at) {
        // Ровно столько, сколько взведено: лишние байты могли бы лежать за страницей.
        std::memcpy(&h.value, spots[h.which].at, spots[h.which].bytes);
    }
    // Раскрутка по таблицам .pdata: стек целый, исключения ещё нет — читается как обычный
    // кадр. Обрыв на первой функции без записи раскрутки (лист без пролога, чужой трамплин).
    CONTEXT walk = *c;
    for (int f = 0; f < k_frames; ++f) {
        h.frames[f] = walk.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(walk.Rip, &image, nullptr);
        if (!fe) {
            break;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, walk.Rip, fe, &walk, &handler_data, &frame,
                         nullptr);
    }
    ring[head.fetch_add(1) % k_ring] = h;
    return EXCEPTION_CONTINUE_EXECUTION;
}

namespace detail {

// LEN в DR7: 00 = 1 байт, 01 = 2, 11 = 4, 10 = 8. Порядок не по возрастанию — это таблица
// из SDM, а не битовое поле.
inline std::uint64_t len_bits(std::uint8_t bytes) {
    return bytes == 1 ? 0b00 : bytes == 2 ? 0b01 : bytes == 4 ? 0b11 : 0b10;
}

// Правило 1: регистры ставит поток-помощник. Выглядит как тупик — взводящий поток сам себя
// и усыпляет, — но тупика нет: он ждёт в `join()`, помощник его будит и выходит.
inline bool set_registers(DWORD target, void* const a[k_spots], std::uint64_t dr7) {
    bool ok = false;
    std::thread worker{[&] {
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                              FALSE, target);
        if (!t) {
            return;
        }
        SuspendThread(t);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = reinterpret_cast<DWORD64>(a[0]);
            c.Dr1 = reinterpret_cast<DWORD64>(a[1]);
            c.Dr2 = reinterpret_cast<DWORD64>(a[2]);
            c.Dr3 = reinterpret_cast<DWORD64>(a[3]);
            c.Dr6 = 0;
            c.Dr7 = dr7;
            ok = SetThreadContext(t, &c) != 0;
        }
        ResumeThread(t);
        CloseHandle(t);
    }};
    worker.join();
    return ok;
}

}  // namespace detail

// Взвести. Звать С ТОГО ПОТОКА, за которым смотрим: регистры у каждого потока свои.
// Отказ — не повод продолжать: не взведено ничего, и в журнале написано почему.
inline bool arm(std::initializer_list<spot> all) {
    if (all.size() > k_spots) {
        print(std::format("[watch] точек у железа {}, просят {}", k_spots, all.size()));
        return false;
    }
    // Проверяем ВСЁ до первой записи в регистры: половина взведённых точек — это ответ,
    // которому нельзя верить, а выглядит он как полный.
    for (const spot& s : all) {
        const auto at = reinterpret_cast<std::uintptr_t>(s.at);
        if (!at) {
            print(std::format("[watch] {}: адрес нулевой", s.name));
            return false;
        }
        if (s.bytes != 1 && s.bytes != 2 && s.bytes != 4 && s.bytes != 8) {
            print(std::format("[watch] {}: длина {} — бывает 1, 2, 4 или 8", s.name, s.bytes));
            return false;
        }
        if (at % s.bytes) {  // правило 2: невыровненная точка молча не срабатывает
            print(std::format("[watch] {}: адрес {:#x} не кратен {}", s.name, at, s.bytes));
            return false;
        }
    }

    void* addrs[k_spots]{};
    std::uint64_t dr7 = 0;
    int i = 0;
    for (const spot& s : all) {
        spots[i] = s;
        addrs[i] = s.at;
        dr7 |= 1ull << (i * 2);         // локальная точка i
        dr7 |= 0x1ull << (16 + i * 4);  // RW = только запись
        dr7 |= detail::len_bits(s.bytes) << (18 + i * 4);
        ++i;
    }
    for (; i < k_spots; ++i) {
        spots[i] = spot{};
    }

    if (!veh) {
        // ponytail: обработчик снимается вместе с процессом. Вне срабатываний он выходит
        // первой строкой по коду исключения, а снимать его в момент, когда ловушка ещё в
        // полёте, дороже, чем оставить.
        veh = AddVectoredExceptionHandler(1, &on_exception);
    }
    watched = GetCurrentThreadId();
    if (!detail::set_registers(watched, addrs, dr7)) {
        print("[watch] регистры не встали");
        watched = 0;
        return false;
    }
    std::string line = "[watch] смотрю за записью:";
    for (const spot& s : all) {
        line += std::format(" {} {} ({}б)", s.name, s.at, s.bytes);
    }
    print(line);
    return true;
}

inline void disarm() {
    if (!watched) {
        return;
    }
    void* none[k_spots]{};
    detail::set_registers(watched, none, 0);
    watched = 0;
    for (spot& s : spots) {
        s = spot{};
    }
}

// Где это было. Адрес игры — смещением от её базы: так он ложится прямо в IDA. Чужой (наш
// плагин, системная DLL) отмечается, чтобы не искать его в бинаре игры зря.
inline std::string where(std::uintptr_t rip) {
    if (!rip) {
        return "-";
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    DWORD64 image = 0;
    const RUNTIME_FUNCTION* fe = RtlLookupFunctionEntry(rip, &image, nullptr);
    // Начало функции — через сцепленную запись раскрутки (UNW_FLAG_CHAININFO): у куска,
    // вынесенного компилятором, своя запись, и без этого шага «функцией» оказался бы он.
    while (fe) {
        const auto* ui = reinterpret_cast<const std::uint8_t*>(image + fe->UnwindData);
        if (!(ui[0] >> 3 & 0x4)) {
            break;
        }
        fe = reinterpret_cast<const RUNTIME_FUNCTION*>(ui + 4 + ((ui[2] + 1) & ~1) * 2);
    }
    if (image != base) {
        return std::format("{:#x}(не игра)", rip);
    }
    return std::format("{:#x}/f{:#x}", rip - base, fe ? fe->BeginAddress : 0);
}

// Правило 5: по строке на ЦЕПОЧКУ, остальное — счётчиком к ней.
struct chain {
    std::uintptr_t at = 0;
    std::uintptr_t from = 0;
    int which = 0;
    bool ours = false;
    int count = 0;
};

inline chain chains[k_chains]{};
inline int chains_n = 0;

// Разобрать накопленное. Печатает только НОВЫЕ цепочки; звать можно хоть каждый кадр.
inline void drain(std::string_view context = {}) {
    const unsigned now = head.load();
    for (; tail != now; ++tail) {
        const hit& h = ring[tail % k_ring];
        chain* found = nullptr;
        for (int i = 0; i < chains_n; ++i) {
            if (chains[i].at == h.frames[0] && chains[i].from == h.frames[1] &&
                chains[i].which == h.which) {
                found = &chains[i];
                break;
            }
        }
        if (found) {
            ++found->count;
            continue;
        }
        if (chains_n < k_chains) {
            chains[chains_n] = {h.frames[0], h.frames[1], h.which, h.ours, 1};
            ++chains_n;
        }
        std::string frames;
        for (const std::uintptr_t f : h.frames) {
            frames += (frames.empty() ? "" : " <- ") + where(f);
        }
        print(std::format("[watch] {} {} -> {:#x} | {} | {}",
                          h.ours ? "СВОЯ запись" : "ЧУЖАЯ ЗАПИСЬ", spots[h.which].name, h.value,
                          frames, context));
    }
}

// Итог: сколько раз каждая цепочка тронула память. Счётчики обнуляются — следующий отрезок
// считается заново.
inline void report() {
    for (int i = 0; i < chains_n; ++i) {
        print(std::format("[watch] итог: {} {} — {} раз | {}", chains[i].ours ? "своя" : "ЧУЖАЯ",
                          spots[chains[i].which].name, chains[i].count, where(chains[i].at)));
    }
    chains_n = 0;
}

}  // namespace graft::watch
