// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Этот файл линкуется в КАЖДЫЙ плагин, поэтому едет с исключением: мод на GRAFT ничего
// не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#include <format>
#include <string>
#include <string_view>

#include "graft/callout.hpp"
#include "graft/engine.hpp"
#include "graft/script.hpp"
#include "graft/types.hpp"

// Пользовательский журнал — это журналы САМОЙ ИГРЫ, а не наш файл.
//
// Тот, кто поставил мод, уже знает, куда смотреть: script_<метка>.log и crash_<метка>.log
// в профиле сервера. Заводить рядом третий файл значит требовать от него помнить ещё
// одно место, поэтому строка мода уходит туда же, куда ушла бы из скрипта:
//
//   graft::print(...)  ->  Print(...)      ->  script-лог
//   graft::error(...)  ->  Error2("", ...) ->  crash-лог
//
// КАК мы их зовём. Обе — ГЛОБАЛЬНЫЕ функции движка: класса у них нет, а значит нет и
// пути через FindFunctionIndex. Зато движок сам проносит их мимо врезки в момент
// РЕГИСТРАЦИИ — там есть и имя, и импл, и хост их запоминает (script::find_global). Это
// тот же поиск по имени, на котором стоит весь graft, только даром.
//
// Через модуль скриптов (ScriptModule.CallFunction) они НЕ достаются, и это проверено:
// GetGame().GameScript — модуль 3_Game, а Print с Error живут в 1_Core, и CallFunction
// по родителям не ходит. Отсюда прямой вызов, а не вызов через скрипт.
//
// ПОЧЕМУ ИМЕННО ЭТИ ДВЕ. Замерено на живом сервере, все три пути в crash-лог:
//   ErrorEx(текст, ERROR)  — пишет, но лепит впереди «[Класс::Метод] :: [ERROR] ::»,
//                            и класс там ЧУЖОЙ: тот скриптовый кадр, из которого нас
//                            позвали, а вовсе не наш плагин;
//   ErrorEx(текст, INFO)   — в crash-лог не попадает вовсе;
//   Error2("", текст)      — строка приходит как есть. Её и берём.
// Стек вызовов движок дописывает сам, любым из путей: это его формат записи, а не наш
// выбор — короткий (кадр мода и OnUpdate), но убрать его нечем.
//
// Ходит всё это по скриптовому потоку: своего у библиотеки нет, а тик и трамплин натива
// идут по нему же.
namespace graft::detail {
namespace {

// Print(void var) — маршалируемая глобаль: кадр вызова собирается вслепую, по донорским
// шаблонам. Для `void`-параметра это ровно то, что делает и сам движок: настоящую
// переменную он в любом случае берёт по фактическому аргументу.
bool say_to_script_log(const std::string& text) {
    void* impl = script::find_global("Print");
    if (!impl) {
        return false;
    }
    const value args[] = {value{text}};
    return call_proto(as_global(impl), nullptr, args).has_value();
}

// Error2(string title, string err) — а вот это `proto native`, обычный вызов двумя
// строками. Заголовок пустой: он от окна с ошибкой, которого на сервере нет.
bool say_to_crash_log(const std::string& text) {
    void* impl = script::find_global("Error2");
    if (!impl) {
        return false;
    }
    reinterpret_cast<void(__fastcall*)(const char*, const char*)>(impl)("", text.c_str());
    return true;
}

}  // namespace

// Строка от имени плагина. Не дошла до игры — не теряем: уходит в системный журнал с
// пометкой. Молчаливая потеря хуже строки не в том файле.
bool say(bool bad, std::string_view who, std::string_view line) {
    const std::string text = std::format("[{}] {}", who, line);
    if (bad ? say_to_crash_log(text) : say_to_script_log(text)) {
        return true;
    }
    log((bad ? "! " : "~ ") + text);
    return false;
}

}  // namespace graft::detail
