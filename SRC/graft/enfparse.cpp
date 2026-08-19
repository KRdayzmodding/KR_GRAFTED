// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/enfparse.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

// Разбор Enforce и зеркало движкового API. Устройство — два прохода и ничего больше:
// сканер режет текст на объявления, генератор печатает по ним C++.
//
// Сканер намеренно тупой. Он не понимает язык — он ищет объявления `proto` и следит за
// вложенностью фигурных скобок, чтобы знать, в каком классе находится. Тела методов,
// enum-ы, typedef-ы и препроцессор просто проезжают мимо. Полноценный парсер Enforce
// здесь был бы платой за то, что нам не нужно: из 16 МБ скриптов нас интересуют только
// строки с `proto`.
namespace graft::enf {
namespace {

bool ident_char(char c) {
    return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_';
}

bool is_identifier(std::string_view s) {
    if (s.empty() || (std::isdigit(static_cast<unsigned char>(s.front())) != 0)) {
        return false;
    }
    return std::ranges::all_of(s, ident_char);
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) != 0)) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) != 0)) {
        s.remove_suffix(1);
    }
    return s;
}

// Модификаторы объявления. Всё это стоит перед типом возврата и типом НЕ является —
// иначе `proto native owned external string ClassName()` дал бы возврат из четырёх слов.
const std::unordered_set<std::string>& modifiers() {
    static const std::unordered_set<std::string> all{
        "static", "private", "protected", "proto",  "native", "volatile", "external",
        "owned",  "notnull", "autoptr",   "ref",    "const",  "override", "sealed",
        "local",  "event",   "out",       "inout",  "reference"};
    return all;
}

