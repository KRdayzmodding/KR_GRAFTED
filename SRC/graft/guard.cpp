// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Этот файл линкуется в КАЖДЫЙ плагин, поэтому едет с исключением: мод на GRAFT ничего
// не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#include <windows.h>

#include <malloc.h>  // _resetstkoflw

#include <utility>

#include "graft/guard.hpp"

// Разбор аппаратного сбоя. Отдельный файл ровно по одной причине: здесь нужен windows.h,
// а заголовок guard.hpp включает КАЖДЫЙ плагин — тащить туда весь Win32 ради одной
// структуры не стоит.
namespace graft::detail {
namespace {

// Порча кучи. В windows.h константы нет — она из ntstatus.h, который тянуть сюда незачем.
constexpr unsigned long kHeapCorruption = 0xC0000374ul;

// Адрес переполнения стека, отложенный фильтром до обработчика. thread_local: два потока
// могут переполнить стек одновременно, и один не должен забрать отчёт другого.
const void*& pending_overflow() {
    thread_local const void* at = nullptr;
    return at;
}

}  // namespace

long fault_filter(void* impl, unsigned long code, void* info) {
    const void* at = nullptr;
    if (auto* pointers = static_cast<EXCEPTION_POINTERS*>(info); pointers && pointers->ExceptionRecord) {
        at = pointers->ExceptionRecord->ExceptionAddress;
    }

    // Порча кучи — не наша, и говорим об этом прямо. Продолжать после неё нельзя: таблицы
    // аллокатора уже испорчены, и следующий new или free упадёт в другом месте, где
    // виноватого не найти. Движок гонит её в верхний фильтр на первом же проходе, но
    // обещание «мы её забираем» было бы враньём независимо от того, доходит она сюда или
    // нет.
    if (code == kHeapCorruption) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Переполнение стека: фильтр работает на том же стеке, который только что кончился.
    // Форматировать строку и писать в журнал здесь — доломать остаток и получить сбой
    // внутри обработки сбоя. Запоминаем адрес и уходим; отчёт напишет after_fault.
    if (code == EXCEPTION_STACK_OVERFLOW) {
        pending_overflow() = at;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    note_fault(impl, static_cast<unsigned>(code), at, nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void after_fault(void* impl, std::size_t depth_was) {
    restore_call_depth(depth_was);

    const void* const overflowed = std::exchange(pending_overflow(), nullptr);
    if (overflowed == nullptr) {
        return;
    }
    // Стек уже раскручен — здесь и только здесь это законно. Без вызова страница-сторож
    // не возвращается, и ВТОРОЕ переполнение убьёт процесс, сколько бы __except вокруг ни
    // стояло: первое ловится, а второго обработчику уже не на чем встретить.
    _resetstkoflw();
    note_fault(impl, EXCEPTION_STACK_OVERFLOW, overflowed, nullptr);
}

}  // namespace graft::detail
