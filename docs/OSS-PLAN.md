# KR_GRAFTED → OSS. План и ТЗ

Репозиторий: `KRdayzmodding/KR_GRAFTED`, ветка `main`, один мейнтейнер (`@6wingSerap`).
Проект: C++20 / clang-cl / Ninja / CMake 3.30+, Windows-only, gtest+ctest, покрытие llvm-cov.
Лицензия: GPL-3.0-or-later + GRAFT plugin exception 1.0.

Общий совет из переписки применён выборочно: часть пунктов для соло-репозитория без
зависимостей — чистый оверхед, они вынесены в «Отложено» с условием включения.

---

## 0. Состояние

Файлы фазы P0 и CI фазы P1 — **написаны и лежат в дереве**. Осталось руками:

| Осталось | Где |
|---|---|
| Импортировать ruleset, мерж-кнопка, Discussions/Wiki/Projects, апрув форк-PR, base permissions организации | приложение А |
| Создать ветку `cla-signatures` | А.6 |
| Завести метки `loading`, `research`, `ci`, `abi-break` | иначе Issue Forms и Dependabot молча их не проставят |
| Включить private vulnerability reporting | `Settings` → `Advanced Security` (иначе ссылка «Report a vulnerability» из SECURITY.md никуда не ведёт) |
| Добавить в required checks `format`, `build`, `tests`, `abi`, `cla` | после первого прогона: раньше GitHub этих контекстов не знает |
| Тег `v0.1.0` и релиз | §4.3, после того как всё вышеперечисленное вольётся |

## 0.1 Что уже есть

| Есть | Нет |
|---|---|
| LICENSE, LICENSE-EXCEPTION, THIRD_PARTY.md | `.github/` целиком |
| README (устройство, сборка, лицензия, вклад) | CI — ни одной проверки |
| `.clang-format`, `.clangd` | CONTRIBUTING, SECURITY, CODE_OF_CONDUCT, CHANGELOG |
| tests/ + ctest, examples/ своими проектами | тегов и релизов — ноль |
| Conventional Commits де-факто (`feat(build):`) | CLA, хотя README уже требует прав на перелицензирование |
| ABI-рукопожатие плагин↔хост (`GRAFT_ABI_VERSION 5`, `GRAFT_LAYOUT_VERSION 2`) | политики совместимости в тексте |

---

## 1. Решения — дефолты приняты, скажи если иначе

| Вопрос | Дефолт | Почему |
|---|---|---|
| Язык | Русский основной (docs, issues, PR). `README.en.md` — короткий: что это, как собрать, ссылка на русский | Аудитория — русскоязычные DayZ-мододелы; полный билингв не потянуть одному. Английский нужен только чтобы проект находили |
| Код и комментарии | Идентификаторы английские, комментарии как сейчас (русские). Новое — так же | Менять 47 файлов ради принципа — не стоит того |
| CLA vs DCO | **CLA**, потому что README обещает право перелицензировать. DCO такого права не даёт | Без CLA обещание в README юридически пустое, а собрать подписи задним числом невозможно |
| Чем подписывать CLA | `contributor-assistant/github-action` в самом репозитории, а не внешний `cla-assistant.io` | Не нужно ставить чужое OAuth-приложение с правами на репозиторий и зависеть от чужого хостинга; подписи лежат в ветке `cla-signatures` |
| Модель доступа | Только форки + PR. Никому write | Один мейнтейнер, командам нечего давать |
| Required approvals | **Ноль**, пока мейнтейнер один; PR обязателен, checks обязательны | Требование ревью заблокирует тебя самого |
| Merge | Только squash, заголовок PR = заголовок коммита | История линейная, changelog собирается из заголовков |

---

## 2. Фаза P0 — до того, как звать людей

Ничего из этого не требует CI и делается за один заход.

### 2.1 Разовая гигиена