// Разбить по пробелам, НЕ заходя внутрь угловых скобок: `array<ref Param>` — один токен.
std::vector<std::string> split_tokens(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    int angle = 0;
    for (char c : text) {
        if (c == '<') {
            ++angle;
        } else if (c == '>') {
            --angle;
        }
        if (angle == 0 && (std::isspace(static_cast<unsigned char>(c)) != 0)) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

// Разбить список аргументов по запятым верхнего уровня.
std::vector<std::string_view> split_args(std::string_view text) {
    std::vector<std::string_view> out;
    int angle = 0;
    int paren = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '<') {
            ++angle;
        } else if (c == '>') {
            --angle;
        } else if (c == '(') {
            ++paren;
        } else if (c == ')') {
            --paren;
        } else if (c == ',' && angle == 0 && paren == 0) {
            out.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(text.substr(start));
    return out;
}

bool parse_param(std::string_view text, param& out) {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    // Значение по умолчанию отрезаем до разбора: в нём бывает что угодно.
    if (const std::size_t eq = text.find('='); eq != std::string_view::npos) {
        out.default_value = std::string{trim(text.substr(eq + 1))};
        text = trim(text.substr(0, eq));
    }
    std::vector<std::string> tokens = split_tokens(text);
    while (!tokens.empty() && modifiers().contains(tokens.front())) {
        if (tokens.front() == "out" || tokens.front() == "inout") {
            out.is_out = true;
        }
        tokens.erase(tokens.begin());
    }
    if (tokens.empty()) {
        return false;
    }
    if (tokens.size() >= 2) {
        out.name = tokens.back();
        tokens.pop_back();
    }
    out.type = tokens.size() == 1 ? tokens.front() : "";
    // Статический массив в параметре (`void param_array[]`) наш вызов не выражает —
    // пустой тип заставит генератор отказаться от всей функции.
    if (out.name.find('[') != std::string::npos || out.type.find('[') != std::string::npos) {
        out.type.clear();
    }
    return true;
}

// Одно объявление вида `[модификаторы] ТИП Имя(аргументы)`. Возвращает false, если это
// не наш случай (нет `proto`, нет скобок, имя не имя).
bool parse_decl(std::string_view text, function& out) {
    const std::size_t lparen = text.find('(');
    if (lparen == std::string_view::npos) {
        return false;
    }
    const std::size_t rparen = text.rfind(')');
    if (rparen == std::string_view::npos || rparen < lparen) {
        return false;
    }
    std::vector<std::string> head = split_tokens(text.substr(0, lparen));
    if (head.size() < 2) {
        return false;
    }
    out.name = head.back();
    head.pop_back();
    if (!is_identifier(out.name)) {
        return false;
    }
    std::vector<std::string> rest;
    bool has_proto = false;
    for (const std::string& token : head) {
        if (token == "proto") {
            has_proto = true;
        } else if (token == "native") {
            out.is_native = true;
        } else if (token == "static") {
            out.is_static = true;
        } else if (!modifiers().contains(token)) {
            rest.push_back(token);
        }
    }
    if (!has_proto || rest.size() != 1) {
        return false;
    }
    out.ret = rest.front();

    const std::string_view args = text.substr(lparen + 1, rparen - lparen - 1);
    for (std::string_view piece : split_args(args)) {
        param p;
        if (parse_param(piece, p)) {
            out.params.push_back(std::move(p));
        }
    }
    return true;
}

// Заголовок класса: `[modded] class Имя [extends|:] База`. Шаблонный класс объявлен как
// `class array<Class T>` — имя берём до угловой скобки.
bool parse_class_head(std::string_view text, std::string& name, std::string& base) {
    // `class IEntity: Managed`, `class A:B`, `class A : B` — форм у двоеточия четыре.
    // Разводим пробелами до разбора, иначе оно прилипает к имени и класс теряется
    // целиком: так пропадал IEntity, а с ним GetOrigin/GetID/GetName у всех сущностей.
    std::string spaced;
    for (const char c : text) {
        if (c == ':') {
            spaced += " : ";
        } else {
            spaced.push_back(c);
        }
    }
    const std::vector<std::string> tokens = split_tokens(spaced);
    std::size_t at = 0;
    while (at < tokens.size() && tokens[at] != "class") {
        ++at;
    }
    if (at + 1 >= tokens.size()) {
        return false;
    }
    name = tokens[at + 1];
    if (const std::size_t angle = name.find('<'); angle != std::string::npos) {
        name.erase(angle);
    }
    if (!is_identifier(name)) {
        return false;
    }
    for (std::size_t i = at + 2; i < tokens.size() && base.empty(); ++i) {
        if (tokens[i] == "extends" || tokens[i] == ":") {
            if (i + 1 < tokens.size()) {
                base = tokens[i + 1];
                if (const std::size_t angle = base.find('<'); angle != std::string::npos) {
                    base.erase(angle);
                }
            }
            break;
        }
        // Форма `class Имя: База` без пробела.
        if (const std::size_t colon = tokens[i].find(':'); colon != std::string::npos) {
            base = tokens[i].substr(colon + 1);
            break;
        }
    }
    return is_identifier(name);
}

}  // namespace

const std::string& class_::primary_base() const {
    static const std::string none;
    for (const base_variant& b : bases) {
        if (b.when.empty()) {
            return b.name;
        }
    }
    return bases.empty() ? none : bases.front().name;
}

bool function::same_as(const function& other) const {
    if (name != other.name || params.size() != other.params.size()) {
        return false;
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (params[i].type != other.params[i].type) {
            return false;
        }
    }
    return true;
}

unit parse(std::string_view source, std::string_view module) {
    unit out;
    // Стек областей: индекс класса либо -1 у чужой скобки (тело метода, enum, инициализатор).
    std::vector<int> scope;
    std::string buffer;
    // Стек условий препроцессора. Ветку НЕ выбираем: какие символы определены в
    // конкретной сборке игры, знает только её владелец, — поэтому каждое объявление
    // запоминает своё условие, а генератор печатает его же обратно в C++.
    std::vector<cond> conditions;
    const auto here = [&] {
        guard g;
        for (const cond& c : conditions) {
            if (!c.name.empty()) {
                g.push_back(c);
            }
        }
        return g;
    };
    // Заголовок класса, который так и не дошёл до `{`, потому что препроцессор увёл его
    // в другую ветку: `#ifdef X class Man extends Person #else class Man extends EntityAI`.
    // Первый вариант виден ТОЛЬКО в момент директивы — иначе он потерян навсегда.
    std::vector<base_variant> pending_bases;
    const auto stash_head = [&] {
        std::string name;
        std::string base;
        if (!parse_class_head(trim(buffer), name, base) || base.empty()) {
            return;
        }
        guard g;
        for (const cond& c : conditions) {
            if (!c.name.empty()) {
                g.push_back(c);
            }
        }
        pending_bases.push_back({base, std::move(g)});
    };
    const auto note_symbol = [&](const std::string& name) {
        if (!name.empty() && std::ranges::find(out.conditions, name) == out.conditions.end()) {
            out.conditions.push_back(name);
        }
    };

    // НЕПОСРЕДСТВЕННАЯ область, а не ближайший класс вверх по стеку: `return false;`
    // внутри тела метода иначе оказался бы полем класса — и оказывался, пока не поймали
    // на ванильных скриптах.
    const auto current_class = [&]() -> class_* {
        if (scope.empty() || scope.back() < 0) {
            return nullptr;
        }
        return &out.classes[static_cast<std::size_t>(scope.back())];
    };
    // Поле класса: утверждение без `proto` и без скобок, вида `[модификаторы] Тип имя`.
    // Составные объявления (`int a, b;`) пропускаем — редки, а разбирать их значит
    // тащить полноценный парсер ради пары случаев.
    const auto take_field = [&](class_* k) {
        std::string_view text = trim(buffer);
        if (text.empty() || text.find('(') != std::string_view::npos ||
            text.find(',') != std::string_view::npos) {
            return;
        }
        if (const std::size_t eq = text.find('='); eq != std::string_view::npos) {
            text = trim(text.substr(0, eq));
        }
        std::vector<std::string> tokens = split_tokens(text);
        bool is_static = false;
        while (!tokens.empty() && modifiers().contains(tokens.front())) {
            is_static = is_static || tokens.front() == "static";
            tokens.erase(tokens.begin());
        }
        if (tokens.size() != 2 || is_static || !is_identifier(tokens[1])) {
            return;
        }
        if (std::ranges::none_of(k->fields, [&](const field& have) { return have.name == tokens[1]; })) {
            k->fields.push_back({tokens[0], tokens[1], here()});
        }
    };

    // Приватный конструктор/деструктор — объявленный автором запрет. Ловим и у формы с
    // телом (кончается на `{`), и у голого объявления (на `;`).
    const auto take_ctor_dtor = [&](class_* k) {
        const std::vector<std::string> tokens = split_tokens(trim(buffer));
        bool is_private = false;
        std::size_t at = 0;
        for (; at < tokens.size() && modifiers().contains(tokens[at]); ++at) {
            is_private = is_private || tokens[at] == "private";
        }
        if (at >= tokens.size() || !is_private) {
            return;
        }
        std::string_view head = tokens[at];
        if (head == "void" && at + 1 < tokens.size()) {
            head = tokens[at + 1];
        }
        const bool destructor = head.starts_with('~');
        if (destructor) {
            head.remove_prefix(1);
        }
        if (head.substr(0, head.find('(')) != k->name) {
            return;
        }
        if (destructor) {
            k->private_dtor = true;
        } else {
            k->private_ctor = true;
        }
    };

    const auto take_decl = [&] {
        class_* owner = current_class();
        if (owner) {
            take_ctor_dtor(owner);
        }
        function f;
        f.when = here();
        if (!parse_decl(trim(buffer), f)) {
            if (owner) {
                take_field(owner);
            }
            return;
        }
        if (class_* k = current_class()) {
            // Один класс бывает объявлен в файле дважды (ветки препроцессора, повтор
            // блока) — повтор объявления это не второй метод.
            if (std::ranges::none_of(k->functions,
                                     [&](const function& have) { return have.same_as(f); })) {
                k->functions.push_back(std::move(f));
            }
        } else if (scope.empty()) {
            out.globals.push_back(std::move(f));
        }
    };

    std::size_t i = 0;
    bool line_start = true;
    while (i < source.size()) {
        const char c = source[i];
        // Препроцессор: строка целиком мимо. Иначе `#ifdef` внутри класса развалил бы
        // область видимости, и следующий класс уехал бы внутрь предыдущего.
        if (c == '#' && line_start) {
            const std::size_t start = i;
            while (i < source.size() && source[i] != '\n') {
                ++i;
            }
            // Директива, ОТКРЫВАЮЩАЯ ветку, сбрасывает накопленное: иначе у класса с
            // двумя вариантами заголовка (`#ifdef ... class Man extends Person #else
            // class Man extends EntityAI #endif`) склеились бы оба, и базой стала бы
            // первая ветка. А `#endif` ничего не сбрасывает — он идёт ПОСЛЕ заголовка
            // последней ветки, и стереть его значило бы потерять базу вовсе.
            //
            // ponytail: выигрывает последняя ветка. Разбирать условия по-настоящему
            // значило бы держать копию дефайнов серверной сборки, а она нам не видна;
            // выбор проверяет рантайм — метода, которого в этой сборке нет, просто не
            // найдётся, и это видно в журнале, а не падением.
            std::string_view line = source.substr(start, i - start);
            // `#ifdef SERVER // комментарий` — комментарий обрезаем здесь: до общего
            // обработчика комментариев эта строка уже не дойдёт.
            if (const std::size_t slashes = line.find("//"); slashes != std::string_view::npos) {
                line = line.substr(0, slashes);
            }
            const std::vector<std::string> parts = split_tokens(line);
            const std::string_view what = parts.empty() ? std::string_view{} : parts.front();
            if (what == "#ifdef" || what == "#ifndef") {
                const std::string name = parts.size() > 1 ? parts[1] : std::string{};
                note_symbol(name);
                conditions.push_back({name, what == "#ifndef"});
                buffer.clear();
            } else if (what.starts_with("#if")) {
                // Составных `#if` в ванильных скриптах нет; если появятся — уровень
                // всё равно должен закрыться, поэтому кладём пустое условие.
                conditions.push_back({});
                buffer.clear();
            } else if (what == "#else") {
                stash_head();
                if (!conditions.empty()) {
                    conditions.back().negated = !conditions.back().negated;
                }
                buffer.clear();
            } else if (what == "#elif") {
                stash_head();
                conditions.pop_back();
                conditions.push_back({});
                buffer.clear();
            } else if (what == "#endif") {
                stash_head();
                if (!conditions.empty()) {
                    conditions.pop_back();
                }
            }
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            i += 2;
            while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) {
                ++i;
            }
            i = std::min(i + 2, source.size());
            continue;
        }
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++i;
            while (i < source.size() && source[i] != quote) {
                i += source[i] == '\\' ? 2 : 1;
            }
            ++i;
            buffer.push_back(' ');
            continue;
        }
        if (!(std::isspace(static_cast<unsigned char>(c)) != 0)) {
            line_start = false;
        } else if (c == '\n') {
            line_start = true;
        }

        if (c == '{') {
            std::string name;
            std::string base;
            if (parse_class_head(trim(buffer), name, base)) {
                // Класс мог уже встретиться в другом файле или выше по этому же.
                auto found = std::ranges::find(out.classes, name, &class_::name);
                if (found == out.classes.end()) {
                    out.classes.push_back({name, {}, std::string{module}, {}, {}, false, false});
                    found = out.classes.end() - 1;
                }
                for (base_variant& v : pending_bases) {
                    if (std::ranges::find(found->bases, v) == found->bases.end()) {
                        found->bases.push_back(std::move(v));
                    }
                }
                // Заголовок, дошедший до `{`, — это последняя ветка препроцессора, и она
                // уже записана как вариант со СВОИМ условием. Сюда она пришла бы второй
                // раз, но уже без условия (стек к этому моменту свёрнут), поэтому
                // одинаковую базу второй раз не берём.
                if (!base.empty() && std::ranges::none_of(found->bases,
                                                          [&](const base_variant& have) {
                                                              return have.name == base;
                                                          })) {
                    found->bases.push_back({base, here()});
                }
                scope.push_back(static_cast<int>(found - out.classes.begin()));
            } else {
                if (class_* owner = current_class()) {
                    take_ctor_dtor(owner);
                }
                scope.push_back(-1);
            }
            pending_bases.clear();
            buffer.clear();
        } else if (c == '}') {
            if (!scope.empty()) {
                scope.pop_back();
            }
            buffer.clear();
        } else if (c == ';') {
            take_decl();
            buffer.clear();
        } else {
            buffer.push_back(c);
        }
        ++i;
    }
    return out;
}

