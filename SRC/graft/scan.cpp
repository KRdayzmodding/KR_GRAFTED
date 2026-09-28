// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/scan.hpp"

#include <span>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <ranges>
#include <span>

#include "graft/asm.hpp"

namespace graft::scan {
namespace {

std::int32_t rel32(const std::uint8_t* at) {
    std::int32_t v;
    std::memcpy(&v, at, sizeof v);
    return v;
}

std::uintptr_t rip_target(std::uintptr_t insn_end, std::int32_t disp) {
    return insn_end + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(disp));
}

// Все вхождения там, где поиск по образу отдаёт по одному: `step(from)` — «следующее
// начиная с from», ноль — больше нет.
std::vector<std::uintptr_t> find_all(auto step) {
    std::vector<std::uintptr_t> all;
    for (std::uintptr_t at = step(0); at != 0; at = step(at + 1)) {
        all.push_back(at);
    }
    return all;
}

// Исполняемые секции: в них ищутся инструкции.
auto code_of(const std::vector<view>& sections) {
    return sections | std::views::filter(&view::exec);
}

// x64 UNWIND_INFO, начало записи: версия и флаги делят первый байт, число кодов раскрутки
// нужно, чтобы шагнуть за них — к сцепленной записи основной функции.
struct unwind_info {
    std::uint8_t version_flags;
    std::uint8_t prolog;
    std::uint8_t codes;
    std::uint8_t frame;
};

} // namespace

std::uintptr_t view::find_cstr(const char* text, std::uintptr_t from) const {
    const std::size_t n = std::strlen(text) + 1; // вместе с терминатором
    if (bytes.size() < n) {
        return 0;
    }
    std::size_t i = (from > base) ? static_cast<std::size_t>(from - base) : 0;
    while (i + n <= bytes.size()) {
        const auto* hit = static_cast<const std::uint8_t*>(
            std::memchr(bytes.data() + i, static_cast<unsigned char>(text[0]), bytes.size() - n - i + 1));
        if (!hit) {
            return 0;
        }
        i = static_cast<std::size_t>(hit - bytes.data());
        // строка должна быть целой, а не хвостом другой строки
        if (std::memcmp(bytes.data() + i, text, n) == 0 && (i == 0 || bytes[i - 1] == 0)) {
            return base + i;
        }
        ++i;
    }
    return 0;
}

std::uintptr_t view::find_rip(pattern_view insn, std::uintptr_t target, std::uintptr_t from) const {
    const std::size_t len = insn.value.size();
    if (insn.hole_size != 4 || bytes.size() < len) {
        return 0; // без дырки под disp32 ссылку на target не посчитать
    }
    // Перебирать побайтово всю секцию дорого (в образе игры это десятки мегабайт, а зовут
    // отсюда в цикле по якорям), поэтому сначала memchr по первому ПОЛНОСТЬЮ сверяемому
    // байту. У маскированного первого байта (`48&FB` у «любого регистра») его нет, зато
    // опкод следом всегда точный.
    std::size_t pivot = 0;
    while (pivot < len && insn.mask[pivot] != 0xFF) {
        ++pivot;
    }
    std::size_t i = (from > base) ? static_cast<std::size_t>(from - base) : 0;
    while (i + len <= bytes.size()) {
        if (pivot < len) {
            const auto* hit = static_cast<const std::uint8_t*>(
                std::memchr(bytes.data() + i + pivot, insn.value[pivot], bytes.size() - len - i + 1));
            if (!hit) {
                return 0;
            }
            i = static_cast<std::size_t>(hit - bytes.data()) - pivot;
        }
        bool same = true;
        for (std::size_t k = 0; k < len && same; ++k) {
            same = (bytes[i + k] & insn.mask[k]) == insn.value[k];
        }
        if (same &&
            rip_target(base + i + len, rel32(bytes.data() + i + insn.hole)) == target) {
            return base + i;
        }
        ++i;
    }
    return 0;
}

