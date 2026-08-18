# minimal: свой плагин на библиотеке graft

Минимальный шаблон плагина: несколько нативов на C++ (глобальные функции, возврат
строки, метод класса, работа с контейнерами) + PBO, который их зовёт. Копируешь
каталог, переименовываешь — дальше правишь только [src/natives.cpp](src/natives.cpp),
[src/plugin.cpp](src/plugin.cpp) и [mod/](mod/).

Или разверни шаблон командой: `graft new MYMOD E:\source`.

## Из чего состоит

| Файл | Правишь? |
|---|---|
| [src/natives.cpp](src/natives.cpp) | **да** — сюда пишешь свои нативы |
| [src/plugin.cpp](src/plugin.cpp) | **да** — имя и версия плагина, одна строчка |
| [mod/EXAMPLE_GRAFT/](mod/EXAMPLE_GRAFT/) | **да** — config.cpp, класс-хозяин, тесты |
| `mod/EXAMPLE_GRAFT/scripts/<модуль>/grafted_natives_<ПЛАГИН>.c` | нет, генерится сборкой |
| [CMakeLists.txt](CMakeLists.txt) | только пути (`GRAFT_ROOT`, `EXAMPLE_MOD_SCRIPTS`) |
| [../README.md](../README.md) | что ещё есть в примерах |
| [build.bat](build.bat) | `GAME_DIR`, а после копирования — и `GRAFT_ROOT` |

## Хост и плагины

Рядом с exe живёт **один** `dwmapi.dll` — это хост: он находит точки движка, ставит
хуки и грузит плагины. Пересобирать его не нужно никому и никогда.

Твой мод собирается в **плагин** — `<ИМЯ>.grafted.dll`, который хост подхватит из
`<мод>/graft/` (плагин едет вместе со своим модом) или из `<каталог игры>/#GRAFTED/`
(общее хранилище на установку). Плагинов может быть сколько угодно, они не мешают друг
другу и могут быть собраны разными компиляторами.

```
DayZ.exe
 └─ dwmapi.dll                      хост, один на игру: graft install <каталог игры>
      ├─ @MYMOD/graft/MYMOD.grafted.dll        плагин внутри своего мода
      ├─ @OTHER/graft/OTHER.grafted.dll
      └─ #GRAFTED/EXAMPLE_GRAFT.grafted.dll    общее хранилище рядом с exe
```

В `#GRAFTED/` нет ничего кроме плагинов: хост открывает оттуда КАЖДЫЙ `.dll`.

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
    bind.klass<StringTable>("ExampleTable")
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

## Сборка

```bat
build.bat            :: EXAMPLE_GRAFT.grafted.dll + объявления в mod/
build.bat deploy     :: то же + копия плагина в <игра>\graft\ (правь GAME_DIR)
```

Весь CMake проекта — это:

```cmake
add_subdirectory("${GRAFT_ROOT}" ... EXCLUDE_FROM_ALL)
include("${GRAFT_ROOT}/cmake/graft_plugin.cmake")

graft_plugin(example_graft
    NAME EXAMPLE_GRAFT VERSION 1
    SOURCES src/natives.cpp src/plugin.cpp
    MOD_SCRIPTS P:/EXAMPLE_GRAFT/scripts)
```

`NAME` попадает в три места сразу: имя DLL, паспорт плагина и суффикс служебного натива
`NativeDispose_<NAME>`.

## Мод

`mod/EXAMPLE_GRAFT/` — шаблон. Рабочий каталог мода должен лежать под `P:\`
**реальной папкой** (dzrun пропускает симлинки), имя папки = stem PBO:

```
xcopy /E /I mod\EXAMPLE_GRAFT P:\EXAMPLE_GRAFT
cmake -B build -DEXAMPLE_MOD_SCRIPTS=P:/EXAMPLE_GRAFT/scripts -DGRAFT_ROOT=E:/source/KR_GRAFT
```

Генератор раскладывает объявления по модулям сам: натив с игровым типом в сигнатуре
(`Object`, `EntityAI`) описывается в отдельном блоке `GRAFT_BINDINGS("3_Game")` и
попадает в `scripts/3_Game/grafted_natives_EXAMPLE_GRAFT.c` — в 1_Core таких типов ещё нет.

Проверка связи: сьюта `example::graft` через dayz-mcp `run_suites(["example::graft"])` —
мод и сьюту предварительно завести в `dayz-mcp.config.json`. Что нашлось и что
зарегистрировалось — в `<каталог игры>/graft.log`, а `graft doctor <каталог игры>`
скажет то же самое человеческим языком.

## Дальше

Пример побольше — шаблонный скриптовый класс, у которого всё хранилище живёт в C++
(`CppHashMap<K,V>` поверх `std::unordered_map`), — лежит в самом репозитории:
[../../tests/plugins/cpp_hashmap.cpp](../../tests/plugins/cpp_hashmap.cpp). Он там, а не здесь,
потому что его гоняет сьюта `seraph::graft`, а в примерах свой сервер не поднимается.
См. [../README.md](../README.md).

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
  через `out`-ссылку на `graft::array<T>`.
- **В теле `co_routine` (uTest-кейсы) нельзя `&&`** — компилятор Enforce на нём падает.
- **Горячей перезагрузки нет.** Регистрация происходит один раз на старте движка,
  скриптовая сторона лежит в PBO. Перезапуск игры.
- **Изоляции падений нет.** SEH в этом процессе не работает — движок ставит свой фильтр
  и валит процесс раньше. Кривой плагин уронит игру; по журналу видно, чей натив
  регистрировался последним.
- Адресов движка нигде нет: они ищутся в загруженном образе по именам ванильных
  нативов, поэтому патч игры ничего не ломает. Подробности — в [../../README.md](../../README.md).
