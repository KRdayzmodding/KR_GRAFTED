# KR_GRAFTED

[![Релиз](https://img.shields.io/github/v/release/KRdayzmodding/KR_GRAFTED?label=релиз&color=success)](https://github.com/KRdayzmodding/KR_GRAFTED/releases/latest)
[![CI](https://github.com/KRdayzmodding/KR_GRAFTED/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/KRdayzmodding/KR_GRAFTED/actions/workflows/ci.yml)
[![Лицензия: GPL-3.0-or-later с исключением для плагинов](https://img.shields.io/badge/лицензия-GPL--3.0--or--later%20%2B%20plugin%20exception-blue)](#лицензия)
[![C++26 · clang-cl · Windows](https://img.shields.io/badge/C%2B%2B26-clang--cl%20%C2%B7%20Windows-orange)](CONTRIBUTING.md#1-окружение)

# **Абсолютно новый вид модов для DayZ, задающий новую эру моддинга**
Мод-лоадер, позволяющий стабильно и унифицированно загружать в игру «external» моды на C++.

**Blazingly-fast**, без ограничений и недостатков языка скриптов. 

Вы наделены полным контролем, ограниченным лишь вашей фантазией.

---


**[Как участвовать](CONTRIBUTING.md)** · **[Обсуждения](https://github.com/KRdayzmodding/KR_GRAFTED/discussions)** · **[Сборки](https://github.com/KRdayzmodding/KR_GRAFTED/releases)** · **[Изменения](CHANGELOG.md)** · **[In English](README.en.md)**

Что такое external мод?

```js
Это мод, подключаемый как .dll-файлик, который исполняется на уровне самого движка игры.

На сервер такой мод можно установить свободно
На клиенте нужно отключать античит BattlEye
```

## Простейший пример

Мод, который не пускает на сервер игроков с мусорным ником. Регулярных выражений в
Enforce нет вообще — а в C++ они лежат в стандартной библиотеке. Весь мод — три файла,
и средний из них создаёт инструментарий graft.

**C++ — `src/plugin.cpp`. Это весь плагин, целиком:**

```cpp
#include <regex>
#include <string_view>

#include <graft/native.hpp>

GRAFT_PLUGIN("MYMOD", 1);                       // паспорт DLL: имя и версия

// C++ функция, которая проверяет ник игрока: 3–16 символов, первый — латинская буква,
// дальше буквы, цифры и подчёркивание. Проверку берёт на себя стандартный regex
bool IsValidNick(std::string_view nick) {
    static const std::regex ok{R"(^[A-Za-z][A-Za-z0-9_]{2,15}$)"}; // regex — современный способ поиска паттернов
    return std::regex_match(nick.begin(), nick.end(), ok);
}

GRAFT_BINDINGS("3_Game") {                      // регистрация: имя в скрипте и модуль
    bind.global<&IsValidNick>("IsValidNick");
}
```

**Объявление — `mod/MYMOD/scripts/3_Game/grafted_natives_MYMOD.c`. Инструментарий graft создаёт его сам:**

```c
#ifdef GRAFTED_MYMOD
proto native bool IsValidNick(string p0);
#endif
```

**Скрипт — `mod/MYMOD/scripts/5_Mission/mymod.c`. Обычный мод, обычный вызов:**

```cpp
modded class MissionServer
{
    override void InvokeOnConnect(PlayerBase player, PlayerIdentity identity)
    {
        super.InvokeOnConnect(player, identity);

        if (!IsValidNick(identity.GetPlainName())) { g_Game.DisconnectPlayer(identity); }
    }
}
```

Со стороны скрипта `IsValidNick` — обычная движковая функция, которая выполнит свою работу на стороне C++-плагина.

Файловая структура выйдет следующим образом:

```
<игра>/hid.dll                         ядро, одно на установку
@MYMOD/addons/MYMOD.pbo                мод как обычно, объявления лежат внутри
@MYMOD/grafted/MYMOD.grafted.dll       плагин — по соседству с addons
```
Обратите внимание, что external моды кладутся в `grafted/` рядом с `addons/`.

## Устройство репозитория

| Где | Что |
|---|---|
| [examples/](examples/) | самостоятельные проекты-примеры, начиная с [hello](examples/hello/) |
| [docs/](docs/) | справочники: [доступ к коду движка](docs/engine-access.md) — поиск, врезка, вызов |
| [INCLUDE/graft/](INCLUDE/graft/), [SRC/](SRC/) | сам graft: библиотека, `graft.exe`, ядро `hid.dll` |
| [cmake/](cmake/) | `graft.boot.cmake` (`graft_import`), `graft_plugin()`, покрытие |
| [tests/](tests/) | тестовое окружение: gtest, два плагина-фикстуры и мод со скриптовой сьютой |
| [RESEARCH/](RESEARCH/) | база знаний по движку: что найдено в бинаре и чем перепроверить |

Тесты и примеры не пересекаются: сборка библиотеки и тестов в `examples/` не заглядывает
вовсе, а примеры собираются своими проектами — ровно так, как их собирает пользователь.

### Доступ к коду движка

Натив — не единственный способ. Бывает, что у метода нет `proto native`, его поведение
зашито в C++, а нужное поле лежит в структуре движка, и скрипту туда не дотянуться.
Плагин дотянется: он сам находит нужное место в движке, без единого зашитого адреса.

| Что нужно | Чем это делается |
|---|---|
| Найти функцию или поле в движке: по строке-маяку, по RTTI или по сигнатуре — байтами либо строчками ассемблера из отладчика | [`graft/scan.hpp`](INCLUDE/graft/scan.hpp), [`graft/asm.hpp`](INCLUDE/graft/asm.hpp) |
| Врезаться в функцию движка, по одной или пачкой. Врезку можно защитить: падение в её коде тогда отменит вызов, а не игру | `graft::hook`, `graft::hook_all` ([`graft/engine.hpp`](INCLUDE/graft/engine.hpp)) |
| Вызвать настоящий метод движка или узнать позицию объекта в мире | `scan::member_call` ([`graft/scan.hpp`](INCLUDE/graft/scan.hpp)), `graft::position` ([`graft/world.hpp`](INCLUDE/graft/world.hpp)) |
| Выяснить, кто пишет в память (аппаратная точка останова). Это инструмент для разбора, в поставку он не входит | [`graft/watch.hpp`](INCLUDE/graft/watch.hpp) |

Как этим пользоваться, чем это обходится и где ловушки — в справочнике
[docs/engine-access.md](docs/engine-access.md).

#### Привязка к ассемблерному коду

Когда у функции движка нет ни натива, ни скриптового объявления, плагин находит её сам: по
строке, которую она использует, и сверяет с листингом из отладчика. Адресов в коде нет —
после патча игры сверяется листинг, а не число.

```cpp
#include "graft/asm.hpp"
#include "graft/scan.hpp"

namespace scan = graft::scan;

// Начало функции из IDA или x64dbg, строчка в строчку. disp32 — «любое число»: его двигает
// патч игры, он же и ответ.
constexpr auto kListing = scan::code<R"(
    sub  rsp,28h
    mov  rax,[rcx+disp32]
)">;

// Смещение поля в движковой структуре — в `value`, то, что стояло на месте disp32.
std::expected<scan::found, graft::miss> find_field() {
    // Функция, которая использует эту строку.
    const auto fn = scan::function_referencing("строка-маяк из функции");
    if (!fn) {
        return std::unexpected(fn.error());
    }

    return scan::find_in_function(*fn, kListing);
}
```

Не нашлось — это не ноль, а причина в типе. `miss::not_found` — маяк пропал, `miss::ambiguous` —
его берут две разные функции, `miss::unreadable` — по адресу нечего читать: чинятся они
по-разному. Это «нет», а не повод взять ближайшее
подходящее: плагин выключает свою ветку и пишет причину в журнал (`graft::to_string`).

В листинге, кроме обычных инструкций: `disp8`, `disp32`, `imm8`, `imm32` — дырка (одна на
сигнатуру), `db 48 8B` — сырые байты, `reg64` — любой регистр. Список инструкций — в шапке
[`graft/asm.hpp`](INCLUDE/graft/asm.hpp). Те же байты можно записать строкой:
`scan::sig<"48 83 EC 28">`.

Игру пропатчили — открой функцию в IDA и сравни с листингом строка в строку. Врезаться в
найденную функцию — `graft::hook`; когда это безопасно — в
[docs/engine-access.md](docs/engine-access.md), «Когда врезка безопасна».

### В случае отсутствия плагина

При разработке можно заложить поведение мода на случай, если плагина нет.
Это нужно, когда мод на сервере есть, а плагин забыли положить или установили неправильно.
Для этого GRAFTED добавляет дефайны в игру:

`GRAFTED` — значит, что загружено ядро GRAFTED.

`GRAFTED_<ИМЯ ПЛАГИНА>` — значит, что загружен плагин с именем `<ИМЯ ПЛАГИНА>`. Плагин `my-mod 2` даёт `GRAFTED_my_mod_2`.

```c
#ifdef GRAFTED_MYMOD
    int fast = MyNativeSum(a, b);   // объявления из grafted_natives_MYMOD.c
#else
    int fast = SlowSumOnEnforce(a, b);
#endif
```

### Логирование

GRAFTED предоставляет несколько механизмов логирования:

```cpp
graft::log("скан не нашёл точку входа");        // системный: про библиотеку, свой файл
graft::print("груз выдан игроку " + steam_id);  // Print игры    -> script-лог
graft::error("конфиг не читается");             // Error2 игры   -> crash-лог
```

**Системный** — файл graft, про саму библиотеку: что нашлось, кого отклонили, чей натив
упал. Лежит там же, где script- и crash-логи игры, — в каталоге `-profiles=` (ключа нет —
рядом с exe), по файлу на запуск и в том же стиле имени:

```
<профиль>/graft_2026-08-19_21-03-55.log
```

Строка начинается со времени до миллисекунды, потом ` | ` и текст.
Первым знаком текста строка сама говорит, что это:

| Знак | Что значит |
| --- | --- |
| `!` | жалоба: что-то не нашлось, не встало или отклонено. Её же считает `graft doctor` |
| `+` | натив зарегистрирован в движке. Служебные глобали самого ядра сведены в одну строку |
| `~` | не жалоба, но отметить стоит: привязка отложена (класса ещё нет, повторится, когда он появится) либо строка плагина не дошла до `Print` игры и осела здесь |
| (без знака) | рассказ о ходе запуска: версии, роль процесса, найденные точки движка |

Жалоба на класс, так и не появившийся, одна на класс: сколько нативов осталось без привязки
и первые шесть имён.

**Пользовательский** — строка мода уходит в журналы САМОЙ ИГРЫ, туда
же, куда ушла бы из скрипта: `print` — это её `Print`, `error` — её же `Error2`.

```
<профиль>/script_2026-08-19_21-03-55.log     SCRIPT : [MYMOD] груз выдан игроку 76561…
<профиль>/crash_2026-08-19_21-03-55.log      Reason: [MYMOD] конфиг не читается
```

Имя плагина подставляется само, из `GRAFT_PLUGIN`. 

### Файлы и пути в нативах

Движковых префиксов (`$profile:`, `$saves:`, `$CurrentDir:`) для C++ не существует —
их понимает только скрипт. `std::ofstream` видит в них обычное имя файла, а `:` на NTFS
открывает **альтернативный поток**: запись проходит успешно, файл нулевой, данных нет.
Рабочий каталог процесса — каталог игры. Поэтому путь, пришедший из скрипта, проверяй
(`:`, `..`, абсолютный — отказ) и раскрывай в свой каталог сам.

Зеркало движкового API (когда C++ сам ходит в игру) — тоже отдельной командой, скрипты
игры сборка не ищет:

```bat
graft apigen P:/scripts include/graft/dayz
```

## Клиентская часть

Ядро может быть загружено и на клиент, однако для его функционирования требуется отключённый античит BattlEye.

Отключить его можно, запустив файл `<игра>\BattlEye\Uninstall_BattlEye.bat`.

- **Известная проблема:** клиент с модом, объявляющим натив плагина, при выходе из игры
  падает внутри движка (минидамп, игра к этому моменту уже всё сделала). Причина не
  установлена, подробности — в [RESEARCH/theory/client.md](RESEARCH/theory/client.md).

### BattlEye

**Под BattlEye ядро не работает.**

- Если BattlEye всё же окажется в процессе позже ядра, **ядро завершит игру** (код
  завершения `0xBE`) и запишет причину в журнал. Клиент BE движок грузит лениво, при
  заходе на сервер, поэтому проверка — не разовая на старте, а подписка на загрузку
  модуля `BEClient_x64.dll`. Не сумев встать на наблюдение, ядро на клиенте не запускается.
- Откатить врезки безопасно нельзя, плагины не выгружаются — поэтому завершение, а не
  «отключиться и играть дальше».

`graft install` на клиентской папке (там лежит `DayZ_BE.exe`) напоминает об этом сам.

## Совместимость плагина и ядра

Совместимость плагина с ядром держат два числа из `INCLUDE/graft/abi.h`:
`GRAFT_ABI_VERSION` (интерфейс ядро↔плагин) и `GRAFT_LAYOUT_VERSION` (раскладка структур
движка). Ядро при загрузке сверяет их и отклоняет плагин, собранный под другие, — молча
работать «почти правильно» оно не станет. Бамп любого из них отмечен в
[CHANGELOG.md](CHANGELOG.md) отдельной строкой; на сегодня это `ABI 7`, `LAYOUT 2`.

## Сборка

Нужен CMake 3.30+, Ninja и clang-cl. Собирается из «x64 Native Tools Command Prompt
for VS» (или Developer PowerShell) — clang-cl берёт оттуда SDK и STL.

```bat
cmake --preset release
cmake --build --preset release
```

Это **только graft**: библиотека, `graft.exe` и ядро `hid.dll`. Тесты и примеры в
`ALL` не входят и на обычной сборке не трогаются — у каждой части свой таргет:

| Команда | Что собирает | Куда кладёт |
|---|---|---|
| `cmake --build --preset release` | библиотека, `graft.exe`, `hid.dll` | `out/release/graft/` |
| `cmake --build --preset tests` | фикстуры-плагины и юнит-тесты | `out/release/tests/` |
| `ctest --preset release` | прогон юнит-тестов | — |
| `cmake --build --preset examples` | четыре примера, каждый своим проектом | `out/release/examples/<имя>/` |
| `cmake --build --preset coverage` | покрытие (нужен `-DGRAFT_COVERAGE=ON`) | консоль + `build/debug/coverage/` |
| `cmake --build --preset mount` | поставить ядро в `GRAFT_GAME_DIR` | `<игра>/hid.dll` |
| `cmake --build --preset unmount` | снять ядро оттуда же | — |

Артефакты и промежуточное разведены: в `build/<конфиг>/` — кэш, объектники и `.lib`,
в `out/<конфиг>/` — только то, что забирают руками:

```
out/release/graft/hid.dll                             ядро, чистая DLL
out/release/graft/graft.exe                           инструмент
out/release/tests/SIXW_GRAFT.grafted.dll              фикстуры
out/release/tests/SIXW_GRAFT.scripts/1_Core/...       их объявления
out/release/examples/hello/HELLO_GRAFT.grafted.dll    пример
out/release/examples/hello/HELLO_GRAFT.scripts/3_Game/...
```

`debug` и `release` лежат в `out/` рядом и не мешают друг другу. PBO проект не собирает
и папки `@МОД` не раскладывает: сборка даёт DLL и каталог `<ИМЯ>.scripts` с
объявлениями, дальше это дело мододела и его обычного инструмента.

Каталоги в дереве повторяют это же деление: [SRC/](SRC/) — сам graft, [tests/](tests/) —
его тесты, [examples/](examples/) — примеры. Ни один таргет из одной части не собирает
другую.

| Опция | По умолчанию | Что делает |
|---|---|---|
| `GRAFT_GAME_DIR` | пусто | каталог игры или сервера для `mount` / `unmount` |
| `GRAFT_API_DIR` | пусто | готовое зеркало движкового API (см. `graft apigen`) |
| `GRAFT_TOOL` | пусто | готовый `graft.exe` вместо сборки инструмента в проекте мода |
| `GRAFT_BUILD_TESTS` | `ON` | подключать каталог тестов |
| `GRAFT_BUILD_EXAMPLES` | `ON` | подключать каталог примеров |
| `GRAFT_COVERAGE` | `OFF` | инструментация для llvm-cov |

Личных путей в репозитории нет — свои держи в `CMakeUserPresets.json` (он в
`.gitignore`):

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "dev",
      "inherits": "release",
      "cacheVariables": { "GRAFT_GAME_DIR": "C:/DayZServer" }
    }
  ]
}
```

Дальше вместо `--preset release` пишешь `--preset dev`.

## Свой плагин

Проект того самого мода из начала — четыре файла, из них твоих три:

```
mymod/
  CMakeLists.txt                             graft_import + graft_plugin
  src/plugin.cpp                             C++: функция и её регистрация
  mod/MYMOD/config.cpp                       обычный конфиг мода DayZ
  mod/MYMOD/scripts/5_Mission/mymod.c        вызов из скрипта
```

`CMakeLists.txt` — целиком:

```cmake
file(DOWNLOAD https://raw.githubusercontent.com/KRdayzmodding/KR_GRAFTED/main/cmake/graft.boot.cmake
     "${CMAKE_BINARY_DIR}/graft.boot.cmake")
include("${CMAKE_BINARY_DIR}/graft.boot.cmake")
graft_import(graft https://github.com/KRdayzmodding/KR_GRAFTED TAG v0.4.0)

graft_plugin(mymod NAME MYMOD VERSION 1 SOURCES src/plugin.cpp MODULES 3_Game)
```

```bat
cmake --preset release
cmake --build --preset release
```

> **Тег, а не `main`.** `main` — ветка разработки самого graft: она движется, и бамп
> `GRAFT_ABI_VERSION` в ней отклонит уже собранные плагины — ядро сверяет числа при
> загрузке. Мод держи на теге и переезжай на следующий осознанно, прочитав
> [CHANGELOG](CHANGELOG.md).

`graft_import` качает graft (или берёт локальный исходник, если передать
`-DGRAFT_SOURCE_DIR=C:/src/graft`) и кэширует его в `~/.graft` — второй мод той же
версией ничего не качает заново. В твоё дерево graft при этом не лезет: каталогов не
создаёт, выходные пути и рантайм (`CMAKE_MSVC_RUNTIME_LIBRARY`) берёт твои. Вместе с
библиотекой один раз собирается `graft.exe`; если он уже стоит — `-DGRAFT_TOOL=путь`, и
инструмент собираться не будет.

Сборка кладёт рядом:

```
build/MYMOD.grafted.dll                                   плагин
build/MYMOD.scripts/3_Game/grafted_natives_MYMOD.c        объявления для PBO
```

Объявления — артефакт сборки, как `.pb.cc` у protobuf: руками их не пишут и разъехаться
с C++ они не могут. **Мод graft не собирает**: копируешь `.c` в свой PBO, `.dll` в
`@MYMOD/grafted/` рядом с `addons/` — и всё. Ядро ставится один раз:
`graft install <каталог игры>`. Готовый скрипт этого шага лежит в шаблоне —
[examples/hello/deploy.ps1](examples/hello/deploy.ps1).

В PBO объявления попасть обязаны, поэтому в дереве мода они лежат — но в репозитории им
делать нечего, источник истины один:

```gitignore
mod/*/scripts/*/grafted_natives_*.c
```

## Монтирование и демонтирование

Монтируется ровно одна вещь — ядро: `hid.dll` рядом с exe игры. Моды кладёт и
убирает мододел, ядро о них не знает.

```bat
graft install   "C:\DayZServer"    :: = cmake --build --preset mount
graft uninstall "C:\DayZServer"    :: = cmake --build --preset unmount
graft list      "C:\DayZServer"    :: что установлено
graft doctor    "C:\DayZServer"    :: почему не работает
```

- **Чужую `hid.dll` не трогаем.** `install` на неё ругается и останавливается,
  `uninstall` её не удаляет: мирить два прокси мы не умеем и делать вид не будем.
- **Чужие моды не удаляем.** `uninstall` снимает ядро и пустую `<игра>/grafted/`, а
  плагины, лежащие в модах, только перечисляет — без ядра они просто мертвы.
- **Строку `-mod=` правишь ты.** О твоём ярлыке мы не знаем.

## Лицензия

GRAFT — **GPL-3.0-or-later** ([LICENSE](LICENSE)) плюс **GRAFT plugin exception 1.0**
([LICENSE-EXCEPTION](LICENSE-EXCEPTION)).

| что ты делаешь | что обязан |
|---|---|
| пишешь мод на GRAFT — закрытый, платный, любой | **ничего** |
| инлайнишь заголовки `graft/*.hpp` в свой плагин | ничего |
| зовёшь ядро через ABI, грузишься из `<мод>/grafted/` | ничего |
| раздаёшь сгенерённый `grafted_natives_MYMOD.c` в своём PBO | ничего, вывод генератора не покрыт |
| правишь файлы платформы и раздаёшь результат | GPLv3 на всё, включая новые файлы форка |
| пишешь своё ядро вместо этого | GPLv3 — это не плагин, исключение не действует |

Смысл ровно один: **зарабатывать на своём поверх GRAFT можно без ограничений, закрыть
саму платформу — нельзя**. Форк остаётся открытым, поэтому продать его как платформу не
получится: любой пересоберёт то же самое бесплатно. Что сверху и своё — твоё.

Исключение — это additional permission по §7 GPL, механика как у Classpath Exception в
OpenJDK и Runtime Library Exception в GCC. Помечены
`GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0` файлы, которые попадают
внутрь плагина: все заголовки `INCLUDE/graft/` и клиентские `SRC/graft/*.cpp`, которые
линкует `graft_client`. Файлы ядра и инструмента — без исключения.

### Оговорки

- **Имя не лицензируется.** Код форкать можно, называть результат GRAFT или KR_GRAFT —
  нет. Лицензия прав на название не выдаёт (GPLv3 §7e), и репутация проекта не должна
  уезжать вместе с чужой сборкой.
- **Правила Bohemia действуют поверх.** То, что GRAFT разрешает продавать твой мод, само
  по себе разрешением от BI не является: монетизация модов DayZ живёт по их правилам, и
  моя лицензия их не отменяет и не может.
- **Приватный форк лицензией не ловится.** Гоняешь изменённую платформу только на своих
  серверах, никому не раздаёшь — распространения нет, обязательств нет. Это свойство
  всего копилефта, кроме AGPL; AGPL здесь была бы лекарством хуже болезни.
- **Сторонний код** — [THIRD_PARTY.md](THIRD_PARTY.md). safetyhook (BSL-1.0), Zydis и
  Zycore (оба MIT) едут внутри `hid.dll`, поэтому этот файл обязан лежать рядом с
  бинарником в поставке.

### Вклад

Присылая PR, ты отдаёшь его под GPL-3.0-or-later с тем же исключением (inbound =
outbound) и разрешаешь правообладателю проекта лицензировать твой код и на других
условиях — иначе смена или расширение лицензии в будущем потребует обзвона всех
контрибьюторов. Формально это оформлено в [docs/CLA.md](docs/CLA.md); подписывается один
раз, ботом, в первом же PR.

Как собрать, как устроены тесты и что не примут — [CONTRIBUTING.md](CONTRIBUTING.md).
Правила общения — [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Уязвимости **не в трекер**, а
приватно: [SECURITY.md](SECURITY.md). Вопросы «как этим пользоваться» —
в [Discussions](https://github.com/KRdayzmodding/KR_GRAFTED/discussions).


```js
ㅤ                ▁▂▁
ㅤ              □░▒▓▒░□     ▂▃▂
ㅤ                """_ "__□░▒▓▒░□        v0x002,
ㅤ           ▁▂▂▁  __." --" """       ╓─<grafted>>
ㅤ         □░▒▓▓▒░□___ "_._  □░▒░□_.">╬═<"0x1"␀"
ㅤ           """"     "" , \_.   "_. ."
ㅤ                  ▁▂▁ _"__ \__./ ."
ㅤ                □░▒▓▒░□"  "_    ./
ㅤ                  '''       (    )
ㅤ       ╭───────────────────╯░░░░░░╰──────────────╮
ㅤ       ┝───github.com/KRdayzmodding/KR_GRAFTED───┥
ㅤ       │   by [KR] 6wingSeraph  &  Maintainers   │
ㅤ       ╰──┰───────────────────────────────────┰──╯
ㅤ  ▄████  ██▀███   ▄▄▄        █████▒▄▄▄█████▓▓█████ ▓█████▄
ㅤ ██▒ ▀█▒▓██ ▒ ██▒▒████▄    ▓██   ▒ ▓  ██▒ ▓▒▓█   ▀ ▒██▀ ██▌
ㅤ▒██░▄▄▄░▓██ ░▄█ ▒▒██  ▀█▄  ▒████ ░ ▒ ▓██░ ▒░▒███   ░██   █▌
ㅤ░▓█  ██▓▒██▀▀█▄  ░██▄▄▄▄██ ░▓█▒  ░ ░ ▓██▓ ░ ▒▓█  ▄ ░▓█▄   ▌
ㅤ░▒▓███▀▒░██▓ ▒██▒ ▓█   ▓██▒░▒█░      ▒██▒ ░ ░▒████▒░▒████▓
ㅤ ░▒   ▒ ░ ▒▓ ░▒▓░ ▒▒   ▓▒█░ ▒ ░      ▒ ░░   ░░ ▒░ ░ ▒▒▓  ▒
ㅤ  ░   ░   ░▒ ░ ▒░  ▒   ▒▒ ░ ░          ░     ░ ░  ░ ░ ▒  ▒
ㅤ░ ░   ░   ░░   ░   ░   ▒    ░ ░      ░         ░    ░ ░  ░
ㅤ      ░    ░           ░  ░                    ░  ░   ░
```