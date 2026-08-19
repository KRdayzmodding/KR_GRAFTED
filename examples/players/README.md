# players — C++ ходит в игру

Обратное направление: не «скрипт зовёт C++», а «C++ сам работает с игрой». Раз в пять
секунд плагин обходит игроков, читает steam id, имя и позицию, кладёт в обычный
`std::map` — и отдаёт наружу нативами.

Будит его движок: библиотека подменяет ванильный `CGame.IsServer()`, который движок
зовёт каждый кадр из обновления миссии, — и корень объектного графа приезжает вместе с
вызовом, прямо в `rcx`. Своего потока у библиотеки нет и быть не должно: всё идёт по
скриптовому.

```
CMakeLists.txt                                    graft_import + graft_plugin + GRAFT_API_DIR
CMakePresets.json                                 release и debug, артефакты в out/<пресет>
src/plugin.cpp                                    паспорт плагина: GRAFT_PLUGIN("EXAMPLE_PLAYERS", 1)
src/main.cpp                                      тик, обход игроков, нативы наружу
src/graft/dayz/{1_Core,2_GameLib,3_Game,4_World}.hpp   зеркало движкового API (graft apigen)
mod/EXAMPLE_PLAYERS/$PBOPREFIX$                   EXAMPLE_PLAYERS
mod/EXAMPLE_PLAYERS/config.cpp                    CfgPatches/CfgMods: 1_Core и 5_Mission
mod/EXAMPLE_PLAYERS/scripts/1_Core/grafted_natives_EXAMPLE_PLAYERS.c
                                                  ^ объявления нативов, печатает сборка
mod/EXAMPLE_PLAYERS/scripts/5_Mission/players_demo.c   единственный скрипт своей рукой
deploy.ps1                                        собрать -> PBO -> разложить по игре
.gitignore                                        build/ out/ dist/
```

Сгенерированный файл лежит прямо в моде и в репозитории (`SCRIPTS_DIR` в
[CMakeLists.txt](CMakeLists.txt)): пример должен показывать мод целиком. Он весь тут —
четыре строки, по одной на натив. Классов нет: тик объявления не требует, движок будит
C++ сам.

```c
proto native float ExampleUptime();
proto native int ExamplePlayersKnown();
proto native owned string ExamplePlayerReport(string p0);
proto native int ExampleDebugMonitor();
```

## Что в C++

```cpp
GRAFT_ON_TICK(dt) {                      // точка входа: раз в кадр, dt — движковый timeslice
    g_uptime += dt;
    g_since_scan += dt;
    if (g_since_scan < 5.0f) return;
    g_since_scan = 0;
    scan_players();
}

void scan_players() {
    const graft::dayz::CGame world = graft::cast<graft::dayz::CGame>(graft::game());
    const auto players = graft::scratch<graft::array<graft::dayz::Man>>();   // буфер под out
    if (!world || !players) return;
    players.clear();
    world.GetPlayers(players);

    for (const graft::dayz::Man man : players) {
        const graft::dayz::PlayerIdentity id = man.GetIdentity();
        if (!id) continue;
        player_row& row = g_players[id.GetPlainId()];
        row.name = id.GetName();
        row.position = man.GetOrigin();
        ++row.seen_ticks;
    }
}
```

Целиком — [src/main.cpp](src/main.cpp).

| Приём | Зачем |
|---|---|
| `GRAFT_ON_TICK(dt) { ... }` | точка входа: C++ просыпается на скриптовом потоке |
| `graft::game()` | корень объектного графа, тот же `GetGame()` |
| `graft::dayz::CGame` и прочие | зеркало движкового API: настоящие методы с сигнатурами |
| `graft::scratch<graft::array<Man>>()` | буфер под `out`-аргумент движка, один на процесс |
| `world["m_DebugMonitorEnabled"_f]` | поле объекта как lvalue, имя живёт в типе |
| `id.GetPlainId()` | маршалируемый `proto` — путь выбирает библиотека, снаружи разницы нет |

## Что в моде

Скрипт про тик не знает и ни о чём не просит — он только **спрашивает уже собранное**,
[mod/EXAMPLE_PLAYERS/scripts/5_Mission/players_demo.c](mod/EXAMPLE_PLAYERS/scripts/5_Mission/players_demo.c):

```c
float uptime = ExampleUptime();          // секунды накопил C++ у себя в тике
int known = ExamplePlayersKnown();
...
string steam = id.GetPlainId();
Print("[EXAMPLE_PLAYERS] " + steam + " -> " + ExamplePlayerReport(steam));
```

В script-логе сервера:

```
[EXAMPLE_PLAYERS] debug monitor (поле CGame из C++) = 0
[EXAMPLE_PLAYERS] тик C++ работает 10.3045 с, известно игроков: 0
[EXAMPLE_PLAYERS] 76561198000000000 -> Survivor @ 7500 300 7500, тиков 4
```

Первая строка — поле движкового объекта, прочитанное из C++ по имени. Вторая печатается
раз в десять секунд и на пустом сервере: секунды копит тик, и по ним видно, что C++
живёт сам. Третья появляется, когда кто-то подключился.

## Зеркало движкового API

Пример написан против настоящих классов игры, а не против `ref<"CGame">` вручную.
Зеркало генерится один раз (и заново после патча игры) отдельной командой — сборка
скрипты игры не ищет:

```
graft apigen P:/scripts src/graft/dayz
```

Дефайны игры ему **не нужны**: он печатает объединение всех веток препроцессора. Чего в
конкретной сборке нет — то не найдётся в рантайме, вернёт нулевое значение и строку в
системный журнал. Стоит такой промах ровно столько же, сколько обычный переход в натив.

## Собрать и запустить

```bat
graft install "C:\DayZServer"          :: один раз на игру
cmake --preset release
cmake --build --preset release
powershell -File deploy.ps1 -Game "C:\DayZServer"
```

Запуск с `-mod=@EXAMPLE_PLAYERS;`. Готовое зеркало лежит в `src/graft/dayz/` — если
своё, укажи путь: `-DGRAFT_API_DIR=<куда>`.

## Чего в примере намеренно нет

**Ссылок на игровые объекты между тиками.** Объект, пришедший от движка, живёт до конца
текущего вызова: движок волен его удалить, а память переиспользовать. Поэтому в
`player_row` лежат ЗНАЧЕНИЯ — строка и позиция, — а не `ref<"Man">`. Если ссылку всё же
нужно пронести, для этого есть `graft::borrowed<T>`: он помнит класс объекта и ловит
переиспользованный адрес.
