// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Этот файл линкуется в КАЖДЫЙ плагин, поэтому едет с исключением: мод на GRAFT ничего
// не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#include <windows.h>

#include "graft/guard.hpp"

// Разбор аппаратного сбоя. Отдельный файл ровно по одной причине: здесь нужен windows.h,
// а заголовок guard.hpp включает КАЖДЫЙ плагин — тащить туда весь Win32 ради одной
// структуры не стоит.
namespace graft::detail {

long fault_filter(void* impl, unsigned long code, void* info) {
    const void* at = nullptr;
    if (auto* pointers = static_cast<EXCEPTION_POINTERS*>(info); pointers && pointers->ExceptionRecord) {
        at = pointers->ExceptionRecord->ExceptionAddress;
    }
    note_fault(impl, static_cast<unsigned>(code), at, nullptr);
    // Порчу кучи движок забирает себе на первом же проходе (его VEH гонит 0xC0000374
    // прямо в верхний фильтр), поэтому сюда она не доходит и обрабатывать её нам нечем.
    // Всё остальное берём: вызов отменяется, игра живёт.
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace graft::detail
