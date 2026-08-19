// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/loader.hpp"

#include <windows.h>

#include <algorithm>
#include <format>
#include <span>
#include <string>

#include "graft/abi.h"
#include "graft/engine.hpp"
#include "graft/native.hpp"
#include "graft/plugins.hpp"
#include "graft/script.hpp"

// Загрузчик плагинов. Живёт только в хосте.
//
// Порядок здесь — не стилистика, а единственное место с настоящей гонкой: install()
// работает в отдельном потоке, пока главный уже несётся к движку. Хуки ставятся ДО
// загрузки плагинов, иначе N вызовов LoadLibrary расширили бы окно до маяка
// MemoryValidation. Каждый шаг помечается временем в журнале — если метки когда-нибудь
// встанут в другом порядке, это будет видно сразу, а не через месяц у одного человека.
namespace graft::loader {
namespace {

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), need);
    return out;
}

// ── Сервисы хоста ────────────────────────────────────────────────────────────
// Тонкие обёртки: сама работа в script_host.cpp. Здесь только перевод в C-типы.

void api_log(const char* line) {
    graft::log(line ? line : "");
}

void* api_find_class(const char* name) {
    return script::find_class(name);
}

void fill(const script::method& fn, graft_method_info* out) {
    if (!out) {
        return;
    }
    out->impl = fn.impl;
    out->desc = fn.desc;
    out->flags = fn.flags;
    out->executable = static_cast<uint8_t>(fn.executable);
}

void api_find_method(void* class_desc, const char* name, graft_method_info* out) {
    fill(script::find_method(class_desc, name), out);
}

void api_find_method_named(const char* class_name, const char* name, graft_method_info* out) {
    fill(script::find_method(class_name, name), out);
}

uint8_t api_register_method_late(const char* class_name, const char* name, void* impl,
                                 uint8_t is_static, uint8_t marshalled) {
    return static_cast<uint8_t>(
        script::register_method_late(class_name, name, impl, is_static != 0, marshalled != 0));
}

void api_note_call_miss(const char* class_name, const char* name, void* self,
                        const graft_method_info* fn) {
    script::method m;
    if (fn) {
        m.impl = fn->impl;
        m.desc = fn->desc;
        m.flags = fn->flags;
        m.executable = fn->executable != 0;
    }
    script::note_call_miss(class_name, name, self, m);
}

void api_add_tick(void (*fn)(float)) {
    add_tick(fn);
}

void* api_script_root() {
    return script_root();
}

void api_note_fault(void* impl, uint32_t code, const void* at, const char* what) {
    note_fault(impl, code, at, what);
}

void api_watch_object(void* self, void (*forget)(void*)) {
    script::watch_object(self, forget);
}

const graft_host_api& host_api() {
    static const graft_host_api api{sizeof(graft_host_api),
                                    GRAFT_ABI_VERSION,
                                    GRAFT_LAYOUT_VERSION,
                                    &api_log,
                                    &api_find_class,
                                    &api_find_method,
                                    &api_find_method_named,
                                    &api_register_method_late,
                                    &api_note_call_miss,
                                    &api_add_tick,
                                    &api_script_root,
                                    &api_note_fault,
                                    &api_watch_object};
    return api;
}

// ── Поиск DLL плагинов ───────────────────────────────────────────────────────

bool absolute(const std::string& path) {
    return path.find(':') != std::string::npos || path.rfind("\\\\", 0) == 0;
}

void list_dlls(const std::wstring& dir, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW found{};
    HANDLE h = FindFirstFileW((dir + L"\\*.dll").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    std::vector<std::wstring> here;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            here.push_back(dir + L"\\" + found.cFileName);
        }
    } while (FindNextFileW(h, &found));
    FindClose(h);
    // Порядок FindFirstFile файловая система не гарантирует, а от порядка зависит
    // победитель коллизии — сортируем, чтобы запуск был воспроизводимым.
    std::sort(here.begin(), here.end());
    out.insert(out.end(), here.begin(), here.end());
}

