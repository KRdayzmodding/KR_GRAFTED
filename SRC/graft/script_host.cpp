// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/script.hpp"

#include <windows.h>

#include <cstring>
#include <format>
#include <string>
#include <unordered_map>

#include "graft/engine.hpp"
#include "graft/loader.hpp"
#include "graft/stages.hpp"

// Здесь остаётся только то, чему нужно состояние: script-контексты движка и адреса,
// найденные сканом. Чистые чтения движковых структур живут inline в script.hpp —
// их зовут на каждый натив, и вызов через границу TU там был бы заметен.
namespace graft::detail {

// Хост сам себе плагин: его собственные нативы тоже под защитой, и разбирать их падения
// некому, кроме него же.
void note_fault(void* impl, unsigned code, const void* at, const char* what) {
    loader::note_fault(impl, code, at, what);
}

}  // namespace graft::detail

namespace graft::script {
namespace {

using find_index_fn = std::int32_t(__fastcall*)(void* class_desc, const char* name);
using reg_method_fn = void*(__fastcall*)(void* ctx, void* cls, const char* name, void* impl,
                                         unsigned ret_buf, char create);
using find_class_fn = void*(__fastcall*)(void* ctx, const char* name);

// Машинный код лежит на исполняемой странице, байткод скриптового метода — в обычной
// куче. Это и есть надёжный признак «можно звать напрямую».
bool is_code(const void* p) {
    if (!p) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kExec) != 0 && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

void* g_ctx = nullptr;
reg_method_fn g_register_method = nullptr;
find_class_fn g_find_class = nullptr;
find_index_fn g_find_index = nullptr;
// Контексты модулей, замеченные при регистрации. Их единицы, поэтому массив.
constexpr std::size_t kMaxContexts = 16;
void* g_contexts[kMaxContexts]{};
std::size_t g_context_count = 0;

}  // namespace

void bind(void* ctx, void* find_class, void* find_index) {
    g_ctx = ctx;
    g_find_class = reinterpret_cast<find_class_fn>(find_class);
    g_find_index = reinterpret_cast<find_index_fn>(find_index);
}

void set_register_method(void* fn) {
    g_register_method = reinterpret_cast<reg_method_fn>(fn);
}

bool register_method_late(const char* class_name, const char* name, void* impl, bool is_static,
                          bool marshalled) {
    void* cls = find_class(class_name);
    if (!g_register_method || !g_ctx || !cls) {
        return false;
    }
    void* desc = g_register_method(g_ctx, cls, name, impl, 0, 1);
    if (!desc) {
        return false;
    }
    // Движок, создавая функцию раньше разбора мода, метит её static|external — для
    // метода это надо снять.
    auto* flags = reinterpret_cast<std::uint32_t*>(static_cast<char*>(desc) + layout::desc_flags);
    if (is_static) {
        *flags |= layout::flag_static;
    } else {
        *flags &= ~(layout::flag_static | layout::flag_external);
    }
    // 0x20 — native, 0x40 — маршалируемый: без этого движок позовёт метод обычным
    // fastcall, а трамплин ждёт блок аргументов.
    if (marshalled) {
        *flags = (*flags | layout::flag_marshalled) & ~layout::flag_native;
    }
    return true;
}

// Отвечает ли движок. Это ступень лестницы, а не собственная самопроверка: раньше здесь
// стоял статик, который запоминал ПЕРВЫЙ ответ, — а первый раз спрашивают на маяке, когда
// классы ещё не разобраны. Из-за этого в журнале навсегда оставалось «lookup failed», при
// полностью рабочем API.
bool available() {
    return stage::reached(stage::step::linked);
}

void* root() {
    return loader::script_root();
}

void note_context(void* ctx) {
    if (!ctx) {
        return;
    }
    for (void* known : g_contexts) {
        if (known == ctx) {
            return;
        }
    }
    if (g_context_count < kMaxContexts) {
        g_contexts[g_context_count++] = ctx;
    }
}

void* find_class(const char* name) {
    if (!g_find_class) {
        return nullptr;
    }
    // Поздние контексты просматриваем первыми: там есть и игровые классы, и все ранние.
    for (std::size_t i = g_context_count; i > 0; --i) {
        if (void* cls = g_find_class(g_contexts[i - 1], name)) {
            return cls;
        }
    }
    return g_ctx ? g_find_class(g_ctx, name) : nullptr;
}

method find_method(void* class_desc, const char* name) {
    method out;
    if (!class_desc || !g_find_index) {
        return out;
    }
    const std::int32_t idx = g_find_index(class_desc, name);
    if (idx < 0) {
        return out;
    }
    auto* table =
        *reinterpret_cast<void***>(static_cast<char*>(class_desc) + layout::class_functions);
    if (!table) {
        return out;
    }
    auto* desc = static_cast<char*>(table[idx]);
    if (!desc) {
        return out;
    }
    out.impl = *reinterpret_cast<void**>(desc + layout::desc_impl);
    out.flags = *reinterpret_cast<std::uint32_t*>(desc + layout::desc_flags);
    // Сам дескриптор нужен обратному направлению: в нём шаблоны переменных-параметров.
    out.desc = desc;
    out.executable = is_code(out.impl);
    return out;
}

method find_method(const char* class_name, const char* name) {
    return find_method(find_class(class_name), name);
}

namespace {
// Глобали движка: имя -> импл, как их пронесла мимо нас регистрация. Строку копируем —
// движок волен держать своё имя где угодно, а таблица живёт до конца процесса.
std::unordered_map<std::string, void*>& globals() {
    static std::unordered_map<std::string, void*> all;
    return all;
}

// Обычная строка вместо char[192]: обрезать диагностику по длине имени класса — ровно
// то, чего от диагностики не ждёшь.
std::string g_last_error;
}  // namespace

void note_global(const char* name, void* impl) {
    if (name && impl) {
        // Первый победил: движок регистрирует своё до нас, и подменять его чужим
        // одноимённым нативом мы не станем.
        globals().emplace(name, impl);
    }
}

void* find_global(const char* name) {
    if (!name) {
        return nullptr;
    }
    const auto found = globals().find(name);
    return found == globals().end() ? nullptr : found->second;
}

void note_call_miss(const char* class_name, const char* name, void* self, const method& fn) {
    const char* why = !self      ? "объект null"
                      : !fn.impl ? "метод не найден"
                                 : "метод не нативный (в impl байткод)";
    g_last_error = std::format("{}.{}: {}", class_name ? class_name : "?", name ? name : "?", why);
    log("! " + g_last_error);
}

const char* last_call_error() {
    return g_last_error.c_str();
}

}  // namespace graft::script
