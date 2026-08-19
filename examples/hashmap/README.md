# hashmap — шаблонный скриптовый класс

`CppHashMap<K,V>` в скрипте — это `std::unordered_map<K,V>` в C++. Скриптовый объект
только оболочка: вся таблица живёт на стороне плагина.

```
CMakeLists.txt                                     graft_import + graft_plugin
CMakePresets.json                                  release и debug, артефакты в out/<пресет>
src/plugin.cpp                                     паспорт плагина: GRAFT_PLUGIN("EXAMPLE_HASHMAP", 1)
src/cpp_hashmap.cpp                                шаблон Table<K,V> + привязка
mod/EXAMPLE_HASHMAP/$PBOPREFIX$                    EXAMPLE_HASHMAP
mod/EXAMPLE_HASHMAP/config.cpp                     CfgPatches/CfgMods: 1_Core и 5_Mission
mod/EXAMPLE_HASHMAP/scripts/1_Core/grafted_natives_EXAMPLE_HASHMAP.c
                                                   ^ class CppHashMap<K,V> ЦЕЛИКОМ, печатает сборка
mod/EXAMPLE_HASHMAP/scripts/5_Mission/hashmap_demo.c   единственный скрипт своей рукой
deploy.ps1                                         собрать -> PBO -> разложить по игре
.gitignore                                         build/ out/ dist/
```

Класс `CppHashMap` руками не написан нигде: его печатает сборка, читая собранную DLL.
В примере он лежит **прямо в моде и в репозитории** — за это отвечает одна строка в
[CMakeLists.txt](CMakeLists.txt):

```cmake
SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mod/EXAMPLE_HASHMAP/scripts"
```

Пример обязан показывать мод целиком, иначе половина кода — «появится после сборки».
Файл переписывается на каждой сборке, поэтому расхождение с C++ увидит обычный
`git diff`. В своём проекте так делать не обязательно: по умолчанию объявления ложатся
рядом с DLL и в мод их забирает `deploy.ps1` — эта раскладка показана в
[hello](../hello/).

Кроме этого сборка кладёт в `out/release/` сам плагин, `EXAMPLE_HASHMAP.grafted.dll`.

## Что в C++

Обычный шаблон обычными типами. Ни базовых классов, ни спецтипов, ни знания про DayZ:

```cpp
template <class K, class V>
struct Table {
    std::unordered_map<K, V> data;
    void Set(K key, V value) { data.insert_or_assign(std::move(key), std::move(value)); }
    V Get(K key) const { ... }
    int Count() const { return static_cast<int>(data.size()); }
    K KeyAt(int index) const { ... }
};

GRAFT_BINDINGS("1_Core") {
    bind.template_class<Table>("CppHashMap<Class K, Class V>")
        .method(GRAFT_METHOD(Set))
        .method(GRAFT_METHOD(Get))
        .method(GRAFT_METHOD(Count))
        .method(GRAFT_METHOD(KeyAt));
}
```

Целиком — [src/cpp_hashmap.cpp](src/cpp_hashmap.cpp), 50 строк вместе с комментариями.

## Что в моде

Вот он, класс целиком — ровно то, что напечатала сборка в
`scripts/1_Core/grafted_natives_EXAMPLE_HASHMAP.c`:

```c
// СГЕНЕРИРОВАНО protogen из C++ реестра нативов — руками не править.
// Источник истины — блоки GRAFT_BINDINGS в исходниках мода.
// Импл живёт в graft-модуле (proxy-DLL рядом с exe); без неё скрипт не слинкуется.
// Модуль: 1_Core

class CppHashMap<Class K, Class V>
{
    proto void Set(K p0, V p1);
    proto V Get(K p0);
    proto bool Contains(K p0);
    proto bool Remove(K p0);
    proto int Count();
    proto void Clear();
    proto K KeyAt(int p0);
}
```

Семь `proto` — это ровно семь `.method(GRAFT_METHOD(...))` из C++, и ни одного лишнего
знака: имена, типы и порядок взяты из собранной DLL. Шаблонный синтаксис
`<Class K, Class V>` — из строки `"CppHashMap<Class K, Class V>"` в привязке. Ни
деструктора, ни служебных методов: освобождением занимается движок — так же, как он
делает это для ванильного `map`, у которого деструктора в скрипте тоже нет.

Сравни с [minimal](../minimal/): там класс-хозяин `class ExampleGraft {}` писать РУКАМИ
надо — генератор печатает `modded class ExampleGraft`, а моддить нечего, если тип никем не
объявлен. У шаблонного класса такой проблемы нет: он объявляется целиком и сразу.

Своего кода в моде — только пользование, [mod/EXAMPLE_HASHMAP/scripts/5_Mission/hashmap_demo.c](mod/EXAMPLE_HASHMAP/scripts/5_Mission/hashmap_demo.c):

```c
CppHashMap<string, int> kills = new CppHashMap<string, int>;
kills.Set("zombies", 12);
kills.Set("bandits", 3);

CppHashMap<int, string> byId = new CppHashMap<int, string>;   // другая инстанциация
byId.Set(1, "Chernarus");                                     // нового кода — ноль
```

В script-логе после запуска:

```
[EXAMPLE_HASHMAP] count=2 zombies=12
[EXAMPLE_HASHMAP] обход: zombies=12 bandits=3
[EXAMPLE_HASHMAP] byId: 1=Chernarus 2=Livonia
[EXAMPLE_HASHMAP] после delete: count=0
```

## Чем отличается от [minimal](../minimal/)

Класс не привязывается по имени, а **разворачивается**: `template_class<Table>` берёт
шаблон и инстанциирует его по всем поддерживаемым парам `<K,V>`. На вызове библиотека
выбирает нужную инстанциацию по имени скриптового класса объекта — `"CppHashMap<string,int>"`.

Владение обычное: объект живёт столько, сколько живёт скриптовый объект. `delete kills` —
и `std::unordered_map` в C++ снесён. Последняя строчка лога (`count=0` у новой таблицы) —
ровно про это.

В скрипте для этого нет НИ ОДНОЙ строчки, и это не фокус: движок разрушает объект
вызовом нулевого слота его же C++ таблицы (тем самым, которым чистят себя `array`, `map`
и `set`), а хост подменяет объекту таблицу на свою копию — слот наш, остальные как были.
Разобрано в [RESEARCH/README.md](../../RESEARCH/README.md#смерть-объекта-где-её-ловить-и-почему-не-в-деструкторе).

## Ограничение, которое видно в коде

`KeyAt(int)` отдаёт ключи по одному, а не заполняет `array<K>`: за строками и ссылками
движок считает ссылки, поэтому заливать такой массив из C++ нельзя. Порядок у
хеш-таблицы не определён (в логе видно: `zombies` раньше `bandits`), но между вызовами
не меняется — обход по индексу согласован сам с собой.

## Собрать и запустить

```bat
graft install "C:\DayZServer"          :: один раз на игру
cmake --preset release
cmake --build --preset release         :: DLL и EXAMPLE_HASHMAP.scripts в out/release/
powershell -File deploy.ps1 -Game "C:\DayZServer"
```

Запуск с `-mod=@EXAMPLE_HASHMAP;`, строки — в script-логе профиля.

Похожий класс (`SeraphHashMap`) есть и в фикстуре тестов — [../../tests/plugins/hashmap.cpp](../../tests/plugins/hashmap.cpp).
Он там ради «двух плагинов в одном процессе», а здесь пример живёт своей жизнью: имена
скриптовых классов у движка глобальны на всю установку, поэтому классы у них разные и
оба мода могут стоять в одной игре.
