# Примеры

Четыре самостоятельных проекта. Каждый — **рабочий мод целиком**: C++, PBO со своим
скриптом и строка в script-логе, по которой видно, что всё сошлось. Собирается,
пакуется и запускается независимо от репозитория библиотеки.

| Пример | Что показывает | Что видно в логе |
|---|---|---|
| [hello/](hello/) | одна функция C++ и один вызов из скрипта — весь цикл разработки | `[HELLO_GRAFT] Hello from C++, DayZ!` |
| [minimal/](minimal/) | все типы, которые ходят через границу, и метод скриптового класса | `[EXAMPLE_GRAFT] FillSquares(4) -> 0,1,4,9` |
| [hashmap/](hashmap/) | шаблонный скриптовый класс: `CppHashMap<K,V>` — это `std::unordered_map` в C++ | `[EXAMPLE_HASHMAP] обход: zombies=12 bandits=3` |
| [players/](players/) | обратное направление: C++ сам будится по тику и ходит в игру | `[EXAMPLE_PLAYERS] тик C++ работает 10.3 с` |

Все четыре проверены вместе, одним сервером и одной командой:
`-mod=@EXAMPLE_GRAFT;@EXAMPLE_HASHMAP;@EXAMPLE_PLAYERS;@HELLO_GRAFT;` — четыре плагина в
одном процессе не мешают друг другу.

## Раскладка, одинаковая у всех четырёх

```
<пример>/
├─ CMakeLists.txt                          graft_import + graft_plugin
├─ CMakePresets.json                       release и debug, артефакты в out/<пресет>
├─ deploy.ps1                              собрать -> PBO -> разложить по игре
├─ .gitignore                              build/ out/ dist/
├─ src/                                    C++: паспорт плагина и нативы
└─ mod/<ИМЯ>/                              ОБЫЧНЫЙ МОД DAYZ
   ├─ $PBOPREFIX$                          <ИМЯ>
   ├─ config.cpp                           CfgPatches/CfgMods: какие модули откуда брать
   └─ scripts/
      ├─ 1_Core/ (или 3_Game/)             класс-хозяин, если нужен, + grafted_natives_<ИМЯ>.c
      └─ 5_Mission/                        демонстрация: отсюда нативы зовут
```

Скриптовый код мода виден целиком, включая написанный не рукой: `grafted_natives_<ИМЯ>.c`
лежит в моде и в репозитории. Его печатает сборка, читая собранную DLL, — в примерах
сразу в мод (`SCRIPTS_DIR` в CMakeLists.txt), поэтому он всегда свежий, а расхождение с
C++ покажет обычный `git diff`.

В своём проекте так делать не обязательно: по умолчанию объявления ложатся рядом с DLL,
а в мод их забирает `deploy.ps1` перед упаковкой PBO — эта раскладка показана в
[hello](hello/), с которого `graft new` разворачивает шаблон.

## Как это устроено под всеми четырьмя

```
DayZ.exe
 └─ dwmapi.dll                    ХОСТ, один на игру: graft install <каталог игры>
      ├─ @EXAMPLE_GRAFT/grafted/EXAMPLE_GRAFT.grafted.dll      ПЛАГИН, едет со своим модом
      ├─ @EXAMPLE_HASHMAP/grafted/EXAMPLE_HASHMAP.grafted.dll
      ├─ @EXAMPLE_PLAYERS/grafted/EXAMPLE_PLAYERS.grafted.dll
      └─ @HELLO_GRAFT/grafted/HELLO_GRAFT.grafted.dll
```

Хост находит точки движка **по именам ванильных нативов** в загруженном образе — адресов
и смещений нигде нет, поэтому патч игры их двигает, а не ломает. Пересобирать хост не
нужно никому и никогда: мододел собирает только свой плагин.

Плагин отдаёт хосту адреса своих функций, и дальше движок зовёт их НАПРЯМУЮ. Через
границу между модулями ходят только регистрация, поиск движкового метода (мемоизирован)
и журнал — раскол на модули не стоил ничего, это замерено.

Скриптовая половина — обычный PBO. Строки `proto native` в нём руками не пишут: их
печатает сборка, читая собранную DLL, поэтому C++ и скрипт разъехаться не могут.