std::vector<std::uintptr_t> view::rel32_after(std::uintptr_t ea, std::size_t span, std::uint8_t opcode) const {
    std::vector<std::uintptr_t> out;
    if (!contains(ea)) {
        return out;
    }
    const std::size_t off = static_cast<std::size_t>(ea - base);
    const std::size_t end = (span > bytes.size() - off) ? bytes.size() : off + span;
    for (std::size_t i = off; i + 5 <= end; ++i) {
        if (bytes[i] == opcode) {
            out.push_back(rip_target(base + i + 5, rel32(bytes.data() + i + 1)));
        }
    }
    return out;
}

std::vector<std::uintptr_t> view::rel32_before(std::uintptr_t ea, std::size_t span, std::uint8_t opcode) const {
    std::vector<std::uintptr_t> out;
    if (!contains(ea)) {
        return out;
    }
    const std::size_t off   = static_cast<std::size_t>(ea - base);
    const std::size_t start = (span > off) ? 0 : off - span;
    for (std::size_t i = start; i + 5 <= off; ++i) {
        if (bytes[i] == opcode) {
            out.push_back(rip_target(base + i + 5, rel32(bytes.data() + i + 1)));
        }
    }
    return out;
}

std::uintptr_t vote(const std::vector<view>& sections, pattern_view insn, const char* const* anchors, bool before, const std::uintptr_t* reject, std::size_t reject_n) {
    struct tally {
        std::uintptr_t ea;
        int            votes;
    };

    std::vector<tally> tallies;

    auto is_code = [&](std::uintptr_t ea) {
        return std::ranges::any_of(code_of(sections), [&](const view& s) { return s.contains(ea); });
    };
    auto rejected = [&](std::uintptr_t ea) { return std::ranges::contains(std::span{reject, reject_n}, ea); };

    auto code_sections = code_of(sections);
    for (const char* const* a = anchors; *a; ++a) {
        for (std::uintptr_t site : rip_refs(sections, *a, insn)) {
            auto code = std::ranges::find_if(code_sections, [&](const view& s) { return s.contains(site); });
            // из всех call'ов рядом берём ближайший настоящий: 0xE8 внутри
            // чужого смещения даст цель вне кода и будет отброшен
            std::vector<std::uintptr_t> cands = before ? code->calls_before(site) : code->calls_after(site);
            if (before) {
                std::reverse(cands.begin(), cands.end());
            }
            std::uintptr_t hit = 0;
            for (std::uintptr_t c : cands) {
                if (is_code(c) && !rejected(c)) {
                    hit = c;
                    break;
                }
            }
            if (!hit) {
                continue;
            }
            if (auto t = std::ranges::find(tallies, hit, &tally::ea); t != tallies.end()) {
                ++t->votes;
            } else {
                tallies.push_back({hit, 1});
            }
        }
    }

    const auto best = std::ranges::max_element(tallies, {}, &tally::votes);
    return best == tallies.end() ? 0 : best->ea;
}

std::uintptr_t first_call(const std::vector<view>& sections, std::uintptr_t fn, std::size_t span) {
    for (const view& code : sections) {
        if (!code.exec || !code.contains(fn)) {
            continue;
        }
        for (std::uintptr_t target : code.calls_after(fn, span)) {
            for (const view& s : sections) {
                if (s.exec && s.contains(target)) {
                    return target;
                }
            }
        }
    }
    return 0;
}

std::vector<std::uintptr_t> rip_refs(const std::vector<view>& sections, const char* text, pattern_view insn) {
    std::vector<std::uintptr_t> out;
    for (const view& data : sections) {
        for (const std::uintptr_t str : find_all([&](std::uintptr_t from) { return data.find_cstr(text, from); })) {
            for (const view& code : code_of(sections)) {
                std::ranges::copy(find_all([&](std::uintptr_t from) { return code.find_rip(insn, str, from); }),
                                  std::back_inserter(out));
            }
        }
    }
    return out;
}

