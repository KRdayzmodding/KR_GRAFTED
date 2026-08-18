// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Этот файл линкуется в КАЖДЫЙ плагин, поэтому едет с исключением: мод на GRAFT ничего
// не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
// Клиентская половина библиотеки: то, что компилируется В КАЖДЫЙ ПЛАГИН.
//
// Здесь ровно две вещи — переходники к сервисам хоста и экспорт, которым плагин
// представляется. Всё остальное (трамплины, разбор аргументов, арена строк, чтения
// движковых структур) живёт в заголовках и компилируется прямо в плагин, поэтому
// горячий путь эту границу не пересекает вовсе.
//
// Правило, которое здесь нельзя нарушать: каждая функция ниже зовётся либо на старте,
// либо один раз на пару <класс, метод> (вызывающая сторона мемоизирует), либо на пути
// ошибки. Появится здесь что-то с горячего пути — плагин потеряет в скорости ровно на
// переход через таблицу импорта, и вентиль Perf_HotPathBudget это увидит.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "graft/abi.h"
#include "graft/engine.hpp"
#include "graft/native.hpp"
#include "graft/script.hpp"

// Задаётся макросом GRAFT_PLUGIN в исходниках плагина. Если его забыли — линковщик
// скажет про неразрешённый graft_plugin_name_, и это лучше молчаливого «безымянный».
extern "C" const char* const graft_plugin_name_;
extern "C" const unsigned graft_plugin_version_;

namespace {

const graft_host_api* g_host = nullptr;

graft::script::method from_abi(const graft_method_info& info) {
    graft::script::method out;
    out.impl = info.impl;
    out.desc = info.desc;
    out.flags = info.flags;
    out.executable = info.executable != 0;
    return out;
}

}  // namespace

namespace graft {

void set_log_path(std::string_view) {
    // Журнал один на процесс и принадлежит хосту: плагину нечего им распоряжаться.
}

void log(std::string_view line) {
    if (g_host) {
        // Сервис хоста — C-функция, ей нужен нольтерминированный указатель. Вьюха его
        // не гарантирует, поэтому здесь строка всё же строится — но только здесь и
        // только на пути журнала, который холодный.
        g_host->log(std::string{line}.c_str());
    }
}

}  // namespace graft

namespace graft::script {

void bind(void*, void*, void*) {}
void set_register_method(void*) {}
void note_context(void*) {}

bool available() {
    return g_host != nullptr;
}

void* find_class(const char* name) {
    return g_host ? g_host->find_class(name) : nullptr;
}

void* root() {
    return (g_host && g_host->script_root) ? g_host->script_root() : nullptr;
}

method find_method(void* class_desc, const char* name) {
    graft_method_info info{};
    if (g_host) {
        g_host->find_method(class_desc, name, &info);
    }
    return from_abi(info);
}

method find_method(const char* class_name, const char* name) {
    graft_method_info info{};
    if (g_host) {
        g_host->find_method_named(class_name, name, &info);
    }
    return from_abi(info);
}

bool register_method_late(const char* class_name, const char* name, void* impl, bool is_static,
                          bool marshalled) {
    return g_host && g_host->register_method_late(class_name, name, impl, is_static, marshalled);
}

void note_call_miss(const char* class_name, const char* name, void* self, const method& fn) {
    if (!g_host) {
        return;
    }
    const graft_method_info info{fn.impl, fn.desc, fn.flags, static_cast<uint8_t>(fn.executable)};
    g_host->note_call_miss(class_name, name, self, &info);
}

}  // namespace graft::script

namespace graft::detail {

// Падение внутри натива. Разбирать его должен хост: только у него есть реестр, по
// которому адрес трамплина превращается в «плагин X, натив Klass.Name».
void note_fault(void* impl, unsigned code, const void* at, const char* what) {
    if (g_host && g_host->note_fault) {
        g_host->note_fault(impl, code, at, what);
    }
}

}  // namespace graft::detail

namespace {
// Подписки, сделанные ДО того, как хост представился. Так бывает всегда, когда
// GRAFT_ON_TICK стоит в файле плагина: статические инициализаторы отрабатывают раньше
// graft_plugin_entry. Функция-локальная статика, а не переменная модуля, — порядок
// инициализации между единицами трансляции не определён.
std::vector<void (*)(float)>& pending_ticks() {
    static std::vector<void (*)(float)> all;
    return all;
}
}  // namespace

namespace graft {

// Точка входа для плагина: своего потока у библиотеки нет, поэтому C++ просыпается там
// же, где живёт скрипт. Список держит хост — он один, и веером из натива GraftTick
// расходится он.
void on_tick(void (*fn)(float)) {
    if (!fn) {
        return;
    }
    if (g_host && g_host->add_tick) {
        g_host->add_tick(fn);
        return;
    }
    pending_ticks().push_back(fn);
}

namespace detail {
tick_binder::tick_binder(void (*fn)(float)) {
    on_tick(fn);
}
}  // namespace detail

}  // namespace graft

namespace {

// Плоский снимок реестра. Строится один раз, живёт до конца процесса: хост держит на
// него указатели и перечитывает при каждой попытке отложенной регистрации.
const std::vector<graft_native_desc>& flatten() {
    static const std::vector<graft_native_desc> all = [] {
        std::vector<const graft::native*> ordered;
        for (const graft::native* n = graft::natives(); n; n = n->next) {
            ordered.push_back(n);
        }
        // Реестр складывается LIFO — разворачиваем, чтобы порядок совпадал с порядком
        // объявления в исходнике. От этого зависит, кто выиграет коллизию.
        std::vector<graft_native_desc> out;
        out.reserve(ordered.size());
        for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
            const graft::native& n = **it;
            graft_native_desc d{};
            d.class_name = n.class_name;
            d.name = n.name;
            d.impl = n.impl;
            d.ret = n.ret;
            d.args = n.args;
            d.module = n.module;
            d.declare_as = n.declare_as;
            d.is_static = static_cast<uint8_t>(n.is_static);
            d.marshalled = static_cast<uint8_t>(n.marshalled);
            d.generate = static_cast<uint8_t>(n.generate);
            out.push_back(d);
        }
        return out;
    }();
    return all;
}

}  // namespace

extern "C" __declspec(dllexport) uint32_t __cdecl graft_plugin_entry(const graft_host_api* host,
                                                                     graft_plugin_info* out) {
    if (!out) {
        return GRAFT_ERR_INTERNAL;
    }
    // host == nullptr — это генератор объявлений в обычном процессе: движка нет,
    // сервисы не понадобятся, заполняем только описание.
    if (host) {
        if (host->size < sizeof(graft_host_api) || host->abi != GRAFT_ABI_VERSION) {
            return GRAFT_ERR_ABI;
        }
        if (host->layout != GRAFT_LAYOUT_VERSION) {
            return GRAFT_ERR_LAYOUT;
        }
        g_host = host;
        // Подписки, сделанные статическими инициализаторами плагина, отдаём хосту
        // теперь — раньше отдавать было некому.
        for (void (*fn)(float) : pending_ticks()) {
            if (g_host->add_tick) {
                g_host->add_tick(fn);
            }
        }
        pending_ticks().clear();
    }

    const std::vector<graft_native_desc>& all = flatten();
    out->size = sizeof(graft_plugin_info);
    out->abi = GRAFT_ABI_VERSION;
    out->layout = GRAFT_LAYOUT_VERSION;
    out->name = graft_plugin_name_;
    out->version = graft_plugin_version_;
    out->count = static_cast<uint32_t>(all.size());
    out->natives = all.data();
    return GRAFT_OK;
}
