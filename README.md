# KR_GRAFT

[![CI](https://github.com/KRdayzmodding/KR_GRAFTED/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/KRdayzmodding/KR_GRAFTED/actions/workflows/ci.yml)
[![Лицензия: GPL-3.0-or-later с исключением для плагинов](https://img.shields.io/badge/лицензия-GPL--3.0--or--later%20%2B%20plugin%20exception-blue)](#лицензия)
[![C++26 · clang-cl · Windows](https://img.shields.io/badge/C%2B%2B26-clang--cl%20%C2%B7%20Windows-orange)](CONTRIBUTING.md#1-окружение)

**Одна запись в C++ — и натив движку, и `proto native` скрипту. Ни одного зашитого адреса.**

**[Как участвовать](CONTRIBUTING.md)** · **[Обсуждения](https://github.com/KRdayzmodding/KR_GRAFTED/discussions)** · **[Сборки](https://github.com/KRdayzmodding/KR_GRAFTED/releases)** · **[Изменения](CHANGELOG.md)** · **[In English](README.en.md)**

Название — от прививки. Ты не вызываешь движок снаружи и не патчишь его по адресам:
твой C++ прирастает к нему и дальше живёт как его часть — движок ходит в твой код
напрямую, без посредника. Место сращения библиотека находит сама, по именам ванильных
нативов, поэтому патч игры двигает адреса, а не ломает прививку.

```cpp
#include <graft/native.hpp>

graft::i32 SeraphPing(graft::i32 token) { return token ^ 0x5E1AF; }

GRAFT_BINDINGS("1_Core") { bind.global<&SeraphPing>("SeraphPing"); }
GRAFT_ON_TICK(dt) { /* ... */ }
```

```
<игра>/dwmapi.dll                                хост, один на установку
@MYMOD/addons/MYMOD.pbo                          мод как обычно
@MYMOD/grafted/MYMOD.grafted.dll                 плагин — по соседству с addons
@MYMOD/scripts/1_Core/grafted_natives_MYMOD.c    объявления, печатает сборка
```

`graft` — механизм (namespace, макросы, каталоги, `graft.exe`).
`grafted` — результат (артефакты, сгенерированные объявления, `IsGrafted("MYMOD")`).

Словарь: подвой — движок, привой — плагин, сращение — точка регистрации, обвязка — guard.

## Устройство репозитория

| Где | Что |
|---|---|
| [INCLUDE/graft/](INCLUDE/graft/), [SRC/](SRC/) | сам graft: библиотека, `graft.exe`, хост `dwmapi.dll` |
| [cmake/](cmake/) | `graft.boot.cmake` (`graft_import`), `graft_plugin()`, покрытие |
| [tests/](tests/) | тестовое окружение: gtest, два плагина-фикстуры и мод со скриптовой сьютой |
| [examples/](examples/) | самостоятельные проекты-примеры, начиная с [hello](examples/hello/) |
| [RESEARCH/](RESEARCH/) | база знаний по движку: что найдено в бинаре и чем перепроверить |

Тесты и примеры не пересекаются: сборка библиотеки и тестов в `examples/` не заглядывает
вовсе, а примеры собираются своими проектами — ровно так, как их собирает пользователь.
Раньше вторым плагином фикстуры собирался пример, и правка примера роняла сьюту.

## Сборка

Нужен CMake 3.30+, Ninja и clang-cl. Собирается из «x64 Native Tools Command Prompt
for VS» (или Developer PowerShell) — clang-cl берёт оттуда SDK и STL.

```bat
cmake --preset release
cmake --build --preset release
```

Это **только graft**: библиотека, `graft.exe` и хост `dwmapi.dll`. Тесты и примеры в
`ALL` не входят и на обычной сборке не трогаются — у каждой части свой таргет:

| Команда | Что собирает | Куда кладёт |
|---|---|---|
| `cmake --build --preset release` | библиотека, `graft.exe`, `dwmapi.dll` | `out/release/graft/` |
| `cmake --build --preset tests` | фикстуры-плагины и юнит-тесты | `out/release/tests/` |
| `ctest --preset release` | прогон юнит-тестов | — |
| `cmake --build --preset examples` | четыре примера, каждый своим проектом | `out/release/examples/<имя>/` |
| `cmake --build --preset coverage` | покрытие (нужен `-DGRAFT_COVERAGE=ON`) | консоль + `build/debug/coverage/` |
| `cmake --build --preset mount` | поставить хост в `GRAFT_GAME_DIR` | `<игра>/dwmapi.dll` |
| `cmake --build --preset unmount` | снять хост оттуда же | — |

Артефакты и промежуточное разведены: в `build/<конфиг>/` — кэш, объектники и `.lib`,
в `out/<конфиг>/` — только то, что забирают руками:

```
out/release/graft/dwmapi.dll                          хост, чистой DLL
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
`.gitignore`), это штатный механизм CMake:

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

Весь проект мода — один CMakeLists:

```cmake
file(DOWNLOAD https://raw.githubusercontent.com/KRdayzmodding/KR_GRAFTED/main/cmake/graft.boot.cmake
     "${CMAKE_BINARY_DIR}/graft.boot.cmake")
include("${CMAKE_BINARY_DIR}/graft.boot.cmake")
graft_import(graft https://github.com/KRdayzmodding/KR_GRAFTED TAG main)

graft_plugin(mymod NAME MYMOD VERSION 1 SOURCES src/natives.cpp MODULES 3_Game)
```

```bat
cmake --preset release
cmake --build --preset release
```

> **`TAG main` — только пока нет релиза.** `main` движется, и бамп `GRAFT_ABI_VERSION`
> в нём отклонит уже собранные плагины: хост сверяет числа при загрузке. Как только
> выйдет первый тег, ставь его — `TAG v0.1.0`, — и обновляй осознанно, читая CHANGELOG.

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
`@MYMOD/grafted/` рядом с `addons/` — и всё. Хост ставится один раз:
`graft install <каталог игры>`. Готовый скрипт этого шага лежит в шаблоне —
[examples/hello/deploy.ps1](examples/hello/deploy.ps1).

В PBO объявления попасть обязаны, поэтому в дереве мода они лежат — но в репозитории им
делать нечего, источник истины один:

```gitignore
mod/*/scripts/*/grafted_natives_*.c
```

### Что публично, а что внутреннее

Публичны заголовки `INCLUDE/graft/`, которые тянет `graft/native.hpp`, — то, что попадает
внутрь твоего плагина. Всё остальное (`SRC/`, заголовки, которые `native.hpp` не тянет)
внутреннее и меняется без предупреждения.

Совместимость плагина с хостом держат два числа из `INCLUDE/graft/abi.h`:
`GRAFT_ABI_VERSION` (интерфейс хост↔плагин) и `GRAFT_LAYOUT_VERSION` (раскладка структур
движка). Хост при загрузке сверяет их и отклоняет плагин, собранный под другие, — молча
работать «почти правильно» он не станет. Бамп любого из них отмечен в
[CHANGELOG.md](CHANGELOG.md) отдельной строкой; на сегодня это `ABI 5`, `LAYOUT 2`.

### Журналы

Их два, и читатели у них разные:

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

**Пользовательский** — не наш файл вовсе. Строка мода уходит в журналы САМОЙ ИГРЫ, туда
же, куда ушла бы из скрипта: `print` — это её `Print`, `error` — её же `Error2`. Третьего
места админу помнить не надо:

```
<профиль>/script_2026-08-19_21-03-55.log     SCRIPT : [MYMOD] груз выдан игроку 76561…
<профиль>/crash_2026-08-19_21-03-55.log      Reason: [MYMOD] конфиг не читается
```

Имя плагина подставляется само, из `GRAFT_PLUGIN`. К записи в crash-лог движок дописывает
свой стек вызовов — это его формат, и убрать его нечем; из C++ он короткий, кадр мода да
`OnUpdate`. Обе функции возвращают, дошло ли до игры: пока движок не поднялся, строка
уходит в системный журнал с пометкой, а не теряется.

Как это работает: у `Print` и `Error2` нет класса, поэтому найти их через `FindClass` нельзя
— но движок сам проносит их мимо врезки, когда регистрирует свои глобальные нативы, и
хост запоминает пару «имя — импл». Тот же поиск по имени, что и везде в graft: патч игры
двигает адреса, а не ломает журнал. Своими руками файл в профиле не открыть: `$profile:`
из C++ не работает (следующий раздел).

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

## Монтирование и демонтирование

Монтируется ровно одна вещь — сам graft: `dwmapi.dll` рядом с exe игры. Моды кладёт и
убирает мододел, graft о них не знает.

```bat
graft install   "C:\DayZServer"    :: = cmake --build --preset mount
graft uninstall "C:\DayZServer"    :: = cmake --build --preset unmount
graft list      "C:\DayZServer"    :: что установлено
graft doctor    "C:\DayZServer"    :: почему не работает
```

- **Чужую `dwmapi.dll` не трогаем.** `install` на неё ругается и останавливается,
  `uninstall` её не удаляет: мирить два прокси мы не умеем и делать вид не будем.
- **Чужие моды не удаляем.** `uninstall` снимает хост и пустую `<игра>/grafted/`, а
  плагины, лежащие в модах, только перечисляет — без хоста они просто мертвы.
- **Строку `-mod=` правишь ты.** О твоём ярлыке мы не знаем.

## Лицензия

GRAFT — **GPL-3.0-or-later** ([LICENSE](LICENSE)) плюс **GRAFT plugin exception 1.0**
([LICENSE-EXCEPTION](LICENSE-EXCEPTION)). Два файла делят мир по одной линии:

| что ты делаешь | что обязан |
|---|---|
| пишешь мод на GRAFT — закрытый, платный, любой | **ничего** |
| инлайнишь заголовки `graft/*.hpp` в свой плагин | ничего |
| зовёшь хост через ABI, грузишься из `<мод>/grafted/` | ничего |
| раздаёшь сгенерённый `grafted_natives_MYMOD.c` в своём PBO | ничего, вывод генератора не покрыт |
| правишь файлы платформы и раздаёшь результат | GPLv3 на всё, включая новые файлы форка |
| пишешь свой хост вместо этого | GPLv3 — это не плагин, исключение не действует |

Смысл ровно один: **зарабатывать на своём поверх GRAFT можно без ограничений, закрыть
саму платформу — нельзя**. Форк остаётся открытым, поэтому продать его как платформу не
получится: любой пересоберёт то же самое бесплатно. Что сверху и своё — твоё.

Исключение — это additional permission по §7 GPL, механика как у Classpath Exception в
OpenJDK и Runtime Library Exception в GCC. Помечены
`GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0` файлы, которые попадают
внутрь плагина: все заголовки `INCLUDE/graft/` и клиентские `SRC/graft/*.cpp`, которые
линкует `graft_client`. Файлы хоста и инструмента — без исключения.

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
- **Сторонний код** — [THIRD_PARTY.md](THIRD_PARTY.md). MinHook и HDE (оба BSD-2) едут
  внутри `dwmapi.dll`, поэтому этот файл обязан лежать рядом с бинарником в поставке.

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