void merge(unit& into, unit&& more) {
    for (class_& k : more.classes) {
        auto found = std::ranges::find(into.classes, k.name, &class_::name);
        if (found == into.classes.end()) {
            into.classes.push_back(std::move(k));
            continue;
        }
        for (base_variant& b : k.bases) {
            if (std::ranges::find(found->bases, b) == found->bases.end()) {
                found->bases.push_back(std::move(b));
            }
        }
        if (found->module.empty()) {
            found->module = k.module;
        }
        found->private_ctor = found->private_ctor || k.private_ctor;
        found->private_dtor = found->private_dtor || k.private_dtor;
        for (function& f : k.functions) {
            if (std::ranges::none_of(found->functions,
                                     [&](const function& have) { return have.same_as(f); })) {
                found->functions.push_back(std::move(f));
            }
        }
        for (field& f : k.fields) {
            if (std::ranges::none_of(found->fields,
                                     [&](const field& have) { return have.name == f.name; })) {
                found->fields.push_back(std::move(f));
            }
        }
    }
    for (const std::string& c : more.conditions) {
        if (std::ranges::find(into.conditions, c) == into.conditions.end()) {
            into.conditions.push_back(c);
        }
    }
    for (function& f : more.globals) {
        if (std::ranges::none_of(into.globals,
                                 [&](const function& have) { return have.same_as(f); })) {
            into.globals.push_back(std::move(f));
        }
    }
}

