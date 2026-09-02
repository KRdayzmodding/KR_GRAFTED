// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstring>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "graft/native.hpp"

// Генерация скриптовой стороны из реестра нативов: тот же источник истины, что и адреса
// impl, поэтому объявление в PBO не может разойтись с реализацией.
//
// Работает по graft_native_desc, а не по внутреннему списку: так один и тот же код
// обслуживает и «свои нативы в этом же процессе», и «дескрипторы, вынутые из чужой DLL
// плагина» — а именно так их достаёт graft.exe, которому реестр плагина недоступен.
namespace graft {
namespace {

graft_native_desc to_desc(const native& n) {
    return {n.class_name,
            n.name,
            n.impl,
            n.ret,
            n.args,
            n.module,
            n.declare_as,
            static_cast<std::uint8_t>(n.is_static),
            static_cast<std::uint8_t>(n.marshalled),
            static_cast<std::uint8_t>(n.generate), n.param_names, n.doc};
}

// Список складывается LIFO — разворачиваем к порядку объявления в исходнике.
std::vector<graft_native_desc> own() {
    std::vector<const native*> ordered;
    for (const native* n = natives(); n; n = n->next) {
        ordered.push_back(n);
    }
    std::vector<graft_native_desc> out;
    out.reserve(ordered.size());
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
        out.push_back(to_desc(**it));
    }
    return out;
}

std::vector<const graft_native_desc*> pointers(const std::vector<graft_native_desc>& all) {
    std::vector<const graft_native_desc*> out;
    out.reserve(all.size());
    for (const graft_native_desc& d : all) {
        out.push_back(&d);
    }
    return out;
}

} // namespace

namespace {

// Имена аргументов из строки "player, uid, zone". Пусто или не хватает — недостающие
// печатаются как p0, p1: объявление обязано остаться валидным, даже если про имена
// забыли.
std::vector<std::string> split_params(const char* csv) {
    std::vector<std::string> out;
    if (!csv) {
        return out;
    }
    std::string_view rest{csv};
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        std::string_view one = rest.substr(0, comma);
        while (!one.empty() && one.front() == ' ') {
            one.remove_prefix(1);
        }
        while (!one.empty() && one.back() == ' ') {
            one.remove_suffix(1);
        }
        if (!one.empty()) {
            out.emplace_back(one);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return out;
}

} // namespace

std::string proto_decl(const graft_native_desc& n) {
    std::string s;
    if (n.class_name && n.is_static) {
        s += "static ";
    }
    s += n.marshalled ? "proto " : "proto native ";
    s += n.ret;
    s += ' ';
    s += n.name;
    s += '(';
    const std::vector<std::string> names = split_params(n.param_names);
    for (const auto [i, arg] : std::views::enumerate(arg_names(n.args))) {
        if (i) {
            s += ", ";
        }
        const auto idx = static_cast<std::size_t>(i);
        if (idx < names.size()) {
            s += std::format("{} {}", arg, names[idx]);
        } else {
            s += std::format("{} p{}", arg, i);
        }
    }
    s += ");";
    return s;
}

std::string proto_decl(const native& n) {
    return proto_decl(to_desc(n));
}

std::string proto_file(const std::vector<const graft_native_desc*>& source, const char* module) {
    std::vector<const graft_native_desc*> all;
    for (const graft_native_desc* n : source) {
        if (n->generate && n->module && std::strcmp(n->module, module) == 0) {
            all.push_back(n);
        }
    }

    std::string out =
        "// СГЕНЕРИРОВАНО protogen из C++ реестра нативов — руками не править.\n"
        "// Источник истины — блоки GRAFT_BINDINGS в исходниках мода.\n"
        "// Импл живёт в graft-модуле (proxy-DLL рядом с exe); без неё скрипт не слинкуется.\n"
        "// Модуль: ";
    out += module;
    out += "\n\n";

    for (const graft_native_desc* n : all) {
        if (!n->class_name) {
            // Описание — строкой комментария над объявлением: пользователь читает
            // сгенерированный файл, и типов ему мало.
            if (n->doc && *n->doc) {
                out += "// ";
                out += n->doc;
                out += "\n";
            }
            out += proto_decl(*n) + "\n";
        }
    }

    std::vector<const char*> classes;
    for (const graft_native_desc* n : all) {
        if (n->class_name && std::none_of(classes.begin(), classes.end(), [&](const char* c) {
                return std::strcmp(c, n->class_name) == 0;
            })) {
            classes.push_back(n->class_name);
        }
    }
    for (const char* c : classes) {
        // Заголовок: если он задан явно (`declare_as`), класс НАШ — и объявляется
        // целиком, `class Имя`. Так печатаются и шаблонные классы (заголовок несёт
        // параметры: `class Имя<Class K, Class V>`), и обычные новые. Если не задан —
        // класс движковый, и мы к нему только дописываем: `modded class Имя`.
        const char* header = c;
        bool        ours   = false;
        for (const graft_native_desc* n : all) {
            if (n->class_name && std::strcmp(n->class_name, c) == 0 && n->declare_as) {
                header = n->declare_as;
                ours   = true;
                break;
            }
        }
        out += ours ? "\nclass " : "\nmodded class ";
        out += header;
        out += "\n{\n";
        for (const graft_native_desc* n : all) {
            if (!n->class_name || std::strcmp(n->class_name, c) != 0) {
                continue;
            }
            if (n->doc && *n->doc) {
                out += "    // ";
                out += n->doc;
                out += "\n";
            }
            out += "    ";
            out += proto_decl(*n) + "\n";
        }
        out += "}\n";
    }
    return out;
}

std::string proto_file(const char* module) {
    const std::vector<graft_native_desc> all = own();
    return proto_file(pointers(all), module);
}

std::vector<std::string> proto_modules(const std::vector<const graft_native_desc*>& source) {
    std::vector<std::string> modules;
    for (const graft_native_desc* n : source) {
        if (n->generate && n->module &&
            std::none_of(modules.begin(), modules.end(), [&](const std::string& m) { return m == n->module; })) {
            modules.emplace_back(n->module);
        }
    }
    return modules;
}

std::vector<std::string> proto_modules() {
    const std::vector<graft_native_desc> all = own();
    return proto_modules(pointers(all));
}

} // namespace graft
