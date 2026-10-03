// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Этот файл линкуется в КАЖДЫЙ плагин, поэтому едет с исключением: мод на GRAFT ничего
// не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#include "graft/plugins.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <optional>
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

// Командная строка по аргументам: пробел вне кавычек разделяет, кавычки снимаются. Так
// читаются обе записи — `-mod="@My Mod;@B"` (руками) и `"-mod=@My Mod;@B"` (лаунчер берёт
// в кавычки весь аргумент). Обратная косая перед кавычкой кавычку не экранирует: путь,
// оканчивающийся на `\`, в `-profiles="C:\prof\"` всё равно закрывается.
std::vector<std::string> split_args(std::string_view cmdline) {
    std::vector<std::string> out;
    std::string              arg;
    bool                     quoted = false;
    bool                     open   = false; // аргумент начат, пусть и пустой: `""`
    for (const char c : cmdline) {
        if (c == '"') {
            quoted = !quoted;
            open   = true;
        } else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (open) {
                out.push_back(std::move(arg));
                arg.clear();
                open = false;
            }
        } else {
            arg += c;
            open = true;
        }
    }
    if (open) {
        out.push_back(std::move(arg));
    }
    return out;
}

// Значение ключа, если аргумент им начинается. Ключ — только в начале аргумента: иначе
// `-profiles=x-mod=@A` подсунул бы нам чужой путь.
std::optional<std::string_view> value_of(std::string_view arg, std::string_view key) {
    if (arg.size() < key.size() || !same_ci(arg.substr(0, key.size()), key)) {
        return std::nullopt;
    }
    return arg.substr(key.size());
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

const char* text_or(const char* s, const char* fallback) {
    return s ? s : fallback;
}

}  // namespace

std::vector<std::string> mod_dirs(std::string_view cmdline) {
    std::vector<std::string> out;
    const auto               args = split_args(cmdline);
    // Сначала все `-mod=`, потом все `-serverMod=`: порядок прежний, от положения ключей
    // в строке победитель коллизии не зависит.
    for (const std::string_view key : {"-mod=", "-serverMod="}) {
        for (const std::string& arg : args) {
            if (const auto list = value_of(arg, key)) {
                append_list(*list, out);
            }
        }
    }
    return out;
}

std::string profile_dir(std::string_view cmdline) {
    for (const std::string& arg : split_args(cmdline)) {
        if (auto path = value_of(arg, "-profiles=")) {
            // `-profiles=X\` — законная запись, а мы к пути дописываем своё имя файла.
            while (!path->empty() && (path->back() == '\\' || path->back() == '/')) {
                path->remove_suffix(1);
            }
            return std::string{*path};
        }
    }
    return {};
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
    return std::format("! name collision: {}{}{} is taken by plugin '{}', plugin '{}' refused",
                       c.class_name,
                       c.class_name.empty() ? "" : ".",
                       c.name,
                       c.first,
                       c.second);
}

std::string describe_unbound(std::string_view                     class_name,
                             const std::vector<std::string_view>& methods) {
    // Шесть имён — чтобы по строке было видно, о чём речь; весь список лежит в плагине.
    constexpr std::size_t kShown = 6;
    std::string           names;
    for (std::size_t i = 0; i < methods.size() && i < kShown; ++i) {
        names += (i ? ", " : "");
        names += methods[i];
    }
    if (methods.size() > kShown) {
        names += std::format(", ... {} more", methods.size() - kShown);
    }
    return std::format("! class {} was never found - {} natives left unbound ({})", class_name, methods.size(), names);
}

namespace {

// Одно разошедшееся число: чьё старее, то и чинить. Старее плагин — пересобрать его под
// текущий graft; старее хост — пересборка плагина не поможет, обновлять надо хост.
std::string mismatch(std::string_view what, std::uint32_t plugin, std::uint32_t host, std::string_view meaning) {
    return std::format("{}: plugin {} {}, host {} {} - {}; {}", meaning, what, plugin, what, host, plugin < host ? "plugin is older than the host" : "host is older than the plugin", plugin < host ? "rebuild the plugin against the current graft" : "update the host (graft install)");
}

} // namespace

std::string reason(const graft_plugin_info& info, std::uint32_t code) {
    if (code == GRAFT_OK) {
        return "ok";
    }
    if (code != GRAFT_ERR_ABI && code != GRAFT_ERR_LAYOUT) {
        return std::format("internal plugin error (code {})", code);
    }
    // Нулевой заголовок: плагин отказал сам и описание не заполнил. Так ведут себя
    // плагины, собранные до того, как отказ стал сообщать свои числа, — то есть старые.
    if (info.size == 0 && info.abi == 0 && info.layout == 0) {
        return std::format(
            "plugin refused the host (ABI {}, LAYOUT {}) and did not report its own versions - built against an "
            "old graft; rebuild the plugin against the current graft",
            GRAFT_ABI_VERSION,
            GRAFT_LAYOUT_VERSION);
    }
    if (info.size < sizeof(graft_plugin_info)) {
        return std::format("plugin description truncated: {} bytes instead of {} - built against a different "
                           "graft; rebuild the plugin against the current graft (ABI {}, LAYOUT {})",
                           info.size,
                           sizeof(graft_plugin_info),
                           GRAFT_ABI_VERSION,
                           GRAFT_LAYOUT_VERSION);
    }
    std::string out;
    if (info.abi != GRAFT_ABI_VERSION) {
        out = mismatch("ABI", info.abi, GRAFT_ABI_VERSION, "host-plugin interface");
    }
    if (info.layout != GRAFT_LAYOUT_VERSION) {
        out += out.empty() ? "" : "; ";
        out += mismatch("LAYOUT", info.layout, GRAFT_LAYOUT_VERSION, "engine layout");
    }
    // Числа сошлись, а отказ есть: противоречие, его и показываем как есть.
    return out.empty() ? std::format("refused with code {} although the versions match (ABI {}, LAYOUT {})",
                                     code,
                                     GRAFT_ABI_VERSION,
                                     GRAFT_LAYOUT_VERSION)
                       : out;
}

}  // namespace graft::plugins