unit parse_tree(const std::string& root) {
    namespace fs = std::filesystem;
    unit all;
    if (!fs::exists(root)) {
        return all;
    }
    // Порядок обхода задаёт, в каком модуле класс объявлен ВПЕРВЫЕ, — поэтому он должен
    // быть детерминированным, а не тем, что вернула файловая система.
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".c") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    for (const fs::path& file : files) {
        const fs::path rel = fs::relative(file, root);
        const std::string module = rel.begin() == rel.end() ? std::string{}
                                                            : rel.begin()->string();
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        merge(all, parse(text.str(), module));
    }
    return all;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Зеркало
// ══════════════════════════════════════════════════════════════════════════════
namespace {

// Имена, под которыми классы Enforce уже есть в библиотеке своими типами либо не могут
// стать классом C++ вовсе.
const std::unordered_map<std::string, std::string>& builtin_types() {
    static const std::unordered_map<std::string, std::string> all{
        {"void", "void"},          {"int", "graft::i32"},   {"float", "graft::f32"},
        {"bool", "bool"},          {"string", "graft::str"}, {"vector", "graft::vector"},
        {"typename", "graft::type"}, {"Class", "graft::obj"}, {"func", ""},
        {"array", ""},             {"set", ""},             {"map", ""},
    };
    return all;
}

bool reserved(const std::string& name) {
    static const std::unordered_set<std::string> words{
        "auto",   "bool",     "case",     "catch",  "char",   "class",    "const",
        "delete", "double",   "else",     "enum",   "export", "extern",   "float",
        "for",    "friend",   "goto",     "if",     "inline", "int",      "long",
        "new",    "operator", "private",  "public", "return", "short",    "signed",
        "sizeof", "static",   "struct",   "switch", "template", "this",   "throw",
        "try",    "typedef",  "typename", "union",  "unsigned", "using",  "virtual",
        "void",   "volatile", "while",    "namespace", "concept", "requires", "co_await"};
    return words.contains(name);
}

std::string sanitize(const std::string& name) {
    return reserved(name) ? name + "_" : name;
}

// Разложить `map<string,int>` на имя и параметры.
bool split_generic(const std::string& type, std::string& base, std::vector<std::string>& args) {
    const std::size_t angle = type.find('<');
    if (angle == std::string::npos || type.back() != '>') {
        return false;
    }
    base = type.substr(0, angle);
    for (std::string_view piece :
         split_args(std::string_view{type}.substr(angle + 1, type.size() - angle - 2))) {
        args.emplace_back(trim(piece));
    }
    return true;
}

// Порядок разбора скриптов движком. Он же — порядок, в котором классы становятся
// видимы: класс из 3_Game в заголовке 1_Core упомянуть нельзя, его там ещё нет.
constexpr std::array<const char*, 5> kModuleOrder{"1_Core", "2_GameLib", "3_Game", "4_World",
                                                  "5_Mission"};

int module_rank(std::string_view module) {
    for (const auto [i, name] : std::views::enumerate(kModuleOrder)) {
        if (module == name) {
            return static_cast<int>(i);
        }
    }
    return -1;  // модуль не задан (тесты) — считаем «виден всегда»
}

// Условие препроцессора обратно в C++. Печатаем единообразно через defined(), чтобы
// цепочку вариантов собирать из #if/#elif/#else.
std::string guard_expr(const guard& g) {
    std::string out;
    for (const cond& c : g) {
        if (!out.empty()) {
            out += " && ";
        }
        out += c.negated ? std::format("!defined({})", c.name) : std::format("defined({})", c.name);
    }
    return out;
}

std::string open_if(const guard& g) {
    return g.empty() ? std::string{} : std::format("#if {}\n", guard_expr(g));
}
std::string close_if(const guard& g) {
    return g.empty() ? std::string{} : std::string{"#endif\n"};
}

class emitter {
public:
    emitter(const unit& all, std::string_view module)
        : all_(all), module_(module), rank_(module_rank(module)) {
        for (const class_& k : all.classes) {
            by_name_.emplace(k.name, &k);
        }
        for (const class_& k : all.classes) {
            if (emittable(k)) {
                emitted_.insert(k.name);
            }
        }
        // Замыкание. Класс без собственных объявлений всё равно нужен, если он БАЗА
        // (иначе наследник потеряет её методы) или если его НАЗЫВАЕТ чья-то сигнатура
        // (PlayerIdentity объявлен пустым, а Man.GetIdentity возвращает именно его —
        // без замыкания метод молча выпал бы).
        for (bool changed = true; changed;) {
            changed = false;
            for (const class_& k : all.classes) {
                if (!emitted_.contains(k.name)) {
                    continue;
                }
                const auto want = [&](const std::string& name) {
                    if (name.empty() || emitted_.contains(name) || !by_name_.contains(name) ||
                        reserved(name) || builtin_types().contains(name)) {
                        return;
                    }
                    emitted_.insert(name);
                    changed = true;
                };
                for (const base_variant& b : k.bases) { want(b.name); }
                for (const function& f : k.functions) {
                    if (f.is_static) {
                        continue;
                    }
                    for (const std::string& leaf : type_leaves(f.ret)) {
                        want(leaf);
                    }
                    for (const param& p : f.params) {
                        for (const std::string& leaf : type_leaves(p.type)) {
                            want(leaf);
                        }
                    }
                }
            }
        }
    }