std::uintptr_t function_start(std::uintptr_t ea) {
    // Флаги записи лежат в старших битах первого байта — туда же сдвигаем и константу.
    constexpr std::uint8_t  kChained = UNW_FLAG_CHAININFO << 3;
    DWORD64                 image    = 0;
    const RUNTIME_FUNCTION* fe       = RtlLookupFunctionEntry(ea, &image, nullptr);
    while (fe != nullptr) {
        const auto* info = reinterpret_cast<const unwind_info*>(image + fe->UnwindData);
        if ((info->version_flags & kChained) == 0) {
            return static_cast<std::uintptr_t>(image) + fe->BeginAddress;
        }
        // За заголовком — коды раскрутки по слову, числом до чётного; дальше запись
        // основной функции: у куска, вынесенного компилятором, своей она и является.
        const std::span codes{reinterpret_cast<const std::uint16_t*>(info + 1), (info->codes + 1u) & ~1u};
        fe = reinterpret_cast<const RUNTIME_FUNCTION*>(codes.data() + codes.size());
    }
    return 0;
}

std::uintptr_t function_referencing(const std::vector<view>& sections, const char* text) {
    auto starts = rip_refs(sections, text, code<"lea reg64,[rip+disp32]">) |
                  std::views::transform(function_start) |
                  std::views::filter([](std::uintptr_t start) { return start != 0; });

    std::uintptr_t found = 0;
    for (const std::uintptr_t start : starts) {
        if (found != 0 && found != start) {
            return 0; // две разные функции — угадывать не будем
        }
        found = start;
    }
    return found;
}

std::vector<std::uintptr_t> rel32_targets(const std::vector<view>& sections, std::uintptr_t ea, std::size_t span, std::uint8_t opcode) {
    auto is_exec = [&](std::uintptr_t at) {
        for (const view& s : sections) {
            if (s.exec && s.contains(at)) {
                return true;
            }
        }
        return false;
    };
    std::vector<std::uintptr_t> out;
    for (const view& code : sections) {
        if (!code.exec || !code.contains(ea)) {
            continue;
        }
        for (std::uintptr_t target : code.rel32_after(ea, span, opcode)) {
            if (is_exec(target)) {
                out.push_back(target);
            }
        }
        break; // адрес лежит ровно в одной секции
    }
    return out;
}