## Что где потрогать

| Возможность | Где | Строчка |
|---|---|---|
| глобальная функция | [hello](hello/src/hello.cpp) | `bind.global<&HelloGraft>("HelloGraft")` |
| `int/float/bool`, строки, `vector` | [minimal](minimal/src/natives.cpp) | `string_view` -> `string`, `array<float,3>` -> `vector` |
| массив копией | [minimal](minimal/src/natives.cpp) | `int ExampleSum(vector<int>)` |
| массив БЕЗ копии (вьюха, `std::ranges`) | [minimal](minimal/src/natives.cpp) | `ranges::count_if(graft::array<int>, ...)` |
| `out`-массив: C++ растит и заполняет | [minimal](minimal/src/natives.cpp) | `values.resize(n)` / `values.set(i, v)` |
| метод скриптового класса | [minimal](minimal/src/natives.cpp) | `struct ExampleGraft : graft::script_object<"ExampleGraft">` |
| шаблонный скриптовый класс | [hashmap](hashmap/src/cpp_hashmap.cpp) | `bind.template_class<Table>("CppHashMap<Class K, Class V>")` |
| состояние объекта живёт в C++ | [hashmap](hashmap/src/cpp_hashmap.cpp) | `std::unordered_map` в поле, освобождение — само, вместе со скриптовым объектом |
| точка входа по тику | [players](players/src/main.cpp) | `GRAFT_ON_TICK(dt)` |
| зеркало движкового API | [players](players/src/main.cpp) | `graft::dayz::CGame`, `man.GetIdentity()` |
| поле движкового объекта | [players](players/src/main.cpp) | `world["m_DebugMonitorEnabled"_f]` |
| буфер под `out` движка | [players](players/src/main.cpp) | `graft::scratch<graft::array<Man>>()` |
| строка в script-лог игры из C++ | [players](players/src/main.cpp) | `graft::print("новый игрок: ...")` |
| строка в crash-лог игры из C++ | [minimal](minimal/src/natives.cpp) | `graft::error(...)` — это `Error2` самой игры |

Чего в примерах нет намеренно: матрица ABI по каждому типу, поля объектов на любую
глубину, замеры горячего пути. Это [tests/](../tests/) — их гоняет сьюта `seraph::graft`
на своём сервере, и примером такое быть не должно.

## Как запустить любой из них

```bat
graft install "C:\DayZServer"      :: один раз на игру: хост рядом с exe
cmake --preset release
cmake --build --preset release     :: <ИМЯ>.grafted.dll + <ИМЯ>.scripts в out/release/
```

Без пресета тоже можно, но тип сборки задай явно — пустой `CMAKE_BUILD_TYPE` даёт
отладочную сборку: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`.

Дальше — забрать `<ИМЯ>.scripts/*` в `mod/<ИМЯ>/scripts/`, упаковать PBO и положить
рядом с игрой:

```
@<ИМЯ>/addons/<ИМЯ>.pbo
@<ИМЯ>/grafted/<ИМЯ>.grafted.dll
```

Эти три шага есть готовым скриптом у каждого примера — `deploy.ps1`:

```bat
powershell -File deploy.ps1 -Game "C:\DayZServer"
```

У всех четырёх одинаковый набор: `CMakePresets.json` (release/debug, артефакты в
`out/<пресет>`), `.gitignore` и `deploy.ps1`. Копируешь каталог — получаешь рабочий
проект, а не заготовку.

Запуск: `-mod=@<ИМЯ>;`, смотреть script-лог в профиле сервера — строки, которые плагин
пишет через `graft::print`, лежат прямо в нём, вперемешку со скриптовыми. Рядом там же
системный журнал библиотеки `graft_<метка>.log`. Не завелось — `graft doctor "C:\DayZServer"`
и этот журнал.

## Собрать все четыре разом

```bat
cmake --build --preset examples   :: каждый пример — своим проектом, как у пользователя
```

Артефакты — в `out/release/examples/<имя>/`. Этот таргет и сторожит примеры от гниения:
любая правка публичного API, которая ломает пример, красит сборку.
