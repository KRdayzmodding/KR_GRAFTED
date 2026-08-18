# hashmap — шаблонный скриптовый класс

`CppHashMap<K,V>` в скрипте — обычный `std::unordered_map<K,V>` в C++.

```cpp
template <class K, class V>
struct Table {
    std::unordered_map<K, V> data;
    void Set(K key, V value) { data.insert_or_assign(std::move(key), std::move(value)); }
    ...
};

GRAFT_BINDINGS("1_Core") {
    bind.template_class<Table>("CppHashMap<Class K, Class V>")
        .method(GRAFT_METHOD(Set))
        .method(GRAFT_METHOD(Get))
        ...;
}
```

```c
// в моде — ни строчки своей рукой, всё это печатает сборка
ref CppHashMap<string, int> counts = new CppHashMap<string, int>();
counts.Set("zombies", 12);
Print(counts.Get("zombies"));   // 12
```

## Чем отличается от [minimal](../minimal/)

Класс не привязывается по имени, а **разворачивается**: `template_class<Table>` берёт
обычный C++-шаблон обычными типами и инстанциирует его по всем поддерживаемым парам
`<K,V>`. На вызове библиотека выбирает нужную инстанциацию по имени скриптового класса
объекта — `"CppHashMap<string,int>"`. Фасадов, спецтипов и ручных специализаций нет:
`Table` не знает, что он вообще имеет отношение к движку.

Владение — тоже обычное: объект живёт столько, сколько живёт скриптовый объект, и
хоронит его служебный натив `NativeDispose_EXAMPLE_HASHMAP` (суффикс из `NAME`, чтобы
два плагина, держащие состояние на одном классе, не спорили за один метод).

## Сборка

```bat
build.bat            :: EXAMPLE_HASHMAP.grafted.dll + объявления в mod/
build.bat deploy     :: плюс копия в <игра>\graft\
```

Эти же исходники собирает и корневой `CMakeLists.txt` — вторым плагином репозитория,
рядом с `SIXW_GRAFT`. Так «два плагина уживаются в одном процессе» проверяется обычным
прогоном сьюты, а не отдельным ритуалом, который забудут запустить.

## Ограничение, которое видно в коде

`KeyAt(int)` отдаёт ключи по одному, а не заполняет `array<K>`: за строками и ссылками
движок считает ссылки, поэтому заливать такой массив из C++ нельзя. Порядок у
хеш-таблицы не определён, но между вызовами не меняется — обход по индексу согласован
сам с собой.