frame_entry find_frame_entry(const std::vector<view>& sections) {
    // Идём по байтам ОДНИМ проходом от якоря: цели вызовов тут не годятся, нужны сами
    // места вызовов — между ними и лежат приметы (маркер float, запись кэша).
    constexpr std::size_t  kWindow = 0x60;
    constexpr std::uint8_t kCall   = 0xE8;
    // Две приметы кадрового OnUpdate. `cvtss2sd` записан байтами: векторных инструкций
    // кодировщик не знает намеренно, а modrm здесь и так отпущен.
    constexpr auto kCvtss2sd   = sig<"F3 0F 5A ??">;    // cvtss2sd xmm,xmm
    constexpr auto kIndexCache = sig<"89 05 [disp32]">; // mov [rip+disp32],eax

    for (const view& data : sections) {
        const std::uintptr_t text = data.find_cstr("OnUpdate");
        if (!text) {
            continue;
        }
        for (const view& code : sections) {
            if (!code.exec) {
                continue;
            }
            std::uintptr_t at = 0;
            while ((at = code.find_rip(lea_rdx, text, at)) != 0) {
                const std::size_t site = static_cast<std::size_t>(at - code.base);
                at += 1;
                if (site + kWindow > code.bytes.size()) {
                    continue;
                }
                const std::uint8_t* w = code.bytes.data() + site;

                // 1) поиск индекса по имени
                std::size_t call1 = 0;
                while (call1 < kWindow && w[call1] != kCall) {
                    ++call1;
                }
                if (call1 >= kWindow) {
                    continue;
                }
                // 2) float-аргумент: cvtss2sd. Байт modrm не сверяем — какой регистр
                //    выберет компилятор игры, нас не касается. Без этой приметы перед нами
                //    одноимённое событие виджета или техники, а не кадр.
                const std::optional<found> float_arg =
                    code.find(code.base + site + call1 + 5, kWindow - call1 - 5, kCvtss2sd);
                if (!float_arg) {
                    continue;
                }
                const std::size_t mark = static_cast<std::size_t>(float_arg->site - code.base) - site;
                // 3) сборка вызова — следующий call после маркера
                std::size_t call2 = mark;
                while (call2 < kWindow && w[call2] != kCall) {
                    ++call2;
                }
                if (call2 + 5 > kWindow) {
                    continue;
                }
                // 4) кэш индекса: `mov [rip+disp32],eax` между двумя вызовами. Адрес
                //    считается от КОНЦА инструкции, а её конец — это длина сигнатуры:
                //    дырка под смещение входит в неё, поэтому руками складывать нечего.
                const std::optional<found> cache =
                    code.find(code.base + site + call1 + 5, call2 - call1 - 5, kIndexCache);
                if (!cache) {
                    continue;
                }
                const auto* const cached = reinterpret_cast<const std::int32_t*>(
                    cache->site + kIndexCache.size() +
                    static_cast<std::uintptr_t>(static_cast<std::intptr_t>(cache->value)));
                const std::uintptr_t prepare =
                    rip_target(code.base + site + call2 + 5, rel32(w + call2 + 1));
                // Цель обязана лежать в исполняемой секции: 0xE8 встречается и в чужих
                // смещениях, и такой «вызов» ведёт куда попало.
                bool sane = false;
                for (const view& v : sections) {
                    sane = sane || (v.exec && v.contains(prepare));
                }
                if (!sane) {
                    continue;
                }
                return {code.base + site + call2,
                        reinterpret_cast<frame_entry::prepare_fn>(prepare),
                        cached};
            }
        }
    }
    return {};
}

api discover(const std::vector<view>& sections) {
    // Якоря — имена ванильных нативов/классов из RegisterCoreNatives. Их регистрация
    // есть в любом билде: это публичный script-API (EnScript.c, EnMath.c, EnSystem.c).
    static const char* const kGlobals[] = {"MemoryValidation", "KillThread", "ThreadFunction", nullptr};
    // Первые методы своих классов: перед ними в коде стоит вызов FindClass.
    static const char* const kMethods[] = {"GetNumberOfSetBits", "GetClassVar", "AsciiToString", nullptr};

    api out{};
    out.register_global = reinterpret_cast<reg_global_fn>(vote(sections, lea_rdx, kGlobals));
    out.register_method = reinterpret_cast<reg_method_fn>(vote(sections, lea_r8, kMethods));

    const std::uintptr_t reject[] = {reinterpret_cast<std::uintptr_t>(out.register_global),
                                     reinterpret_cast<std::uintptr_t>(out.register_method)};
    out.find_class                = reinterpret_cast<find_class_fn>(
        vote(sections, lea_r8, kMethods, /*before=*/true, reject, std::size(reject)));
    return out;
}

// ── Секции образа и сверка байтов ────────────────────────────────────────────

