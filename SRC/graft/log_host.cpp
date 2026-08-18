// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <fstream>
#include <string_view>

#include "graft/engine.hpp"

// Единственный канал наружу процесса игры: дописываемый текстовый файл рядом с exe.
// Отдельная TU без windows.h — её линкует и генератор объявлений, которому движок
// не нужен (там путь не задан и log() молчит).
namespace graft {
namespace {

std::string g_path;

}  // namespace

void set_log_path(std::string_view path) {
    g_path = path;
}

void log(std::string_view line) {
    if (g_path.empty()) {
        return;
    }
    std::ofstream out(g_path, std::ios::app);
    out << line << '\n';
}

}  // namespace graft
