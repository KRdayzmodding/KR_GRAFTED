// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-GRAFT-plugin-exception-1.0
// Этот файл линкуется в КАЖДЫЙ плагин, поэтому едет с исключением: мод на GRAFT ничего
// не обязан — даже закрытый и платный. См. LICENSE-EXCEPTION.
#include <windows.h>

#include <filesystem>
#include <format>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "graft/engine.hpp"
#include "graft/guard.hpp"
#include "graft/loader.hpp"

// Хранилище результата загрузки, отдельно от самой загрузки.
//
// Не косметика: класс Graft (host_natives.cpp) читает эти списки, а компилируется он и
// в хост, и в graft.exe — инструменту загрузчик не нужен и не может быть нужен, там нет
// ни игры, ни плагинов. Так у него оказываются те же символы, но пустые, а не отсутствующие.
namespace graft::loader {
namespace {

std::vector<plugins::entry> g_registry;
std::vector<row> g_rows;
std::vector<plugins::collision> g_collisions;
std::vector<void (*)(float)> g_ticks;

}  // namespace

void add_tick(void (*fn)(float)) {
    if (fn) {
        g_ticks.push_back(fn);
    }
}

// Зовётся с кадра движка или натива GraftTick, то есть по скриптовому потоку. Обработчик может
// звать движок — там же, где это делает сам скрипт. Ловушка — у каждого своя: раньше одна на
// всех, и упавший тик одного плагина отменял в этом кадре тики всех, кто после него.
void run_ticks(float dt) {
    for (void (*fn)(float) : g_ticks) {
        const fault_place place{reinterpret_cast<void*>(fn), "GRAFT_ON_TICK"};
        ::graft::detail::guarded<void>(reinterpret_cast<void*>(fn), [fn, dt] { fn(dt); });
    }
}

std::size_t tick_count() {
    return g_ticks.size();
}

namespace {
void* g_root = nullptr;
}

void set_script_root(void* game) {
    if (game) {
        g_root = game;
    }
}

void* script_root() {
    return g_root;
}

namespace {
// Где упал чужой код, когда это не натив: ставит fault_place на время вызова.
thread_local void*       g_fault_code  = nullptr;
thread_local const char* g_fault_where = nullptr;
} // namespace

fault_place::fault_place(void* code, const char* where) : code_was_(g_fault_code), where_was_(g_fault_where) {
    g_fault_code  = code;
    g_fault_where = where;
}

fault_place::~fault_place() {
    g_fault_code  = code_was_;
    g_fault_where = where_was_;
}

std::string module_of(const void* address) {
    HMODULE mod = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        static_cast<LPCWSTR>(address),
                                        &mod)) {
        return {};
    }
    wchar_t     path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
    if (n == 0) {
        return {};
    }
    const std::wstring file = std::filesystem::path{std::wstring(path, n)}.filename().wstring();
    const int          size = WideCharToMultiByte(CP_UTF8, 0, file.data(), static_cast<int>(file.size()), nullptr, 0, nullptr, nullptr);
    std::string        out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, file.data(), static_cast<int>(file.size()), out.data(), size, nullptr, nullptr);
    return out;
}

namespace {
// Замок на весь отчёт о падении. Не роскошь: сбой может прийти с любого потока, и два
// одновременных падения рвали бы unordered_map ниже прямо внутри обработчика падений.
// Падение кода, который существует ради того, чтобы не падать, — худший вид падения.
//
// Форматирование оставлено СНАРУЖИ замка: под ним только правки состояния и журнал.
std::mutex  g_fault_lock;
std::size_t g_faults = 0;
std::string g_last_fault;
// Сколько строк в журнал отдано на один трамплин. Натив, падающий каждый кадр, иначе
// пишет по строке шестьдесят раз в секунду и добивает сервер тем, от чего его спасли.
// Счётчик падений при этом растёт всегда — молчаливая потеря хуже шумной.
std::unordered_map<void*, std::size_t> g_fault_lines;
constexpr std::size_t kMaxLinesPerNative = 3;
}

// Кто упал — ищем по адресу трамплина в реестре: там и имя плагина, и имя натива в
// скрипте. Ничего другого у нас на руках нет, а этого достаточно, чтобы в журнале была
// не «где-то в hid», а «плагин SIXW_GRAFT, натив SeraphNode.Id».
void note_fault(void* impl, std::uint32_t code, const void* at, const char* what) {
    const char* owner = "?";
    const char* class_name = nullptr;
    const char* name = "?";
    bool        native     = false;
    for (const plugins::entry& e : registry()) {
        if (e.desc && e.desc->impl == impl) {
            owner = e.owner ? e.owner : "?";
            class_name = e.desc->class_name;
            name = e.desc->name;
            native     = true;
            break;
        }
    }
    // Не натив — тик, обработчик события или задание: DLL по адресу кода, место — от вызвавшего.
    std::string module;
    if (!native && (g_fault_code || g_fault_where)) {
        module = module_of(g_fault_code ? g_fault_code : impl);
        owner  = module.empty() ? "?" : module.c_str();
        name   = g_fault_where ? g_fault_where : "?";
    }
    const std::string who =
        class_name ? std::format("{}.{}", class_name, name) : std::string{name};
    std::string line =
        what ? std::format("[{}] {}: exception - {}", owner, who, what)
             : std::format("[{}] {}: fault {:#x} at {}", owner, who, code, at);

    const std::scoped_lock held{g_fault_lock};
    ++g_faults;
    g_last_fault           = std::move(line);
    const std::size_t said = g_fault_lines[impl]++;
    if (said < kMaxLinesPerNative) {
        graft::log("! " + g_last_fault + " - call cancelled, the game keeps running");
    } else if (said == kMaxLinesPerNative) {
        graft::log("! " + who + ": keeps faulting - silent from here on, count in GraftFaultCount()");
    }
}

std::size_t fault_count() {
    const std::scoped_lock held{g_fault_lock};
    return g_faults;
}

std::string last_fault() {
    const std::scoped_lock held{g_fault_lock};
    return g_last_fault;
}

namespace detail {
std::vector<plugins::entry>& registry_ref() {
    return g_registry;
}
std::vector<row>& rows_ref() {
    return g_rows;
}
std::vector<plugins::collision>& collisions_ref() {
    return g_collisions;
}
}  // namespace detail

const std::vector<plugins::entry>& registry() {
    return g_registry;
}

const std::vector<row>& rows() {
    return g_rows;
}

const std::vector<plugins::collision>& collisions() {
    return g_collisions;
}

// Веха установки. Порядок «хуки -> плагины -> маяк» — единственное место в библиотеке
// с настоящей гонкой, и увидеть его перестановку можно только по журналу; время строке
// ставит сам журнал.
void mark(const char* what) {
    graft::log(what ? what : "");
}

}  // namespace graft::loader
