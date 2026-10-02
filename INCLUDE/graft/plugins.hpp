// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "graft/abi.h"

// Логика загрузчика без единого вызова WinAPI: что грузить, годится ли оно и не спорят
// ли плагины за имена. Отделено ради проверяемости — эти функции решают, заведётся мод
// у человека или нет, и гонять их ради этого игру не нужно (tests/plugins_test.cpp).
namespace graft::plugins {

// Куда клиент кладёт свои плагины, относительно каталога игры. Одно имя на хост и на
// `graft`: сторона плагина определяется папкой, а не содержимым DLL, поэтому серверный
// плагин, лежащий по-старому в grafted/, на клиенте не заведётся никогда.
inline constexpr std::wstring_view client_dir = L"grafted\\client";

// Папки модов из командной строки процесса: `-mod=` и `-serverMod=`, склеенные в один
// список в порядке появления, без пустых и без повторов. Порядок важен: он же задаёт
// порядок загрузки плагинов и, значит, победителя при коллизии имён.
std::vector<std::string> mod_dirs(std::string_view cmdline);

// Каталог профиля сервера из `-profiles=` — тот, куда игра кладёт script- и crash-логи.
// Пусто — ключа нет (тогда журналы ложатся рядом с exe). Хвостовой разделитель снят.
std::string profile_dir(std::string_view cmdline);

// Запись объединённого реестра: натив и плагин, который его принёс.
struct entry {
    const graft_native_desc* desc = nullptr;
    const char* owner = nullptr;  // имя плагина
};

struct collision {
    std::string class_name;   // пусто — глобальная функция
    std::string name;
    std::string first;   // плагин, за которым имя осталось
    std::string second;  // плагин, которому отказано
};

// Годится ли плагин к загрузке: GRAFT_OK либо код ошибки.
std::uint32_t check(const graft_plugin_info& info);

// Слияние реестров. Имя занимает первый по порядку; остальным отказ, и каждый отказ
// попадает в out. Возвращаются принятые записи.
std::vector<entry> merge(const std::vector<entry>& all, std::vector<collision>& out);

std::string describe(const collision& c);

// Класс так и не появился, и его методы остались без привязки: ОДНА строка-жалоба на
// класс — «!», имя, сколько методов и первые из них. Раньше было по строке на метод:
// плагин с двумя десятками методов отсутствующего класса засыпал журнал, а `graft doctor`
// считал каждую строку отдельной жалобой.
std::string describe_unbound(std::string_view                     class_name,
                             const std::vector<std::string_view>& methods);

// Почему плагин не загружен — строка для журнала: какие числа разошлись (свои и хоста),
// у кого они старее и что с этим делать. code — итог entry либо check.
std::string reason(const graft_plugin_info& info, std::uint32_t code);

}  // namespace graft::plugins
