#include <windows.h>

#include <cstring>
#include <format>

#include "graft/engine.hpp"
#include "graft/frame.hpp"
#include "graft/guard.hpp"
#include "graft/loader.hpp"
#include "graft/script.hpp"

// Кадр БЕЗ строки в скрипте.
//
// ЗАЧЕМ. Раньше C++ просыпался оттого, что мод звал GraftTick из своего OnUpdate. Это
// работает и стоит одну строку, но строка эта в ЧУЖОМ моде, и забыть её нельзя ничем,
// кроме документации.
//
// К ЧЕМУ ПРИЦЕПЛЕНО. К движку, а не к ванильным скриптам. Раз в кадр движок зовёт
// скриптовый `DayZGame.OnUpdate(bool doSim, float timeslice)` через общую пару «собрать
// кадр вызова по индексу метода» + «исполнить». Как ищется это место — scan.hpp.
//
// ПЕРЕНАПРАВЛЯЕТСЯ МЕСТО ВЫЗОВА, А НЕ ФУНКЦИЯ, и это не стилистика. Сама функция общая:
// её зовут из сотен мест, и у каждого свои аргументы — где-то вещественное число едет в
// xmm2, где-то нет. Детур на C++ поверх такой функции затирает xmm (компилятор считает их
// своими), и ЧУЖОЙ вызов уезжает в движок с испорченным аргументом. Отсюда падения на
// подключении игрока: пустой сервер зовёт скрипт редко, а живой — постоянно и с
// вещественными аргументами. Отчёты о падении это и показывали: чтение по 0 и по -1 в
// движке, четырьмя кадрами выше нашего.
//
// Патчим `call` в самой CGame::Update. Тогда к нам приходит ровно то, что кладёт это
// место, и только оно: ни чужих вызывающих, ни совпадений индексов, ни игры с регистрами.
// rel32 дотягивается на ±2 ГБ, а образ игры и наша библиотека разъезжаются на сотни (по
// отчётам о падении — 546 ГБ), поэтому рядом с местом вызова кладётся заплатка с
// абсолютным переходом.
//
// Отсюда достаётся всё сразу: сам кадр, скриптовый поток, объект CGame первым аргументом
// (тот самый корень) и настоящий timeslice, а не наши часы.
//
// ЧТО НЕ СРАБОТАЛО, чтобы не пробовали заново:
//   MsgWaitForMultipleObjects — в NO_GUI-сервере не зовётся ВООБЩЕ (замер: 0 вызовов);
//   Sleep                     — бьётся на ЧУЖОМ потоке;
//   GetGame через RegisterGlobal — движок не вносит эту глобаль регистрацией;
//   подмена ванильного CGame.IsServer — работала (1.3–2.6 вызова на кадр), но держалась
//     на том, что ванильный MissionBase.OnUpdate спрашивает «я сервер?», плюс тянула за
//     собой EnProfiler.GetGameFrame для отсечки кадров и приватное поле m_DeltaTime ради
//     dt. Три зависимости от чужих скриптов вместо одной от движка.
namespace graft::frame {
namespace {

scan::frame_entry g_entry;
scan::frame_entry::prepare_fn g_orig_prepare = nullptr;

// Класс игры: последняя проверка на случай, если скан нашёл не то место. Тогда тика
// просто не будет — вместо падения на чужих аргументах.
void* game_class() {
    static void* cached = nullptr;
    if (!cached) {
        cached = script::find_class("DayZGame");
        if (!cached) {
            cached = script::find_class("CGame");
        }
    }
    return cached;
}

// Сюда ведёт пропатченный `call`. Аргументы — ровно те, что кладёт CGame::Update.
void __fastcall hook_prepare(void* self, void* frame, std::uint32_t index, std::uint32_t sim,
                             double dt) {
    // Наше — ДО того, как движок начнёт собирать кадр вызова: пока он ничего не строил,
    // портить нечего.
    if (self && script::class_of(self) == game_class() && game_class()) {
        detail::note_frame(static_cast<float>(dt));
        // Обработчики — чужой код плагинов, а мы стоим внутри движковой функции: падение
        // здесь уронило бы игру на её собственном коде.
        ::graft::detail::guarded<void>(reinterpret_cast<void*>(&hook_prepare), [&] {
            loader::set_script_root(self);
            loader::run_ticks(static_cast<float>(dt));
        });
    } else {
        detail::note_reject(true);
    }
    g_orig_prepare(self, frame, index, sim, dt);
}

// Заплатка с абсолютным переходом, размещённая в пределах ±2 ГБ от места вызова: только
// так rel32 до неё дотягивается. Гранулярность резервирования у Windows своя, поэтому
// шагаем ею и в обе стороны — свободная страница рядом находится всегда.
void* near_stub(std::uintptr_t near_to, void* target) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t step = si.dwAllocationGranularity;
    const std::uintptr_t base = near_to & ~(static_cast<std::uintptr_t>(step) - 1);
    for (std::uintptr_t away = step; away < 0x40000000ull; away += step) {
        for (int dir = 0; dir < 2; ++dir) {
            const std::uintptr_t at = dir ? base + away : base - away;
            void* page = VirtualAlloc(reinterpret_cast<void*>(at), step,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!page) {
                continue;
            }
            // jmp qword ptr [rip+0]; <адрес>
            std::uint8_t code[14] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
            std::memcpy(code + 6, &target, sizeof target);
            std::memcpy(page, code, sizeof code);
            FlushInstructionCache(GetCurrentProcess(), page, sizeof code);
            return page;
        }
    }
    return nullptr;
}

// Переписать смещение в `call rel32`. Пишем одним 32-битным словом: инструкция уже
// исполняется другими потоками, и рвать её на части нельзя.
bool retarget_call(std::uintptr_t site, void* to) {
    void* stub = near_stub(site, to);
    if (!stub) {
        return false;
    }
    const std::intptr_t rel = static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(stub)) -
                              static_cast<std::intptr_t>(site + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) {
        return false;
    }
    auto* at = reinterpret_cast<std::int32_t*>(site + 1);
    DWORD was = 0;
    if (!VirtualProtect(at, sizeof *at, PAGE_EXECUTE_READWRITE, &was)) {
        return false;
    }
    *at = static_cast<std::int32_t>(rel);
    VirtualProtect(at, sizeof *at, was, &was);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
    return true;
}

}  // namespace

// Звать после того, как образ разобран сканом.
void install(const std::vector<scan::view>& sections) {
    g_entry = scan::find_frame_entry(sections);
    if (!g_entry) {
        detail::note_state("точка входа кадра не найдена");
        log("! привязка к кадру: точка входа не найдена — тика не будет, "
            "зовите GraftTick(dt, GetGame()) из мода");
        return;
    }
    g_orig_prepare = g_entry.prepare;
    const bool ok = retarget_call(g_entry.site, reinterpret_cast<void*>(&hook_prepare));
    detail::note_state(ok ? "движковая точка входа кадра" : "перенаправление не удалось");
    const auto rva = [](std::uintptr_t p) {
        return p - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    };
    log(std::format("привязка к кадру: {} место_вызова={:#x} кэш_индекса={:#x}",
                    ok ? "встала" : "НЕ ВСТАЛА", rva(g_entry.site),
                    rva(reinterpret_cast<std::uintptr_t>(g_entry.index))));
}

}  // namespace graft::frame
