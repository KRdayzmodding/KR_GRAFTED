#include "graft/scan.hpp"

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

}  // namespace

std::uintptr_t view::find_cstr(const char* text, std::uintptr_t from) const {
    const std::size_t n = std::strlen(text) + 1;  // вместе с терминатором
    if (!data || size < n) {
        return 0;
    }
    std::size_t i = (from > base) ? static_cast<std::size_t>(from - base) : 0;
    while (i + n <= size) {
        const auto* hit = static_cast<const std::uint8_t*>(
            std::memchr(data + i, static_cast<unsigned char>(text[0]), size - n - i + 1));
        if (!hit) {
            return 0;
        }
        i = static_cast<std::size_t>(hit - data);
        // строка должна быть целой, а не хвостом другой строки
        if (std::memcmp(data + i, text, n) == 0 && (i == 0 || data[i - 1] == 0)) {
            return base + i;
        }
        ++i;
    }
    return 0;
}

std::uintptr_t view::find_lea(const std::uint8_t (&opcode)[3], std::uintptr_t target,
                              std::uintptr_t from) const {
    constexpr std::size_t kLen = 7;  // 3 байта опкода + rel32
    if (!data || size < kLen) {
        return 0;
    }
    std::size_t i = (from > base) ? static_cast<std::size_t>(from - base) : 0;
    while (i + kLen <= size) {
        const auto* hit = static_cast<const std::uint8_t*>(
            std::memchr(data + i, opcode[0], size - kLen - i + 1));
        if (!hit) {
            return 0;
        }
        i = static_cast<std::size_t>(hit - data);
        if (data[i + 1] == opcode[1] && data[i + 2] == opcode[2] &&
            rip_target(base + i + kLen, rel32(data + i + 3)) == target) {
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
    const std::size_t end = (span > size - off) ? size : off + span;
    for (std::size_t i = off; i + 5 <= end; ++i) {
        if (data[i] == 0xE8) {
            out.push_back(rip_target(base + i + 5, rel32(data + i + 1)));
        }
    }
    return out;
}

std::vector<std::uintptr_t> view::calls_before(std::uintptr_t ea, std::size_t span) const {
    std::vector<std::uintptr_t> out;
    if (!contains(ea)) {
        return out;
    }
    const std::size_t off = static_cast<std::size_t>(ea - base);
    const std::size_t start = (span > off) ? 0 : off - span;
    for (std::size_t i = start; i + 5 <= off; ++i) {
        if (data[i] == 0xE8) {
            out.push_back(rip_target(base + i + 5, rel32(data + i + 1)));
        }
    }
    return out;
}

std::uintptr_t vote(const std::vector<view>& sections, const std::uint8_t (&opcode)[3],
                    const char* const* anchors, bool before, const std::uintptr_t* reject,
                    std::size_t reject_n) {
    struct tally {
        std::uintptr_t ea;
        int votes;
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
                 str_ea = str_sec.find_cstr(*a, str_ea + 1)) {
                for (const view& code : sections) {
                    if (!code.exec) {
                        continue;
                    }
                    for (std::uintptr_t site = code.find_lea(opcode, str_ea); site;
                         site = code.find_lea(opcode, str_ea, site + 1)) {
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

    std::uintptr_t best = 0;
    int best_votes = 0;
    for (const tally& t : tallies) {
        if (t.votes > best_votes) {
            best = t.ea;
            best_votes = t.votes;
        }
    }
    return best;
}

std::uintptr_t first_call(const std::vector<view>& sections, std::uintptr_t fn,
                          std::size_t span) {
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
    constexpr std::size_t kWindow = 0x60;
    constexpr std::uint8_t kCall = 0xE8;

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
                if (site + kWindow > code.size) {
                    continue;
                }
                const std::uint8_t* w = code.data + site;

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
                        reinterpret_cast<frame_entry::prepare_fn>(prepare), cached};
            }
        }
    }
    return {};
}

api discover(const std::vector<view>& sections) {
    // Якоря — имена ванильных нативов/классов из RegisterCoreNatives. Их регистрация
    // есть в любом билде: это публичный script-API (EnScript.c, EnMath.c, EnSystem.c).
    static const char* const kGlobals[] = {"MemoryValidation", "KillThread", "ThreadFunction",
                                           nullptr};
    // Первые методы своих классов: перед ними в коде стоит вызов FindClass.
    static const char* const kMethods[] = {"GetNumberOfSetBits", "GetClassVar", "AsciiToString",
                                           nullptr};

    api out{};
    out.register_global = reinterpret_cast<reg_global_fn>(vote(sections, lea_rdx, kGlobals));
    out.register_method = reinterpret_cast<reg_method_fn>(vote(sections, lea_r8, kMethods));

    const std::uintptr_t reject[] = {reinterpret_cast<std::uintptr_t>(out.register_global),
                                     reinterpret_cast<std::uintptr_t>(out.register_method)};
    out.find_class = reinterpret_cast<find_class_fn>(
        vote(sections, lea_r8, kMethods, /*before=*/true, reject, std::size(reject)));
    return out;
}

}  // namespace graft::scan
