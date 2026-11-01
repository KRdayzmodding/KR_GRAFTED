# Погода: явления, движковый генератор прогнозов и чем её вести из плагина

Погода в DayZ — движковая симуляция, которую скрипты настраивают, но не считают.
Это редкий для graft «идеальный» случай: почти весь её API объявлен `proto native`/
`proto` и закрывается штатными путями (`ref::call`, `callout`, зеркало `graft apigen`)
— без хуков, скана и адресов.

## Откуда известно и чем перепроверять

Фактов из бинаря здесь нет ни одного — всё видно из публичных скриптов игры:

| Источник | Что дал |
|---|---|
| зеркало `BohemiaInteractive/DayZ-Script-Diff`, ветка main, снято 2026-11-01 (строка 1.30) | `3_Game/weather.c`, `worlddata.c`, `global/game.c`, `sandstorm.c`, `debugweatherrpcdata.c`, `5_mission/gui/scriptconsoleweathertab.c`, `4_world/systems/sandstorm/scriptedsandstormcontroller.c`, `4_world/classes/undergroundhandlerclient.c` — API, арбитраж, бури, диагностическая консоль |
| та же система в ~1.14 (`dayzexplorer.zeroy.com`) | где жил конечный автомат до переезда в движок; тело `OnBeforeChange` сравнивал с текущим |
| BI Wiki: `DayZ:Weather_Configuration`, `DayZ:cfgweather.xml`; практика модов (OpenDayZ, init.c-гайды) | конфигурация без скриптов; употребление `MissionWeather` живыми серверами |

После патча игры: перегенерить зеркало (`graft apigen`) и сверить файлы выше в
зеркале скриптов. Расхождение зеркала с игрой ловит сам graft: `ref::call` ответит
`wrong_type` со строкой в журнале, а не падением сервера.

## Три слоя

1. **Движок.** Хранит и симулирует явления, интерполирует их к прогнозу, сам
   синхронизирует состояние на клиенты, сам рисует молнии (на клиенте).
2. **Скриптовая обёртка** (`3_Game/weather.c`): `Weather` и `WeatherPhenomenon` —
   почти целиком `proto native`. Это и есть публичный API.
3. **Настройка**: `cfgweather.xml` — его читает сам движок, в скриптах он не
   разбирается вовсе; и `WorldData` (`3_Game/worlddata.c` + подклассы карт) с
   порогами и veto-хуком.

## Явление: actual, forecast и расписание

`EWeatherPhenomenon`: `OVERCAST, FOG, RAIN, SNOWFALL, WIND_DIRECTION, WIND_MAGNITUDE,
VOLFOG_HEIGHT_DENSITY, VOLFOG_DISTANCE_DENSITY, VOLFOG_HEIGHT_BIAS`; песчаная буря —
отдельным методом контроллера.

Overcast — главное значение: дождь и молнии возможны, только когда оно в заданном
окне (`SetRainThresholds`, дефолт 0.6–1.0; при выходе из окна дождь гаснет за 30 с;
штормовой порог ≈0.85–0.9 по миру). Кто хочет рулить дождём — рулит сначала overcast
или раздвигает окно порогов.

У каждого явления есть текущее значение, прогноз и время, за которое движок ДОЕДЕТ к
прогнозу, и момент следующего пересчёта. Извне задаются только ЦЕЛИ — интерполяция
движковая, плавные переходы достаются бесплатно.

Рецепт мгновенной смены — из диагностической консоли самих разработчиков
(`scriptconsoleweathertab.c`): интерполяция 0 + длительность 960 — это потолки
слайдеров `INTERPOLATION_MAX = 920` и `DURATION_MAX = 960`. Консоль даже на выделенном
сервере применяет погоду через серверный RPC (`DEV_SET_WEATHER`) — ещё одно
подтверждение, что писатель всегда сервер. `DebugWeatherRPCData` (значение +
интерполяция + длительность на явление, −1 = «не трогать») — только отладочный канал
поверх, продакшн-синхронизация целиком внутри движка.

| Метод `WeatherPhenomenon` | Форма | Что |
|---|---|---|
| `GetActual()`, `GetForecast()`, `GetNextChange()` | proto native | состояние и расписание, включая БУДУЩЕЕ |
| `Set(forecast, time, minDuration)`, `SetNextChange(t)` | proto native | задать цель и момент пересчёта |
| `SetLimits`, `SetForecastChangeLimits`, `SetForecastTimeLimits` | proto native | границы, в которых живёт генератор |
| `GetLimits`, `GetForecastChangeLimits`, `GetForecastTimeLimits` | **proto (марш.)** | те же границы наружу: out-параметры |
| `OnBeforeChange(change, time)` | скриптовый | арбитраж перед каждым пересчётом, см. ниже |

