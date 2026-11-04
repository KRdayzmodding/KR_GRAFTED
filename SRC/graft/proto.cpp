// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstring>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "graft/defines.hpp"
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
            static_cast<std::uint8_t>(n.generate),
            n.param_names,
            n.doc};
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
        std::string_view  one   = rest.substr(0, comma);
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

// Описание — комментарием НАД объявлением. Многострочное печатается несколькими
// строками: одну строку на всё объяснение читать невозможно, а голый перенос разорвал
// бы комментарий и сломал файл.
void emit_doc(std::string& out, const char* doc, std::string_view indent) {
    if (!doc || !*doc) {
        return;
    }
    std::string_view rest{doc};
    while (true) {
        const std::size_t      nl   = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        out += indent;
        out += line.empty() ? "//" : "// ";
        out += line;
        out += '\n';
        if (nl == std::string_view::npos) {
            return;
        }
        rest.remove_prefix(nl + 1);
    }
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

std::string proto_file(const std::vector<const graft_native_desc*>& source, const char* module, std::string_view plugin) {
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

    // Весь файл — под дефайном плагина: хост кладёт его, только если плагин РЕАЛЬНО
    // загрузился (defines.cpp). Нет плагина — нет и объявлений, и мод компилируется
    // дальше, а не падает на `proto native` без импла. Хост представлен дефайном GRAFTED.
    // Кроме 1_Core: проверено на сервере, этот модуль компилируется БЕЗ дефайнов (не видны
    // даже DIAG и имя мода) — обёрнутые объявления там пропадали бы всегда.
    std::string guard;
    if (!plugin.empty() && std::strcmp(module, "1_Core") != 0) {
        guard = plugin == "graft" ? "GRAFTED" : defines::define_name(plugin);
        out += "#ifdef " + guard + "\n";
    }

    for (const graft_native_desc* n : all) {
        if (!n->class_name) {
            // Пользователь читает сгенерированный файл, и типов ему мало.
            emit_doc(out, n->doc, "");
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
    // Заголовок: если он задан явно (`declare_as`), класс НАШ — и объявляется целиком,
    // `class Имя`. Так печатаются и шаблонные классы (заголовок несёт параметры:
    // `class Имя<Class K, Class V>`), и новые с базой (`class Имя : База`). Если не задан —
    // класс движковый, и мы к нему только дописываем: `modded class Имя`.
    const auto header_of = [&](const char* c) -> const char* {
        for (const graft_native_desc* n : all) {
            if (n->class_name && std::strcmp(n->class_name, c) == 0 && n->declare_as) {
                return n->declare_as;
            }
        }
        return nullptr;
    };
    // База из этого же файла печатается раньше наследника, в каком бы порядке их ни
    // привязали. Заголовок приходит из чужой DLL плагина, поэтому и цикл баз («A : B»,
    // «B : A») обязан кончиться, а не уронить генератор: такой файл отвергнет уже
    // компилятор скрипта, со своей понятной ошибкой.
    const auto base_of = [&](const char* c) -> std::string_view {
        const char* header = header_of(c);
        const char* colon  = header ? std::strchr(header, ':') : nullptr;
        if (!colon) {
            return {};
        }
        std::string_view base{colon + 1};
        base.remove_prefix(std::min(base.find_first_not_of(' '), base.size()));
        return base.substr(0, base.find_first_of(" <"));
    };
    const auto among = [](const std::vector<const char*>& list, const char* c) {
        return std::ranges::any_of(list, [&](const char* o) { return std::strcmp(o, c) == 0; });
    };
    std::vector<const char*> ordered;
    std::vector<const char*> placing;
    const auto               place = [&](this const auto& self, const char* c) -> void {
        if (among(ordered, c) || among(placing, c)) {
            return;
        }
        placing.push_back(c);
        const std::string_view base = base_of(c);
        for (const char* b : classes) {
            if (!base.empty() && base == b) {
                self(b);
            }
        }
        placing.pop_back();
        ordered.push_back(c);
    };
    for (const char* c : classes) {
        place(c);
    }

    for (const char* c : ordered) {
        const char* declared = header_of(c);
        const bool  ours     = declared != nullptr;
        const char* header   = ours ? declared : c;
        out += ours ? "\nclass " : "\nmodded class ";
        out += header;
        out += "\n{\n";
        for (const graft_native_desc* n : all) {
            if (!n->class_name || std::strcmp(n->class_name, c) != 0) {
                continue;
            }
            emit_doc(out, n->doc, "    ");
            out += "    ";
            out += proto_decl(*n) + "\n";
        }
        out += "}\n";
    }
    if (!guard.empty()) {
        out += "#endif\n";
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
