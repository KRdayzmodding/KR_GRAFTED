<div align="center">
    <a href="https://github.com/KRdayzmodding/KR_GRAFTED"><img src="https://github.com/user-attachments/assets/96d2e5e9-2532-467b-a4af-03a473c5e369" /></a>
</div>

<div align="center">
    <a href="https://github.com/KRdayzmodding/KR_GRAFTED/releases/latest"><img src="https://img.shields.io/github/v/release/KRdayzmodding/KR_GRAFTED?label=%D1%80%D0%B5%D0%BB%D0%B8%D0%B7&color=success&style=for-the-badge" alt="релиз" /></a>
    <a href="https://github.com/KRdayzmodding/KR_GRAFTED/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/KRdayzmodding/KR_GRAFTED/ci.yml?branch=main&label=CI&style=for-the-badge" alt="CI" /></a>
    <a href="#лицензия"><img src="https://img.shields.io/badge/лицензия-GPL--3.0--or--later%20%2B%20plugin%20exception-blue?style=for-the-badge" alt="лицензия: GPL-3.0-or-later с исключением для плагинов" /></a>
    <a href="CONTRIBUTING.md#1-окружение"><img src="https://img.shields.io/badge/C%2B%2B26-clang--cl%20%C2%B7%20Windows-orange?style=for-the-badge" alt="C++26 · clang-cl · Windows" /></a>
</div>

<div align="center">
    <a href="CONTRIBUTING.md"><b>Как участвовать</b></a>
    &bull; <a href="https://github.com/KRdayzmodding/KR_GRAFTED/discussions"><b>Обсуждения</b></a>
    &bull; <a href="https://github.com/KRdayzmodding/KR_GRAFTED/releases"><b>Сборки</b></a>
    &bull; <a href="CHANGELOG.md"><b>Изменения</b></a>
    &bull; <a href="README.en.md"><b>In English</b></a>
</div>

<br />

<div align="center">
  📎DayZ C++ Mod-Loader/Framework
</div>

<div align="center">
  <sub>
    Built with love 
    &bull; Brought to you by <a href="https://github.com/KRdayzmodding">@KR</a>
    and other <a href="https://github.com/KRdayzmodding/KR_GRAFTED/graphs/contributors">contributors</a>
  </sub>
</div>

# Вступление

KR_GRAFTED — мод-лоадер и фреймворк для модов DayZ на C++. Код собирается в
DLL-плагин, который загружается вместе с модом. Скрипты мода вызывают функции плагина как
встроенные функции движка.

Плагину доступно то, чего нет в Enforce Script: стандартная и сторонние библиотеки C++, внутренние структуры движка и скорость нативного кода. На сервере мод работает без ограничений, клиент требует отключения BattlEye.

Плагину доступно то, чего нет в Enforce Script: стандартная и сторонние библиотеки C++, внутренние структуры движка и скорость нативного кода. На сервере мод работает без ограничений, клиент требует отключения BattlEye.

## Как это работает

Ядро KR_GRAFTED устанавливается один раз в папку игры или сервера, плагины лежат в папках
модов:

```
DayZServer/
├── DayZServer_x64.exe
├── hid.dll                          ядро KR_GRAFTED
└── @MYMOD/
    ├── addons/MYMOD.pbo             скрипты мода и объявления функций плагина
    └── grafted/MYMOD.grafted.dll    плагин мода
```

При запуске игра загружает `hid.dll`. Ядро находит плагины в папках `grafted/`
подключённых модов и регистрирует их функции в движке. Объявления `proto native`
генерируются при сборке плагина и упаковываются в PBO вместе со скриптами мода.

## Возможности

### Ядро

- Устанавливается один раз и работает с `DayZServer_x64.exe`, `DayZ_x64.exe` и
  `DayZDiag_x64.exe`.
- Загружает плагины модов из `-mod=` и `-serverMod=` без дополнительной настройки.
- Не использует фиксированные адреса: точки движка ищутся при каждом запуске.
- Изолирует ошибки плагинов: при падении или исключении функция возвращает значение по
  умолчанию, ошибка пишется в лог, игра продолжает работу.
