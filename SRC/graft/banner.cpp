// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <format>
#include <string_view>

#include "graft/engine.hpp"
#include "graft/loader.hpp"

// Шапка: кто здесь стоит, какой версии и куда идти с вопросами. Печатается дважды и в
// разные места, потому что читатели разные: в СВОЙ журнал — сразу при установке, ещё до
// того как движок проснулся (там её увидит тот, у кого что-то не завелось), и в журнал
// ИГРЫ — на первом же кадре, рядом с её собственным выводом.
namespace graft {
namespace {

constexpr std::string_view kArt[] = {
    "                                                            ",
    "   ▄████  ██▀███   ▄▄▄        █████▒▄▄▄█████▓▓█████ ▓█████▄ ",
    "  ██▒ ▀█▒▓██ ▒ ██▒▒████▄    ▓██   ▒ ▓  ██▒ ▓▒▓█   ▀ ▒██▀ ██▌",
    " ▒██░▄▄▄░▓██ ░▄█ ▒▒██  ▀█▄  ▒████ ░ ▒ ▓██░ ▒░▒███   ░██   █▌",
    " ░▓█  ██▓▒██▀▀█▄  ░██▄▄▄▄██ ░▓█▒  ░ ░ ▓██▓ ░ ▒▓█  ▄ ░▓█▄   ▌",
    " ░▒▓███▀▒░██▓ ▒██▒ ▓█   ▓██▒░▒█░      ▒██▒ ░ ░▒████▒░▒████▓ ",
    "  ░▒   ▒ ░ ▒▓ ░▒▓░ ▒▒   ▓▒█░ ▒ ░      ▒ ░░   ░░ ▒░ ░ ▒▒▓  ▒ ",
    "   ░   ░   ░▒ ░ ▒░  ▒   ▒▒ ░ ░          ░     ░ ░  ░ ░ ▒  ▒ ",
    " ░ ░   ░   ░░   ░   ░   ▒    ░ ░      ░         ░    ░ ░  ░ ",
    "       ░    ░           ░  ░                    ░  ░   ░    ",
    "                                                     ░      "};

std::string tail() {
    return std::format("      v{}  •  {}  •  {}", GRAFT_VERSION,
                       "github.com/KRdayzmodding/KR_GRAFTED", "6wingSeraph");
}

// Первый кадр — самое раннее место, где законно звать движок: до него Print ещё некому
// исполнять. Дальше обработчик молчит.
void on_first_frame(float) {
    static bool said = false;
    if (said) {
        return;
    }
    said = true;
    for (const std::string_view line : kArt) {
        detail::to_script_log(line);
    }
    detail::to_script_log(tail());
}

}  // namespace

void say_banner() {
    for (const std::string_view line : kArt) {
        log(line);
    }
    log(tail());
    loader::add_tick(&on_first_frame);
}

}  // namespace graft
