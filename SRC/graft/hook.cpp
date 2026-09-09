// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// tlhelp32.h требует windows.h раньше себя — порядок здесь значащий.
#include <tlhelp32.h>

#include <safetyhook/inline_hook.hpp>
#include <safetyhook/os.hpp>

#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "graft/engine.hpp"
#include "graft/freeze.hpp"

// Единственная копия механики врезки в процессе. Своя TU, а не кусок engine.cpp, по двум
// причинам: её линкует не только hid.dll, но и сьюта (иначе graft::hook проверялся бы
// только выездом на стенд), и в ней нет ни одного обращения к состоянию библиотеки —
// только к системе.
//
// Плагин зовёт эти же функции, но через таблицу сервисов: у него своей копии нет и быть
// не должно, см. комментарий у graft::hook в engine.hpp.
//
// РАЗБОР ИНСТРУКЦИЙ — Zydis (внутри safetyhook). Он знает набор команд целиком, включая
// VEX и EVEX; HDE64 внутри MinHook, стоявший здесь раньше, на таком прологе отказывал.
// СЛОЙ ОС — свой, см. freeze.hpp: штатный у safetyhook держит в процессе векторный
// обработчик и никогда не чистит список отравленных страниц, а это ровно то, что
// отобрало бы у guard.hpp его обещание.
static_assert(sizeof(void*) == 8, "слой врезки правит Rip: рассчитан на x64");

// ── Заморозка потоков ────────────────────────────────────────────────────────
namespace graft::freeze {
namespace {

// Замок общий на весь патч: два потока не должны переписывать чужой код одновременно.
// Рекурсивный, потому что область вложенная — пачка открывает свою, а каждое включение
// внутри неё открывает ещё одну.
std::recursive_mutex g_lock;
int g_depth = 0;
std::vector<HANDLE> g_frozen;

// Собрать потоки процесса, КРОМЕ своего. Отдельным шагом до первой остановки: и снимок
// toolhelp, и вектор дескрипторов выделяют память, а под заморозкой этого делать нельзя —
// кучный замок может оказаться у остановленного потока, и мы встанем насмерть.
//
// Здесь же и вся цена врезки: снимок снимает потоки ВСЕЙ СИСТЕМЫ и лишь потом
// фильтруется по своему процессу. Отсюда десятки миллисекунд, отсюда же и пачка.
void collect() {
    g_frozen.clear();
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;  // не смогли перечислить — патчим без заморозки, это лучше, чем не патчить
    }
    const DWORD mine = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    if (Thread32First(snap, &entry) != FALSE) {
        do {
            // dwSize у записи свой: поле процесса читаем, только если оно вообще пришло.
            if (entry.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD) ||
                entry.th32OwnerProcessID != mine || entry.th32ThreadID == self) {
                continue;
            }
            const HANDLE thread =
                OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                           entry.th32ThreadID);
            if (thread != nullptr) {
                g_frozen.push_back(thread);
            }
        } while (Thread32Next(snap, &entry) != FALSE);
    }
    CloseHandle(snap);
}

}  // namespace

scope::scope() {
    g_lock.lock();
    if (g_depth++ > 0) {
        return;  // уже заморожены: вложенная область только держит замок
    }
    collect();
    for (const HANDLE thread : g_frozen) {
        SuspendThread(thread);
    }
}

scope::~scope() {
    if (--g_depth == 0) {
        for (const HANDLE thread : g_frozen) {
            ResumeThread(thread);
            CloseHandle(thread);
        }
        g_frozen.clear();
    }
    g_lock.unlock();
}

void scope::relocate(void* from, void* to, std::size_t len) const {
    auto* const first = static_cast<std::uint8_t*>(from);
    for (const HANDLE thread : g_frozen) {
        // CONTEXT на x64 обязан быть выровнен по 16 — иначе GetThreadContext откажет.
        alignas(16) CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(thread, &ctx) == FALSE) {
            continue;
        }
        auto* const rip = reinterpret_cast<std::uint8_t*>(ctx.Rip);
        if (rip < first || rip >= first + len) {
            continue;
        }
        // Отображение побайтовое: первые len байт трамплина — копия пролога, и байт i
        // там значит то же, что байт i здесь.
        //
        // ponytail: неверно ровно для одного случая — если в прологе был короткий переход
        // и трамплин растянул его до длинного, байты за ним съезжают. Точное отображение
        // требует таблицы границ инструкций (так делает MinHook); заводить её стоит
        // тогда, когда найдётся движковая цель с переходом в первых байтах.
        safetyhook::fix_ip(&ctx, rip, static_cast<std::uint8_t*>(to) + (rip - first));
        SetThreadContext(thread, &ctx);
    }
}

}  // namespace graft::freeze