- [x] **gitleaks по всей истории — чисто.** `gitleaks 8.28.0`, `git --all --full-history`:
      16 коммитов, 1.31 MB, `no leaks found`. Переписывать историю не нужно, ротировать
      нечего.
- [x] **`RESEARCH/` проверен.** Отслеживается 65 файлов: `README.md`, `re.ps1` и 63
      IDAPython-скрипта (179 KB суммарно, ни одного длинного hex-литерала, ни одного файла
      больше 8 KB). Бинарей, баз IDA и распакованных игровых файлов в репозитории нет —
      `RESEARCH/bin/` и `RESEARCH/out/` в `.gitignore`. **Но** `RESEARCH/README.md`
      содержит таблицу адресов `sub_*` для сборки 1.29.0.163451, раскладки структур,
      ~8 строк аннотированного дизассемблера и несколько 2–6-строчных фрагментов вывода
      Hex-Rays. Формулировка «кусков декомпиляции нет» была неверной — правило переписано
      под то, что защитимо на самом деле (см. 2.3, п. 8).
- [ ] Включить Discussions, выключить Wiki и Projects (приложение А, шаг 1).

### 2.2 Настройки репозитория

**Ruleset на `main`** (Settings → Rules → Rulesets, target `main`):

| Правило | Значение |
|---|---|
| Require a pull request before merging | ON, approvals **0** |
| Dismiss stale approvals | ON |
| Require status checks | ON: `build`, `tests`, `format` (появятся в P1) |
| Require branches to be up to date | ON |
| Require linear history | ON |
| Block force pushes | ON |
| Restrict deletions | ON |
| Bypass list | пусто (используй PR даже для своих правок — иначе checks не гоняются) |

**Merge button**: оставить только «Squash and merge», автоудаление ветки после мержа.

**Actions → General**: «Require approval for all outside collaborators» — форк-PR не
запускают CI без твоего клика.

**Ветвление**: `main` — стабильное. Фичи — `feat/<тема>`, фиксы — `fix/<тема>`.
Ветку `dev` не заводить: при одном мейнтейнере и линейной истории она пустой обряд.
Заведёшь, когда появится второй человек, чья работа не влезает в один PR.

### 2.3 Файлы

```
CONTRIBUTING.md
CODE_OF_CONDUCT.md
SECURITY.md
CHANGELOG.md
README.en.md
.github/
  CODEOWNERS
  PULL_REQUEST_TEMPLATE.md
  ISSUE_TEMPLATE/config.yml
  ISSUE_TEMPLATE/bug_report.yml
  ISSUE_TEMPLATE/plugin_not_loading.yml
  ISSUE_TEMPLATE/feature_request.yml
  ISSUE_TEMPLATE/research_finding.yml
  workflows/ci.yml
  dependabot.yml
```

**CODEOWNERS** — одна строка плюс охрана ABI:

```
*                    @6wingSerap
/INCLUDE/graft/      @6wingSerap
/cmake/              @6wingSerap
```

**CONTRIBUTING.md** — самое важное здесь не «как оформить PR», а среда сборки. Разделы:

1. **Среда.** VS 2022 + clang-cl + Ninja + CMake 3.30. Собирать из «x64 Native Tools
   Command Prompt for VS» — иначе clang-cl не найдёт SDK и STL. Это разъезд №1 у новичка.
2. **Сборка и тесты.** `cmake --preset release` → `--build --preset release` →
   `--build --preset tests` → `ctest --preset release`. Свои пути — только в
   `CMakeUserPresets.json`, он в `.gitignore`.
3. **PR обязан.** Собираться, проходить ctest, проходить clang-format, менять примеры и
   тесты вместе с кодом.
