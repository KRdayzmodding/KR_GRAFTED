# Hello world

Самый маленький мод на graft: одна функция C++ и одна строчка скрипта, которая её зовёт.
Отсюда начинают — остальные примеры показывают, что бывает дальше.

```
CMakeLists.txt                             graft_import + graft_plugin, больше ничего
src/hello.cpp                              паспорт плагина + функция + привязка
mod/HELLO_GRAFT/config.cpp                 обычный мод DayZ
mod/HELLO_GRAFT/scripts/5_Mission/hello.c  вызов из скрипта
```

Сборка добавляет к этому два файла — и оба в каталоге сборки, не в исходниках:

```
build/HELLO_GRAFT.grafted.dll
build/HELLO_GRAFT.scripts/3_Game/grafted_natives_HELLO_GRAFT.c
```

Итог в игре — строка в script-логе:

```
[HELLO_GRAFT] Hello from C++, DayZ!
```

## Как это устроено

C++ пишется обычным C++, никаких особых типов в сигнатуре:

```cpp
std::string HelloGraft(std::string_view name) {
    return std::format("Hello from C++, {}!", name);
}

GRAFT_BINDINGS("3_Game") { bind.global<&HelloGraft>("HelloGraft"); }
```

Из этой записи получаются сразу две вещи: адрес impl, который движок зовёт напрямую, и
строка объявления для PBO — её печатает сборка, читая собранную DLL:

```c
proto native owned string HelloGraft(string p0);
```

Разъехаться они не могут: источник один. Руками объявление не пишут и не правят.

## Цикл разработки

```bat
:: 1. один раз: поставить хост рядом с exe игры
graft install "C:\DayZServer"

:: 2. собрать: graft подтянется сам, объявления появятся рядом с DLL
cmake -B build -G Ninja
cmake --build build

:: 3. забрать к себе в мод и собрать PBO своим обычным способом
xcopy /E /Y build\HELLO_GRAFT.scripts mod\HELLO_GRAFT\scripts

:: 4. положить рядом с игрой: DLL в @МОД\grafted\, PBO как обычно в @МОД\addons\
copy build\HELLO_GRAFT.grafted.dll "C:\DayZServer\@HELLO_GRAFT\grafted\"

:: 5. запустить с -mod=@HELLO_GRAFT и посмотреть script-лог
```

Правишь C++ — повторяешь шаги 2 и 4. Шаг 3 нужен только когда изменился список нативов:
объявления печатаются на каждой сборке, но меняются редко.

Горячей перезагрузки нет: регистрация происходит один раз на старте движка. Поменял
C++ — перезапусти игру.

## Что проверить, если натив не завёлся

```bat
graft list   "C:\DayZServer"     :: какие плагины видит хост и сколько в них нативов
graft doctor "C:\DayZServer"     :: почему не работает: ABI, раскладка движка, коллизии
```

Там же, рядом с exe, хост пишет `graft.log` — в нём видно, что нашлось и что
зарегистрировалось.

## Дальше

| Пример | Что добавляет |
|---|---|
| [../minimal/](../minimal/) | остальные типы: массивы, `out`, `vector`, методы скриптового класса |
| [../hashmap/](../hashmap/) | шаблонный скриптовый класс, состояние которого целиком живёт в C++ |
| [../players/](../players/) | обратное направление: C++ сам ходит в игру — тик, зеркало движкового API |
