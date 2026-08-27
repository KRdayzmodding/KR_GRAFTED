// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/scan.hpp"

#include <span>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iterator>

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

std::uintptr_t view::find_lea(std::span<const std::uint8_t, 3> opcode, std::uintptr_t target, std::uintptr_t from) const {
    constexpr std::size_t kLen = 7; // 3 байта опкода + rel32
    if (bytes.size() < kLen) {
        return 0;
    }
    std::size_t i = (from > base) ? static_cast<std::size_t>(from - base) : 0;
    while (i + kLen <= bytes.size()) {
        const auto* hit = static_cast<const std::uint8_t*>(
            std::memchr(bytes.data() + i, opcode[0], bytes.size() - kLen - i + 1));
        if (!hit) {
            return 0;
        }
        i = static_cast<std::size_t>(hit - bytes.data());
        if (bytes[i + 1] == opcode[1] && bytes[i + 2] == opcode[2] &&
            rip_target(base + i + kLen, rel32(bytes.data() + i + 3)) == target) {
            return base + i;
        }
        ++i;
    }
    return 0;
}

std::vector<std::uintptr_t> view::calls_after(std::uintptr_t ea, std::size_t span) const {
    std::vector<std::uintptr_t> out;
    if (!contains(ea)) {
        return out;
    }
    const std::size_t off = static_cast<std::size_t>(ea - base);
    const std::size_t end = (span > bytes.size() - off) ? bytes.size() : off + span;
    for (std::size_t i = off; i + 5 <= end; ++i) {
        if (bytes[i] == 0xE8) {
            out.push_back(rip_target(base + i + 5, rel32(bytes.data() + i + 1)));
        }
    }
    return out;
}

std::vector<std::uintptr_t> view::calls_before(std::uintptr_t ea, std::size_t span) const {
    std::vector<std::uintptr_t> out;
    if (!contains(ea)) {
        return out;
    }
    const std::size_t off   = static_cast<std::size_t>(ea - base);
    const std::size_t start = (span > off) ? 0 : off - span;
    for (std::size_t i = start; i + 5 <= off; ++i) {
        if (bytes[i] == 0xE8) {
            out.push_back(rip_target(base + i + 5, rel32(bytes.data() + i + 1)));
        }
    }
    return out;
}