    // Имена классов внутри типа: `map<string,Man>` -> {map, string, Man}.
    static std::vector<std::string> type_leaves(const std::string& type) {
        std::string base;
        std::vector<std::string> args;
        if (!split_generic(type, base, args)) {
            return {type};
        }
        std::vector<std::string> out{base};
        for (const std::string& a : args) {
            for (std::string& leaf : type_leaves(a)) {
                out.push_back(std::move(leaf));
            }
        }
        return out;
    }

    bool emittable(const class_& k) const {
        if (!is_identifier(k.name) || reserved(k.name) || builtin_types().contains(k.name)) {
            return false;
        }
        return std::ranges::any_of(k.functions,
                                   [](const function& f) { return !f.is_static; });
    }

    bool in_module(const class_& k) const {
        return k.module.empty() || module_.empty() || k.module == module_;
    }

    // Тип Enforce в тип C++. Пусто — значит выразить не можем, и метод выпускать нельзя:
    // движок зовётся по сигнатуре напрямую, расхождение это падение, а не ошибка сборки.
    std::string cpp_type(const std::string& enf_name, bool marshalled_string) const {
        if (const auto it = builtin_types().find(enf_name); it != builtin_types().end()) {
            // Маршалируемый возврат строки движок отдаёт через переменную, и её буфер —
            // не наш: забираем значением.
            if (enf_name == "string" && marshalled_string) {
                return "std::string";
            }
            return it->second;
        }
        std::string base;
        std::vector<std::string> args;
        if (split_generic(enf_name, base, args)) {
            std::vector<std::string> mapped;
            for (const std::string& a : args) {
                const std::string one = cpp_type(a, false);
                // Элемент контейнера лежит в памяти движка как есть, поэтому поддержаны
                // только те типы, чей размер в скрипте достоверно известен: 4 у чисел,
                // 8 у строки и ссылки. bool, vector и typename туда не кладутся —
                // сторожит is_element в container.hpp, и лучше не выпустить метод, чем
                // выпустить не собирающийся заголовок.
                if (one.empty() || one == "void" || one == "bool" || one == "graft::vector" ||
                    one == "graft::type" || one.starts_with("graft::array") ||
                    one.starts_with("graft::set") || one.starts_with("graft::map")) {
                    return {};
                }
                mapped.push_back(one);
            }
            if ((base == "array" || base == "set") && mapped.size() == 1) {
                return std::format("graft::{}<{}>", base, mapped[0]);
            }
            if (base == "map" && mapped.size() == 2) {
                return std::format("graft::map<{}, {}>", mapped[0], mapped[1]);
            }
            return {};
        }
        if (!emitted_.contains(enf_name)) {
            return {};
        }
        // Класс из более позднего модуля в этом заголовке ещё не объявлен — и не будет:
        // движок разбирает модули в том же порядке.
        if (const auto it = by_name_.find(enf_name); it != by_name_.end() && rank_ >= 0) {
            const int other = module_rank(it->second->module);
            if (other > rank_) {
                return {};
            }
        }
        return sanitize(enf_name);
    }

    // Разобранный метод: объявление внутри примеси и тело ОТДЕЛЬНО.
    //
    // Тело обязано стоять после определения всех конкретных классов: возвращаемый тип
    // здесь не зависит от параметра шаблона, а такое компилятор проверяет сразу — в
    // теле, написанном внутри примеси, PlayerIdentity был бы ещё неполным.
    struct emitted_method {
        std::string decl;
        std::string def;
        explicit operator bool() const { return !decl.empty(); }
    };

