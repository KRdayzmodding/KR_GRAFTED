// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <string>
#include <string_view>
#include <vector>

// Разбор объявлений Enforce и зеркало движкового API на C++.
//
// Источник истины — ванильные скрипты игры (E:\DayZ\PDrive\scripts): там объявлены и
// `proto native`, и маршалируемые `proto`, и иерархия классов. Из них строится
// graft/dayz/*.hpp, где движковый API выглядит обычным C++:
//
//     graft::dayz::PlayerIdentity id = player.GetIdentity();
//     std::string steam = id.GetPlainId();
//
// Зачем генератор, а не «звать по имени руками». Звать-то можно и так (ref::call), но
// сигнатуру тогда каждый пишет по памяти, а движок зовётся по ней НАПРЯМУЮ: расхождение
// это не ошибка компиляции, а падение. Здесь сигнатура берётся из того же файла, по
// которому её проверяет компилятор Enforce.
//
// Живёт только в инструменте и тестах: в игре разбирать нечего.
namespace graft::enf {

// Условие препроцессора, под которым объявление существует. В ванильных скриптах
// встречаются только `#ifdef`/`#ifndef`/`#else`/`#endif` — ни одного составного `#if`,
// поэтому условие это конъюнкция простых «определён / не определён».
struct cond {
    std::string name;
    bool negated = false;
    friend bool operator==(const cond&, const cond&) = default;
};

// Пусто — «есть всегда».
using guard = std::vector<cond>;

// Заголовок класса тоже бывает под условием: `#ifdef X class Man extends Person #else
// class Man extends EntityAI #endif`. Выбирать ветку за пользователя нельзя — его сборка
// игры может быть любой, — поэтому храним все и печатаем все.
struct base_variant {
    std::string name;
    guard when;
    friend bool operator==(const base_variant&, const base_variant&) = default;
};

struct param {
    std::string type;           // "array<Man>", "string", "void" (= любой)
    std::string name;
    std::string default_value;  // как записано в скрипте; пусто, если его нет
    bool is_out = false;
};

struct function {
    std::string name;
    std::string ret;  // без модификаторов: owned/external — не часть типа
    std::vector<param> params;
    guard when;       // под каким условием препроцессора объявлена
    bool is_static = false;
    bool is_native = false;  // `proto native` (прямой вызов) против `proto` (маршалируемый)

    // Совпадают ли объявления. Нужно слиянию: один и тот же класс объявлен в нескольких
    // файлах, и повторы — норма.
    bool same_as(const function& other) const;
};

struct field {
    std::string type;
    std::string name;
    guard when;
};

struct class_ {
    std::string name;
    std::vector<base_variant> bases;  // вариантов может быть несколько — см. base_variant
    std::string module;               // 1_Core / 3_Game / ...; пусто — «любой»

    // База без условий, а если такой нет — первая по порядку. Нужна там, где вариант
    // выбирать не из чего: обход иерархии и порядок выпуска.
    const std::string& primary_base() const;
    std::vector<function> functions;
    std::vector<field> fields;
    // Скрипт умеет запрещать и создание, и удержание ссылкой — приватным конструктором
    // и приватным деструктором. Это не оформление, а объявленная автором модель памяти,
    // и донести её до C++ надо так, чтобы нарушение НЕ СОБИРАЛОСЬ.
    bool private_ctor = false;
    bool private_dtor = false;
};

struct unit {
    std::vector<class_> classes;
    std::vector<function> globals;
    // Все символы препроцессора, встреченные в скриптах. Генератор печатает их списком:
    // какие из них определены в КОНКРЕТНОЙ сборке игры, знает только её владелец.
    std::vector<std::string> conditions;
};

// Разбор одного файла. Всё, что не объявление `proto`, пропускается — тела, комментарии,
// препроцессор, enum-ы, typedef-ы.
unit parse(std::string_view source, std::string_view module = {});

// Слить: `modded class` и повторное `class X` дополняют один и тот же класс.
void merge(unit& into, unit&& more);

// Разобрать целое дерево скриптов (каталог с 1_Core/2_GameLib/...). Модуль берётся из
// первого сегмента пути.
unit parse_tree(const std::string& root);

// Зеркало на C++ для одного модуля. include_prev — заголовок предыдущего модуля, если
// классы этого наследуют оттуда.
std::string mirror(const unit& all, std::string_view module, std::string_view include_prev = {});

// Модули, в которых есть что отражать, в порядке зависимости.
std::vector<std::string> mirror_modules(const unit& all);

// Сколько объявлений разобрано и сколько выпущено — для отчёта генератора. Разница это
// не потеря, а осознанный отказ (неизвестный тип, статический метод, зарезервированное
// имя), и видеть её надо цифрой.
struct coverage {
    std::size_t classes = 0;
    std::size_t emitted_classes = 0;
    std::size_t functions = 0;
    std::size_t emitted_native = 0;
    std::size_t emitted_marshalled = 0;
    std::size_t skipped_static = 0;
    std::size_t skipped_type = 0;
};
coverage measure(const unit& all);

}  // namespace graft::enf