std::uintptr_t vote(const std::vector<view>& sections, std::span<const std::uint8_t, 3> opcode, const char* const* anchors, bool before, const std::uintptr_t* reject, std::size_t reject_n) {
    struct tally {
        std::uintptr_t ea;
        int            votes;
    };

    std::vector<tally> tallies;

    auto is_code = [&](std::uintptr_t ea) {
        for (const view& s : sections) {
            if (s.exec && s.contains(ea)) {
                return true;
            }
        }
        return false;
    };
    auto rejected = [&](std::uintptr_t ea) {
        for (std::size_t i = 0; i < reject_n; ++i) {
            if (reject[i] == ea) {
                return true;
            }
        }
        return false;
    };

    for (const char* const* a = anchors; *a; ++a) {
        for (const view& str_sec : sections) {
            for (std::uintptr_t str_ea = str_sec.find_cstr(*a); str_ea;
                 str_ea                = str_sec.find_cstr(*a, str_ea + 1)) {
                for (const view& code : sections) {
                    if (!code.exec) {
                        continue;
                    }
                    for (std::uintptr_t site = code.find_lea(opcode, str_ea); site;
                         site                = code.find_lea(opcode, str_ea, site + 1)) {
                        // из всех call'ов рядом берём ближайший настоящий: 0xE8 внутри
                        // чужого смещения даст цель вне кода и будет отброшен
                        std::vector<std::uintptr_t> cands =
                            before ? code.calls_before(site) : code.calls_after(site);
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
                        bool known = false;
                        for (tally& t : tallies) {
                            if (t.ea == hit) {
                                ++t.votes;
                                known = true;
                                break;
                            }
                        }
                        if (!known) {
                            tallies.push_back({hit, 1});
                        }
                    }
                }
            }
        }
    }

    std::uintptr_t best       = 0;
    int            best_votes = 0;
    for (const tally& t : tallies) {
        if (t.votes > best_votes) {
            best       = t.ea;
            best_votes = t.votes;
        }
    }
    return best;
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

frame_entry find_frame_entry(const std::vector<view>& sections) {
    // Идём по байтам ОДНИМ проходом от якоря: цели вызовов тут не годятся, нужны сами
    // места вызовов — между ними и лежат приметы (маркер float, запись кэша).
    constexpr std::size_t  kWindow = 0x60;
    constexpr std::uint8_t kCall   = 0xE8;

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
            while ((at = code.find_lea(lea_rdx, text, at)) != 0) {
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
                // 2) float-аргумент: cvtss2sd. Байт modrm не проверяем — какой регистр
                //    выберет компилятор игры, нас не касается. Без него это одноимённое
                //    событие виджета или техники, а не кадр.
                std::size_t mark = call1 + 5;
                while (mark + 3 <= kWindow &&
                       !(w[mark] == 0xF3 && w[mark + 1] == 0x0F && w[mark + 2] == 0x5A)) {
                    ++mark;
                }
                if (mark + 3 > kWindow) {
                    continue;
                }
                // 3) сборка вызова — следующий call после маркера
                std::size_t call2 = mark;
                while (call2 < kWindow && w[call2] != kCall) {
                    ++call2;
                }
                if (call2 + 5 > kWindow) {
                    continue;
                }
                // 4) кэш индекса: `mov cs:disp32, eax` между двумя вызовами. Смещение
                //    rip-относительное, считается от конца инструкции.
                const std::int32_t* cached = nullptr;
                for (std::size_t k = call1 + 5; k + 6 <= call2; ++k) {
                    if (w[k] == 0x89 && w[k + 1] == 0x05) {
                        cached = reinterpret_cast<const std::int32_t*>(
                            rip_target(code.base + site + k + 6, rel32(w + k + 2)));
                        break;
                    }
                }
                if (!cached) {
                    continue;
                }
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

namespace {

// Читать по чужому адресу можно только убедившись, что страница есть: кандидат мог
// прийти из мусорного смещения, и тогда сверка обязана вернуть «не совпало», а не
// уронить процесс.
bool readable(std::uintptr_t ea, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    auto*                    at = reinterpret_cast<const void*>(ea);
    if (ea == 0 || VirtualQuery(at, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return ea + n <= end;
}

} // namespace

bool begins_with(std::uintptr_t ea, std::span<const std::uint8_t> sig) {
    return readable(ea, sig.size()) &&
           std::memcmp(reinterpret_cast<const void*>(ea), sig.data(), sig.size()) == 0;
}

namespace {

// Позиция `nth`-го вхождения опкода в теле. npos-подобный ответ — span.
std::size_t nth_at(std::uintptr_t ea, std::size_t span, std::span<const std::uint8_t> opcode, std::size_t tail, unsigned nth) {
    if (!readable(ea, span) || nth == 0) {
        return span;
    }
    const std::size_t n    = opcode.size();
    const auto*       body = reinterpret_cast<const std::uint8_t*>(ea);
    for (std::size_t i = 0; i + n + tail <= span; ++i) {
        if (std::memcmp(body + i, opcode.data(), n) == 0 && --nth == 0) {
            return i;
        }
    }
    return span;
}

} // namespace

bool disp32_of(std::uintptr_t ea, std::size_t span, std::span<const std::uint8_t> opcode, std::int32_t& out, std::uintptr_t* site, unsigned nth) {
    const std::size_t at = nth_at(ea, span, opcode, 4, nth);
    if (at == span) {
        return false;
    }
    std::memcpy(&out, reinterpret_cast<const std::uint8_t*>(ea) + at + opcode.size(), sizeof out);
    if (site != nullptr) {
        *site = ea + at;
    }
    return true;
}

bool disp8_of(std::uintptr_t ea, std::size_t span, std::span<const std::uint8_t> opcode, unsigned& out, unsigned nth) {
    const std::size_t at = nth_at(ea, span, opcode, 1, nth);
    if (at == span) {
        return false;
    }
    out = reinterpret_cast<const std::uint8_t*>(ea)[at + opcode.size()];
    return true;
}

} // namespace graft::scan
