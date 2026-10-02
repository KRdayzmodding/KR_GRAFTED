// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/engine.hpp"
#include "graft/battleye.hpp"
#include "graft/defines.hpp"
#include "graft/frame.hpp"

#include <windows.h>

#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "graft/loader.hpp"
#include "graft/native.hpp"
#include "graft/plugins.hpp"
#include "graft/scan.hpp"
#include "graft/stages.hpp"
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

// Каталог игры: тот, где лежит exe, а с ним и hid.dll.
std::wstring game_dir() {
    const std::wstring path = exe_path();
    const std::size_t  slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

// Есть ли хоть одна DLL в <игра>/grafted/client. Это и есть согласие человека: клиент
// грузит только то, что он положил сам.
bool has_client_plugins() {
    WIN32_FIND_DATAW   found{};
    const std::wstring mask = game_dir() + L"\\" + std::wstring{plugins::client_dir} + L"\\*.dll";
    HANDLE             h    = FindFirstFileW(mask.c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    FindClose(h);
    return true;
}

// Профиль клиента, когда `-profiles=` не задан: игра кладёт script- и crash-логи в
// %LOCALAPPDATA%\DayZ, и наш журнал должен лежать с ними, а не рядом с exe. Пусто — не
// нашли переменную; тогда журнал уйдёт к exe, как на сервере.
std::string default_client_profile() {
    wchar_t     buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    return loader::narrow(std::wstring(buf, n) + L"\\DayZ");
}
} // namespace

// Роль и нужда в хосте — по тому, как игру запустили. Ни на что внутри движка это не
// опирается и потому работает ещё до того, как он проснулся.
//
// ЗАЧЕМ РАЗЛИЧАЕМ. Библиотека, положенная рядом с exe, грузится в ЛЮБОЙ процесс, который
// импортирует hid, а сборки клиента и сервера разные: то, что скан нашёл в одной, в другой
// указывает в другое место. Проверено дорого: привязка к кадру, выверенная на сервере,
// уронила клиент. Поэтому клиент без клиентских плагинов хост не будит вовсе, а с ними —
// только при отсутствии BattlEye (см. battleye.hpp). Скан на розничном клиенте 1.29 все
// точки находит и по форме совпадает с серверным (RESEARCH/theory/client.md), но это не
// повод будить хост там, где его никто не звал.
role process_role() {
    static const role cached = role_of(exe_path(), GetCommandLineW());
    return cached;
}

bool wanted() {
    return process_role() == role::server || has_client_plugins();
}

namespace {

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

// Врезка сообщает ФАКТ и больше ничего: кто на него подписан — дело подписчиков
// (см. install). Ни шапки, ни отложенных нативов здесь знать не надо.
void* __fastcall hook_register_method(void* ctx, void* cls, const char* name, void* impl,
                                      unsigned ret_buf, char create) {
    script::note_context(ctx);
    void* result = g_orig_method(ctx, cls, name, impl, ret_buf, create);
    stage::note_registration(ctx, script::class_name(cls));
    stage::pump();
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
        register_all(ctx);
        // Дальше движок начнёт разбирать модули; лестница поймает это сама.
        stage::pump();
        g_busy = false;
    }
    return result;
}

// Проверка линковки модуля: скрипты разобраны, движок сверяет native-методы с импл. Что
// это за окно и кому оно нужно — в stages.hpp (on_link). Имени у функции нет, есть строка
// предупреждения — одна на образ.
constexpr const char* kLinkCheck = "Method not linked '%s.%s'";
using link_check_fn              = std::uint64_t(__fastcall*)(void* compiler, void* errors);
link_check_fn g_orig_link_check  = nullptr;

std::uint64_t __fastcall hook_link_check(void* compiler, void* errors) {
    stage::note_link();
    return g_orig_link_check(compiler, errors);
}

}  // namespace

