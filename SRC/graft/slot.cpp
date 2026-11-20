// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Этот файл линкуется в КАЖДЫЙ плагин, поэтому едет с исключением: мод на GRAFT ничего
// не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#include <windows.h>

#include <atomic>
#include <thread>

#include "graft/events.hpp"

// Медленная половина снятия подписки (graft/events.hpp, call_slot — быстрая). Асимметричный барьер: поток
// скрипта отмечает вызов обычной записью, а порядок «снят → вызова нет» держит FlushProcessWriteBuffers —
// он проходит по всем ядрам, где идут потоки процесса. Так цена ложится на редкое снятие с чужого потока,
// а не на каждый вызов обработчика.
namespace graft::detail {

void wait_idle(slot& s) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    FlushProcessWriteBuffers();
    // Обработчики короткие: сначала уступить ядро, потом — спать. Ждать некого — ни одного круга.
    for (int round = 0; s.busy.load(std::memory_order_acquire) != 0; ++round) {
        if (round < 64) {
            std::this_thread::yield();
        } else {
            Sleep(1);
        }
    }
}

} // namespace graft::detail