    emitted_method method(const class_& owner, const function& f, coverage& out) const {
        if (f.is_static) {
            ++out.skipped_static;
            return {};
        }
        if (!is_identifier(f.name) || reserved(f.name)) {
            ++out.skipped_type;
            return {};
        }
        // Метод, названный как свой класс, в Enforce законен, а в C++ это конструктор —
        // с типом возврата он не соберётся, да ещё и спрячет само имя типа (`Widget`).
        if (f.name == owner.name) {
            ++out.skipped_type;
            return {};
        }
        const std::string ret = cpp_type(f.ret, !f.is_native);
        if (ret.empty()) {
            ++out.skipped_type;
            return {};
        }
        // Значение по умолчанию в C++ обязано идти хвостом. Enforce это не требует
        // (`SetHeading(float yaw, float dt = -1, float maxSpeed)`), поэтому считаем
        // справа: как только встретился параметр без значения, всё левее его теряет своё.
        std::vector<bool> keep_default(f.params.size(), false);
        for (std::size_t i = f.params.size(); i-- > 0;) {
            const bool tail_ok = i + 1 == f.params.size() || keep_default[i + 1];
            keep_default[i] = tail_ok && !f.params[i].default_value.empty() &&
                              simple_default(f.params[i].default_value);
        }

        std::string decl_args;
        std::string def_args;
        std::string pass;
        for (const auto [i, p] : std::views::enumerate(f.params)) {
            const std::string type = cpp_type(p.type, false);
            if (type.empty() || type == "void") {
                // `void` в параметре — это «любой тип» маршалируемого пути. Выразить его
                // типизированно нельзя, а тихо подменить — соврать.
                ++out.skipped_type;
                return {};
            }
            const std::string name = p.name.empty() ? std::format("p{}", i) : sanitize(p.name);
            if (i) {
                decl_args += ", ";
                def_args += ", ";
                pass += ", ";
            }
            decl_args += std::format("{} {}", type, name);
            def_args += std::format("{} {}", type, name);
            // Значение по умолчанию — только в объявлении: повтор в определении вне
            // класса запрещён.
            if (keep_default[static_cast<std::size_t>(i)]) {
                decl_args += " = " + p.default_value;
            }
            pass += name;
        }
        const char* how = f.is_native ? "call" : "proto";
        (f.is_native ? out.emitted_native : out.emitted_marshalled) += 1;
        return {std::format("    {} {}({}) const;\n", ret, sanitize(f.name), decl_args),
                std::format("template <graft::name_t N>\n{} {}_<N>::{}({}) const {{ return "
                            "this->template {}<{}, \"{}\">({}); }}\n",
                            ret, sanitize(owner.name), sanitize(f.name), def_args, how, ret,
                            f.name, pass)};
    }

    // Значение по умолчанию переносим, только если оно и в C++ значит ровно то же.
    // Именованные константы Enforce (RF_DEFAULT) в C++ не существуют — такой аргумент
    // просто станет обязательным, и это лучше, чем не собраться.
    static bool simple_default(const std::string& text) {
        if (text == "true" || text == "false") {
            return true;
        }
        return !text.empty() && std::ranges::all_of(text, [](char c) {
            return (std::isdigit(static_cast<unsigned char>(c)) != 0) || c == '-' || c == '.' ||
                   c == 'f';
        });
    }

    // Модель памяти скриптового класса. Правила — со слов автора движка, а не догадки:
    // Managed даёт поведение shared_ptr, наследники сущностей живут до удаления из мира,
    // а приватный деструктор означает «удерживать нельзя вовсе».
    // Запреты НАСЛЕДУЮТСЯ: если базу нельзя удерживать или создавать, то и наследника
    // тоже. Иначе PlayerIdentity выглядел бы обычным Managed, хотя его база объявлена
    // «C++ managed class, so script has no business managing the lifetime».
    // Сам класс, затем цепочка ОТ ЗАДАННОЙ базы. База задаётся отдельно, потому что у
    // класса их бывает несколько — по одной на ветку препроцессора.
    template <class Pick>
    bool inherited(const class_& k, const std::string& from, Pick pick) const {
        if (pick(k)) {
            return true;
        }
        std::unordered_set<std::string> seen{k.name};
        for (auto at = by_name_.find(from);
             at != by_name_.end() && seen.insert(at->second->name).second;) {
            if (pick(*at->second)) {
                return true;
            }
            at = by_name_.find(at->second->primary_base());
        }
        return false;
    }

    std::string lifetime_via(const class_& k, const std::string& base) const {
        if (inherited(k, base, [](const class_& c) { return c.private_dtor; })) {
            return "engine";
        }
        if (inherited(k, base, [](const class_& c) {
                return c.name == "IEntity" || c.name == "Entity" || c.name == "Object" ||
                       c.name == "EntityAI";
            })) {
            return "entity";
        }
        if (inherited(k, base, [](const class_& c) { return c.name == "Managed"; })) {
            return "managed";
        }
        return "plain";
    }

    std::string lifetime_of(const class_& k) const { return lifetime_via(k, k.primary_base()); }

    bool spawnable_of(const class_& k) const {
        return !inherited(k, k.primary_base(),
                          [](const class_& c) { return c.private_ctor; });
    }

    // Варианты базы, которые вообще можно выпустить.
    std::vector<base_variant> usable_bases(const class_& k) const {
        std::vector<base_variant> out;
        for (const base_variant& b : k.bases) {
            if (emitted_.contains(b.name) && by_name_.contains(b.name)) {
                out.push_back(b);
            }
        }
        return out;
    }

