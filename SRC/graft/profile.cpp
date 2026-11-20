// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/profile.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <vector>

#include "graft/engine.hpp"
#include "graft/loader.hpp"
#include "graft/plugins.hpp"

namespace graft::profile {

const std::uint32_t every_s = plugins::profile_seconds(GetCommandLineA());

void meter::frame(std::uint64_t cycles) {
    ++frames_;
    cycles_ += cycles;
    worst_ = (std::max)(worst_, cycles);
}

void meter::call(void* code, std::string_view where, std::uint64_t cycles) {
    called_ += cycles;
    auto found = std::ranges::find_if(calls_, [&](const stat& s) { return s.site == where.data() && s.where == where; });
    if (found == calls_.end()) {
        calls_.push_back(stat{.site = where.data(), .where = std::string(where), .code = code});
        found = calls_.end() - 1;
    }
    ++found->calls;
    found->cycles += cycles;
    found->worst = (std::max)(found->worst, cycles);
}

std::string meter::report(double ns_per_cycle, double seconds, std::size_t top) const {
    const auto               us    = [ns_per_cycle](double cycles) { return cycles * ns_per_cycle / 1000.0; };
    const double             share = seconds > 0 ? us(static_cast<double>(cycles_)) / (seconds * 1e6) * 100.0 : 0.0;
    const double             per   = frames_ ? 1.0 / static_cast<double>(frames_) : 0.0;
    const double             own   = cycles_ > called_ ? static_cast<double>(cycles_ - called_) : 0.0;
    std::string              out   = std::format("profile {:.0f} с: кадров {} ({:.0f}/с); graft за кадр — среднее {:.3f} мкс (сам graft {:.3f}, "
                                                 "обработчики {:.3f}), худший {:.1f} мкс; всего {:.3f}% времени",
                                  seconds,
                                  frames_,
                                  seconds > 0 ? static_cast<double>(frames_) / seconds : 0.0,
                                  us(static_cast<double>(cycles_)) * per,
                                  us(own) * per,
                                  us(static_cast<double>(called_)) * per,
                                  us(static_cast<double>(worst_)),
                                  share);
    std::vector<const stat*> by_cost;
    by_cost.reserve(calls_.size());
    for (const stat& s : calls_) {
        by_cost.push_back(&s);
    }
    std::ranges::sort(by_cost, std::greater<>{}, [](const stat* s) { return s->cycles; });
    for (std::size_t i = 0; i < by_cost.size() && i < top; ++i) {
        const stat&       s   = *by_cost[i];
        const std::string dll = loader::module_of(s.code);
        out += std::format("\n  {} {} — вызовов {}, всего {:.1f} мкс, худший {:.1f} мкс", dll.empty() ? "graft" : dll, s.where, s.calls, us(static_cast<double>(s.cycles)), us(static_cast<double>(s.worst)));
    }
    return out;
}

void meter::clear() {
    frames_ = 0;
    cycles_ = 0;
    worst_  = 0;
    called_ = 0;
    calls_.clear();
}

namespace {

struct window {
    meter                                 m;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    std::uint64_t                         tsc   = now();
};

window& current() {
    static window& w = *new window; // до конца процесса, как и хаб
    return w;
}

} // namespace

void call(void* code, std::string_view where, std::uint64_t cycles) {
    current().m.call(code, where, cycles);
}

void frame(std::uint64_t cycles) {
    window& w = current();
    w.m.frame(cycles);
    // Часы — раз в 64 кадра: окно секундами, а QPC в каждом кадре — уже цена.
    if ((w.m.frames() & 63) != 0) {
        return;
    }
    const auto   t       = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(t - w.since).count();
    if (seconds < every_s) {
        return;
    }
    const std::uint64_t tsc = now();
    graft::log(w.m.report(seconds * 1e9 / static_cast<double>(tsc - w.tsc), seconds));
    w.m.clear();
    w.since = t;
    w.tsc   = now();
}

} // namespace graft::profile
