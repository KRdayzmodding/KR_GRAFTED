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

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "graft/engine.hpp"

// Единственная копия механики врезки в процессе. Своя TU, а не кусок engine.cpp, по двум
// причинам: её линкует не только hid.dll, но и сьюта (иначе graft::hook проверялся бы
// только выездом на стенд), и в ней нет ни одного обращения к состоянию библиотеки —
// только к системе.
//
// Плагин зовёт эти же функции, но через таблицу сервисов: у него своей копии нет и быть
// не должно, см. комментарий у graft::hook в engine.hpp.
//
// Под капотом safetyhook как есть, без единой правки: разбор пролога у него на Zydis,
// знающем набор команд целиком, включая VEX и EVEX. Своего здесь ровно одно — реестр
// занятых целей, которого у safetyhook нет.
//
// Как он патчит и почему это важно знать за пределами этого файла: safetyhook снимает
// права со страницы, пишет переход и своим векторным обработчиком ловит тех, кто по
// странице исполняется, отвечая им «продолжай». То есть врезка ШТАТНО порождает в
// процессе поток обращений к недоступной памяти. Что из этого следует для обработки
// сбоев — записано в guard.hpp, и это не теория: обработчик, считавший такие сбои
// аномалией, вешал сьюту наглухо.
namespace graft {
namespace {

// Реестр целей. У safetyhook его нет вовсе: вторую врезку в ту же функцию он примет и
// построит цепочку, а снятие в неверном порядке порвёт её молча — первый вернёт на место
// исходные байты поверх чужого перехода, и трамплин второго поведёт в никуда. Обещание
// «одна механика на процесс, повторная врезка — честный отказ» держит именно этот
// словарь, а не библиотека под ним.
std::mutex g_lock;

// Реестры живут до конца процесса и НЕ разрушаются: статический деструктор снимал бы
// врезки на выходе, а снятие — это патч чужого кода, и делать его, когда остальные потоки
// ещё бегут и порядок разрушения статики уже не наш, незачем и небезопасно. Процесс и так
// уходит. Тот же приём, что у таблиц в objects.cpp.
std::map<void*, safetyhook::InlineHook>& hooks() {
    static auto* all = new std::map<void*, safetyhook::InlineHook>;
    return *all;
}

// Снятые врезки, которые НЕ разрушены. Причина не в аккуратности, а в том, как патч
// переставляет чужие потоки: поток, стоявший внутри переписываемых байт, патч переводит
// В ТРАМПЛИН и отпускает. Разрушить хук сразу после этого значит освободить память под
// ногами у того, кто в ней исполняется.
//
// Измерено: без отставки стресс-кейс (четыре потока на цели, врезку со снятием чередуют
// тысячами) даёт то неверное значение на цели, то падение процесса. С отставкой —
// чисто. Заморозка потоков, которую тут пробовали раньше, эту же гонку НЕ закрывала:
// она останавливает потоки на время патча, но отпускает их в трамплин ровно так же.
//
// ponytail: список не чистится — на каждое снятие остаётся трамплин, десятки байт.
// Снятие редко (врезку ставят на старте и не трогают), поэтому потолка хватает. Если
// однажды понадобится снимать врезки пачками по кругу, сюда просится освобождение по
// эпохам: отпускать отставленное, когда ни один поток не был замечен внутри.
std::vector<safetyhook::InlineHook>& retired() {
    static auto* all = new std::vector<safetyhook::InlineHook>;
    return *all;
}

// ── Поток, снятый планировщиком на прологе ───────────────────────────────────
// Единственная гонка, которую механика патча не закрывает и закрыть не может.
//
// safetyhook на время записи снимает со страницы права и ловит своим обработчиком тех,
// кто по ней ИСПОЛНЯЕТСЯ: им он переставляет rip в трамплин. Но поток, снятый
// планировщиком ВНУТРИ переписываемых байт, сбоя не получает — он просто спит. Права
// вернут, его разбудят, и он продолжит с середины уже другой инструкции.
//
// Так это и выглядит, поймано фильтром необработанных исключений на стресс-кейсе:
//
//     [СБОЙ] code=C000001D rip=...51F1 ... answer=...51F0
//
// C000001D — недопустимая инструкция, rip на один байт внутри цели. Ровно тот случай.
//
// Лечится одним способом: остановить потоки на время патча и переставить rip тем, кто
// попал внутрь. Здесь это делается ВОКРУГ safetyhook, а не вместо него: библиотека
// остаётся нетронутой, её собственная ловушка под заморозкой просто не срабатывает —
// исполняться по странице некому.
class frozen_threads {
public:
    frozen_threads() {
        // Собрать ДО первой остановки: снимок и вектор выделяют память, а под заморозкой
        // этого делать нельзя — кучный замок может оказаться у остановленного потока.
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            return;  // не перечислили — патчим без заморозки: это лучше, чем не патчить
        }
        const DWORD mine = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        THREADENTRY32 entry{};
        entry.dwSize = sizeof entry;
        if (Thread32First(snap, &entry) != FALSE) {
            do {
                if (entry.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD) ||
                    entry.th32OwnerProcessID != mine || entry.th32ThreadID == self) {
                    continue;
                }
                const HANDLE one = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                    entry.th32ThreadID);
                if (one != nullptr) {
                    held_.push_back(one);
                }
            } while (Thread32Next(snap, &entry) != FALSE);
        }
        CloseHandle(snap);
        for (const HANDLE one : held_) {
            SuspendThread(one);
        }
    }

    ~frozen_threads() {
        for (const HANDLE one : held_) {
            ResumeThread(one);
            CloseHandle(one);
        }
    }

    frozen_threads(const frozen_threads&) = delete;
    frozen_threads& operator=(const frozen_threads&) = delete;

    // Переставить rip тем, кто стоит внутри [from, from + len). Звать ПОСЛЕ записи и ДО
    // разморозки: снаружи потоки уже бегут, и правка контекста опоздает.
    void relocate(const void* from, const void* to, std::size_t len) const {
        const auto* const first = static_cast<const std::uint8_t*>(from);
        for (const HANDLE one : held_) {
            // CONTEXT на x64 обязан быть выровнен по 16 — иначе GetThreadContext откажет.
            alignas(16) CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(one, &ctx) == FALSE) {
                continue;
            }
            const auto* const rip = reinterpret_cast<const std::uint8_t*>(ctx.Rip);
            if (rip < first || rip >= first + len) {
                continue;
            }
            // Отображение побайтовое: первые len байт трамплина — копия пролога.
            //
            // ponytail: неверно ровно для одного случая — если в прологе был короткий
            // переход и трамплин растянул его до длинного, байты за ним съезжают. Точное
            // отображение требует таблицы границ инструкций; заводить её стоит тогда,
            // когда найдётся движковая цель с переходом в первых байтах.
            ctx.Rip = reinterpret_cast<DWORD64>(static_cast<const std::uint8_t*>(to) +
                                                (rip - first));
            SetThreadContext(one, &ctx);
        }
    }

