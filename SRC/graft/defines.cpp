// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/defines.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <optional>

#include "graft/abi.h"
#include "graft/asm.hpp"
#include "graft/engine.hpp"
#include "graft/guard.hpp"
#include "graft/loader.hpp"
#include "graft/stages.hpp"

// ДЕФАЙН НА КАЖДЫЙ ЗАГРУЖЕННЫЙ ПЛАГИН.
//
// ЗАЧЕМ. Мод, которому нужен натив, зависит от своего плагина жёстко: нет импла — нет
// линковки, и деградировать тут нечем. Но сам файл `<МОД>/grafted/<ИМЯ>.grafted.dll`
// отдельный от PBO: его забывают положить, его уносит антивирус, он не встаёт на чужой
// сборке игры. Сегодня это отказ компиляции на нерезолвнутом `proto native` — то есть
// мод падает целиком, даже если плагин нужен ему в одной функции из ста.
//
// `#ifdef GRAFTED_<ИМЯ>` даёт моду выбор: ветку с нативами компилировать, только когда
// плагин РЕАЛЬНО загрузился. Имя класса в CfgMods дефайном становится и без нас (см.
// ниже), но оно означает «PBO подключён» — про DLL оно не знает ничего.
//
// ── КАК ДЕФАЙНЫ УСТРОЕНЫ У ДВИЖКА ───────────────────────────────────────────
// Источников четыре, и это проверено запуском, а не прочитано:
//   1. имя класса в CfgMods                     -> дефайн, глобально (MY_MOD, DayZ, sakhal);
//   2. `defines[] = {"A", {"B", 0}}` у мода     -> дефайн, глобально (так сделан CF);
//   3. `defs > <x>ScriptModule > value = "N"`   -> дефайн с этого модуля и ниже;
//   4. сам движок                               -> DAYZ_1_29, SERVER, PLATFORM_*, DIAG...
// Итог печатается строкой `Module: %s; ... defines: "%s"` в script-лог — ею и проверяется
// всё, что здесь написано.
//
// Пункты 1-3 собирает enf::AddonManager, разбирая CfgMods по одному классу за раз. Внутри
// у каждого аддона (enf::CAddon) два массива: пути скриптовых модулей и дефайны.
//
// ВАЖНОЕ СВОЙСТВО, на котором всё держится: дефайны аддона ГЛОБАЛЬНЫ. Мод, который кладёт
// файлы только в 1_Core и 3_Game, виден по имени и в дефайнах модуля GameLib, куда он не
// приносит ничего. Значит, в какой именно аддон дописать — неважно, и мы дописываем в
// первый попавшийся.
//
// ── ПОЧЕМУ НЕ КОМАНДНАЯ СТРОКА ──────────────────────────────────────────────
// У движка есть ключ `-scrDef=ИМЯ`, и подменить командную строку было бы дешевле всего.
// Он есть ТОЛЬКО в DayZDiag_x64.exe: в DayZServer_x64.exe и DayZ_x64.exe строки `scrDef`
// нет вовсе (1.29, все три бинаря проверены). Путь мёртв для всего, кроме диага.
//
// ── КУДА ВРЕЗАНО ────────────────────────────────────────────────────────────
// В CAddon::AddScriptModulePath — «добавить путь скриптового модуля аддону». Не в разбор
// CfgMods целиком: тот аддон создаёт у себя локально, наружу не отдаёт и возвращает bool,
// так что доставать объект пришлось бы из внутренностей менеджера. У AddScriptModulePath
// аддон приходит ПЕРВЫМ АРГУМЕНТОМ — брать нечего и гадать не о чем.
//
// Зовётся она на каждый `files[]` каждого мода, а нам нужен один раз; первый же вызов и
// работает, дальше флаг.
//
// ── КАК ИЩЕТСЯ, БЕЗ АДРЕСОВ ─────────────────────────────────────────────────
// Якорь — строка предупреждения `[CfgMods::Defines] :: [Warning] :: ...`: она лежит
// ВНУТРИ разбора CfgMods и одна на образ. От ссылки на неё смотрим вызовы в окне ±0x400 и
// отбираем по ФОРМЕ тела:
//
//     mov eax, [rcx+count]      счётчик
//     lea r64, [rcx+array]      сам массив, и count == array + 12 (указатель, ёмкость, счёт)
//     shl rax, 4                шаг элемента 16 байт -> это дефайны {строка, флаг}
//                               без него шаг 8 -> это пути
//
// На 1.29 обе функции так находятся однозначно во всех трёх бинарях (diag, сервер,
// клиент), лишних совпадений в окне нет. Двусмысленность — отказ: лучше без дефайнов,
// чем врезка в угаданное место.
//
// Поиск живёт здесь, а не в scan.cpp рядом с остальным: scan.cpp линкуется в КАЖДЫЙ
// плагин (graft_common), а дефайны — дело одного хоста.
//
// ── СТРОКА ДВИЖКА ───────────────────────────────────────────────────────────
// Массив дефайнов хранит сырой `const char*`, но потребители читают длину из шапки перед
// символами (`*(u16*)(p-4)`), а не strlen. Поэтому строку собираем в том же виде, в каком
// её строит движок: [-6] ёмкость, [-4] длина, [-2] счётчик ссылок.
namespace graft::defines {
namespace {

constexpr const char* kAnchor =
    "[CfgMods::Defines] :: [Warning] :: Skipping malformed define entry at index %i.";

// Окно вокруг ссылки на якорь, в котором ищем вызовы. Обе цели лежат в пределах 0x270
// байт от неё; берём с запасом, лишнее отсеет проверка формы.
constexpr std::size_t kWindow = 0x400;

api               g_api;
api::add_path_fn  g_orig_add_path = nullptr;
std::atomic<bool> g_done{false};
bool              g_warned = false;

// Ссылка на строку-маяк: `lea reg64,[rip+disp32]`, каким бы ни был регистр-приёмник — его
// выбирает компилятор игры. Ищет это scan::view::find_rip, одна на весь проект.
//
// Раньше здесь стоял свой цикл по байтам, и он расходился с собственным комментарием:
// префикс REX сверялся целиком (`0x48`), поэтому `lea r8..r15,[rip+...]` не находился вовсе.
// С маской на бите R находятся все шестнадцать, а форма адресации (mod=00, rm=101) сверяется
// по-прежнему — отпустить байт modrm целиком нельзя, под него подойдёт другая адресация.
constexpr auto kRipLea = scan::code<"lea reg64,[rip+disp32]">;

// Форма «дописать в массив, лежащий в объекте из rcx»: счётчик элементов, сам массив и —
// только у дефайнов — шаг элемента 16 байт вместо 8.
constexpr auto kCount = scan::code<"mov eax,[rcx+disp8]">;
constexpr auto kWide  = scan::code<"shl rax,4">;
// А вот здесь REX сверяется ЦЕЛИКОМ, а не маской, и это не забывчивость: у обеих
// разобранных функций приёмник из младших восьми регистров, а `find` берёт ПЕРВОЕ
// совпадение. Отпусти бит R — и любой случайный `lea r9,[rcx+n]` раньше по телу станет
// ответом, проверка `счётчик == массив + 12` не сойдётся, и дефайны молча выключатся.
// Расширять сверку можно, только посмотрев в IDA, что там действительно бывает.
constexpr auto kArray = scan::sig<"48 8D 41&C7 [disp8]">; // lea r{rax..rdi},[rcx+disp8]

enum class shape : std::uint8_t {
    none,
    path,
    define
};

// Форма «дописать в массив, лежащий в объекте из rcx». Смотрим только начало функции:
// обе цели — листья в сотню байт. Окно НЕ больше короткой из них (0x60), иначе в ответ
// попали бы байты соседней функции: там `shl rax,4` есть у кого угодно, и путь для
// модулей стал бы «дефайнами».
shape shape_of(const scan::view& code, std::uintptr_t fn) {
    if (!code.contains(fn)) {
        return shape::none;
    }
    // Окно НЕ больше короткой из двух целей (0x60), иначе в ответ попали бы байты соседней
    // функции: там `shl rax,4` есть у кого угодно, и путь для модулей стал бы «дефайнами».
    constexpr std::size_t kBody = 0x60;
    const auto            count = code.find(fn, kBody, kCount);
    const auto            array = code.find(fn, kBody, kArray);
    // Тройка {указатель, ёмкость, счёт} лежит подряд: счётчик на 12 байт дальше массива.
    // Это и отличает наши две функции от любого другого вызова, попавшего в окно.
    if (!count || !array || count->value != array->value + 12) {
        return shape::none;
    }
    return code.find(fn, kBody, kWide) ? shape::define : shape::path;
}

// Дописать дефайны в аддон; возвращает то, что дописано, — одной строкой для журнала.
// Отдельной функцией — её зовут из-под врезки, где падать нельзя: движок в этот момент
// собирает свой список модов.
std::string inject(void* addon) {
    std::string said;
    auto        add = [&](const std::string& name) {
        g_api.add_define(addon, engine_string(name), true);
        said += said.empty() ? "" : " ";
        said += name;
    };
    // Сам хост: моду бывает нужно знать, что graft в процессе вообще есть.
    add("GRAFTED");
    for (const loader::row& r : loader::rows()) {
        // Хост стоит в том же реестре под именем "graft" (loader.cpp) — про него уже
        // сказано строкой выше, второй раз тем же самым дефайном ни к чему.
        if (r.status == GRAFT_OK && r.name != "graft") {
            add(define_name(r.name));
        }
    }
    return said;
}

void* __fastcall hook_add_path(void* addon, const char* path) {
    void* result = g_orig_add_path(addon, path);
    // Врезка стоит РАНЬШЕ, чем загружены плагины (порядок в engine.cpp обязателен: хуки
    // до LoadLibrary). Поэтому не ждём загрузчик под движком, а пропускаем аддон и
    // пробуем на следующем — их десятки, а модов движок разбирает заметно дольше, чем
    // мы грузим горстку DLL.
    if (!g_done.load(std::memory_order_relaxed) && addon != nullptr) {
        if (!stage::reached(stage::step::armed)) {
            if (!g_warned) {
                g_warned = true;
                log("addon defines: the engine reached CfgMods before the loader - waiting");
            }
        } else if (!g_done.exchange(true, std::memory_order_relaxed)) {
            // Захват по ссылке — указатель, и лямбда остаётся тривиально разрушаемой:
            // guard.hpp этого и требует от того, что уезжает под __try.
            std::string said;
            ::graft::detail::guarded<void>(reinterpret_cast<void*>(&hook_add_path),
                                           [&said, addon] { said = inject(addon); });
            log(std::format("addon defines: {}", said.empty() ? "none" : said));
        }
    }
    return result;
}

} // namespace

const char* engine_string(std::string_view text) {
    // 0xFFFE — потолок длины в шапке движка; имена плагинов на три порядка короче.
    const std::size_t   len = std::min<std::size_t>(text.size(), 0xFFFE);
    const std::uint16_t cap = static_cast<std::uint16_t>(len + 1);
    const std::uint16_t n   = static_cast<std::uint16_t>(len);
    // Счётчик ссылок заведомо большой: движок освобождает строку СВОИМ аллокатором, когда
    // счётчик доходит до единицы, а эта пришла из нашей кучи. До единицы он не дойдёт.
    const std::uint16_t ref = 0x4000;
    // Живёт до конца процесса — десяток байт на плагин, освобождать нечему и незачем.
    auto* raw = new std::uint8_t[6 + len + 1];
    std::memcpy(raw + 0, &cap, sizeof cap);
    std::memcpy(raw + 2, &n, sizeof n);
    std::memcpy(raw + 4, &ref, sizeof ref);
    if (len) { // у пустого string_view data() законно нулевой, memcpy с него — UB
        std::memcpy(raw + 6, text.data(), len);
    }
    raw[6 + len] = 0;
    return reinterpret_cast<const char*>(raw + 6);
}

std::expected<api, miss> find(const std::vector<scan::view>& sections) {
    std::expected<std::uintptr_t, miss> anchor = std::unexpected(miss::not_found);
    for (const scan::view& s : sections) {
        if (!s.exec && (anchor = s.find_cstr(kAnchor))) {
            break;
        }
    }
    if (!anchor) {
        return std::unexpected(anchor.error());
    }
    api out;
    for (const scan::view& code : sections) {
        if (!code.exec) {
            continue;
        }
        const auto site = code.find_rip(kRipLea, *anchor);
        if (!site) {
            continue;
        }
        std::vector<std::uintptr_t>       targets = code.calls_before(*site, kWindow);
        const std::vector<std::uintptr_t> after   = code.calls_after(*site, kWindow);
        targets.insert(targets.end(), after.begin(), after.end());
        for (std::uintptr_t t : targets) {
            // Двусмысленность — отказ целиком: врезаться в угаданное из двух место хуже,
            // чем остаться без дефайнов и сказать об этом в журнал.
            switch (shape_of(code, t)) {
                case shape::path: {
                    const auto found = reinterpret_cast<api::add_path_fn>(t);
                    if (out.add_path && out.add_path != found) {
                        return std::unexpected(miss::ambiguous);
                    }
                    out.add_path = found;
                    break;
                }
                case shape::define: {
                    const auto found = reinterpret_cast<api::add_define_fn>(t);
                    if (out.add_define && out.add_define != found) {
                        return std::unexpected(miss::ambiguous);
                    }
                    out.add_define = found;
                    break;
                }
                case shape::none:
                    break;
            }
        }
    }
    if (!out.add_path || !out.add_define) {
        return std::unexpected(miss::not_found);
    }
    return out;
}

void install(const std::vector<scan::view>& sections) {
    const auto entry = find(sections);
    if (!entry) {
        log(std::format("! addon defines: CfgMods parser not found ({}) - there will be no #ifdef GRAFTED_*",
                        to_string(entry.error())));
        return;
    }
    g_api        = *entry;
    void* target = reinterpret_cast<void*>(g_api.add_path);
    void* detour = reinterpret_cast<void*>(&hook_add_path);
    if (!hook(target, detour, reinterpret_cast<void**>(&g_orig_add_path))) {
        log("! addon defines: hook failed");
        g_api = {};
        return;
    }
    const auto rva = [](const void* p) {
        return reinterpret_cast<std::uintptr_t>(p) -
               reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    };
    log(std::format("addon defines: AddScriptModulePath={:#x} AddDefine={:#x}",
                    rva(reinterpret_cast<void*>(g_api.add_path)),
                    rva(reinterpret_cast<void*>(g_api.add_define))));
}

} // namespace graft::defines
