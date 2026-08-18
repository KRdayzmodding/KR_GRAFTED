// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Мод на GRAFT ничего не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#ifndef GRAFT_ABI_H
#define GRAFT_ABI_H
// Граница между хостом (dwmapi.dll) и плагином (<мод>/graft/*.dll). Чистый C: через неё
// не проходит ни один тип C++, поэтому плагин можно собрать другим компилятором, другой
// STL и другим стандартом.
//
// ГЛАВНОЕ, ЧТО НАДО ЗНАТЬ ПРО ЭТУ ГРАНИЦУ: горячий путь её не пересекает. Движок зовёт
// трамплин плагина НАПРЯМУЮ — при регистрации мы отдаём ему сырой адрес impl, и дальше
// он ходит по нему сам. Всё, что здесь перечислено, зовётся либо на старте (регистрация),
// либо один раз на пару <класс, метод> (поиск движкового метода мемоизирован), либо на
// пути ошибки. Добавлять сюда что-то, что зовётся на каждый вызов натива, нельзя.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Версия интерфейса: меняется при любой правке структур ниже.
#define GRAFT_ABI_VERSION 3u

// Версия РАСКЛАДКИ движковых структур (graft::layout::version). Отдельная от ABI, потому
// что ломается по другой причине: смещения запекаются в машинный код плагина, поэтому
// патч движка чинится не заменой хоста, а пересборкой всех плагинов. Хост обязан
// отказать плагину с другой раскладкой, иначе тот будет молча портить память.
#define GRAFT_LAYOUT_VERSION 2u

// Имя экспорта, который обязан быть у каждого плагина.
#define GRAFT_PLUGIN_ENTRY_NAME "graft_plugin_entry"

// Коды возврата graft_plugin_entry.
#define GRAFT_OK 0u
#define GRAFT_ERR_ABI 1u        // плагин собран под другую версию интерфейса
#define GRAFT_ERR_LAYOUT 2u     // плагин собран под другую раскладку движка
#define GRAFT_ERR_INTERNAL 3u

typedef struct graft_method_info {
    void* impl;
    // Сам дескриптор функции. Нужен только обратному направлению (звать движковый
    // `proto`): в нём лежат шаблоны переменных-параметров и их количество.
    void* desc;
    uint32_t flags;
    uint8_t executable;  // impl лежит на исполняемой странице — можно звать напрямую
} graft_method_info;

// Сервисы хоста. Всё это требует состояния движка (script-контексты, найденные сканом
// адреса) и потому не может жить в плагине.
typedef struct graft_host_api {
    uint32_t size;    // sizeof(graft_host_api) — задел на расширение
    uint32_t abi;     // GRAFT_ABI_VERSION хоста
    uint32_t layout;  // GRAFT_LAYOUT_VERSION хоста

    void (*log)(const char* line);
    void* (*find_class)(const char* name);
    void (*find_method)(void* class_desc, const char* name, graft_method_info* out);
    void (*find_method_named)(const char* class_name, const char* name, graft_method_info* out);
    uint8_t (*register_method_late)(const char* class_name, const char* name, void* impl,
                                    uint8_t is_static, uint8_t marshalled);
    void (*note_call_miss)(const char* class_name, const char* name, void* self,
                           const graft_method_info* fn);
    // Позвать плагин на каждом тике. Точка входа нужна, потому что своего потока у
    // библиотеки нет и быть не должно: всё обязано идти по скриптовому потоку. Зовётся
    // из натива GraftTick, который мод дёргает из своего OnUpdate.
    void (*add_tick)(void (*fn)(float));
    // Корень объектного графа игры (CGame). Его приносит мод вместе с тиком: движок
    // отдать глобальную переменную отказывается — см. graft::try_global.
    void* (*script_root)(void);
    // Натив упал или бросил исключение. impl — адрес трамплина: по нему хост находит в
    // реестре, чей это натив и как он называется в скрипте. what — текст исключения C++
    // либо NULL, если это аппаратный сбой (тогда осмысленны code и at).
    void (*note_fault)(void* impl, uint32_t code, const void* at, const char* what);
} graft_host_api;

// Один натив. POD-зеркало graft::native без указателя на следующий: строки живут в
// статике плагина, который не выгружается, поэтому хост их только читает.
typedef struct graft_native_desc {
    const char* class_name;         // NULL — глобальная функция
    const char* name;          // имя в скрипте
    void* impl;                // адрес трамплина в плагине
    const char* ret;           // тип возврата в терминах Enforce
    const char* const* args;   // типы аргументов, NULL-terminated
    const char* module;        // скриптовый модуль для объявления (1_Core / 3_Game / ...)
    const char* declare_as;    // заголовок класса, если отличается от class_name
    uint8_t is_static;
    uint8_t marshalled;        // объявлять как `proto`, а не `proto native`
    uint8_t generate;          // печатать объявление генератором
} graft_native_desc;

typedef struct graft_plugin_info {
    uint32_t size;
    uint32_t abi;
    uint32_t layout;
    const char* name;     // имя плагина: им он представляется в журнале и коллизиях
    uint32_t version;     // версия плагина, произвольная
    uint32_t count;
    const graft_native_desc* natives;
} graft_plugin_info;

// Единственный экспорт плагина.
//
// host == NULL допустим и обязан работать: так плагин загружает генератор объявлений в
// обычном процессе, где движка нет. В этом случае функция только заполняет out.
// Наружу не должно вылетать ни одного исключения.
typedef uint32_t(__cdecl* graft_plugin_entry_fn)(const graft_host_api* host,
                                                 graft_plugin_info* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // GRAFT_ABI_H
