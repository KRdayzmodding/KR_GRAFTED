# minimal: свой плагин на библиотеке graft

Минимальный шаблон плагина: несколько нативов на C++ (глобальные функции, возврат
строки, метод класса, работа с контейнерами) + PBO, который их зовёт. Копируешь
каталог, переименовываешь — дальше правишь только [src/natives.cpp](src/natives.cpp),
[src/plugin.cpp](src/plugin.cpp) и [mod/](mod/).

Или разверни шаблон командой `graft new MYMOD E:\source` — она копирует
[hello](../hello/), в котором есть ещё пресеты, `.gitignore` и `deploy.ps1`.

```
CMakeLists.txt                                    graft_import + graft_plugin
CMakePresets.json                                 release и debug, артефакты в out/<пресет>
src/plugin.cpp                                    паспорт плагина: GRAFT_PLUGIN("EXAMPLE_GRAFT", 1)
src/natives.cpp                                   нативы на все типы + привязка
mod/EXAMPLE_GRAFT/$PBOPREFIX$                     EXAMPLE_GRAFT
mod/EXAMPLE_GRAFT/config.cpp                      CfgPatches/CfgMods: 1_Core и 5_Mission
mod/EXAMPLE_GRAFT/scripts/1_Core/example_class.c  class ExampleGraft {} — хозяин нативных методов
mod/EXAMPLE_GRAFT/scripts/1_Core/grafted_natives_EXAMPLE_GRAFT.c
                                                  ^ объявления нативов, печатает сборка
mod/EXAMPLE_GRAFT/scripts/5_Mission/example_demo.c   вызовы и Print в лог
deploy.ps1                                        собрать -> PBO -> разложить по игре
.gitignore                                        build/ out/ dist/
```

Сгенерированный файл лежит прямо в моде и в репозитории (`SCRIPTS_DIR` в
[CMakeLists.txt](CMakeLists.txt)): пример должен показывать мод целиком. Вот он весь —
ровно то, что напечатала сборка, читая собранную DLL:

```c
proto native int ExampleAdd(int p0, int p1);
proto native owned string ExampleGreet(string p0);
proto native int ExampleSum(array<int> p0);
proto native int ExampleCountAbove(array<int> p0, int p1);
proto native int ExampleFillSquares(out array<int> p0, int p1);
proto native vector ExampleScale(vector p0, float p1);

modded class ExampleGraft
{
    proto native int Version();
}
```

Обрати внимание на `modded`: методы **добавляются** к типу, а объявить сам тип — твоё
дело, это [scripts/1_Core/example_class.c](mod/EXAMPLE_GRAFT/scripts/1_Core/example_class.c)
на две строки. У шаблонного класса иначе — см. [hashmap](../hashmap/), там генератор
печатает класс целиком.

## Из чего состоит

| Файл | Правишь? |
|---|---|
| [src/natives.cpp](src/natives.cpp) | **да** — сюда пишешь свои нативы |
| [src/plugin.cpp](src/plugin.cpp) | **да** — имя и версия плагина, одна строчка |
| [mod/EXAMPLE_GRAFT/](mod/EXAMPLE_GRAFT/) | **да** — config.cpp, класс-хозяин, демонстрация в 5_Mission |
| `out/<пресет>/EXAMPLE_GRAFT.scripts/<модуль>/grafted_natives_<ПЛАГИН>.c` | нет, печатает сборка |
| [CMakeLists.txt](CMakeLists.txt) | ничего: `graft_import` тянет библиотеку сам |
| [CMakePresets.json](CMakePresets.json), [deploy.ps1](deploy.ps1) | по вкусу: сборка и раскладка по игре |
| [../README.md](../README.md) | что ещё есть в примерах |

## Хост и плагины

Рядом с exe живёт **один** `dwmapi.dll` — это хост: он находит точки движка, ставит
хуки и грузит плагины. Пересобирать его не нужно никому и никогда.

