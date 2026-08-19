// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Этот файл линкуется в КАЖДЫЙ плагин, поэтому едет с исключением: мод на GRAFT ничего
// не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#include "graft/plugins.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <ranges>

namespace graft::plugins {
namespace {

constexpr char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Имена модов и ключи командной строки регистронезависимы: игру запускают и через
// лаунчер, и руками, и `-serverMod=` встречается в обоих написаниях.
bool same_ci(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, {}, lower, lower);
}

// Ключ начинается либо в начале строки, либо после пробела: иначе `-profiles=x-mod=@A`
// подсунул бы нам чужой путь.
bool at_boundary(std::string_view cmdline, std::size_t at) {
    return at == 0 || std::isspace(static_cast<unsigned char>(cmdline[at - 1]));
}

std::size_t find_switch(std::string_view cmdline, std::string_view key, std::size_t from) {
    for (std::size_t at = from; at + key.size() <= cmdline.size(); ++at) {
        if (!at_boundary(cmdline, at)) {
            continue;
        }
        if (same_ci(cmdline.substr(at, key.size()), key)) {
            return at;
        }
    }
    return std::string_view::npos;
}

// Значение ключа: до пробела, а если открылась кавычка — до закрывающей (в пути мода
// пробел законен: `-mod="@My Mod;@B"`).
std::string_view value_at(std::string_view cmdline, std::size_t at) {
    if (at < cmdline.size() && cmdline[at] == '"') {
        const std::size_t end = cmdline.find('"', at + 1);
        return cmdline.substr(at + 1, (end == std::string_view::npos ? cmdline.size() : end) -
                                          at - 1);
    }
    const std::size_t end = cmdline.find(' ', at);
    return cmdline.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
}

// `@A;@B;` -> {"@A", "@B"}. Пустые куски (хвостовая точка с запятой — норма для DayZ)
// и повторы отбрасываются: список задаёт ещё и порядок загрузки плагинов, а повтор
// сделал бы победителя коллизии зависящим от того, сколько раз мод перечислили.
void append_list(std::string_view list, std::vector<std::string>& out) {
    for (const auto part : list | std::views::split(';')) {
        const std::string_view item{part};
        if (item.empty() || std::ranges::any_of(out, [&](const std::string& have) {
                return same_ci(have, item);
            })) {
            continue;
        }
        out.emplace_back(item);
    }
}

void collect(std::string_view cmdline, std::string_view key, std::vector<std::string>& out) {
    for (std::size_t at = find_switch(cmdline, key, 0); at != std::string_view::npos;
         at = find_switch(cmdline, key, at + key.size())) {
        append_list(value_at(cmdline, at + key.size()), out);
    }
}

const char* text_or(const char* s, const char* fallback) {
    return s ? s : fallback;
}

}  // namespace

std::vector<std::string> mod_dirs(std::string_view cmdline) {
    std::vector<std::string> out;
    collect(cmdline, "-mod=", out);
    collect(cmdline, "-serverMod=", out);
    return out;
}

std::string profile_dir(std::string_view cmdline) {
    constexpr std::string_view key = "-profiles=";
    const std::size_t at = find_switch(cmdline, key, 0);
    if (at == std::string_view::npos) {
        return {};
    }
    std::string_view path = value_at(cmdline, at + key.size());
    // `-profiles=X\\` — законная запись, а мы к пути дописываем своё имя файла.
    while (!path.empty() && (path.back() == '\\' || path.back() == '/')) {
        path.remove_suffix(1);
    }
    return std::string{path};
}

std::uint32_t check(const graft_plugin_info& info) {
    // Обрезанная структура означает плагин из другого времени: полям верить нельзя,
    // и это тот же случай, что чужой ABI.
    if (info.size < sizeof(graft_plugin_info) || info.abi != GRAFT_ABI_VERSION) {
        return GRAFT_ERR_ABI;
    }
    if (info.layout != GRAFT_LAYOUT_VERSION) {
        return GRAFT_ERR_LAYOUT;
    }
    return GRAFT_OK;
}

// Что делает натив «тем же самым»: пара <класс, имя>. Глобальная функция и метод с
// одинаковым именем — разные сущности, поэтому nullptr в class_name значим.
bool same_native(const graft_native_desc& a, const graft_native_desc& b) {
    if ((a.class_name == nullptr) != (b.class_name == nullptr)) {
        return false;
    }
    if (a.class_name && std::string_view{a.class_name} != b.class_name) {
        return false;
    }
    return std::string_view{a.name} == b.name;
}

std::vector<entry> merge(const std::vector<entry>& all, std::vector<collision>& out) {
    std::vector<entry> kept;
    kept.reserve(all.size());
    for (const entry& e : all) {
        if (!e.desc || !e.desc->name) {
            continue;
        }
        const auto clash = std::ranges::find_if(
            kept, [&](const entry& have) { return same_native(*have.desc, *e.desc); });
        if (clash != kept.end()) {
            out.push_back({text_or(e.desc->class_name, ""), e.desc->name,
                           text_or(clash->owner, "?"), text_or(e.owner, "?")});
            continue;
        }
        kept.push_back(e);
    }
    return kept;
}

std::string describe(const collision& c) {
    return std::format("! коллизия имён: {}{}{} занято плагином '{}', плагину '{}' отказано",
                       c.class_name, c.class_name.empty() ? "" : ".", c.name, c.first, c.second);
}

const char* explain(std::uint32_t code) {
    switch (code) {
        case GRAFT_OK:
            return "ok";
        case GRAFT_ERR_ABI:
            return "другая версия интерфейса (пересобрать плагин)";
        case GRAFT_ERR_LAYOUT:
            return "другая раскладка движка (пересобрать плагин)";
        default:
            return "внутренняя ошибка плагина";
    }
}

}  // namespace graft::plugins
