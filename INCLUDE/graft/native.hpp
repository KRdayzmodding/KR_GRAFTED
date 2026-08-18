// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#pragma once
// Объявление собственных `proto native` для DayZ прямо на C++.
//
//   graft::i32 Ping(graft::i32 token) { return token ^ 0x5E1AF; }
//
//   GRAFT_BINDINGS("1_Core") {                   // -> proto native int Ping(int p0);
//       bind.global<&Ping>("Ping");
//   }
//
// Одна запись в C++ даёт и адрес impl для движка, и текст объявления для скрипта
// (graft.exe генерит .c мода при сборке) — разъехаться они не могут.
//
// Путей два, и выбирается он сам по сигнатуре:
//   * `proto native` — движок зовёт напрямую, обычным x64 fastcall по сигнатуре
//     прототипа (у метода arg0 = this). Маршалинга нет: типы C++ ниже подобраны так,
//     что компилятор раскладывает их как движок;
//   * `proto` (маршалируемый) — включается, если в сигнатуре есть graft::value или
//     graft::param<"имя">. Движок передаёт блок аргументов, у каждого — тег типа.
//     Отсюда свои шаблонные классы (один импл на любые K и V) и out-аргументы.
// Проверено декомпиляцией ванильных нативов, см. re/out/diag/pseudo/ и README.
//
// Чего не умеет `proto native` (ограничение движка): out/inout у значений — компилятор
// Enforce отвергает ("Native functions don't support 'out' arguments"), для них есть
// маршалируемая форма; и возврат string без `owned` — там движок ждёт буфер.

// Заголовок-зонтик: подключает все части. Порядок здесь и есть порядок зависимостей.
//
//   name.hpp       имя типа как значение шаблона
//   types.hpp      str, vector, text, ref, value — то, что пишут в сигнатуре
//   enf_type.hpp   имя скриптового типа по типу C++
//   container.hpp  array / set / map / out — вьюхи на память движка
//   registry.hpp   struct native: что регистрируем и что объявляем
//   convert.hpp    перевод обычных типов C++ и сторож ABI
//   thunk.hpp      трамплины `proto native` + экземпляр класса на объект
//   marshal.hpp    трамплины `proto` (блок аргументов с тегами)
//   callout.hpp    вызов В ДРУГУЮ СТОРОНУ: движковые `proto` из C++
//   world.hpp      вход в объектный граф игры: spawn / глобальные переменные / владение
//   dispatch.hpp   выбор инстанциации шаблонного класса на вызове
//   bindings.hpp   GRAFT_BINDINGS / GRAFT_PLUGIN
#include "graft/bindings.hpp"
