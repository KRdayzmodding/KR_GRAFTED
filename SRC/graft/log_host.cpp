// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

#include "graft/engine.hpp"

// Системный журнал — файл в каталоге профиля сервера, там же, где script- и crash-логи
// игры. Пользовательский идёт не сюда, а в журналы самой игры (log_script.cpp).
//
// Отдельная TU без windows.h — её линкует и генератор объявлений, которому движок не
// нужен (там каталог не задан, и обе функции молчат).
namespace graft {
namespace {

std::string g_dir;

std::tm local_now() {
    const std::time_t now = std::time(nullptr);
    std::tm out{};
    localtime_s(&out, &now);
    return out;
}

// Одна метка на запуск: по ней узнаются все файлы одного старта сервера — ровно так же
// связаны между собой script_<метка>.log и crash_<метка>.log самой игры.
const std::string& stamp() {
    static const std::string once = [] {
        const std::tm t = local_now();
        return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", t.tm_year + 1900, t.tm_mon + 1,
                           t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    }();
    return once;
}

void write(const std::string& stem, std::string_view line) {
    if (g_dir.empty()) {
        return;
    }
    // Час:минута:секунда в начале строки — как в script-логах игры: без метки две записи
    // из разных мест не составить в один рассказ.
    const std::tm t = local_now();
    std::ofstream out(g_dir + "\\" + stem + "_" + stamp() + ".log", std::ios::app);
    out << std::format("{:02}:{:02}:{:02} | ", t.tm_hour, t.tm_min, t.tm_sec) << line << '\n';
}

}  // namespace

void set_log_dir(std::string_view dir) {
    g_dir = dir;
    if (g_dir.empty()) {
        return;
    }
    // Каталог профиля игра создаёт сама, но мы просыпаемся раньше неё: без этого первые
    // строки — а это как раз установка хуков — пропадали бы молча.
    std::error_code ignored;
    std::filesystem::create_directories(g_dir, ignored);
}

void log(std::string_view line) {
    write("graft", line);
}

// Хост — не мод, но сказать админу ему тоже бывает что: строка уйдёт в журналы игры
// под именем graft, как у любого плагина.
bool print(std::string_view line) {
    return detail::say(false, "graft", line);
}

bool error(std::string_view line) {
    return detail::say(true, "graft", line);
}

}  // namespace graft
