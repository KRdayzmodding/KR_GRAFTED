// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/engine.hpp"
#include "graft/frame.hpp"

#include <windows.h>

#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <MinHook.h>

#include "graft/loader.hpp"
#include "graft/native.hpp"
#include "graft/plugins.hpp"
#include "graft/scan.hpp"
#include "graft/script.hpp"

namespace graft {
namespace {

// Маяк порядка: движок регистрирует ядро одним проходом; ловим момент по имени
// ванильного натива и до-регистрируем свои в том же script-контексте (модуль 1_Core).
constexpr const char* kAnchor = "MemoryValidation";

scan::api g_api{};
scan::reg_global_fn g_orig = nullptr;

bool g_busy = false;
// Поиск функции класса по имени: у него нет своей строки-маяка, но он — первый вызов
// внутри линковщика, а тот — первый вызов внутри RegisterMethod (re/.../sub_140352DB0.c).
std::uintptr_t g_find_index = 0;

std::wstring exe_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

// Серверный ли это процесс.
//
// ЗАЧЕМ. Нативы пишутся под серверную логику мода, а библиотека, положенная рядом с exe,
// грузится в ЛЮБОЙ процесс, который импортирует dwmapi, — в том числе в клиент. Клиенту
// от нас ничего не нужно, а вред мы принести можем: сборки клиента и сервера разные, и
// то, что скан нашёл в одной, в другой указывает в другое место. Проверено дорого:
// привязка к кадру, выверенная на сервере, уронила клиент.
//
// КАК ОТЛИЧАЕМ. Ровно так же, как отличает сам движок, — по тому, как игру запустили.
// Отдельному серверному exe хватает имени; диаг-сборка одна на всё и различается флагом
// -server. Ни на что внутри движка это не опирается и потому работает ещё до того, как
// он проснулся.
}  // namespace

bool serving() {
    const std::wstring path = exe_path();
    if (path.find(L"Server") != std::wstring::npos) {
        return true;
    }
    const wchar_t* cmd = GetCommandLineW();
    return cmd && has_flag(cmd, L"-server");
}

namespace {

std::vector<scan::view> sections_of(HMODULE mod) {
    auto* base = reinterpret_cast<std::uint8_t*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return {};
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return {};
    }
    std::vector<scan::view> out;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ) || sec->Misc.VirtualSize == 0) {
            continue;
        }
        std::uint8_t* at = base + sec->VirtualAddress;
        out.push_back({at, sec->Misc.VirtualSize, reinterpret_cast<std::uintptr_t>(at),
                       (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
    }
    return out;
}

// Общая часть: зарегистрировать метод на уже найденном классе и привести флаги к
// member-виду (движок, создавая функцию раньше разбора мода, метит её static|external).
void register_on_class(void* ctx, void* cls, const char* owner, const char* class_name,
                       const char* name, void* impl, bool is_static, bool marshalled) {
    void* desc = g_api.register_method(ctx, cls, name, impl, 0, 1);
    std::uint32_t before = 0;
    std::uint32_t after = 0;
    if (desc) {
        auto* flags =
            reinterpret_cast<std::uint32_t*>(static_cast<char*>(desc) + layout::desc_flags);
        before = *flags;
        if (is_static) {
            *flags |= layout::flag_static;
        } else {
            *flags &= ~(layout::flag_static | layout::flag_external);
        }
        // 0x20 — native, 0x40 — маршалируемый. Компилятор ставит их по модификатору
        // `native`; у методов шаблонного класса дескриптор пустой, поэтому ставим сами.
        if (marshalled) {
            *flags = (*flags | layout::flag_marshalled) & ~layout::flag_native;
        }
        after = *flags;
    }
    log(std::format("+ [{}] {}.{} ({}, флаги {:#x} -> {:#x})", owner, class_name, name,
                    is_static ? "static" : "member", before, after));
}

// Классы, которых в момент врезки ещё нет. Таких два вида: игровые (Object объявлен в
// 3_Game) и ИНСТАНЦИАЦИИ шаблонных классов мода — `CppHashMap<string,int>` это отдельный
// скриптовый класс, и появляется он вместе с модулем, где шаблон впервые использован.
// Держим их в очереди и пробуем снова на регистрации каждого следующего модуля.
std::vector<plugins::entry> g_pending;
bool g_retrying = false;

void register_all(void* ctx) {
    for (const plugins::entry& e : loader::registry()) {
        const graft_native_desc& n = *e.desc;
        const char* owner = e.owner ? e.owner : "?";
        if (!n.class_name) {
            // через трамплин, иначе рекурсия в собственный хук
            g_orig(ctx, n.name, n.impl, 0);
            log(std::format("+ [{}] global {}", owner, n.name));
            continue;
        }
        void* cls = g_api.find_class(ctx, n.class_name);
        if (!cls) {
            g_pending.push_back(e);
            log(std::format("~ отложено: [{}] {}.{} (класса ещё нет)", owner, n.class_name, n.name));
            continue;
        }
        // Регистрация возвращает дескриптор функции. Если объявления в скрипте ещё нет
        // (мод разбирается позже ядра), движок создаёт функцию сам — и помечает её
        // static/external. Позже компилятор наткнётся на member-объявление и скажет
        // «linked as static/external but declared as member». Поэтому сразу приводим
        // флаги к тому виду, в каком их ждёт объявление: у ванильных member-нативов
        // бита 0x4 нет (string.Length = 0x4a28, map.Count = 0xc08).
        register_on_class(ctx, cls, owner, n.class_name, n.name, n.impl, n.is_static != 0,
                          n.marshalled != 0);
    }
}

// Контексты модулей копим на регистрации методов: в основном контексте игровых классов
// (Object, EntityAI) ещё нет, они появляются в поздних модулях.
scan::reg_method_fn g_orig_method = nullptr;

// Ещё раз пройтись по отложенным: к моменту регистрации следующего модуля недостающие
// классы уже разобраны. Дёшево — очередь короткая и пустеет.
void retry_pending() {
    if (g_pending.empty() || g_retrying) {
        return;
    }
    g_retrying = true;
    std::vector<plugins::entry> still;
    for (const plugins::entry& e : g_pending) {
        const graft_native_desc& n = *e.desc;
        if (!script::register_method_late(n.class_name, n.name, n.impl, n.is_static != 0,
                                          n.marshalled != 0)) {
            still.push_back(e);
            continue;
        }
        log(std::format("+ [{}] {}.{} (отложенный)", e.owner ? e.owner : "?", n.class_name, n.name));
    }
    g_pending.swap(still);
    g_retrying = false;
}

void* __fastcall hook_register_method(void* ctx, void* cls, const char* name, void* impl,
                                      unsigned ret_buf, char create) {
    script::note_context(ctx);
    void* result = g_orig_method(ctx, cls, name, impl, ret_buf, create);
    retry_pending();
    return result;
}

void* __fastcall hook_register_global(void* ctx, const char* name, void* impl,
                                      unsigned ret_buf) {
    void* result = g_orig(ctx, name, impl, ret_buf);
    // Заодно запоминаем чужую глобаль: другого способа узнать импл движковой функции без
    // класса нет, а через них ходят журналы игры (Print, ErrorEx) — см. script::find_global.
    script::note_global(name, impl);
    // Проход регистрации ядра — наш момент: скрипты модуля уже разобраны (FindClass
    // видит и классы мода), но ещё не слинкованы. На 1.29 проход ровно один; если
    // движок когда-то начнёт гонять его на каждый ctx — подсядем в каждый.
    if (!g_busy && name && std::strcmp(name, kAnchor) == 0) {
        g_busy = true;  // свои же вызовы идут через этот хук — не зациклиться
        loader::mark("маяк пойман, регистрируем");
        // Сначала оживляем доступ к методам классов: он нужен уже самой регистрации,
        // чтобы понять, есть ли для натива объявление в скрипте.
        script::set_register_method(reinterpret_cast<void*>(g_api.register_method));
        script::bind(ctx, reinterpret_cast<void*>(g_api.find_class),
                     reinterpret_cast<void*>(g_find_index));
        script::available();   // самопроверка на ванильном string.Length, пишет в журнал
        register_all(ctx);
        g_busy = false;
    }
    return result;
}

}  // namespace

