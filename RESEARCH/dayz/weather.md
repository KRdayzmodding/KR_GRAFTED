# Погода: явления, движковый генератор прогнозов и чем её вести из плагина

Погода в DayZ — движковая симуляция, которую скрипты настраивают, но не считают.
Это редкий для graft «идеальный» случай: почти весь её API объявлен `proto native`/
`proto` и закрывается штатными путями (`ref::call`, `callout`, зеркало `graft apigen`)
— без хуков, скана и адресов.

## Откуда известно и чем перепроверять

Фактов из бинаря здесь нет ни одного — всё видно из публичных скриптов игры:

| Источник | Что дал |
|---|---|
| зеркало `BohemiaInteractive/DayZ-Script-Diff`, ветка main, снято 2026-11-01 (строка 1.30) | `3_Game/weather.c`, `3_Game/worlddata.c`, `3_Game/global/game.c` — весь API и арбитраж |
| та же система в ~1.14 (`dayzexplorer.zeroy.com`) | где жил конечный автомат до переезда в движок |
| BI Wiki: `DayZ:Weather_Configuration`, `DayZ:cfgweather.xml` | конфигурация без скриптов |

После патча игры: перегенерить зеркало (`graft apigen`) и сверить три файла выше в
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

`Weather.GetTime()` — секунды от старта сервера: расписание погоды детерминировано
относительно аптайма, а не реального времени.

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

`m_UpdateFrozen` и `m_missionWeather` — скриптовые поля, но читает их ДВИЖОК у
своего же объекта: в ~1.14 `MissionWeather` был ещё `proto native`, теперь поле.
Что именно генератор делает с флагом миссии дальше — из скриптов не видно; это
открытый вопрос на живой замер (кейс игровой сьюты), а не факт.

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
| `Get*`-пары с out | `callout.hpp` (`call_proto`): арность и шаблоны — по дескриптору |
| владение | приватный ctor → `lifetime::engine`: между тиками не держать, брать заново `game.GetWeather()` (один нативный вызов) или `borrowed<>` |
| взятие управления | `MissionWeather`/`SetWeatherUpdateFreeze` — скриптовые поля, а сеттера скриптовых полей в graft нет (чтение есть — `try_script_var` через `EnScript.GetClassVar`; нужен парный `SetClassVar`). Обход без нового кода: пер-тиковые `Set()` поверх автопилота |
| перехват | НЕ нужен и опасен: погодные нативы — float-аргументы, зовутся отовсюду; детуры таких функций запрещены см. docs/engine-access.md |

Чего сделать НЕЛЬЗЯ: региональной и персональной погоды нет — явления это глобальные
скаляры сервера, у вольфога лишь глобальные параметры высоты/дистанции. «Дождь над
одним городом» движковой погодой не выражается.

## Источники

- `github.com/BohemiaInteractive/DayZ-Script-Diff` — `scripts/3_game/weather.c`,
  `scripts/3_game/worlddata.c`, `scripts/3_game/global/game.c` (текущая строка).
- `dayzexplorer.zeroy.com` — `weather_8c`, `chernarusplus_8c` (~1.14): скриптовый
  автомат до переезда в движок.
- `community.bistudio.com/wiki/DayZ:Weather_Configuration`,
  `community.bistudio.com/wiki/DayZ:cfgweather.xml`.
