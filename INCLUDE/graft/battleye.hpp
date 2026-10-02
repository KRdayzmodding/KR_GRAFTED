// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <string_view>

// BattlEye в клиентском процессе. Хост под ним не работает. Через лаунчер BE игра с нашей
// hid.dll не стартует вовсе — BE блокирует её загрузку (замер на 1.29, RESEARCH/theory/
// client.md). Если же BE оказался в процессе позже нас, хост роняет игру, а не делает вид,
// что ничего не случилось: откатить врезки безопасно нельзя, плагины не выгружаются.
//
// КАК УЗНАЁМ. Клиент BE движок грузит САМ и лениво: функция, зовущая
// LoadLibraryA("BattlEye\BEClient_x64.dll"), стоит на пути подключения к серверу, а не
// запуска игры (RESEARCH/theory/client.md). Поэтому ни имя родителя, ни состояние службы
// не годятся: лаунчер к этому моменту давно вышел, а служба общая для всех BE-игр. Верный
// признак один — BEClient_x64.dll оказался В ЭТОМ ПРОЦЕССЕ, и ловим мы именно его:
// уведомление загрузчика о загрузке модуля плюс проверка уже загруженных на момент
// регистрации (иначе между «проверили» и «подписались» остаётся щель).
//
// Заголовок без WinAPI ради проверяемости: сопоставление имени — чистая функция.
namespace graft::battleye {

inline constexpr std::wstring_view kClientModule = L"BEClient_x64.dll";

// Тот ли это модуль: имя файла или полный путь, регистр не важен (так различает и
// Windows, а загрузчик отдаёт имя так, как его написал вызывающий).
inline bool is_client_module(std::wstring_view name) {
    const std::size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) {
        name.remove_prefix(slash + 1);
    }
    if (name.size() != kClientModule.size()) {
        return false;
    }
    const auto lower = [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    };
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (lower(name[i]) != lower(kClientModule[i])) {
            return false;
        }
    }
    return true;
}

using found_fn = void (*)();

// Встать на наблюдение: on_found зовётся, когда BEClient_x64.dll появится в процессе, — и
// сразу, если он там уже есть. Зовут её под замком загрузчика, поэтому внутри нельзя
// грузить библиотеки и ждать другие потоки. false — наблюдение не встало (нет API
// загрузчика либо уже стоит); тогда продолжать клиентом нельзя, не зная, что BE придёт.
bool watch(found_fn on_found);

// Снять наблюдение. Хосту не нужно — он живёт до конца процесса; нужно тестам.
void unwatch();

// Действие хоста: записать причину и завершить процесс. TerminateProcess, а не ExitProcess:
// мы под замком загрузчика, и выход через него повис бы на детачах.
[[noreturn]] void kill();

} // namespace graft::battleye