std::vector<view> sections_of(void* module) {
    auto* base = static_cast<std::uint8_t*>(module);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (base == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return {};
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return {};
    }
    std::vector<view>     out;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ) || sec->Misc.VirtualSize == 0) {
            continue;
        }
        std::uint8_t* at = base + sec->VirtualAddress;
        out.push_back({std::span{std::as_const(at), sec->Misc.VirtualSize},
                       reinterpret_cast<std::uintptr_t>(at),
                       (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
    }
    return out;
}

// Читать по чужому адресу можно только убедившись, что страница есть: кандидат мог
// прийти из мусорного смещения, и тогда сверка обязана вернуть «не совпало», а не
// уронить процесс. Единственная реализация этой проверки на всю библиотеку —
// script::detail::readable надстраивает над ней сверку «похоже на указатель».
bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (p == nullptr || VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<std::uintptr_t>(p) + n <= end;
}

namespace {

bool readable(std::uintptr_t ea, std::size_t n) {
    return scan::readable(reinterpret_cast<const void*>(ea), n);
}

// Байты по адресу против сигнатуры. Сверяются только биты маски: у джокера и у байтов
// дырки она нулевая, у обычного байта — 0xFF, у части байта — что-то между.
bool same(const std::uint8_t* body, pattern_view sig) {
    for (std::size_t i = 0; i < sig.value.size(); ++i) {
        if ((body[i] & sig.mask[i]) != sig.value[i]) {
            return false;
        }
    }
    return true;
}

// Содержимое дырки со знаком: x86 смещения и короткие константы знаковые, и 0x80 в disp8
// значит -128. Через промежуточный знаковый тип, а не сдвигами: знак расширяет компилятор.
std::int64_t hole_value(const std::uint8_t* at, unsigned bytes) {
    switch (bytes) {
        case 1: {
            std::int8_t v = 0;
            std::memcpy(&v, at, sizeof v);
            return v;
        }
        case 2: {
            std::int16_t v = 0;
            std::memcpy(&v, at, sizeof v);
            return v;
        }
        case 4: {
            std::int32_t v = 0;
            std::memcpy(&v, at, sizeof v);
            return v;
        }
        default: {
            std::int64_t v = 0;
            std::memcpy(&v, at, sizeof v);
            return v;
        }
    }
}

// Общий ход поиска: по буферу, адреса считаются от base_ea. Через него идут и свободная
// find (буфер — чужая память, её читаемость спрошена заранее), и view::find (буфер — байты
// самой секции, спрашивать нечего).
std::optional<found> find_in(const std::uint8_t* body,
                             std::size_t         span,
                             std::uintptr_t      base_ea,
                             pattern_view        sig,
                             unsigned            nth) {
    const std::size_t n = sig.value.size();
    if (n == 0 || nth == 0 || n > span) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i + n <= span; ++i) {
        if (!same(body + i, sig) || --nth != 0) {
            continue;
        }
        return found{base_ea + i, sig.hole_size != 0 ? hole_value(body + i + sig.hole, sig.hole_size) : 0};
    }
    return std::nullopt;
}

} // namespace

bool matches(std::uintptr_t ea, pattern_view sig) {
    return !sig.value.empty() && readable(ea, sig.value.size()) &&
           same(reinterpret_cast<const std::uint8_t*>(ea), sig);
}

std::optional<found> find(std::uintptr_t ea, std::size_t span, pattern_view sig, unsigned nth) {
    if (!readable(ea, span)) {
        return std::nullopt;
    }
    return find_in(reinterpret_cast<const std::uint8_t*>(ea), span, ea, sig, nth);
}

std::optional<found> view::find(std::uintptr_t ea, std::size_t span, pattern_view sig, unsigned nth) const {
    if (!contains(ea)) {
        return std::nullopt;
    }
    // Без std::min: windows.h в этой TU приносит макрос min, и вызов через него не собрать.
    const std::size_t off  = static_cast<std::size_t>(ea - base);
    const std::size_t left = bytes.size() - off;
    return find_in(bytes.data() + off, span < left ? span : left, ea, sig, nth);
}

namespace {

// Таблица, перед которой лежит указатель на locator: линковщик кладёт его последним
// элементом перед первым слотом, поэтому сам слот — это `место указателя + 8`.
std::uintptr_t table_after(const std::vector<view>& sections, std::uintptr_t locator) {
    for (const view& v : sections) {
        if (v.exec) {
            continue;
        }
        const std::uint8_t* b = v.bytes.data();
        for (std::size_t o = 0; o + sizeof(void*) <= v.bytes.size(); o += sizeof(void*)) {
            std::uintptr_t held = 0;
            std::memcpy(&held, b + o, sizeof held);
            if (held == locator) {
                return v.base + o + sizeof(void*);
            }
        }
    }
    return 0;
}

} // namespace

