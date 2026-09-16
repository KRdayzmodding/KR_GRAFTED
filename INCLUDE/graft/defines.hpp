// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "graft/scan.hpp"

// Дефайн на каждый загруженный плагин: `#ifdef GRAFTED_<ИМЯ>` в скрипте мода.
// Только хост. Почему именно так и где это врезано — в SRC/graft/defines.cpp.
namespace graft::defines {

// Две точки движка, обе — методы enf::CAddon. Ищутся по форме, без единого адреса.
struct api {
    using add_path_fn   = void*(__fastcall*)(void* addon, const char* path);
    using add_define_fn = void*(__fastcall*)(void* addon, const char* name, bool enabled);

    add_path_fn   add_path   = nullptr; // куда врезаемся: аддон приходит первым аргументом
    add_define_fn add_define = nullptr; // что зовём: она и кладёт дефайн в аддон

    explicit operator bool() const { return add_path != nullptr && add_define != nullptr; }
};

api find(const std::vector<scan::view>& sections);

// Строка в формате движка (шапка из трёх u16 перед символами). Живёт до конца процесса.
const char* engine_string(std::string_view text);

// Имя дефайна для плагина: `GRAFTED_<ИМЯ>`, всё неидентификаторное — в подчёркивание.
// Inline: тем же правилом генератор (graft.exe) оборачивает объявления плагина в #ifdef,
// а defines.cpp в инструмент не линкуется.
inline std::string define_name(std::string_view plugin) {
    std::string out = "GRAFTED_";
    for (char c : plugin) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

void install(const std::vector<scan::view>& sections);

} // namespace graft::defines