// ── Врезка ───────────────────────────────────────────────────────────────────
namespace graft {
namespace {

// Реестр целей. У safetyhook его нет вовсе: вторую врезку в ту же функцию он примет и
// построит цепочку, а снятие в неверном порядке порвёт её молча — первый вернёт на место
// исходные байты поверх чужого перехода, и трамплин второго поведёт в никуда. Обещание
// «одна копия на процесс» держится именно этим словарём, а не библиотекой.
std::mutex g_lock;
std::map<void*, safetyhook::InlineHook> g_hooks;

// Создать выключенным и сразу отдать адрес трамплина. Выключенным — потому что включение
// это заморозка потоков, и в пачке она одна на всех; адрес сразу — потому что оригинал
// обязан быть достижим ДО того, как детур станет доступен чужому потоку.
bool prepare(const hook_request& want, std::vector<safetyhook::InlineHook>& into) {
    auto made = safetyhook::InlineHook::create(want.target, want.detour,
                                               safetyhook::InlineHook::StartDisabled);
    if (!made) {
        return false;
    }
    *want.original = made->original<void*>();
    into.push_back(std::move(*made));
    return true;
}

}  // namespace

bool hook(void* target, void* detour, void** original) {
    if (target == nullptr || detour == nullptr || original == nullptr) {
        return false;
    }
    const std::scoped_lock held{g_lock};
    if (g_hooks.contains(target)) {
        return false;
    }
    std::vector<safetyhook::InlineHook> made;
    if (!prepare({target, detour, original}, made)) {
        return false;
    }
    // Врезка и включение — одно действие: созданный, но выключенный хук наружу выглядит
    // как успех, которого не случилось.
    if (!made.front().enable()) {
        *original = nullptr;
        return false;
    }
    g_hooks.emplace(target, std::move(made.front()));
    return true;
}

bool unhook(void* target) {
    if (target == nullptr) {
        return false;
    }
    const std::scoped_lock held{g_lock};
    const auto found = g_hooks.find(target);
    if (found == g_hooks.end()) {
        return false;
    }
    // Разрушение возвращает пролог на место и освобождает трамплин: иначе пролог остался
    // бы пропатченным до выгрузки процесса.
    g_hooks.erase(found);
    return true;
}

bool hook_all(std::span<const hook_request> all) {
    if (all.empty()) {
        return true;  // ничего не просили — ничего и не сломалось
    }
    const std::scoped_lock held{g_lock};
    for (std::size_t i = 0; i < all.size(); ++i) {
        const hook_request& want = all[i];
        if (want.target == nullptr || want.detour == nullptr || want.original == nullptr ||
            g_hooks.contains(want.target)) {
            return false;
        }
        // Повтор ВНУТРИ пачки словарь не поймает — цели в нём ещё нет. Перебором: заявок
        // в пачке единицы, и это дешевле любого множества.
        for (std::size_t j = 0; j < i; ++j) {
            if (all[j].target == want.target) {
                return false;
            }
        }
    }

    // Создание пролог не трогает и потоки не морозит: только разбирает инструкции и
    // строит трамплин. Морозит включение — поэтому создаём все, включаем одной очередью.
    std::vector<safetyhook::InlineHook> made;
    made.reserve(all.size());
    for (const hook_request& want : all) {
        if (!prepare(want, made)) {
            return false;  // не включено ещё ничего — откатывать нечего
        }
    }

    {
        // Вот здесь — одна остановка потоков на всю пачку. Она же держит обещание «либо
        // целиком, либо никак» буквально: пока идёт откат, чужой поток не бежит и не
        // может застать половину пачки живой.
        const freeze::scope frozen;
        for (std::size_t i = 0; i < made.size(); ++i) {
            if (!made[i].enable()) {
                for (std::size_t j = 0; j < i; ++j) {
                    (void)made[j].disable();
                }
                for (const hook_request& want : all) {
                    *want.original = nullptr;
                }
                return false;
            }
        }
    }

    for (std::size_t i = 0; i < all.size(); ++i) {
        g_hooks.emplace(all[i].target, std::move(made[i]));
    }
    return true;
}

}  // namespace graft
