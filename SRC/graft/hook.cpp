// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <MinHook.h>

#include "graft/engine.hpp"

// Единственная копия MinHook в процессе. Своя TU, а не кусок engine.cpp, по двум
// причинам: её линкует не только hid.dll, но и сьюта (иначе graft::hook проверялся бы
// только выездом на стенд), и в ней нет ни одного обращения к состоянию библиотеки —
// только к системе.
//
// Плагин зовёт эти же две функции, но через таблицу сервисов: у него своей копии нет и
// быть не должно, см. комментарий у graft::hook в engine.hpp.
namespace graft {
namespace {

// Идемпотентно: MinHook отвечает ALREADY_INITIALIZED, и это не ошибка. Свой флаг здесь
// был бы враньём — состояние держит сама библиотека, а не мы.
bool ready() {
    const MH_STATUS status = MH_Initialize();
    return status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
}

}  // namespace

bool hook(void* target, void* detour, void** original) {
    if (!target || !detour || !original || !ready()) {
        return false;
    }
    // Врезка и включение — одно действие: созданный, но выключенный хук наружу выглядит
    // как успех, которого не случилось.
    if (MH_CreateHook(target, detour, original) != MH_OK) {
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        return false;
    }
    return true;
}

bool unhook(void* target) {
    if (!target || !ready()) {
        return false;
    }
    // Выключить и убрать: иначе пролог остаётся пропатченным до выгрузки процесса.
    MH_DisableHook(target);
    return MH_RemoveHook(target) == MH_OK;
}

bool hook_all(std::span<const hook_request> all) {
    if (all.empty()) {
        return true;  // ничего не просили — ничего и не сломалось
    }
    if (!ready()) {
        return false;
    }
    // Создание пролог не трогает и потоки не морозит — морозит включение. Поэтому
    // сначала создаём все, а включаем одной очередью.
    const auto undo = [&](std::size_t upto) {
        for (std::size_t i = 0; i < upto; ++i) {
            MH_RemoveHook(all[i].target);
        }
    };
    for (std::size_t i = 0; i < all.size(); ++i) {
        const hook_request& r = all[i];
        if (!r.target || !r.detour || !r.original ||
            MH_CreateHook(r.target, r.detour, r.original) != MH_OK) {
            undo(i);
            return false;
        }
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (MH_QueueEnableHook(all[i].target) != MH_OK) {
            undo(all.size());
            return false;
        }
    }
    // Вот здесь — одна остановка потоков на всю пачку.
    if (MH_ApplyQueued() != MH_OK) {
        undo(all.size());
        return false;
    }
    return true;
}

}  // namespace graft
