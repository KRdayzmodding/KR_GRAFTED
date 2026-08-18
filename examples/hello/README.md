# Hello world

Самый маленький мод на graft: одна функция C++ и одна строчка скрипта, которая её зовёт.
Отсюда начинают — остальные примеры показывают, что бывает дальше.

```
CMakeLists.txt                             graft_import + graft_plugin, больше ничего
CMakePresets.json                          release и debug, артефакты в out/<пресет>
src/hello.cpp                              паспорт плагина + функция + привязка
mod/HELLO_GRAFT/config.cpp                 обычный мод DayZ
mod/HELLO_GRAFT/scripts/5_Mission/hello.c  вызов из скрипта
deploy.ps1                                 собрать -> PBO -> разложить по игре
.gitignore                                 build/ out/ dist/ и сгенерированные объявления
```

Сборка добавляет к этому два файла — и оба в `out/`, не в исходниках:

```
out/release/HELLO_GRAFT.grafted.dll
out/release/HELLO_GRAFT.scripts/3_Game/grafted_natives_HELLO_GRAFT.c
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
cmake --preset release
cmake --build --preset release

:: 3. забрать к себе в мод, упаковать PBO, положить в игру
powershell -File deploy.ps1 -Game "C:\DayZServer"

:: 4. запустить с -mod=@HELLO_GRAFT и посмотреть script-лог
```

Правишь C++ — повторяешь шаги 2 и 3; `deploy.ps1` делает оба.

Без пресета тоже можно, но тип сборки задай явно:
`cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`. Пустой `CMAKE_BUILD_TYPE` даёт
отладочную сборку, и это редко то, что кладут в мод.

Сгенерированные объявления в `mod/` лежать обязаны — иначе PBO не соберётся, — но в
репозиторий не едут: за это отвечает строка `mod/*/scripts/*/grafted_natives_*.c`
в [.gitignore](.gitignore). В моде они появляются на шаге 3, их кладёт `deploy.ps1`.

Остальные три примера сделаны наоборот: у них сборка пишет объявления сразу в мод
(`SCRIPTS_DIR`), и файл лежит в репозитории — пример должен показывать мод целиком.
Обе раскладки рабочие; здесь, в шаблоне, дерево исходников остаётся чистым.

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