Твой мод собирается в **плагин** — `<ИМЯ>.grafted.dll`. Он едет вместе со своим модом:
папка `grafted/` по соседству с `addons/`. Плагинов может быть сколько угодно, они не
мешают друг другу и могут быть собраны разными компиляторами.

```
DayZ.exe
 └─ dwmapi.dll                      хост, один на игру: graft install <каталог игры>
      ├─ @MYMOD/addons/MYMOD.pbo               мод как обычно
      ├─ @MYMOD/grafted/MYMOD.grafted.dll      плагин — по соседству с addons
      ├─ @OTHER/grafted/OTHER.grafted.dll
      └─ grafted/EXAMPLE_GRAFT.grafted.dll     та же папка рядом с exe, для разработки
```

В `grafted/` нет ничего кроме плагинов: хост открывает оттуда КАЖДЫЙ `.dll`.

**Скорость от этого не страдает.** Движок зовёт трамплин плагина напрямую — при
регистрации он получает сырой адрес, и дальше ходит по нему сам. Через границу между
модулями ходят только регистрация, поиск движкового метода (мемоизирован) и журнал.
Замерено: раскол на модули не стоил ничего, см. `Perf_HotPathBudget` в сьюте основного
проекта.

## Стиль: обычный C++

Своих типов в сигнатурах нет. Пишешь то же, что писал бы в любом другом проекте, —
перевод через границу делает трамплин:

| Пишешь | В скрипте |
|---|---|
| `int` / `float` / `bool` | `int` / `float` / `bool` |
| `std::string_view` | `string` (аргумент, без копии) |
| `std::string` | `string` (аргумент) / `owned string` (возврат) |
| `std::vector<T>` | `array<T>` (аргумент, копия) |
| `std::array<float, 3>` или `graft::vector` | `vector` |
| `T&` — неконстантная ссылка | `out T` |

```cpp
int ExampleAdd(int a, int b) { return a + b; }

std::string ExampleGreet(std::string_view who) {
    return std::format("hello, {}", who.empty() ? "world" : who);
}

int ExampleSum(std::vector<int> values) {
    return std::ranges::fold_left(values, 0, std::plus{});
}

GRAFT_BINDINGS("1_Core") {
    bind.global<&ExampleAdd>("ExampleAdd")
        .global<&ExampleGreet>("ExampleGreet")
        .global<&ExampleSum>("ExampleSum");
}
```

Своих типов ровно два, и оба там, где обычным C++ не выразить:

- **`graft::array<T>` / `set` / `map`** — ВЬЮХА на движковую память. Это не контейнер,
  а ручка: данные принадлежат движку, и копии не происходит. Она полноценный range,
  так что `std::ranges` по ней работают. Нужна копия и обычный C++ — бери
  `std::vector<T>`; нужна скорость и запись обратно — бери вьюху.
- **`graft::ref<"Класс">`** — ссылка на скриптовый объект. `graft::script_object<"Имя">`
  — то же самое, но как база своего класса: тогда объект приезжает как `this`.

Ещё есть `graft::vector` — тот же скриптовый `vector`, что и `std::array<float, 3>`
(раскладка одна), но с `.x/.y/.z` и арифметикой. Для геометрии читается лучше; выбор
между ними ни на что не влияет.

Классу не нужно ничего наследовать и не нужно знать про DayZ:

```cpp
struct StringTable {                                   // обычный класс C++
    std::unordered_map<std::string, std::string> data; // обычное поле
    void Set(std::string k, std::string v) { data.insert_or_assign(std::move(k), std::move(v)); }
    int Count() const { return static_cast<int>(data.size()); }
};

GRAFT_BINDINGS("1_Core") {
    bind.class_<StringTable>("ExampleTable")
        .method<&StringTable::Set>("Set")
        .method<&StringTable::Count>("Count");
}
```

Указателей и буферов в нативах нет вообще.

### Строку возвращай через graft::text::of

```cpp
graft::text Greet(std::string_view who) { return graft::text::of("hello, {}", who); }  // 0 аллокаций
std::string  Greet(std::string_view who) { return std::format("hello, {}", who); }     // 1 аллокация
```

