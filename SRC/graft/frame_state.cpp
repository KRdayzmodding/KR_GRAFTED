// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Этот файл линкуется в КАЖДЫЙ плагин, поэтому едет с исключением: мод на GRAFT ничего
// не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#include <atomic>

#include "graft/frame.hpp"

// Счётчики привязки к кадру — отдельно от самого хука, по той же причине, что и
// loader_state.cpp: класс с нативами Graft* компилируется и в хост, и в graft.exe, а
// инструменту ни врезки, ни движка не нужно и быть не может. Так у него те же символы,
// но пустые, а не отсутствующие.
namespace graft::frame {
namespace {

std::atomic<std::size_t> g_frames{0};
std::atomic<float> g_last_dt{0.0f};
std::string_view         g_state = "not installed";
std::atomic<std::size_t> g_rejected_class{0};
std::atomic<std::size_t> g_rejected_unresolved{0};

}  // namespace

namespace detail {

void note_frame(float dt) {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_last_dt.store(dt, std::memory_order_relaxed);
}

void note_reject(bool wrong_class) {
    (wrong_class ? g_rejected_class : g_rejected_unresolved)
        .fetch_add(1, std::memory_order_relaxed);
}

void note_state(std::string_view what) {
    g_state = what;
}

}  // namespace detail

std::size_t frames() {
    return g_frames.load(std::memory_order_relaxed);
}

float last_dt() {
    return g_last_dt.load(std::memory_order_relaxed);
}

std::size_t rejected_class() {
    return g_rejected_class.load(std::memory_order_relaxed);
}

std::size_t rejected_unresolved() {
    return g_rejected_unresolved.load(std::memory_order_relaxed);
}

std::string_view state() {
    return g_state;
}

}  // namespace graft::frame