4. **Тесты.** Новый натив/тип/парсер — тест в `tests/`. Ссылка на `tests/README.md`.
5. **Коммиты.** Conventional Commits, заголовок PR = будущий squash-коммит.
6. **Что не принимается:**
   - зашитые адреса и RVA в коде библиотеки — точки движка ищутся по именам (`scan.hpp`),
     это принцип, а не деталь;
   - код, скопированный из движка, — исходный или восстановленный декомпилятором;
   - `git submodule` на приватные репозитории;
   - правки `grafted_natives_*.c` руками — это артефакт генератора;
   - изменения `GRAFT_ABI_VERSION` / `GRAFT_LAYOUT_VERSION` без обоснования (см. 4).
7. **Лицензия вклада.** Текст из README + требование подписать CLA ботом.
8. **RESEARCH/ — где проходит граница.** Правило сформулировано по факту того, что там
   уже лежит, и должно оставаться выполнимым:
   - **можно**: факты, изложенные своими словами, — адреса и RVA с указанием сборки,
     раскладки структур, биты флагов, формы вызова; каждый findings подписан конкретной
     сборкой игры и снабжён скриптом, которым перепроверяется. Вывод без скрипта — не
     findings;
   - **можно**: короткие иллюстративные фрагменты дизассемблера и псевдокода — несколько
     строк, аннотированных собственным разбором, там где без них объяснение разваливается;
   - **нельзя**: бинарь и его части, распакованные игровые файлы, базы IDA (`.i64`),
     ванильные скрипты, файлы из `RESEARCH/out/` — то есть любые **дампы** вывода
     декомпилятора и дизассемблера. Эти каталоги в `.gitignore`, и оттуда их не вынимать.

**SECURITY.md** — здесь это не формальность: graft грузит DLL в процесс игры и сервера,
дыра в загрузчике или в маршалинге — RCE на чужом сервере. Приватный репорт через
GitHub Security Advisories («Report a vulnerability»), почта как запасной канал, срок
ответа — 7 дней, публичный issue для уязвимости не заводить.

**CODE_OF_CONDUCT.md** — Contributor Covenant 2.1 как есть, контакт — твоя почта.

**CHANGELOG.md** — Keep a Changelog, секции `Unreleased` / версии. Отдельная строка в
каждом релизе: **с какой сборкой DayZ проверено** и **менялся ли ABI**.

**Issue Forms** (только `.yml`, blank issues выключить в `config.yml`, ссылку на
Discussions — добавить). Обязательные поля, специфичные для проекта:

| Шаблон | Обязательные поля |
|---|---|
| `bug_report` | версия/тег graft, версия и сборка DayZ (клиент/сервер), вывод `graft doctor <каталог>`, `graft_*.log` из профиля, шаги |
| `plugin_not_loading` | то же + `graft list`, раскладка `@МОД/grafted/`, есть ли чужая `dwmapi.dll`, версия clang-cl |
| `feature_request` | задача, а не решение; что сейчас не даёт сделать |
| `research_finding` | сборка игры, скрипт из `RESEARCH/scripts/`, вывод, как перепроверить |

Поле с выводом `graft doctor` — главная экономия времени: половина тикетов закроется
самим шаблоном.

**PULL_REQUEST_TEMPLATE.md** — чеклист в пять строк: собирается, ctest зелёный,
format прогнан, примеры/тесты обновлены, ABI не тронут (или тронут и почему).

---

## 3. Фаза P1 — CI

Единственный по-настоящему ценный кусок: у C++-проекта на clang-cl без CI ломается всё и
незаметно. Раннер `windows-latest` (в образе есть LLVM, Ninja, CMake, VS 2022).

`.github/workflows/ci.yml`, триггеры `pull_request` + `push: main`, три джобы:

| Джоба | Что делает | Required |
|---|---|---|
| `format` | `git clang-format --diff` по **изменённым строкам** PR | да |
| `build` | `ilammy/msvc-dev-cmd@v1` → `cmake --preset release` → `--build --preset release` → `--build --preset examples`; артефакт `out/release/graft/` | да |
| `tests` | `--build --preset tests` → `ctest --preset release` | да |

Тонкости, которые сэкономят полдня:

