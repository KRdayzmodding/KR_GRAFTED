// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "graft/plugins.hpp"

// Загрузка плагинов. Только хост: плагин про загрузчик ничего не знает.
namespace graft::loader {

// Строка отчёта на каждый найденный модуль — то, что показывает `graft doctor` и что
// уходит в журнал.
struct row {
    std::string name;
    std::uint32_t version = 0;
    std::string path;
    std::uint32_t status = 0;  // GRAFT_OK либо код отказа
    std::uint32_t count = 0;   // сколько нативов принёс
};

// Найти и загрузить всё. Звать ПОСЛЕ установки хуков: см. комментарий в loader.cpp.
void load(const std::wstring& game_dir);


// Объединённый реестр после слияния — по нему идёт регистрация.
const std::vector<plugins::entry>& registry();
const std::vector<row>& rows();
const std::vector<plugins::collision>& collisions();

// Отметка времени в журнале: ею размечены шаги установки, чтобы порядок был виден.
void mark(const char* what);

// ── Тик ──────────────────────────────────────────────────────────────────────
// Список обработчиков держит хост, а не каждый плагин: скриптовый натив GraftTick один
// на процесс, и веером по плагинам должен расходиться он.
void add_tick(void (*fn)(float));
void run_ticks(float dt);
std::size_t tick_count();

// Корень объектного графа игры. Приходит вместе с тиком — движок отдать глобальную
// переменную отказывается (см. graft::try_global), а объект CGame больше взять неоткуда.
void set_script_root(void* game);
void* script_root();

// ── Падения нативов ──────────────────────────────────────────────────────────
// Кривой плагин больше не уносит сервер: вызов возвращает нулевое значение, а сюда
// приходит запись о том, кто именно упал. Счётчик — чтобы это было видно не только в
// журнале: молчаливо переживать падения хуже, чем падать.
void note_fault(void* impl, std::uint32_t code, const void* at, const char* what);
std::size_t fault_count();
std::string last_fault();

// Хранилище результата — им наполняет load(). Отдельно от загрузки, чтобы класс Graft
// линковался и в инструмент, где загрузчика нет (см. loader_state.cpp).
namespace detail {
std::vector<plugins::entry>& registry_ref();
std::vector<row>& rows_ref();
std::vector<plugins::collision>& collisions_ref();
}  // namespace detail

}  // namespace graft::loader
