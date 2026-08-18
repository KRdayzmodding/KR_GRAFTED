// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstring>
#include <format>
#include <ranges>
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
            static_cast<std::uint8_t>(n.generate)};
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

}  // namespace

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
    for (const auto [i, arg] : std::views::enumerate(arg_names(n.args))) {
        if (i) {
            s += ", ";
        }
        s += std::format("{} p{}", arg, i);
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
        // Заголовок: у шаблонного класса он несёт параметры и объявляется целиком
        // (`class Имя<Class K, Class V>`), у обычного — правится существующий
        // (`modded class Имя`).
        const char* header = c;
        for (const graft_native_desc* n : all) {
            if (n->class_name && std::strcmp(n->class_name, c) == 0 && n->declare_as) {
                header = n->declare_as;
                break;
            }
        }
        const bool templated = std::strchr(header, '<') != nullptr;
        out += templated ? "\nclass " : "\nmodded class ";
        out += header;
        out += "\n{\n";
        const char* dispose = nullptr;
        for (const graft_native_desc* n : all) {
            if (!n->class_name || std::strcmp(n->class_name, c) != 0) {
                continue;
            }
            // Освобождение состояния — служебный метод: прячем его и зовём из
            // деструктора. Имя несёт суффикс плагина, чтобы два плагина, держащие
            // состояние на одном классе, не спорили за один и тот же метод.
            const bool is_dispose = std::strncmp(n->name, "NativeDispose", 13) == 0;
            if (is_dispose) {
                dispose = n->name;
            }
            out += is_dispose ? "    private " : "    ";
            out += proto_decl(*n) + "\n";
        }
        if (dispose) {
            out += "\n    void ~";
            out.append(c);
            out += "()\n    {\n        ";
            out += dispose;
            out += "();   // состояние объекта на стороне C++\n    }\n";
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
            std::none_of(modules.begin(), modules.end(),
                         [&](const std::string& m) { return m == n->module; })) {
            modules.emplace_back(n->module);
        }
    }
    return modules;
}

std::vector<std::string> proto_modules() {
    const std::vector<graft_native_desc> all = own();
    return proto_modules(pointers(all));
}

}  // namespace graft