`graft::text::of` форматирует прямо в арену, в которой строка и так должна дожить до
конца вызова. Возврат `std::string` читается привычнее и стоит одной аллокации — на
строке до 15 байт не стоит и её (SSO). Разница измерена: 21% на строке в 64 байта.

Аргументы бери `std::string_view` — он смотрит в память движка и не копирует ничего.

### Имя метода и поля пиши в типе, а не строкой

```cpp
o.call<graft::vector, "GetOrigin">();   // а не o.call<graft::vector>("GetOrigin")
node.field<int, "m_id">();              // а не node.field<int>("m_id")
```

Разница — 22 раза. Форма со строкой ищет дескриптор заново на каждом вызове (обход
контекстов, хеш в движке, `VirtualQuery`); форма с именем в типе — один раз за процесс.
Строковые формы оставлены для случая, когда имя действительно приезжает в рантайме.

## Сборка и запуск

```bat
graft install "C:\DayZServer"       :: один раз на игру: хост рядом с exe
cmake --preset release
cmake --build --preset release      :: DLL и EXAMPLE_GRAFT.scripts в out/release/
powershell -File deploy.ps1 -Game "C:\DayZServer"    :: объявления -> мод -> PBO -> игра
```

Запуск с `-mod=@EXAMPLE_GRAFT;` — и в script-логе профиля появляется вся демонстрация:

```
[EXAMPLE_GRAFT] Add(2, 40) = 42
[EXAMPLE_GRAFT] Greet("dayz") = hello, dayz
[EXAMPLE_GRAFT] Version() = 1
[EXAMPLE_GRAFT] Sum([1,2,39]) = 42
[EXAMPLE_GRAFT] CountAbove([4,8,15], 5) = 2
[EXAMPLE_GRAFT] FillSquares(4) -> 0,1,4,9
[EXAMPLE_GRAFT] Scale(<1,2,3>, 2.5) = <2.500000, 5.000000, 7.500000>
```

Без пресета тоже можно, но тип сборки задай явно: пустой `CMAKE_BUILD_TYPE` даёт
отладочную сборку. `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`.

Весь CMake проекта — это:

```cmake
graft_import(graft https://github.com/KRdayzmodding/KR_GRAFTED TAG main)

graft_plugin(example_graft
    NAME EXAMPLE_GRAFT VERSION 1
    SOURCES src/natives.cpp src/plugin.cpp
    MODULES 1_Core 3_Game)
```

`NAME` попадает в три места сразу: имя DLL, паспорт плагина и суффикс служебного натива
`NativeDispose_<NAME>`.

## Мод

Своей рукой в моде написаны ровно два файла:

| Файл | Что в нём |
|---|---|
| [scripts/1_Core/example_class.c](mod/EXAMPLE_GRAFT/scripts/1_Core/example_class.c) | `class ExampleGraft {}` — пустой хозяин для нативных методов |
| [scripts/5_Mission/example_demo.c](mod/EXAMPLE_GRAFT/scripts/5_Mission/example_demo.c) | `modded class MissionBase.OnInit` — зовёт каждый натив и печатает ответ |

Третий файл, `scripts/1_Core/grafted_natives_EXAMPLE_GRAFT.c`, печатает сборка. В PBO он
лежать обязан, в репозитории ему делать нечего — за это отвечает строка
`mod/*/scripts/*/grafted_natives_*.c` в [.gitignore](.gitignore).

Генератор раскладывает объявления по модулям сам: натив с игровым типом в сигнатуре
(`Object`, `EntityAI`) описывается в отдельном блоке `GRAFT_BINDINGS("3_Game")` и
попадает в `EXAMPLE_GRAFT.scripts/3_Game/...` — в 1_Core таких типов ещё нет.

Не завелось — `graft doctor <каталог игры>` и `graft.log` рядом с exe: там видно, что
нашлось и что зарегистрировалось.

## Дальше