void install() {
    const std::wstring dir  = game_dir();
    const role         self = process_role();
    // Журналы кладём туда же, куда игра кладёт свои script- и crash-логи: в профиль.
    // Админ ищет их там, а не рядом с exe, — и относительный путь из `-profiles=`
    // разрешится от того же каталога, что и у самой игры. Клиент без ключа — в
    // %LOCALAPPDATA%\DayZ, где его логи лежат на самом деле. Пути везде UTF-8: у клиента
    // имя пользователя бывает любым, и обрезка до ASCII оставила бы его без журнала.
    std::string profile = plugins::profile_dir(loader::narrow(GetCommandLineW()));
    if (profile.empty() && self == role::client) {
        profile = default_client_profile();
    }
    set_log_dir(profile.empty() ? loader::narrow(dir) : profile);
    say_banner();
    log(std::format("роль процесса: {}", self == role::server ? "сервер" : "клиент"));

    // Клиент: ПЕРВЫМ делом — наблюдение за BattlEye, до скана, врезок и плагинов. Под BE
    // хост не работает; не сумев встать на наблюдение, клиентом не продолжаем: BE мог бы
    // прийти позже, а мы об этом не узнали бы (см. battleye.hpp).
    if (self == role::client && !battleye::watch(&battleye::kill)) {
        log("! наблюдение за BattlEye не встало — клиент без него не запускаем");
        return;
    }

    const std::vector<scan::view> sections = scan::sections_of(GetModuleHandleW(nullptr));
    g_api = scan::discover(sections);
    if (!g_api) {
        return;  // в процессе нет движка Enforce — просто уходим
    }
    const std::uintptr_t linker =
        scan::first_call(sections, reinterpret_cast<std::uintptr_t>(g_api.register_method));
    g_find_index = linker ? scan::first_call(sections, linker) : 0;

    // Через тот же сервис, что отдаётся плагинам: механика врезки в процессе одна, и хост
    // не исключение — иначе «одна копия» держалась бы на честном слове.
    hook(g_api.register_method, &hook_register_method, &g_orig_method);

    if (!hook(g_api.register_global, &hook_register_global, &g_orig)) {
        log("! hook failed");
        return;
    }

    // Привязка к кадру: хук на движковую точку входа скриптового OnUpdate.
    frame::install(sections);

    const std::uintptr_t link_check = scan::function_referencing(sections, kLinkCheck);
    if (!link_check ||
        !hook(reinterpret_cast<link_check_fn>(link_check), &hook_link_check, &g_orig_link_check)) {
        log("! проверка линковки не найдена: методы классов мода встанут позже неё, "
            "в script-логе будет «Method not linked» (работать они будут)");
    }

    // Дефайн на каждый загруженный плагин: врезка ДО загрузчика (плагины грузятся ниже),
    // а сработает она позже — когда движок дойдёт до CfgMods. См. SRC/graft/defines.cpp.
    defines::install(sections);

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

    // Кто чего ждёт — записано здесь, одним списком, а не размазано по врезкам.
    //
    // Отложенные нативы ждут КЛАССА: игровые классы и классы мода появляются не в первом
    // слое. Окно — проверка линковки модуля: модуль разобран, импл ещё можно положить.
    // Конец слоя — запасной путь, если проверку не нашли.
    stage::on_link([] { retry_pending(); });
    stage::on_layer_end([](const stage::layer&) { retry_pending(); });
    // Шапка ждёт, когда движку станет чем печатать: кадр вызова Print собирается по
    // шаблонам скриптовых переменных, а их приносит линковка модуля.
    stage::on(stage::step::linked, [] { say_banner_to_game(); });

    // ТОЛЬКО ТЕПЕРЬ грузим плагины: LoadLibrary — это десятки миллисекунд на каждый,
    // а маяк движка может прийти в любой момент. Хуки уже стоят, поэтому опоздать
    // некуда: обработчик маяка просто увидит готовый реестр.
    loader::load(dir);
    loader::mark("плагины загружены");
    stage::detail::reach(stage::step::armed);
}

void bind_pending() {
    retry_pending();
    for (const plugins::entry& e : g_pending) {
        log(std::format("! так и не найден класс {} (метод {})", e.desc->class_name, e.desc->name));
    }
}

}  // namespace graft