// ── Таблицы классов ──────────────────────────────────────────────────────────

bool is_code(const void* p) {
    if (!p) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    constexpr DWORD kExec =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kExec) != 0 && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

std::size_t vtable_slots(void* const* vt) {
    constexpr std::size_t kMaxSlots = 256;
    if (!vt) {
        return 0;
    }
    std::size_t n = 0;
    // Спрашиваем систему и про сам слот: у последней таблицы в секции следующая страница
    // может быть не отображена, и чтение за край — падение на ровном месте.
    while (n < kMaxSlots && readable(reinterpret_cast<std::uintptr_t>(&vt[n]), sizeof(void*)) &&
           is_code(vt[n])) {
        ++n;
    }
    return n;
}

std::uintptr_t rtti_vtable(const std::vector<view>& sections, std::uintptr_t image_base, const char* mangled) {
    // 1. ВСЕ места, где лежит это имя, а не первое попавшееся. Их в образе несколько:
    //    одно — в дескрипторе типа, остальные — обычные строковые литералы. И первым
    //    почти всегда оказывается литерал ТОГО, КТО ИЩЕТ: имя приезжает сюда как
    //    `rtti_vtable(mod, ".?AVFoo@@")`, то есть своя же копия строки лежит в .rdata
    //    вызывающего. Поиск по первому совпадению на живом бинаре не находил ничего.
    //
    //    Дескриптор типа при этом лежит в ЗАПИСЫВАЕМОЙ секции (у него есть поле-кэш
    //    декорированного имени), а локатор и таблица — в read-only. Поэтому по правам
    //    страниц здесь отсеять нечего: смотрим все неисполняемые секции.
    std::vector<std::uint32_t> wanted;
    for (const view& v : sections) {
        if (v.exec) {
            continue;
        }
        for (std::uintptr_t at = v.find_cstr(mangled); at; at = v.find_cstr(mangled, at + 1)) {
            if (at >= image_base + 16) {
                wanted.push_back(static_cast<std::uint32_t>(at - 16 - image_base));
            }
        }
    }
    if (wanted.empty()) {
        return 0;
    }
    // 2. Локатор, который на один из них ссылается. Один проход на всех кандидатов сразу:
    //    проход стоит куда дороже сверки, а кандидатов единицы.
    //
    //    Локатор узнаётся не «похоже на структуру», а ПО СЕБЕ САМОМУ: у x64-версии есть
    //    поле pSelf со своим же RVA (сигнатура == 1 именно об этом и говорит). Случайный
    //    dword, равный RVA имени, такую проверку не проходит.
    for (const view& v : sections) {
        if (v.exec) {
            continue;
        }
        const std::uint8_t* b = v.bytes.data();
        for (std::size_t o = 0; o + 24 <= v.bytes.size(); o += 4) {
            std::uint32_t field[6]{};
            std::memcpy(field, b + o, sizeof field);
            if (field[0] != 1 || field[5] != v.base + o - image_base) {
                continue;
            }
            if (std::find(wanted.begin(), wanted.end(), field[3]) == wanted.end()) {
                continue;
            }
            // 3. Указатель на локатор. Он лежит ровно перед таблицей — она и есть ответ.
            if (const std::uintptr_t vt = table_after(sections, v.base + o)) {
                return vt;
            }
        }
    }
    return 0;
}

std::uintptr_t rtti_vtable(void* module, const char* mangled) {
    return rtti_vtable(sections_of(module), reinterpret_cast<std::uintptr_t>(module), mangled);
}

} // namespace graft::scan
