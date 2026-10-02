// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
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

std::string g_dir; // UTF-8: у клиента путь профиля бывает любым, обрезать до ASCII нельзя

// ofstream и create_directories читают narrow-строку как ANSI, поэтому путь собирается из
// UTF-8 явно — иначе журнал клиента с кириллицей в имени пользователя молча не писался бы.
std::filesystem::path as_path(std::string_view utf8) {
    return std::filesystem::path{
        std::u8string{reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()}};
}

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
    // Час:минута:секунда.миллисекунда в начале строки: без метки две записи из разных мест
    // не составить в один рассказ, а на запуске десятки событий укладываются в одну
    // секунду, и без миллисекунд не видно, что чего ждало. Всё, что читает `graft doctor`
    // (признак жалобы «!»), стоит после «| », и формат этого не трогает.
    const auto        now  = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const auto        ms   = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
                    std::chrono::seconds{1};
    std::tm t{};
    localtime_s(&t, &secs);
    // Строка собирается целиком и уходит одной записью.
    const std::string text = std::format("{:02}:{:02}:{:02}.{:03} | {}\n", t.tm_hour, t.tm_min, t.tm_sec, ms.count(), line);
    std::ofstream out(as_path(g_dir) / (stem + "_" + stamp() + ".log"), std::ios::app);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
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
    std::filesystem::create_directories(as_path(g_dir), ignored);
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