std::vector<std::wstring> candidates(const std::wstring& game_dir) {
    std::vector<std::wstring> out;
    // Плагин едет вместе со своим модом: @МОД/grafted/*.dll, по соседству с addons.
    // Порядок — как в -mod=, то есть тот же, в каком движок разбирает скрипты.
    for (const std::string& mod : plugins::mod_dirs(narrow(GetCommandLineW()))) {
        const std::wstring base = absolute(mod) ? widen(mod) : game_dir + L"\\" + widen(mod);
        list_dlls(base + L"\\grafted", out);
    }
    // Та же папка, но рядом с exe: <игра>/grafted/*.dll. Одно имя на оба места, чтобы
    // не держать в голове два. Нужна для разработки и для установок, где всё лежит в
    // одном моде (PBO собраны в @CLIENT/addons, отдельных папок под мод нет).
    list_dlls(game_dir + L"\\grafted", out);
    return out;
}

// Реестр самого хоста: его нативы участвуют в слиянии наравне с плагинскими, чтобы
// коллизия с ними находилась там же и тем же кодом.
const std::vector<graft_native_desc>& own_natives() {
    static const std::vector<graft_native_desc> all = [] {
        std::vector<const native*> ordered;
        for (const native* n = natives(); n; n = n->next) {
            ordered.push_back(n);
        }
        std::vector<graft_native_desc> out;
        out.reserve(ordered.size());
        for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
            const native& n = **it;
            out.push_back({n.class_name, n.name, n.impl, n.ret, n.args, n.module, n.declare_as,
                           static_cast<uint8_t>(n.is_static), static_cast<uint8_t>(n.marshalled),
                           static_cast<uint8_t>(n.generate)});
        }
        return out;
    }();
    return all;
}

void take(const graft_plugin_info& info, const char* owner, std::vector<plugins::entry>& into) {
    into.reserve(into.size() + info.count);
    for (const graft_native_desc& d : std::span{info.natives, info.count}) {
        into.push_back({&d, owner});
    }
}

}  // namespace

void load(const std::wstring& game_dir) {
    std::vector<plugins::entry> all;

    // Хост идёт первым: своё имя он занимает раньше любого плагина.
    {
        constexpr const char* self = "graft";
        for (const graft_native_desc& d : own_natives()) {
            all.push_back({&d, self});
        }
        detail::rows_ref().push_back({self, 0, "(хост)", GRAFT_OK,
                                      static_cast<std::uint32_t>(own_natives().size())});
    }

    for (const std::wstring& path : candidates(game_dir)) {
        row r{};
        r.path = narrow(path);
        HMODULE mod = LoadLibraryW(path.c_str());
        if (!mod) {
            r.name = "?";
            r.status = GRAFT_ERR_INTERNAL;
            graft::log("! не загрузилась " + r.path);
            detail::rows_ref().push_back(r);
            continue;
        }
        auto entry = reinterpret_cast<graft_plugin_entry_fn>(
            GetProcAddress(mod, GRAFT_PLUGIN_ENTRY_NAME));
        if (!entry) {
            // Обычная чужая DLL, случайно лежащая рядом, — не ошибка, просто не наша.
            FreeLibrary(mod);
            continue;
        }
        graft_plugin_info info{};
        const std::uint32_t code = entry(&host_api(), &info);
        r.status = code == GRAFT_OK ? plugins::check(info) : code;
        r.name = info.name ? info.name : "?";
        r.version = info.version;
        r.count = info.count;
        if (r.status != GRAFT_OK) {
            graft::log("! плагин " + r.name + " отклонён: " +
                       plugins::explain(r.status) + " (" + r.path + ")");
            detail::rows_ref().push_back(r);
            continue;
        }
        detail::rows_ref().push_back(r);
        // Владельца берём из САМОГО ПЛАГИНА, а не из только что положенной строки:
        // строка живёт в векторе, а он на следующем push_back переезжает, и короткие
        // имена (SSO) переезжают вместе с ним — указатель повис бы. У плагина же это
        // литерал из его образа, а плагины не выгружаются.
        take(info, info.name, all);
    }

    detail::registry_ref() = plugins::merge(all, detail::collisions_ref());
    for (const plugins::collision& c : collisions()) {
        graft::log(plugins::describe(c));
    }
    mark(std::format("плагинов: {}, нативов: {}, коллизий: {}", rows().size(), registry().size(),
                     collisions().size())
             .c_str());
    for (const row& r : rows()) {
        graft::log(std::format("  {:<20} v{:<4} {:>4} нативов  {}", r.name, r.version, r.count,
                               r.status == GRAFT_OK ? "" : plugins::explain(r.status)));
    }
}

}  // namespace graft::loader