- Отклоняет плагины несовместимой версии с указанием причины.
- Сообщает скриптам список загруженных плагинов через дефайны.
- Поставляется с утилитой `graft.exe` для установки, удаления и диагностики.

### Фреймворк

- Функции плагина пишутся на C++ без обёрток, типы преобразуются в типы Enforce Script
  автоматически.
- Объявления `proto native` генерируются при каждой сборке и совпадают с кодом плагина.
- Поддерживаются глобальные функции, методы скриптовых классов, новые и шаблонные классы.
- Блок `GRAFT_ON_TICK` выполняется каждый кадр, без вызова из скриптов.
- Скриптовый API игры доступен из C++ через сгенерированный интерфейс.
- Плагин находит функции движка по строкам, RTTI и сигнатурам, вызывает и перехватывает
  их.
- Из C++ доступны script-лог и crash-лог игры.
- graft подключается к CMake-проекту двумя функциями: `graft_import` и `graft_plugin`.

## Пример

Мод исключает подключение игроков с недопустимым ником. В Enforce Script нет регулярных
выражений, в отличие от стандартной библиотеки C++.

Код плагина (`src/plugin.cpp`):

```cpp
#include <regex>
#include <string_view>

#include <graft/native.hpp>

GRAFT_PLUGIN("MYMOD", 1);  // имя и версия плагина

// Ник: 3–16 символов. Первый — латинская буква,
// остальные — латинские буквы, цифры или подчёркивания.
bool IsValidNick(std::string_view nick) {
    static const std::regex pattern{R"(^[A-Za-z][A-Za-z0-9_]{2,15}$)"};
    return std::regex_match(nick.begin(), nick.end(), pattern);
}

// Функции для скриптов. Объявления попадут в модуль 3_Game.
GRAFT_BINDINGS("3_Game") {
    bind.global<&IsValidNick>("IsValidNick");
}
```

Сборка генерирует объявление в `scripts/3_Game/grafted_natives_MYMOD.c`:

```c
#ifdef GRAFTED_MYMOD
proto native bool IsValidNick(string p0);
#endif
```

Скрипт мода (`scripts/5_Mission/mymod.c`):

```c
modded class MissionServer
{
    override void InvokeOnConnect(PlayerBase player, PlayerIdentity identity)
    {
        super.InvokeOnConnect(player, identity);

        if (!IsValidNick(identity.GetPlainName()))
            g_Game.DisconnectPlayer(identity);
    }
}
```