| Пример | Что добавляет |
|---|---|
| [../hashmap/](../hashmap/) | шаблонный скриптовый класс: `CppHashMap<K,V>` поверх `std::unordered_map`, состояние объекта целиком в C++ |
| [../players/](../players/) | обратное направление: тик, зеркало движкового API, чтение полей движковых объектов |

## Тесты без игры

Нативы — обычные функции C++, и `graft::client` — обычная статическая библиотека, так что
логику плагина гоняют обычным тестовым фреймворком, не поднимая DayZ:

```cmake
add_executable(example_tests tests/logic_test.cpp src/natives.cpp)
target_link_libraries(example_tests PRIVATE graft::client GTest::gtest_main)
```

Блоки `GRAFT_BINDINGS` в таком экзешнике просто наполняют реестр и никуда не ходят —
движок нужен только тем нативам, которые сами зовут его (`graft::ref`, `world.hpp`).
Их и оставляют на сьюту в игре.

Как выглядит сьюта uTest поверх graft-нативов — [../../tests/mod/SIXW_GRAFT/scripts/3_Game/seraph_graft_test.c](../../tests/mod/SIXW_GRAFT/scripts/3_Game/seraph_graft_test.c).
В примере её нет намеренно: фреймворку тестов нужен свой мод в зависимостях, а примеру
хватает `Print` в лог.

## Что важно знать

- **Имена глобальны на всю игру.** Пара (класс, метод), имя глобальной функции, имя
  шаблонного класса — плоское пространство движка. Два плагина с `CppHashMap` не
  уживутся: хост отклонит второго и напишет об этом. Префиксы обязательны.
- **Два плагина не могут держать состояние на ОДНОМ скриптовом классе.** Освобождение
  у каждого своё (`NativeDispose_<ПЛАГИН>`), а деструктор в скрипте может быть один.
  Хост это ловит и жалуется; методы без состояния так делить можно.
- **Статическая инициализация плагина должна быть чистой.** Она отрабатывает и очень
  рано в игре, и в обычном процессе, когда `graft protogen` загружает DLL ради
  дескрипторов. Ничего движкового в статических конструкторах.
- **`out` (неконстантная ссылка) работает только со ссылочными типами** — array/set/map,
  класс: так же, как у самого движка. Значения (int/float) через `out` умеет только
  маршалируемая форма. Строку через `out` вернуть нельзя вообще — ею владеет движок;
  отдавай её возвратом (`std::string`).
- **`std::vector<T>` возвратом не отдать** — память массива принадлежит движку. Заполняй
  через `out`-ссылку на `graft::array<T>`: поэлементно `set(i, v)`, пачкой `assign(range)`,
  а простую сортировку сделает сам движок — `values.sort()` / `values.sort(true)`.
  Итератор вьюхи не пишущий, поэтому `std::ranges::sort` прямо по ней не пройдёт:
  скопируй (`std::ranges::to<std::vector<int>>(values)`), посчитай, верни `assign`.
- **Движковых путей в C++ нет.** `$profile:`, `$saves:`, `$CurrentDir:` понимает только
  скрипт. Для `std::ofstream` это обычное имя файла, а `:` на NTFS открывает
  АЛЬТЕРНАТИВНЫЙ ПОТОК: файл «сохраняется» успешно и исчезает бесследно. Рабочий каталог
  процесса — каталог игры. Принимай из скрипта относительное имя, проверяй его
  (`:`, `..`, абсолютный путь — отказ) и раскрывай в свой каталог сам.
- **В теле `co_routine` (uTest-кейсы) нельзя `&&`** — компилятор Enforce на нём падает.
- **Горячей перезагрузки нет.** Регистрация происходит один раз на старте движка,
  скриптовая сторона лежит в PBO. Перезапуск игры.
- **Изоляции падений нет.** SEH в этом процессе не работает — движок ставит свой фильтр
  и валит процесс раньше. Кривой плагин уронит игру; по журналу видно, чей натив
  регистрировался последним.
- Адресов движка нигде нет: они ищутся в загруженном образе по именам ванильных
  нативов, поэтому патч игры ничего не ломает. Подробности — в [../../README.md](../../README.md).