void install() {
    const std::wstring path = exe_path();
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    // пути установок ASCII; ofstream на MSVC трактует narrow-путь как ANSI
    const std::string exe_dir(dir.begin(), dir.end());
    // Журналы кладём туда же, куда игра кладёт свои script- и crash-логи: в профиль
    // сервера. Админ ищет их там, а не рядом с exe, — и относительный путь из
    // `-profiles=` разрешится от того же каталога, что и у самой игры.
    const std::wstring cmd = GetCommandLineW();
    const std::string profile = plugins::profile_dir(std::string(cmd.begin(), cmd.end()));
    set_log_dir(profile.empty() ? exe_dir : profile);

    const std::vector<scan::view> sections = sections_of(GetModuleHandleW(nullptr));
    g_api = scan::discover(sections);
    if (!g_api) {
        return;  // в процессе нет движка Enforce — просто уходим
    }
    const std::uintptr_t linker =
        scan::first_call(sections, reinterpret_cast<std::uintptr_t>(g_api.register_method));
    g_find_index = linker ? scan::first_call(sections, linker) : 0;

    if (MH_Initialize() != MH_OK) {
        log("! MinHook init failed");
        return;
    }

    if (MH_CreateHook(reinterpret_cast<void*>(g_api.register_method),
                      reinterpret_cast<void*>(&hook_register_method),
                      reinterpret_cast<void**>(&g_orig_method)) == MH_OK) {
        MH_EnableHook(reinterpret_cast<void*>(g_api.register_method));
    }

    if (MH_CreateHook(reinterpret_cast<void*>(g_api.register_global),
                      reinterpret_cast<void*>(&hook_register_global),
                      reinterpret_cast<void**>(&g_orig)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(g_api.register_global)) != MH_OK) {
        log("! hook failed");
        return;
    }

    // Привязка к кадру: хук на движковую точку входа скриптового OnUpdate.
    frame::install(sections);

    const auto rva = [&](const void* p) {
        return reinterpret_cast<std::uintptr_t>(p) -
               reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    };
    log(std::format("graft armed: RegisterGlobal={:#x} RegisterMethod={:#x} FindClass={:#x} "
                    "FindFunction={:#x}",
                    rva(reinterpret_cast<void*>(g_api.register_global)),
                    rva(reinterpret_cast<void*>(g_api.register_method)),
                    rva(reinterpret_cast<void*>(g_api.find_class)),
                    g_find_index ? rva(reinterpret_cast<void*>(g_find_index)) : 0));
    loader::mark("хуки установлены");

    // ТОЛЬКО ТЕПЕРЬ грузим плагины: LoadLibrary — это десятки миллисекунд на каждый,
    // а маяк движка может прийти в любой момент. Хуки уже стоят, поэтому опоздать
    // некуда: обработчик маяка просто увидит готовый реестр.
    loader::load(dir);
    loader::mark("плагины загружены");
}

void bind_pending() {
    retry_pending();
    for (const plugins::entry& e : g_pending) {
        log(std::format("! так и не найден класс {} (метод {})", e.desc->class_name, e.desc->name));
    }
}

}  // namespace graft