    // Базовый класс примеси — ПЕРВАЯ ветка наследования. Остальные не теряются:
    // их методы допечатываются прямо в примесь (extra_from_branches). Выбрать ветку
    // по-настоящему нельзя — дефайны запущенной игры неизвестны, — поэтому в C++
    // уезжает объединение, а не выбор.
    std::string base_clause(const class_& k) const {
        const std::vector<base_variant> bases = usable_bases(k);
        if (bases.empty()) {
            return "    graft::ref<N>\n";
        }
        return std::format("    {}_<N>\n", sanitize(bases.front().name));
    }

    // Поле класса как lvalue-прокси. Тип в объявлении не нужен: прокси берёт его из
    // контекста обращения, поэтому выпускаются ВСЕ поля, а не только выразимые. Поля,
    // объявленного в этой сборке игры нет, — у прокси просто exists() == false.
    std::string field_accessor(const class_& k, const field& f) const {
        if (!is_identifier(f.name) || reserved(f.name)) {
            return {};
        }
        // Метод с таким же именем уже занял место: перегрузки по возвращаемому типу в
        // C++ нет, и уступает поле — оно всегда доступно как obj["имя"_f].
        if (std::ranges::any_of(k.functions,
                                [&](const function& fn) { return fn.name == f.name; })) {
            return {};
        }
        return std::format(
            "    graft::field_proxy<{0}_<N>, \"{1}\"> {1}() const {{ return "
            "graft::field_proxy<{0}_<N>, \"{1}\">{{this->ptr}}; }}\n",
            sanitize(k.name), f.name);
    }

    // Всё, что объявлено по цепочке от заданной базы. Нужно, чтобы у класса с
    // несколькими ветками наследования (`#ifdef X class Man extends Person #else
    // class Man extends EntityAI`) не потерялась ни одна: наследуем первую, а методы
    // остальных дописываем прямо в примесь.
    void collect_chain(const std::string& from, std::vector<const function*>& out) const {
        std::unordered_set<std::string> seen;
        for (auto at = by_name_.find(from);
             at != by_name_.end() && seen.insert(at->second->name).second;) {
            for (const function& f : at->second->functions) {
                out.push_back(&f);
            }
            at = by_name_.find(at->second->primary_base());
        }
    }

    // Методы ВТОРЫХ и далее веток, которых нет в первой. Их и допечатываем.
    std::vector<const function*> extra_from_branches(const class_& k) const {
        const std::vector<base_variant> bases = usable_bases(k);
        if (bases.size() < 2) {
            return {};
        }
        std::vector<const function*> primary;
        collect_chain(bases.front().name, primary);
        const auto known = [&](const function& f) {
            if (std::ranges::any_of(k.functions,
                                    [&](const function& own) { return own.same_as(f); })) {
                return true;
            }
            return std::ranges::any_of(primary,
                                       [&](const function* have) { return have->same_as(f); });
        };
        std::vector<const function*> out;
        for (const base_variant& b : bases | std::views::drop(1)) {
            std::vector<const function*> chain;
            collect_chain(b.name, chain);
            for (const function* f : chain) {
                if (!known(*f) && std::ranges::none_of(out, [&](const function* have) {
                        return have->same_as(*f);
                    })) {
                    out.push_back(f);
                }
            }
        }
        return out;
    }

    // Модель памяти по ВСЕМ веткам сразу: берём самую строгую. Так запрет из ветки,
    // которой в этой сборке может и не быть, всё равно защищает — он только ЗАПРЕЩАЕТ
    // лишнее, а разрешить лишнего не может.
    std::string lifetime_clause(const class_& k) const {
        static constexpr std::array<const char*, 4> kByStrictness{"engine", "entity", "plain",
                                                                  "managed"};
        std::string strictest = "managed";
        const auto consider = [&](const std::string& kind) {
            const auto rank = [](const std::string& what) {
                return std::ranges::find(kByStrictness, what) - kByStrictness.begin();
            };
            if (rank(kind) < rank(strictest)) {
                strictest = kind;
            }
        };
        const std::vector<base_variant> bases = usable_bases(k);
        if (bases.empty()) {
            consider(lifetime_via(k, {}));
        }
        for (const base_variant& b : bases) {
            consider(lifetime_via(k, b.name));
        }
        return std::format(
            "    static constexpr graft::lifetime script_lifetime = graft::lifetime::{};\n",
            strictest);
    }

    // Классы модуля в порядке наследования: база раньше наследника.
    std::vector<const class_*> ordered() const {
        std::vector<const class_*> out;
        std::unordered_set<std::string> done;
        const auto visit = [&](auto&& self, const class_& k) -> void {
            if (done.contains(k.name)) {
                return;
            }
            done.insert(k.name);
            // ВСЕ варианты базы: примесь наследует ту, что выберет препроцессор, но
            // объявлены к этому моменту должны быть обе.
            for (const base_variant& b : k.bases) {
                const auto base = by_name_.find(b.name);
                if (base != by_name_.end() && emitted_.contains(b.name) &&
                    in_module(*base->second)) {
                    self(self, *base->second);
                }
            }
            out.push_back(&k);
        };
        for (const class_& k : all_.classes) {
            if (emitted_.contains(k.name) && in_module(k)) {
                visit(visit, k);
            }
        }
        return out;
    }