private:
    std::vector<HANDLE> held_;
};

// Включить под заморозкой и подобрать тех, кто остался внутри пролога.
bool turn_on(safetyhook::InlineHook& one) {
    const frozen_threads frozen;
    if (!one.enable()) {
        return false;
    }
    frozen.relocate(one.target(), one.trampoline().data(), one.original_bytes().size());
    return true;
}

// Создать выключенным и сразу отдать адрес трамплина.
//
// Выключенным — потому что пачка обязана либо встать целиком, либо не встать вовсе, а
// создание, в отличие от включения, чужой пролог не трогает: откатывать после него
// нечего. Адрес сразу — потому что оригинал обязан быть достижим ДО того, как детур
// станет доступен чужому потоку, иначе первый же вызов уйдёт в ноль.
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
    if (hooks().contains(target)) {
        return false;
    }
    std::vector<safetyhook::InlineHook> made;
    if (!prepare({target, detour, original}, made)) {
        return false;
    }
    // Врезка и включение — одно действие: созданный, но выключенный хук наружу выглядит
    // как успех, которого не случилось.
    if (!turn_on(made.front())) {
        *original = nullptr;
        return false;
    }
    hooks().emplace(target, std::move(made.front()));
    return true;
}

bool unhook(void* target) {
    if (target == nullptr) {
        return false;
    }
    const std::scoped_lock held{g_lock};
    const auto found = hooks().find(target);
    if (found == hooks().end()) {
        return false;
    }
    // Выключение возвращает пролог на место — без него он остался бы пропатченным до
    // выгрузки процесса. А вот разрушать нельзя: см. retired().
    safetyhook::InlineHook going = std::move(found->second);
    hooks().erase(found);
    bool off = false;
    {
        // Снятие — тот же патч наоборот, и та же гонка: поток может стоять внутри
        // трамплина, куда его перевела врезка. Переставляем обратно в цель.
        const frozen_threads frozen;
        off = going.disable().has_value();
        if (off) {
            frozen.relocate(going.trampoline().data(), going.target(),
                            going.original_bytes().size());
        }
    }
    // В отставку в любом случае, даже когда выключить не вышло: разрушить сейчас значит
    // освободить трамплин, а в нём может стоять чужой поток. Выделение памяти здесь, а не
    // под заморозкой, — намеренно: кучный замок мог бы оказаться у остановленного потока.
    retired().push_back(std::move(going));
    return off;
}

bool hook_all(std::span<const hook_request> all) {
    if (all.empty()) {
        return true;  // ничего не просили — ничего и не сломалось
    }
    const std::scoped_lock held{g_lock};
    for (std::size_t i = 0; i < all.size(); ++i) {
        const hook_request& want = all[i];
        if (want.target == nullptr || want.detour == nullptr || want.original == nullptr ||
            hooks().contains(want.target)) {
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

    std::vector<safetyhook::InlineHook> made;
    made.reserve(all.size());
    const auto forget_originals = [&] {
        // Адреса трамплинов уже розданы, а трамплины сейчас исчезнут вместе с made.
        // Оставить их у вызывающего значит подложить ему указатель в освобождённую
        // память; ноль он хотя бы заметит на первом же вызове.
        for (const hook_request& want : all) {
            *want.original = nullptr;
        }
    };
    for (const hook_request& want : all) {
        if (!prepare(want, made)) {
            forget_originals();
            return false;  // не включено ещё ничего — откатывать нечего
        }
    }
    {
        // Одна заморозка на всю пачку: пока идёт откат, чужой поток не бежит и половину
        // пачки живой не застанет.
        const frozen_threads frozen;
        for (std::size_t i = 0; i < made.size(); ++i) {
            if (!made[i].enable()) {
                for (std::size_t j = 0; j < i; ++j) {
                    (void)made[j].disable();
                }
                forget_originals();
                return false;
            }
            frozen.relocate(made[i].target(), made[i].trampoline().data(),
                            made[i].original_bytes().size());
        }
    }

    for (std::size_t i = 0; i < all.size(); ++i) {
        hooks().emplace(all[i].target, std::move(made[i]));
    }
    return true;
}

}  // namespace graft
