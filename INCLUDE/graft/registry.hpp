// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <span>

#include "graft/container.hpp"

// Реестр: что регистрировать в движке и что объявлять в скрипте. Одна запись даёт и
// адрес impl, и текст объявления — разъехаться они не могут.
namespace graft {

// ── Реестр: что регистрировать в движке и что объявлять в скрипте ────────────
struct native {
    const char* class_name;         // nullptr — глобальная функция, иначе имя класса
    const char* name;          // имя в скрипте
    void* impl;                // адрес C++ функции
    bool is_static;            // метод объявлен static (у impl нет this)
    const char* ret;           // тип возврата в терминах Enforce
    const char* const* args;   // типы аргументов, nullptr-terminated (без this)
    const native* next;
    bool marshalled = false;   // объявлять как `proto` (движок зовёт через блок
                               // аргументов) вместо `proto native`
    // В каком скриптовом модуле печатать объявление. Игровые типы (Object, EntityAI,
    // PlayerBase) в 1_Core ещё не существуют, поэтому такие нативы объявляются в
    // 3_Game/4_World — регистрация всё равно одна и та же, контекст у модулей общий.
    const char* module = "1_Core";
    // false — объявление уже написано в скрипте руками, генератор его не трогает.
    // Нужно там, где генератор не выразит синтаксис: свои модификаторы, объявление
    // внутри уже существующего класса.
    bool generate = true;
    // Заголовок класса для объявления, если он отличается от имени регистрации:
    // "CppHashMap<Class K, Class V>" против класса "CppHashMap". Такой класс печатается
    // как `class ...`, а не `modded class ...`.
    const char* declare_as = nullptr;
};

// Типы аргументов приезжают из C-ABI nullptr-terminated массивом — там иначе нельзя.
// Наружу отдаём обычный range, чтобы по нему работали views и enumerate, а длину никто
// не считал заново в каждом месте.
inline std::span<const char* const> arg_names(const char* const* args) {
    std::size_t n = 0;
    while (args && args[n]) {
        ++n;
    }
    return {args, n};
}

namespace detail {

// Голова односвязного списка нативов. Список складывается статическими конструкторами
// блоков GRAFT_BINDINGS, поэтому и живёт указателем, а не контейнером: порядок
// инициализации между TU не определён, а тут он и не нужен.
inline const native*& head() {
    static const native* h = nullptr;
    return h;
}

}  // namespace detail
}  // namespace graft