    const std::unordered_set<std::string>& emitted() const { return emitted_; }
    const std::unordered_map<std::string, const class_*>& by_name() const { return by_name_; }

private:
    const unit& all_;
    std::string_view module_;
    int rank_;
    std::unordered_map<std::string, const class_*> by_name_;
    std::unordered_set<std::string> emitted_;
};

}  // namespace

std::string mirror(const unit& all, std::string_view module, std::string_view include_prev) {
    const emitter e{all, module};
    const std::vector<const class_*> classes = e.ordered();
    coverage stats;

    std::string out = std::format(
        "// СГЕНЕРИРОВАНО `graft apigen` из скриптов игры — руками не править.\n"
        "// Источник истины: объявления `proto` в PDrive\\scripts, тот же файл, по которому\n"
        "// сигнатуру проверяет компилятор Enforce.\n"
        "//\n"
        "// Устройство. Каждый класс — примесь, параметризованная ИМЕНЕМ РЕАЛЬНОГО класса:\n"
        "//     Man_<N> : Person_<N> : graft::ref<N>,   struct Man : Man_<\"Man\"> {{}}\n"
        "// иначе метод базового класса искался бы на базе, а движок держит его ниже по\n"
        "// цепочке, и find_method(\"Person\", \"GetIdentity\") ничего бы не нашёл.\n"
        "//\n"
        "// Ссылка, пришедшая от движка, — ЧУЖАЯ: она действительна до конца текущего\n"
        "// вызова. Между тиками спрашивай заново либо держи в graft::borrowed.\n"
        "//\n"
        "// ЗДЕСЬ ОБЪЯВЛЕНО БОЛЬШЕ, ЧЕМ ЕСТЬ В ЛЮБОЙ ОТДЕЛЬНОЙ СБОРКЕ ИГРЫ. Часть API\n"
        "// объявлена под `#ifdef`, а какие дефайны были у ЗАПУЩЕННОЙ игры, узнать\n"
        "// неоткуда. Поэтому запечены ВСЕ ветки сразу: и методы, и поля, и обе цепочки\n"
        "// наследования — объединением, а не выбором.\n"
        "//\n"
        "// Чего в этой сборке нет, то не найдётся в рантайме: вызов вернёт нулевое\n"
        "// значение и строку в журнал, try_call даст miss::not_found, у поля\n"
        "// exists() будет false. Спросить заранее: obj.has<\"Метод\">().\n"
        "// Модуль: {}\n#pragma once\n#include \"graft/native.hpp\"\n",
        module.empty() ? std::string_view{"все"} : module);
    if (!include_prev.empty()) {
        out += std::format("#include \"{}\"\n", include_prev);
    }
    out += "\nnamespace graft::dayz {\n\n";

    for (const class_* k : classes) {
        out += std::format("struct {};\n", sanitize(k->name));
    }
    out += "\n";

    std::string bodies;
    for (const class_* k : classes) {
        out += std::format("template <graft::name_t N>\nstruct {}_ :\n", sanitize(k->name));
        out += e.base_clause(*k);
        out += "{\n";
        for (const function& f : k->functions) {
            if (const auto m = e.method(*k, f, stats)) {
                out += m.decl;
                bodies += m.def;
            }
        }
        // Методы других веток наследования: наследуем первую, остальные дописываем сюда.
        // Так объединение доезжает целиком, а чего нет в этой сборке игры — промахнётся
        // в рантайме со строкой в журнале.
        for (const function* f : e.extra_from_branches(*k)) {
            if (const auto m = e.method(*k, *f, stats)) {
                out += m.decl;
                bodies += m.def;
            }
        }
        for (const field& f : k->fields) {
            out += e.field_accessor(*k, f);
        }
        out += "};\n";
    }
    out += "\n";
    for (const class_* k : classes) {
        out += std::format("struct {0} : {0}_<\"{1}\"> {{\n", sanitize(k->name), k->name);
        out += e.lifetime_clause(*k);
        out += std::format("    static constexpr bool script_spawnable = {};\n}};\n",
                           e.spawnable_of(*k) ? "true" : "false");
    }
    // Тела — последними: только здесь все конкретные классы уже полные, а возвращаемые
    // типы в них от параметра шаблона не зависят и проверяются сразу.
    out += "\n";
    out += bodies;
    out += "\n}  // namespace graft::dayz\n";
    return out;
}

std::vector<std::string> mirror_modules(const unit& all) {
    // Порядок разбора скриптов движком; в нём же классы наследуют друг друга.
    static constexpr std::array<const char*, 5> order{"1_Core", "2_GameLib", "3_Game", "4_World",
                                                      "5_Mission"};
    std::vector<std::string> out;
    for (const char* module : order) {
        if (std::ranges::any_of(all.classes, [&](const class_& k) {
                return k.module == module &&
                       std::ranges::any_of(k.functions,
                                           [](const function& f) { return !f.is_static; });
            })) {
            out.emplace_back(module);
        }
    }
    return out;
}

coverage measure(const unit& all) {
    coverage out;
    const emitter e{all, {}};
    out.classes = all.classes.size();
    for (const class_& k : all.classes) {
        out.functions += k.functions.size();
        if (e.emitted().contains(k.name)) {
            ++out.emitted_classes;
            for (const function& f : k.functions) {
                e.method(k, f, out);
            }
        }
    }
    return out;
}

}  // namespace graft::enf
