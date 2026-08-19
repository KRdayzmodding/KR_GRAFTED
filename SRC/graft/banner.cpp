// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <format>
#include <string_view>

#include "graft/callout.hpp"
#include "graft/engine.hpp"
#include "graft/script.hpp"

// Шапка: кто здесь стоит, какой версии и куда идти с вопросами. Печатается в оба журнала,
// потому что читатели разные.
//
// В СВОЙ журнал — сразу при установке, ещё до того как движок проснулся: там её увидит
// тот, у кого что-то не завелось.
//
// В журнал ИГРЫ — первой же строкой, раньше её собственного «Module: ... loaded». Раньше
// нельзя физически: кадр вызова Print собирается по шаблонам скриптовых переменных, а до
// линковки первого модуля их не существует. Поэтому попытка делается на каждом проходе
// регистрации (engine.cpp), и первая же удачная печатает — дальше функция стоит копейки.
namespace graft {
namespace {

// Рисунок правится как рисунок: сырой литерал, а не массив строк в кавычках. Резать его
// на строки приходится нам — в журнале у каждой записи своё время, а Print игры
// многострочную строку одной записью и оставит.
constexpr std::string_view kArt = R"(
  ▄████  ██▀███   ▄▄▄        █████▐██████▒█████ ▓█████▄ 
 ██▒ ▀█▒▓██ ▒ ██▒▒████▄    ▓██   ▒  ▓██▒ ▒█   ▀ ▒██▀ ██▌
▒██░▄▄▄░▓██ ░▄█ ▒▒██  ▀█▄  ▒████ ░  ▓██░ ░███   ░██   ██
░▓█  ██▓▒██▀▀█▄  ░██▄▄▄▄██ ░▓█▒  ░  ▒▐█▓ ░▓█  ▄ ░▓█▄   █
░▒▓███▀▒░██▓ ▒██▒ ▓█   ▓██▒░▒█░     ▒▐▌▒ ░▒████▒░▒██████
 ░▒   ▒ ░ ▒▓ ░▒▓░ ▒▒   ▓▒▐░ ▒▌░     ░ ░░  ░ ▒░ ░ ▒▒▓  ▓ 
  ░   ░   ░▒ ░ ▒░  ▒   ▒▒ ░ ░         ░   ░ ░  ░ ░ ▒  ▒ 
░ ░   ░   ░░   ░   ░   ▒    ░ ░     ░       ░    ░ ░  ▒ 
      ░    ░           ░  ░                 ░  ░   ░  ░ 
)";

std::string tail() {
    return std::format("      v{}  •  {}  by  {}", GRAFT_VERSION,
                       "github.com/KRdayzmodding/KR_GRAFTED", "[KR] 6wingSeraph");
}

// Обход строк без копий: string_view нарезается по месту.
template <class F>
void for_each_line(std::string_view text, F emit) {
    if (text.starts_with('\n')) {
        text.remove_prefix(1);  // сырой литерал начинается переводом строки
    }
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        if (end == std::string_view::npos) {
            emit(text);
            return;
        }
        emit(text.substr(0, end));
        text.remove_prefix(end + 1);
    }
}

// Один раз за процесс, как можно раньше.
bool g_said_to_game = false;

// Может ли движок печатать. Условие ровно одно, и оно проверяемое: чтобы позвать Print,
// нужно собрать скриптовую переменную-строку, а её шаблон берётся у ванильного параметра
// (donor_template). До линковки модуля шаблона не существует — печатать нечем, и это не
// ошибка, а «ещё рано».
bool ready() {
    return script::find_global("Print") != nullptr &&
           detail::donor_template(script::tag_string) != nullptr;
}

}  // namespace

bool say_banner_to_game() {
    if (g_said_to_game || !ready()) {
        return g_said_to_game;
    }
    // Пустая строка первой — она отделяет шапку от заголовка журнала.
    detail::to_script_log("");
    for_each_line(kArt, [](std::string_view line) { detail::to_script_log(line); });
    detail::to_script_log(tail());
    g_said_to_game = true;
    return true;
}

void say_banner() {
    for_each_line(kArt, [](std::string_view line) { log(line); });
    log(tail());
}

}  // namespace graft