Пары с `out float` — маршалируемые неспроста: движок запрещает `out` у значений в
нативах («Native functions don't support 'out' arguments») — тот же ABI-факт, по
которому graft различает две формы и в своих нативах, и в вызове чужих.

## Контроллер Weather

Вход: `CGame.GetWeather()` — `proto native` (`3_Game/global/game.c`); объект создаёт
движок, конструктор приватный. Дальше: геттеры явлений (`GetOvercast/GetRain/GetFog/
GetSnowfall/GetWindDirection/GetWindMagnitude/GetSandstorm`), ветер (`GetWind/
SetWind(vector)`, `Get/SetWindSpeed`, `SetWindMaximumSpeed`, `SetWindFunctionParams`),
шторм `SetStorm(density, threshold, timeOut)` — плотность молний, порог overcast,
пауза между разрядами, — объёмный туман `SetDynVolFog*`, снег.

Что НЕ синхронизировано и потому живёт только на той стороне, где его задали:

| Метод | Где имеет смысл |
|---|---|
| `SuppressLightningSimulation(bool)` | клиент: симуляция молний вместе со звуком |
| `SetSnowflakeScale` | клиент; ванильный сервер задаёт детерминированно (движок это значение не разносит) |
| `MissionWeather(bool)`, `SetWeatherUpdateFreeze(bool)` | сервер: скриптовые ПОЛЯ объекта Weather, арбитраж генератора, см. ниже |

Ванильный образец употребления `SuppressLightningSimulation` —
`undergroundhandlerclient.c`: игрок ушёл под землю глубже `EUndergroundPresence.OUTER`
— выключили, вышел — включили, и то же в деструкторе (уборка за собой). Готовый
шаблон для клиентских плагинов.

`Weather.GetTime()` — секунды от старта сервера: расписание погоды детерминировано
относительно аптайма, а не реального времени.

## Песчаная буря: единственная пространственная погода

`Weather.GetSandstorm()` — не явление-скаляр, а ДВИЖУЩИЙСЯ объект с географией
(`3_Game/sandstorm.c`, класс `SandstormController`; конструктор приватный — создаёт
движок):

| Метод | Что |
|---|---|
| `GetPosition()`, `GetDirection()`, `GetSpeed()` | где фронт и куда едет (м/с) |
| `IsPositionAtEnd()`, `GetRemainingMovementDuration()` | сколько движения осталось |
| `GetPointOnEdge(worldPos, out outPos)` | проекция на границу; ОТРИЦАТЕЛЬНОЕ расстояние — точка внутри бури |
| `GetIntensity(worldPos)`, `GetMagnitude()` | локальная интенсивность в любой точке мира |
| `Start(duration, calledByWeather)`, `Stop(duration, ...)` | сервер-only: собраться за duration, движение, угасание |

Синхронизация — движковая («driven by the server and replicated to all clients»),
опоздавший клиент получает `OnStart(continuedSession)`. Маршрут движок выбирает сам:
у `Start` нет аргументов-позиций. Скриптовая прослойка
(`scriptedsandstormcontroller.c`) — только клиентские эффекты: пер-игровая
интенсивность с «укрытиями» (под землёй / в здании по рейкасту и звуку / в машине —
фейд 1.5 с; глубже 2 м от поверхности — линейное затухание), PPE-эффект, симптом
прищура, модификатор exposure. Модам — хук `GetScriptedType()` (подменить контроллер
своим классом) и ScriptInvoker'и старта/остановки. Триггер ванильных бурь —
`WorldData.StartSandstorm`: ветер направлением в окне −1.77..−1.20 и силой ≥18,
частота из cfgGameplay.json.

## Кто пишет прогнозы: движковый генератор и арбитраж

Прогнозы пересчитывает ДВИЖОК на сервере. Перед применением изменения он зовёт
скриптовый `WeatherPhenomenon.OnBeforeChange(change, time)` — «Called on server
only». Ответ решает судьбу изменения: `true` — «скрипт уже задал состояние», движок
своё не применяет; `false` — применяет своё. Тело колбэка — три ступени:

```
MissionWeather(true)  -> сразу false, WorldData не спрашивается
SetWeatherUpdateFreeze(true) -> true  заморозка: движок пропускает изменения
иначе -> WorldData.WeatherOnBeforeChange(type, actual, change, time)
         база: false («не трогал»); подкласс карты делает свои Set(...) и
         возвращает true
```

`m_UpdateFrozen` — скриптовое поле, читает его сам колбэк, семантика однозначна.
А вот `m_missionWeather` ПРОТИВОРЕЧИВ даже по исходникам. Тело `OnBeforeChange` в
~1.14 и сегодня идентично (`MissionWeather(true)` -> сразу `false`); комментарий
worlddata.c («поведение по умолчанию — как MissionWeather(true)») и конструктор
(`m_missionWeather = false`, при живой динамической погоде в ваниле) согласуются
только с чтением «false (дефолт) = автопилот работает». Но рабочие init.c-гайды
«вечно ясно» (BI Wiki, OpenDayZ) строятся на `MissionWeather(false)` как на
ОТКЛЮЧЕНИИ автопилота. Сходится лишь если флаг читает сам движок в обход колбэка
(в ~1.14 метод был `proto native`) — либо гайды устарели с переездом автомата в
движок. Из скриптов не разрешается; закрывает живой замер: сервер, тик пишет
`GetNextChange()`/`GetForecast()` всех явлений -> `MissionWeather(false)` -> смотрим,
двигается ли расписание; контрольный прогон с `m_UpdateFrozen = true`.

Исторически (до ~1.15) весь конечный автомат жил в скриптах подкласса `WorldData`
(`ChernarusPlusData`: CLEAR/CLOUDY/BAD, полосы дождя по overcast, туманное утро с
шансом 15%) — потом он переехал в движок, скриптам остался veto и подстройка:
`CalculateWind`, `CalculateVolFog`, пороги в `WorldDataWeatherSettings` (rain 0.6,
storm 0.85, thundersnow 0.98, snowfall 0.3), песчаные бури по направлению и силе
ветра (частота — из cfgGameplay.json).

## Синхронизация и потребители

Сервер — единственный писатель; движок сам разносит явления клиентам, у всех игроков
одна погода. Отсюда практическое следствие: серверный плагин меняет погоду, и клиенты
видят её БЕЗ ЕДИНОГО МОДА на своей стороне.

Потребители в скриптах: `Environment` (температура игрока: охлаждение с высотой,
«утепление» облаками, wind chill через WorldData.GetTemperature), мокрота, AI-шум
(с 1.30 — нативно через AIParams, скриптовый `GetNoiseReductionByWeatherEx` —
Obsolete), сбор дождевой воды (`RainProcurementManager`).

## Что из этого для graft

| Задача | Путь |
|---|---|
| чтение на тике | `graft::game()` → `cast<graft::dayz::CGame>` → `GetWeather()` → `GetOvercast().GetActual()`; всё `ref::call` по дескриптору, зеркало — `graft apigen` на весь `3_Game` (паттерн — examples/players) |
| прогнозы наружу | `GetForecast`/`GetNextChange` читаются дёшево: будущие изменения уже лежат в объекте, их можно анонсировать, а не угадывать |
| управление | те же `Set*` из СЕРВЕРНОГО плагина; клиентский плагин — только чтение и несинхронизируемая локаль (молнии, масштаб снежинок) |
| пространственные бури | `GetIntensity(worldPos)`/`GetPointOnEdge` — нативы: экспозиция любой точки карты дёшево и без скриптов; `Start/Stop` — серверные нативы, плагин может водить бури сам |
| `Get*`-пары с out | `callout.hpp` (`call_proto`): арность и шаблоны — по дескриптору |
| владение | приватный ctor → `lifetime::engine`: между тиками не держать, брать заново `game.GetWeather()` (один нативный вызов) или `borrowed<>` |
| взятие управления | `MissionWeather`/`SetWeatherUpdateFreeze` — скриптовые поля, и graft их уже пишет: `field_proxy::operator=` (`graft/types.hpp`) кладёт int/float/bool/vector прямо в поле, зеркало генерирует `Weather::m_UpdateFrozen()` и `m_missionWeather()` — `weather.m_UpdateFrozen() = true;` работает сегодня, `EnScript.SetClassVar` не нужен |
| перехват | не нужен: весь API погоды открыт нативами, врезаться не во что |

Чего сделать НЕЛЬЗЯ: региональной погоды для явлений-скаляров нет — overcast/дождь/
туман глобальны на весь сервер, у вольфога лишь глобальные параметры высоты и
дистанции. «Дождь над одним городом» движковой погодой не выражается; единственное
исключение — песчаная буря: ОДИН движковый пространственный фронт на сервер
(см. раздел выше).

## Источники

- `github.com/BohemiaInteractive/DayZ-Script-Diff` — `scripts/3_game/weather.c`,
  `worlddata.c`, `global/game.c`, `sandstorm.c`, `debugweatherrpcdata.c`;
  `4_world/systems/sandstorm/scriptedsandstormcontroller.c`;
  `4_world/classes/undergroundhandlerclient.c`;
  `5_mission/gui/scriptconsoleweathertab.c` (текущая строка).
- `dayzexplorer.zeroy.com` — `weather_8c`, `chernarusplus_8c` (~1.14): скриптовый
  автомат до переезда в движок.
- `community.bistudio.com/wiki/DayZ:Weather_Configuration`,
  `community.bistudio.com/wiki/DayZ:cfgweather.xml`; практика `MissionWeather(false)`
  в init.c: opendayz.net, гайды r/dayz.