- **Формат проверяется по строкам, а не по файлам, и это вынужденно.** Замер: по
  `.clang-format` из репозитория не отформатировано **60 файлов из 66**, суммарно 1415
  правок — код писался до появления проверки. Гейт `--dry-run -Werror` по дереву был бы
  красным с первого дня, а массовое переформатирование переписало бы 60 файлов и снесло
  вручную выровненные комментарии (`ColumnLimit: 0` в конфиге — как раз про них).
  `git clang-format --diff <base>` смотрит только тронутые PR строки: новое едет по
  стилю, старое не трогается. Проверено локально — на последнем коммите чисто, на всей
  истории даёт 6500 строк диффа, то есть обе ветки поведения работают.
- Каталоги `mod/` из проверки исключены: `.c` там — Enforce Script, а `config.cpp` —
  конфиг аддона. Ни то, ни другое не C++, и clang-format их только испортит.
- `msvc-dev-cmd` нужен именно потому, что clang-cl тянет SDK и STL из окружения VS.
- Кэшировать `FetchContent` для gtest (`~/.cache` не про Windows — кэшируй
  `build/*/_deps`, ключ по `tests/CMakeLists.txt`).
- `examples` собирать обязательно и в отдельном шаге: примеры собираются своими
  проектами, и это ровно тот путь, которым пользуется мододел. Коммит
  `2fa52f8` и история с уронённой сьютой — про это.
- Артефакт сборки прикладывать к каждому PR: ревьюер и репортер могут проверить DLL,
  не собирая ничего.

`dependabot.yml` — только экосистема `github-actions` (питона и npm тут нет, gtest
прибит тегом в `FetchContent` и Dependabot его не видит).

**Лишнее для этого проекта**: self-hosted runner, сборка PBO в CI, матрица конфигураций.
`pull_request_target` в сборке не нужен и опасен (он даст коду из форка права на
репозиторий) — но в `cla.yml` он обязателен и используется там, где кода PR не касаются
вовсе: боту нужны права на запись подписи, а `pull_request` с форка их не даёт. Debug-конфигурация — только если начнут ловиться баги, которых нет в
release.

---

## 4. Фаза P2 — совместимость и релизы

Это самый специфичный для graft раздел, и в переписке его нет.

### 4.1 Контракт ABI

Плагины собираются отдельно и грузятся хостом; `plugins.cpp:108` отклоняет всё, у чего
`abi != GRAFT_ABI_VERSION` или `layout != GRAFT_LAYOUT_VERSION`. Значит бамп этих чисел
разом ломает **все** чужие DLL в природе.

Правила, в CONTRIBUTING и в шаблон PR:

- бамп `GRAFT_ABI_VERSION` → минорная версия graft, метка `abi-break`, строка в CHANGELOG,
  раздел в release notes «пересоберите плагины»;
- `GRAFT_LAYOUT_VERSION` бампается отдельно и означает «движок переехал», а не «мы
  поменяли API»;
- проверка в CI: если diff трогает `INCLUDE/graft/abi.h`, а `CHANGELOG.md` — нет, джоба
  падает. Три строки на `git diff --name-only`, ловит ровно то, что забывается.

### 4.2 Граница публичного API

`INCLUDE/graft/` — публично, всё остальное — внутреннее. Написать это в README одной
строкой: заголовки, которые не входят в `native.hpp`, могут поменяться в любой версии.
Сейчас граница есть де-факто (файлы с исключением GPL = то, что попадает в плагин), но
нигде не сказана.

### 4.3 Теги и релизы

Сейчас в README:

```cmake
graft_import(graft https://github.com/KRdayzmodding/KR_GRAFTED TAG main)
```

Все пользователи сидят на движущемся `main` — первый же бамп ABI сломает всех разом.
Что делать:

1. Поставить тег `v0.1.0` на текущее состояние.
2. В README заменить `TAG main` на `TAG v0.1.0` и добавить строчку «`main` — нестабилен,
   для разработки graft, а не для мода».
