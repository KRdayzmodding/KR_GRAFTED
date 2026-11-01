# Central Economy DayZ: карта подсистемы и путь к живому reload экономики

Всё ниже снято с **DayZServer_x64 1.30.164200.56 (retail)** и перепроверено автором graft
на **1.30.164078.38** (постоянный сдвиг +0x98d0). Адреса — RVA, паттерны для плагина —
строка-якорь + сигнатура пролога (`scan.hpp`).

Метод — офлайн-скан по якорям (тем же, что `scan.hpp` в рантайме).
Скрипт для перепроверки: [`scripts/ce_map_scan.py`](../scripts/ce_map_scan.py).

## Зачем

Живой reload экономики на работающем сервере: перечитать types.xml и применить
параметры без рестарта. Движкового API нет; путь — найти загрузчик и позвать
повторно, или найти type entries и писать напрямую.

## Архитектура CE (полная)

```
Runtime orchestrator (0x8f43e0) "Loading core data"
  ├─ vtable → Shared zone (0x6a0000-0x6d0000)
  │    ├─ SpawnRandomLoot (0x6c9d20, 0x10d0) — спавн
  │    ├─ DynamicEvent (0x6c89b0, 0xbd6) — события
  │    ├─ LootRespawner (0x6d5fd0, 0x79) — «Initial Spawn DISABLED»
  │    │    └─ 0x6d5e29 — «Initially (re)spawned»
  │    ├─ TypeSetup (0x6bbc40, 0x91f) — регистрация + cfgspawnabletypes
  │    ├─ Type lookup (0x6b4570, 0x19) — hash-table поиск
  │    └─ Transformer (0x6b5e70, 0x2f0) — spawn point creation
  └─ file parsers (runtime zone 0x8f0000+)
       ├─ types.xml (0x8fd370) ← подтверждено по "Failed to read types file"
       ├─ events.xml (0x8fbda0) ← НЕ types! "Failed to read events file"
       ├─ globals (0x8fc400), messages (0x8fcb10)
       └─ economy.xml (0x8fb210)
```

## Иерархия хранения типов (открытие)

```
Hash Table (491 записей, 1024 бакетов)
  └─ wrapper (hash table value, 24-байтные бакеты)
       └─ [промежуточная обработка через Transformer 0x6b5e70]
            └─ Object (создаётся трансформером)
                 └─ [+0x60] → массив GROUPS
                      └─ Group → [+0x10] → массив TYPE ENTRIES
                           └─ TYPE ENTRY:
                                [+0x54] = NOMINAL (int32)
                                [+0x58] = MIN (int32)
                                [+0x2c] = counter_на_земле
                                [+0x30] = counter_в_контейнерах
```

Type entries НЕ хранятся в hash table напрямую. Hash table содержит wrappers;
type entries создаются при spawn-инициализации через Transformer, в иерархии
object → group → type_entry (3-4 уровня косвенности).

## Структура TYPE ENTRY (подтверждена)

| Смещение | Тип | Поле |
|---|---|---|
| +0x1c | float | restock timer |
| +0x20 | float | другой timer |
| +0x2c | int32 | counter: items on ground |
| +0x30 | int32 | counter: items in containers |
| +0x50 | int32 | lifetime / категория |
| +0x54 | int32 | **NOMINAL** |
| +0x58 | int32 | **MIN** |
| +0x69 | bool | flag |
| +0x88 | int32 | flags |

## Формула спавна (из RESPAWN CANDIDATE 0x6d6863)

```asm
mov esi, dword ptr [rax + 0x54]  ; nominal
mov edx, dword ptr [rbx + 0x2c]  ; count1
mov ecx, dword ptr [rbx + 0x30]  ; count2
lea eax, [rdx + rcx]             ; total = count1 + count2
cmp esi, eax                      ; nominal > total?
jg respawn                        ; да → спавнить (nominal - total) штук
```

## Классы по RTTI

~25 классов `CE*` + `ADBOffline`/`ADBOnline`. Для контекста: скриптовый `CEApi`.

## Хук type lookup (0x6b4570)

Пролог: `48 83 EC 08 83 79 28 00` (sub rsp,8; cmp [rcx+0x28],0).
Находится через SpawnRandomLoot (смещение ~0x157B0 от него).
Вызывается при буте для каждого типа. Возвращает wrapper (не type entry!).

```
поиск: function_referencing("[CE][SpawnRandomLoot] (%s) :: !!! Attempt to spawn unknown class :: %s")
       → spawn_fn → spawn_fn - 0x157B0 → проверить пролог → hook
```

## Эксперимент: HeapWalk + запись nominal (частичный успех)

HeapWalk перебирает аллокации кучи, находит type entry по паре (nominal, min).
Запись nominal работает технически, но вызывает масс-spawn: CE's счётчики
не синхронизированы с записью, CE видит противоречие и respawnит всё.

**Вывод:** прямая запись nominal без обновления счётчиков → масс-spawn.
Безопасного способа обновить счётчики не найдено (+0x30 — не простой int32,
запись 0 вызвала крэш 0xe06d7363).

## Эксперимент: полный повторный OnInit (отрицательный)

Hook на `[CE][Hive] :: Loading core data` (0x8f43e0), повторный вызов:
- Фазы проходят (CoreData → TypeSetup → Storage), ~3 сек
- Через ~3 мин при спавне лута → `0xC0000005` (freed allocation)

## Открытые вопросы

1. **Кто пишет счётчики?** В функциях спавна (LootRespawner, SpawnRandomLoot)
   нет записей в +0x2c/+0x30 — только чтение. Обновление счётчиков происходит
   в другой подсистеме (callback? периодический scan?).

2. **Безопасный триггер респавна для одного типа** — нужно найти функцию,
   которая вызывает respawn check для конкретного типа (не для всех).

3. **Идентификация type entry по имени** — сейчас ищем по (nominal, min),
   что не уникально. Нужна цепочка hash_table → type_id → type_entry.

## Что работает (для DP_Storage)

- Hook types.xml loader → захват CE context
- Hook type lookup → захват wrapper
- HeapWalk → технически находит entries
- Вайпы слоёв/игроков/лута — всё работает
- **Надёжный способ смены экономики: types.xml + рестарт сервера**
