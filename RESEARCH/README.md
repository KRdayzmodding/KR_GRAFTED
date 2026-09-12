# База знаний: как устроен Enfusion изнутри

Всё, что известно о движке из разбора его бинаря, — и инструменты, которыми это добыто.
Библиотека адресов не хранит: точки движка ищутся в рантайме по именам ванильных
нативов (`INCLUDE/graft/scan.hpp`). Эта папка отвечает на другой вопрос — **почему**
поиск устроен именно так, и **чем перепроверить** после патча игры.

Читать сверху вниз необязательно, это справочник:

| Раздел | О чём |
|---|---|
| [Что уже известно о цели](#что-уже-известно-о-цели) | строки-маяки компилятора, точки регистрации, биты флагов |
| [ABI прототипов](#abi-прототипов-декомпиляция-20-ванильных-импл-researchoutdiagpseudo) | как движок зовёт `proto native` и `proto` |
| [Регистрация](#регистрация) | `RegisterGlobal` / `RegisterMethod` и что значит последний аргумент |
| [Смерть объекта](#смерть-объекта-где-её-ловить-и-почему-не-в-деструкторе) | почему деструктор нативом не сделать и через что чистят себя `array`/`map`/`set` |
| [Вызов В ДРУГУЮ СТОРОНУ](#вызов-в-другую-сторону-как-движок-сам-готовит-аргументы) | как самому позвать движковый `proto` |
| [Падение нашего кода](#падение-нашего-кода-чьё-оно) | почему SEH ведёт себя не так, как ожидается |
| [Карта кадра](#карта-кадра-от-точки-входа-потока-до-нашего-трамплина) | путь от точки входа потока до нашего трамплина |
| [theory/watch.md](theory/watch.md) | кто пишет в память: точка останова на запись, прямо из плагина |
| [scripts/](scripts/) | IDAPython-скрипты, которыми всё выше и добыто, и [pe.py](scripts/pe.py) — то же без IDA |

Каждый вывод здесь получен на конкретной сборке игры и подписан ею. Патч игры findings
не отменяет — но проверяются они заново теми же скриптами.

## Инструменты

Всё headless, чтобы разбор был воспроизводимым, а не «покликал и запомнил».

| Задача | Чем |
|---|---|
| Статика + декомпилятор | IDA Pro + Hex-Rays (`idat64.exe`, headless) |
| Второе мнение | Ghidra (подключаем, только если Hex-Rays на чём-то сломался) |
| Динамика, брейкпойнты | x64dbg |
| Скрипты анализа | Python (IDAPython) |
| То же, но без IDA | Python + capstone — [scripts/pe.py](scripts/pe.py): дизассемблер, импорты, ссылки |
| Кто пишет в память | аппаратная точка останова прямо из плагина — [`graft/watch.hpp`](../INCLUDE/graft/watch.hpp), [theory/watch.md](theory/watch.md) |
| Распаковка PBO | Mikero Tools |

Путей к чужой машине в репозитории нет — свои задаются переменными окружения:

| Переменная | Что |
|---|---|
| `GRAFT_IDA_DIR` | каталог IDA (там `idat64.exe`) |
| `GRAFT_GAME_DIR` | каталог игры (`DayZDiag_x64.exe`) |
| `GRAFT_SERVER_DIR` | каталог сервера (`DayZServer_x64.exe`), только для `-On server` |

## Цель анализа

```
DayZServer_x64.exe   1.29.0.163451   x64   16.9 MB
sha256 -> RESEARCH/out/target.sha256
```

Смещения ниже привязаны к этой версии. После обновления игры — `import` + `analyze`
заново; выводы про раскладку и ABI переживают патч, конкретные адреса нет.

## Команды

```powershell
$env:GRAFT_IDA_DIR  = 'C:\Program Files\IDA Pro'
$env:GRAFT_GAME_DIR = 'C:\...\steamapps\common\DayZ'

.\RESEARCH\re.ps1 import              # копия бинаря в RESEARCH\bin (Steam не трогаем)
.\RESEARCH\re.ps1 analyze             # авто-анализ -> RESEARCH\bin\<name>.i64 (долго)
$env:RE_QUERY = "Prototype error"
.\RESEARCH\re.ps1 run probe.py        # строки -> xref-функции -> Hex-Rays в out\pseudo\
$env:RE_ADDRS = "0x14032C5B0"        # или просто вывалить псевдокод по адресам
.\RESEARCH\re.ps1 run natives.py      # найти RegisterCoreNatives частотным анализом
.\RESEARCH\re.ps1 run discover.py     # сухой прогон рантайм-поиска (как в graft/scan.cpp)
.\RESEARCH\re.ps1 gui                 # открыть базу руками
```

Падение игры разбирается быстрее в отладчике, чем в IDA: минидамп лежит рядом со
script-логом теста, `cdb -z <dump> -c ".ecxr; kb; q"` сразу даёт стек и RVA (cdb — из
Windows SDK Debugging Tools).

## Что уже известно о цели

Компилятор Enforce Script целиком лежит в серверном exe (`enf_scriptcompiler.cpp`,
`enf_scriptcontext.cpp`, `enf_scriptthread.cpp` — пути из ассертов). Строки-маяки
для поиска точки регистрации нативов:

- `Prototype error '%s'` — компилятор не смог связать `proto native` с движком
- `Native functions don't support 'out' arguments '%s'`
- `Engine class '%s' cannot be modded.` / `Sealed type '%s' can't be modded`
- `Game script module entry function "%s" not found!`
- `Can't compile "%s" script module!`

Модули скриптов и точки входа — из `dayz.gproj`: `core / gameLib / game (CreateGame) /
world / mission (CreateMission)`.

## Теория

Разбор вынесен в [theory/](theory/) — по файлу на тему, чтобы можно было читать то, что
нужно сейчас, а не пролистывать полторы тысячи строк.

| Файл | О чём |
|---|---|
| [theory/abi.md](theory/abi.md) | ABI прототипов, регистрация нативов, смерть объекта, вызов в обратную сторону |
| [theory/crashes.md](theory/crashes.md) | чьи обработчики стоят на самом деле и почему `__except` «не работал» |
| [theory/frame.md](theory/frame.md) | где движок зовёт скрипт каждый кадр и как туда встроиться |
| [theory/watch.md](theory/watch.md) | кто пишет в эту память: аппаратная точка останова на запись |