Сборка и упаковка описаны в разделе [«Создание мода»](#создание-мода), готовые проекты — в
[examples/](examples/).

## Установка

### Сервер

1. Скачайте [последний релиз](https://github.com/KRdayzmodding/KR_GRAFTED/releases/latest) —
   архив с ядром `hid.dll` и утилитой `graft.exe`.
2. Установите ядро:

   ```bat
   graft install "C:\DayZServer"
   ```

   Утилита копирует `hid.dll` к `DayZServer_x64.exe` и создаёт папку `grafted/` для
   плагинов, не привязанных к модам. Сторонний файл с тем же именем не перезаписывается.
3. Подключите моды через `-mod=` или `-serverMod=`. Плагин поставляется внутри мода:

   ```
   @MYMOD/
   ├── addons/MYMOD.pbo
   └── grafted/MYMOD.grafted.dll
   ```

### Клиент

Установка та же, но ядро несовместимо с BattlEye: `DayZ_BE.exe` блокирует `hid.dll`, и
игра не запускается. Поэтому:

- запускайте `DayZ_x64.exe` или `DayZ_BE.exe -noBE`;
- подключайтесь только к серверам без античита.

Для полного удаления BattlEye: `<папка игры>\BattlEye\Uninstall_BattlEye.bat`.

Если античит всё же загрузится при подключении к серверу, ядро завершит игру с кодом `0xBE`
с указанием причины в лог: снять перехваты и выгрузить плагины на ходу нельзя.

Плагин выполняется с правами пользователя, поэтому устанавливайте моды только из
доверенных источников — см. [SECURITY.md](SECURITY.md).

> **Известная проблема.** С модом, объявляющим функции плагина, клиент падает при выходе —
> уже после завершения работы игры, оставляя минидамп. Причина пока не найдена,
> подробности — в [RESEARCH/theory/client.md](RESEARCH/theory/client.md).

### Проверка установки

```bat
graft list   "C:\DayZServer"   :: ядро и найденные плагины
graft doctor "C:\DayZServer"   :: поиск проблем: версии, конфликты, ошибки в логе
```

Лог ядра `graft_<дата>.log` пишется в папку `-profiles=`, а без неё — в зависимости от роли
процесса:

| Роль | Процессы | Папка лога |
|---|---|---|
| сервер | `DayZServer_x64.exe`, `DayZDiag_x64.exe -server` | рядом с exe |
| клиент | остальные | `%LOCALAPPDATA%\DayZ` |

### Удаление

```bat
graft uninstall "C:\DayZServer"
```

Команда удаляет ядро и пустую папку `grafted/`, не трогая стороннюю `hid.dll`. Моды и
параметры `-mod=` остаются на месте: без ядра плагины не загружаются.

### Команды graft.exe

| Команда | Назначение |
|---|---|
| `graft install <папка> [hid.dll]` | установить ядро |
| `graft uninstall <папка>` | удалить ядро |
| `graft list <папка>` | показать ядро и плагины |
| `graft doctor <папка>` | найти проблемы |
| `graft new <имя> <куда> [шаблон]` | создать проект из шаблона |
| `graft apigen <скрипты> <куда>` | построить C++-интерфейс к API игры |
| `graft protogen <плагин.dll> <скрипты>` | сгенерировать объявления для скриптов (вызывается сборкой) |

## Создание мода

### Требования

- Windows.
- Visual Studio 2022+ либо Build Tools for Visual Studio.
- clang-cl 17+.
- CMake 3.30+ и Ninja.

Сборку запускайте из «x64 Native Tools Command Prompt for VS» или Developer PowerShell —
только там заданы пути к заголовкам Windows SDK и стандартной библиотеки C++.

### Новый проект

Шаблон проекта — [examples/hello](examples/hello/): CMake-конфигурация, код плагина, мод и
скрипт развёртывания. Скопируйте пример или выполните
`graft new MYMOD <куда> <путь к examples/hello>`, затем замените `HELLO_GRAFT` на имя своего
мода во всех файлах и в названии папки `mod/HELLO_GRAFT`.

Структура проекта:

```
mymod/
├── CMakeLists.txt
├── src/
│   └── plugin.cpp                  код плагина
└── mod/MYMOD/
    ├── config.cpp                  конфигурация мода
    └── scripts/5_Mission/mymod.c   скрипты мода
```

Подключение graft в `CMakeLists.txt`:

```cmake
file(DOWNLOAD https://raw.githubusercontent.com/KRdayzmodding/KR_GRAFTED/main/cmake/graft.boot.cmake
     "${CMAKE_BINARY_DIR}/graft.boot.cmake")
include("${CMAKE_BINARY_DIR}/graft.boot.cmake")
graft_import(graft https://github.com/KRdayzmodding/KR_GRAFTED TAG v0.4.0)

graft_plugin(mymod NAME MYMOD VERSION 1 SOURCES src/plugin.cpp MODULES 3_Game)
```

`graft_import` скачивает graft по тегу и собирает его вместе с проектом, а `graft_plugin`
описывает плагин: имя, версию, исходники и модули для объявлений.

### Сборка и упаковка

```bat
cmake --preset release
cmake --build --preset release
```

Сборка выдаёт два файла:

```
MYMOD.grafted.dll                              → @MYMOD/grafted/
MYMOD.scripts/3_Game/grafted_natives_MYMOD.c   → в PBO мода
```

Упаковку и развёртывание graft не выполняет: в примерах это делает
[deploy.ps1](examples/hello/deploy.ps1), которому нужен MakePbo из Mikero DePboTools.

```bat
powershell -File deploy.ps1 -Game "C:\DayZServer"
```

Объявления перезаписываются при каждой сборке, так что править их вручную бесполезно.
Изменения плагина вступают в силу после перезапуска игры: функции регистрируются при
старте.

<details>
<summary>Дополнительные настройки сборки</summary>

- Скачанные версии graft кэшируются в `~/.graft`, путь меняется переменной `GRAFT_CACHE`.
- `-DGRAFT_SOURCE_DIR=<путь>` — взять graft из локальной папки, а не из сети.
- `-DGRAFT_TOOL=<путь к graft.exe>` — не собирать утилиту, а использовать готовую.
- `SCRIPTS_DIR` в `graft_plugin` задаёт каталог для объявлений: по умолчанию рядом с DLL, в
  примерах — внутри мода. Во втором случае добавьте в `.gitignore`:

  ```gitignore
  mod/*/scripts/*/grafted_natives_*.c
  ```

- Выходные папки и `CMAKE_MSVC_RUNTIME_LIBRARY` остаются под контролем проекта.

</details>

### Версии graft

В `graft_import` указывайте тег, например `v0.4.0`: плагин, собранный с `main`, может
оказаться несовместим с ядром из релиза. Примеры ссылаются на `main`, но в своём проекте
так делать не стоит.

Совместимость определяют два числа из [`INCLUDE/graft/abi.h`](INCLUDE/graft/abi.h); при
расхождении ядро отклоняет плагин и пишет причину в лог:

| Константа | Что описывает | v0.4.0 |
|---|---|---|
| `GRAFT_ABI_VERSION` | интерфейс ядра и плагина | 7 |
| `GRAFT_LAYOUT_VERSION` | структуры движка | 2 |

Смена значений отмечается в [CHANGELOG.md](CHANGELOG.md).

## Написание плагина

### Функции и типы

Функции регистрируются в `GRAFT_BINDINGS("3_Game")`, где `3_Game` — модуль, в который
попадут сгенерированные объявления. Имена параметров и описание необязательны:

```cpp
GRAFT_PLUGIN("MYMOD", 1);

bool Resettle(int slot, std::string_view zone, graft::obj player);

GRAFT_BINDINGS("3_Game") {
    bind.global<&IsValidNick>("IsValidNick");
    bind.global<&Resettle>("Resettle", "slot, zone, player", "Переселить игрока в зону.");
}
```

Результат для `Resettle`:

```c
// Переселить игрока в зону.
proto native bool Resettle(int slot, string zone, Class player);
```

Типы преобразуются автоматически:

| C++ | Enforce Script |
|---|---|
| `int`, `float`, `bool` | `int`, `float`, `bool` |
| `std::string_view` | `string` (аргумент, без копирования) |
| `std::string` | `string` (аргумент), `owned string` (возвращаемое значение) |
| `std::vector<T>` | `array<T>` (аргумент, копия) |
| `graft::array<T>` | `array<T>` (массив движка, без копирования) |
| `std::array<float, 3>`, `graft::vector` | `vector` |
| `T&` | `out T` |
| `graft::obj` | `Class` (любой объект) |
| `graft::ref<"Имя">` | объект скриптового класса `Имя` |
| `graft::value` | значение любого типа |

Подробнее, с примерами, — в [examples/minimal](examples/minimal/).

### Скриптовые классы

Методы скриптового класса реализуются на C++ и регистрируются через `bind.class_`:

```cpp
struct ExampleGraft : graft::script_object<"ExampleGraft"> {
    int Version() const { return 1; }
};

GRAFT_BINDINGS("1_Core") {
    bind.class_<ExampleGraft>().method<&ExampleGraft::Version>("Version");
}
```

Сам `class ExampleGraft {}` объявляется в скриптах мода (модуль `1_Core`), а сборка
допишет методы через `modded class`. С флагом `graft::fresh` класс генерируется целиком.

Наследуйте от `graft::script_object`, если методам нужен доступ к скриптовому объекту;
иначе на каждый объект создаётся отдельный экземпляр C++.

Шаблонные классы регистрируются через `bind.template_class`, и одна реализация работает
для всех параметров. Пример — `CppHashMap<K, V>` на основе `std::unordered_map` в
[examples/hashmap](examples/hashmap/).

### Код без вызова из скриптов

`GRAFT_ON_TICK` выполняется каждый кадр в потоке скриптов. Объекты игры доступны через
сгенерированный C++-интерфейс с теми же классами и методами, что в Enforce Script:

```cpp
#include "graft/dayz/3_Game.hpp"  // сгенерированный интерфейс к API игры
#include "graft/engine.hpp"
#include "graft/native.hpp"

int g_online = 0;

GRAFT_ON_TICK(dt) {
    const auto game    = graft::cast<graft::dayz::CGame>(graft::game());
    const auto players = graft::scratch<graft::array<graft::dayz::Man>>();

    if (!game || !players) {
        return;                   // игра ещё не загрузилась
    }

    players.clear();
    game.GetPlayers(players);     // то же, что GetGame().GetPlayers() в скрипте

    g_online = static_cast<int>(players.size());
}
```

`graft apigen` строит интерфейс по скриптам игры, распакованным на `P:` через DayZ Tools;
после патча команду нужно повторить:

```bat
graft apigen P:/scripts include/graft/dayz
```

Полный пример — в [examples/players](examples/players/).

### Логи

```cpp
graft::print("Игроку выдан груз: " + steam_id);  // script-лог игры, как Print
graft::error("Не удалось прочитать конфиг");     // crash-лог игры, как Error2
graft::log("Точка входа не найдена");            // лог graft
```

`print` и `error` добавляют в начало строки имя плагина:

```
script_2026-08-19_21-03-55.log    SCRIPT : [MYMOD] Игроку выдан груз: 76561…
crash_2026-08-19_21-03-55.log     Reason: [MYMOD] Не удалось прочитать конфиг
```

В собственный лог, `graft_<дата>.log` в папке профиля, graft пишет найденные точки движка,
загрузку и отклонение плагинов, сбои функций.

<details>
<summary>Формат лога graft</summary>

Строка состоит из времени с миллисекундами, разделителя ` | ` и текста, первый символ
которого задаёт тип записи:

| Символ | Значение |
|---|---|
| `!` | проблема: что-то не найдено, не установлено или отклонено; такие строки выводит `graft doctor` |
| `+` | функция зарегистрирована в движке; нативы самого ядра сведены в одну запись |
| `~` | замечание: регистрация отложена до появления класса, или сообщение плагина не попало в лог игры |
| без символа | ход запуска: версии, роль процесса, найденные точки движка |

Если класс так и не появился, в лог попадает сводка: число незарегистрированных функций и
первые шесть имён.

</details>

### Работа без плагина

Ядро определяет в скриптах общий `GRAFTED` и `GRAFTED_<ИМЯ>` для каждого загруженного
плагина. Недопустимые символы имени заменяются подчёркиванием: `my-mod 2` →
`GRAFTED_my_mod_2`. Через `#ifdef` мод подключает запасную реализацию на Enforce Script:

```c
#ifdef GRAFTED_MYMOD
    int sum = MyNativeSum(a, b);
#else
    int sum = SlowSumOnEnforce(a, b);
#endif
```

Объявления функций обёрнуты в тот же `#ifdef`, поэтому без загруженного плагина вызов вне
проверки ломает компиляцию скриптов.

### Пути к файлам

Префиксы `$profile:`, `$saves:` и `$CurrentDir:` понимает только скриптовый движок. В C++
`$profile:config.json` — имя с двоеточием, и NTFS трактует его как альтернативный поток:
запись проходит без ошибок, а файл остаётся пустым.

Относительные пути считаются от папки игры. Строки из скриптов проверяйте: `..`, `:` и
абсолютные значения отклоняйте, остальное склеивайте со своим базовым каталогом.

## Доступ к движку

Чего нет в скриптовом API, плагин находит в движке сам — по строкам, RTTI или сигнатурам,
при каждом запуске и без зашитых адресов:

| Задача | Где |
|---|---|
| Найти функцию или данные по строке, RTTI или сигнатуре — байтовой или в виде ассемблерного листинга | [`graft/scan.hpp`](INCLUDE/graft/scan.hpp), [`graft/asm.hpp`](INCLUDE/graft/asm.hpp) |
| Перехватить одну функцию или атомарно несколько. В форме `hook<&детур>` падение детура отменяет вызов, а не роняет игру | `graft::hook`, `graft::hook_all` ([`graft/engine.hpp`](INCLUDE/graft/engine.hpp)) |
| Вызвать метод движка, получить позицию объекта в мире | `scan::member_call` ([`graft/scan.hpp`](INCLUDE/graft/scan.hpp)), `graft::position` ([`graft/world.hpp`](INCLUDE/graft/world.hpp)) |
| Найти код, пишущий в заданную память, аппаратными точками останова — для исследования, не для релиза | [`graft/watch.hpp`](INCLUDE/graft/watch.hpp) |

Перехваты ставятся в `GRAFT_ON_LOAD()`: ядро уже загружено, а движок ещё не запустил потоки
и не начал компиляцию скриптов. Ограничения и примеры — в
[docs/engine-access.md](docs/engine-access.md).

### Привязка к ассемблерному коду

Функцию удобно искать по строке, на которую она ссылается, и проверять по фрагменту
листинга из IDA или x64dbg: после патча достаточно сравнить его с новым кодом.

<details>
<summary>Пример: смещение поля в структуре движка</summary>

```cpp
#include "graft/asm.hpp"
#include "graft/scan.hpp"

namespace scan = graft::scan;

// Начало функции из IDA или x64dbg. disp32 совпадает с любым числом:
// смещение меняется от версии к версии, поиск вернёт найденное значение.
constexpr auto kListing = scan::code<R"(
    sub  rsp,28h
    mov  rax,[rcx+disp32]
)">;

// Найденное смещение — в поле value результата.
std::expected<scan::found, graft::miss> find_field() {
    // Функция, использующая эту строку.
    const auto fn = scan::function_referencing("строка из нужной функции");
    
    if (!fn) {
        return std::unexpected(fn.error());
    }

    return scan::find_in_function(*fn, kListing);
}
```

При неудаче результат содержит причину:

- `miss::not_found`  — строка не найдена;
- `miss::ambiguous`  — строку используют несколько функций;
- `miss::unreadable` — память по адресу недоступна.

В этом случае отключите зависящую от поиска функциональность и запишите ошибку в лог через
`graft::to_string`.

Помимо инструкций, в листинге допустимы:

- заполнители `disp8`, `disp32`, `imm8` и `imm32`, не больше одного на сигнатуру;
- `reg64` вместо любого регистра;
- сырые байты: `db 48 8B`.

Поддерживаемые мнемоники перечислены в [`graft/asm.hpp`](INCLUDE/graft/asm.hpp), а
сигнатура из байтов задаётся через `scan::sig<"48 83 EC 28">`.

</details>

## Разработка graft

### Сборка из исходников

Для мода собирать graft отдельно не нужно: `graft_import` подключает его к проекту сам, а
ядро ставится из релиза. Окружение описано в разделе [«Требования»](#требования),
подробности — в [CONTRIBUTING.md](CONTRIBUTING.md#1-окружение).

```bat
cmake --preset release
cmake --build --preset release
```

По умолчанию собираются библиотека, `graft.exe` и `hid.dll`, остальное — отдельными
пресетами:

| Команда | Что собирает | Куда |
|---|---|---|
| `cmake --build --preset release` | библиотека, `graft.exe`, `hid.dll` | `out/release/graft/` |
| `cmake --build --preset tests` | тестовые плагины и юнит-тесты | `out/release/tests/` |
| `ctest --preset release` | запуск юнит-тестов | — |
| `cmake --build --preset examples` | примеры, каждый отдельным проектом | `out/release/examples/<имя>/` |
| `cmake --build --preset coverage` | отчёт о покрытии (нужен `-DGRAFT_COVERAGE=ON`) | консоль и `build/debug/coverage/` |
| `cmake --build --preset mount` | установка ядра в `GRAFT_GAME_DIR` | `<игра>/hid.dll` |
| `cmake --build --preset unmount` | удаление ядра из `GRAFT_GAME_DIR` | — |

<details>
<summary>Параметры CMake и локальные пресеты</summary>

| Параметр | По умолчанию | Назначение |
|---|---|---|
| `GRAFT_GAME_DIR` | пусто | папка игры или сервера для `mount` и `unmount` |
| `GRAFT_API_DIR` | пусто | готовый C++-интерфейс к API игры (см. `graft apigen`) |
| `GRAFT_TOOL` | пусто | готовый `graft.exe` вместо сборки утилиты в проекте мода |
| `GRAFT_BUILD_TESTS` | `ON` | подключить тесты |
| `GRAFT_BUILD_EXAMPLES` | `ON` | подключить примеры |
| `GRAFT_COVERAGE` | `OFF` | инструментирование для llvm-cov |

Локальные пути держите в `CMakeUserPresets.json` — он указан в `.gitignore`:

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

После этого доступен `--preset dev`.

Промежуточные файлы лежат в `build/<конфигурация>/`, результаты — в `out/<конфигурация>/`,
поэтому `debug` и `release` не пересекаются.

</details>

### Участие в разработке

Окружение, тесты и оформление коммитов описаны в [CONTRIBUTING.md](CONTRIBUTING.md).

- При первом pull request бот попросит подписать [соглашение контрибьютора](docs/CLA.md):
  вклад принимается под GPL-3.0-or-later с тем же исключением, и правообладатель вправе
  лицензировать его также на других условиях.
- Об уязвимостях сообщайте приватно, по [SECURITY.md](SECURITY.md), а не в issues.
- Вопросы задавайте в [Discussions](https://github.com/KRdayzmodding/KR_GRAFTED/discussions).
- Правила общения — в [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md), благодарности — в
  [CREDITS.md](CREDITS.md).

## Структура репозитория

```
.
├── INCLUDE/graft/   публичные заголовки для плагинов
├── SRC/             исходный код библиотеки, ядра и утилиты graft.exe
├── cmake/           CMake-модули: graft_import, graft_plugin, покрытие
├── examples/        примеры модов: hello, minimal, hashmap, players
├── docs/            документация: доступ к движку, соглашение контрибьютора
├── tests/           юнит-тесты, тестовые плагины и мод со скриптовыми тестами
└── RESEARCH/        исследования движка и скрипты для их проверки
```

## Лицензия

KR_GRAFTED распространяется по лицензии **[GPL-3.0-or-later](LICENSE)** с исключением для
плагинов **[GRAFT plugin exception 1.0](LICENSE-EXCEPTION)**.

| Что вы делаете | Что требуется |
|---|---|
| Пишете мод на GRAFT, даже закрытый или платный | **ничего** |
| Включаете заголовки `graft/*.hpp` в свой плагин | ничего |
| Используете ядро через ABI и загружаете плагин из `<мод>/grafted/` | ничего |
| Распространяете сгенерированный `grafted_natives_MYMOD.c` в своём PBO | ничего: вывод генератора лицензией не покрывается |
| Изменяете файлы платформы и распространяете результат | GPLv3 на всё, включая новые файлы |
| Пишете собственное ядро на замену | GPLv3: ядро не плагин, исключение на него не действует |

Коротко: на модах поверх GRAFT можно зарабатывать, но закрыть саму платформу нельзя —
любой её форк остаётся открытым, и продавать его бессмысленно.

Исключение оформлено как дополнительное разрешение (§7 GPL), подобно Classpath Exception
в OpenJDK и Runtime Library Exception в GCC, и действует только для файлов внутри плагина:
заголовков `INCLUDE/graft/` и клиентских `SRC/graft/*.cpp` из `graft_client` с меткой
`GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0`. Ядро и утилита
распространяются под обычной GPL.

### Оговорки

- **Бренд не лицензируется.** Форк нельзя называть GRAFT или KR_GRAFT: GPLv3 не даёт прав
  на товарные знаки (§7e).
- **Правила Bohemia Interactive действуют отдельно.** Разрешение GRAFT продавать мод не
  отменяет требований BI к монетизации модов DayZ.
- **Доработанную платформу можно держать на своих серверах, не публикуя изменения.** Без
  передачи копий нет распространения, а значит, и обязательств — так устроены все
  копилефт-лицензии, кроме AGPL.
- **Сторонний код.** В `hid.dll` входят safetyhook (BSL-1.0), Zydis и Zycore (MIT), поэтому
  ядро распространяется вместе с [THIRD_PARTY.md](THIRD_PARTY.md).


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