3. Релиз: `git tag -a vX.Y.Z` → `gh release create vX.Y.Z --generate-notes` + приложить
   `dwmapi.dll`, `graft.exe`, `THIRD_PARTY.md` (обязан ехать рядом с бинарником, BSD-2
   MinHook/HDE) одним архивом.
4. В release notes: сборка DayZ, на которой проверено, и `ABI: 5` строкой.

Версионирование до 1.0: `0.MINOR.PATCH`, ломающее — в MINOR.

**Отложено**: `release-please` / `changesets`. `gh release create --generate-notes` уже
собирает заметки из заголовков PR, а CHANGELOG при релизах раз в месяц пишется руками
быстрее, чем настраивается бот. Включать, когда релизы станут чаще одного в две недели.

---

## 5. Отложено — и при каком условии включать

| Что | Когда |
|---|---|
| Организационные команды, write-доступ | появится второй регулярный контрибьютор |
| Required approvals ≥ 1 | появится второй мейнтейнер |
| `amannn/action-semantic-pull-request` (линт заголовка) | когда чужие PR начнут приходить с «update code» |
| `actions/labeler`, stale-бот | > 30 открытых issue |
| `release-please` | релизы чаще раза в две недели |
| Ветка `dev`, ветки `release/*` | появится необходимость чинить старую версию, не выпуская новую |
| Debug-матрица в CI | всплывёт баг, невидимый в release |
| Подписанные коммиты | по желанию, ценность для одиночного репо низкая |

---

## 6. Порядок работ

| # | Шаг | Результат |
|---|---|---|
| ~~1~~ | ~~gitleaks по истории, ревизия `RESEARCH/`~~ — сделано, чисто | нечего стыдиться в публичном репо |
| ~~2~~ | ~~`ci.yml` (format + build + tests + examples)~~ — написан | зелёная галка на PR |
| 3 | Ruleset на `main` (импорт готов), checks в required — **после первого прогона** | прямой push в `main` невозможен |
| ~~4~~ | ~~CONTRIBUTING + CODEOWNERS + PR-шаблон~~ | понятно, как собрать и что не примут |
| ~~5~~ | ~~Issue Forms с `graft doctor`, config.yml~~; Discussions — вручную | тикеты приходят пригодными к чтению |
| ~~6~~ | ~~SECURITY + CODE_OF_CONDUCT + CHANGELOG + README.en~~ | набор «серьёзного проекта» полон |
| ~~7~~ | ~~CLA-бот~~ — нужна ветка `cla-signatures` и добавление в required checks | вклад можно перелицензировать |
| 8 | Тег `v0.1.0`, релиз с артефактами, README на тег | пользователи перестают сидеть на `main` |
| ~~9~~ | ~~CI-гейт «трогаешь abi.h — трогай CHANGELOG»~~ — джоба `abi` | бампы ABI перестают быть тихими |

Шаги 1–3 имеет смысл сделать до любого анонса; 4–6 — вместе с анонсом; 7–9 — до того,
как появится первый внешний плагин в природе.

---

## Приложение А. Настройки GitHub — по кликам

Всё ниже делается в вебе: API-доступа из рабочей копии нет (ни `gh`, ни сохранённого
токена), а заводить его ради семи галочек — лишнее.

### А.1 Почему «пункты не сходятся»

Rulesets — это не старый Branch protection, и половина расхождений отсюда:

| Что мешает | Как на самом деле |
|---|---|
| Не найти «Require status checks» с нужными галками | Это отдельное правило в списке `Rules`, а выпадающий список контекстов **показывает только те проверки, которые GitHub уже видел**. CI ещё нет — значит `build`/`tests`/`format` добавить физически нельзя. Это делается после того, как `ci.yml` отработает хотя бы раз |
| «Required approvals» не находится отдельным пунктом | Он вложен внутрь `Require a pull request before merging` — раскрывается только когда галка включена |
| Непонятно, что делать с `Restrict updates` / `Restrict creations` | **Не включать.** `Restrict updates` запрещает обновлять ветку вообще — с ним не вольётся ни один PR. Нужные ограничения — это `Restrict deletions` и `Block force pushes` |
| Bypass list требует кого-то указать | Не требует. Пустой список — правильно: approvals ноль, поэтому сам себя ты не блокируешь и обходить нечего |
| Ruleset вроде создан, но ничего не запрещает | `Enforcement status` остался `Disabled` или `Evaluate`. Нужен **`Active`** |

### А.2 Импорт ruleset вместо ручного набора

В репозитории лежит готовый файл — [.github/rulesets/main.json](../.github/rulesets/main.json).

`Settings` → `Rules` → `Rulesets` → стрелка рядом с `New ruleset` → **`Import a ruleset`**
→ выбрать `main.json` → `Create`.

Что в нём:

| Правило | Значение |
|---|---|
| Target | `~DEFAULT_BRANCH` (то есть `main`) |
| Enforcement | `Active` |
| Restrict deletions | вкл |
| Block force pushes (`non_fast_forward`) | вкл |
| Require linear history | вкл |
| Require a pull request before merging | вкл, **approvals = 0** |
| ↳ Dismiss stale approvals | вкл |
| ↳ Require conversation resolution | вкл |
| ↳ Allowed merge methods | только `squash` |
| Bypass list | пусто |

**Что добавить позже, когда проверки появятся** (`Settings` → `Rules` → `main` → `Edit`):
галка `Require status checks to pass` + `Require branches to be up to date`, а в списке
контекстов выбрать `format`, `build`, `tests` (после первого прогона `ci.yml`) и проверку
CLA (после первого PR — её имя увидишь в списке checks этого PR).

### А.3 Мерж

`Settings` → `General` → `Pull Requests`:

- `Allow merge commits` — **выкл**
- `Allow rebase merging` — **выкл**
- `Allow squash merging` — **вкл**, `Default message` = **`Pull request title and description`**
- `Automatically delete head branches` — **вкл**

Ruleset дублирует запрет методов на своём уровне: даже если галку случайно вернут,
влить не-squash в `main` не выйдет.

### А.4 Доступ

- `Settings` → `Collaborators and teams` — пусто. Внешние работают через форк и PR.
- Репозиторий в организации, поэтому проверить и её: `KRdayzmodding` → `Settings` →
  `Member privileges` → `Base permissions` = **`Read`** (не `Write`). Иначе любой участник
  организации получает право на push мимо всех договорённостей.
- `Settings` → `Actions` → `General` → `Fork pull request workflows from outside
  collaborators` = **`Require approval for all outside collaborators`**. CI с форка не
  запустится без твоего клика; проверку CLA это не тормозит — она на
  `pull_request_target` и живёт в контексте базовой ветки.

### А.5 Возможности репозитория

`Settings` → `General` → `Features`: `Issues` — вкл, `Discussions` — **вкл**,
`Wikis` — выкл, `Projects` — выкл.

Discussions нужны как адрес для «как этим пользоваться»: в `ISSUE_TEMPLATE/config.yml`
на них уйдёт ссылка, а blank issues будут выключены.

### А.6 Хранилище подписей CLA

Ветка `cla-signatures` создаётся один раз, руками. Она сирота (orphan) — в ней нет и не
должно быть кода:

```bash
git switch --orphan cla-signatures
git commit --allow-empty -m "chore(cla): хранилище подписей"
git push -u origin cla-signatures
git switch main
```

Ruleset её не трогает (он нацелен только на `main`), и это принципиально: бот пишет
подписи прямым push, а в `main` прямой push запрещён.

Проверка после первого чужого PR: бот оставляет комментарий со ссылкой на
[docs/CLA.md](CLA.md), контрибьютор отвечает строкой из комментария, в
`cla-signatures/signatures/version1/cla.json` появляется запись, проверка зеленеет.
