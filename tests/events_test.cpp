// События, поток скрипта и таймеры (graft/events.hpp) — без игры: хаб хоста подменён своим,
// поток скрипта — поток теста, часы — поддельные. Публичный API тот же, что у плагина.
#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "graft/abi.h"
#include "graft/engine.hpp"
#include "graft/events.hpp"
#include "graft/hub.hpp"
#include "graft/loader.hpp"
#include "graft/plugins.hpp"

namespace {

using namespace std::chrono_literals;
using graft::events::clock;

clock::time_point fake_now{};

clock::time_point now_for_tests() {
    return fake_now;
}

struct ping {
    static constexpr std::string_view name = "test.ping";
    int                               n    = 0;
};

struct ping_wide { // то же имя, другой размер
    static constexpr std::string_view name = "test.ping";
    double                            n    = 0;
    int                               k    = 0;
};

struct mood {
    static constexpr std::string_view name = "test.mood";
    int                               v    = 0;
};

class Events : public ::testing::Test {
protected:
    graft::events::hub    hub{&now_for_tests};
    graft::events::scoped use{hub};

    void SetUp() override {
        fake_now = clock::time_point{} + 1h;
        hub.bind_here();
    }

    void frame(clock::duration budget = 1h) { hub.frame(budget); }
};

std::string tid() {
    return std::to_string(GetCurrentThreadId());
}

// ── Подписка и публикация ────────────────────────────────────────────────────

TEST_F(Events, PublishOnScriptThreadCallsSubscribersInOrder) {
    std::vector<std::string> seen;
    graft::on<ping>          a{[&](const ping& p) { seen.push_back(std::format("a{}", p.n)); }};
    graft::on<ping>          b{[&](const ping& p) { seen.push_back(std::format("b{}", p.n)); }};
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_TRUE(graft::publish(ping{.n = 1}));
    EXPECT_EQ(seen, (std::vector<std::string>{"a1", "b1"}));
}

TEST_F(Events, HandlerWithoutArgumentsIsFine) {
    int             n = 0;
    graft::on<ping> a{[&] { ++n; }};
    graft::publish(ping{.n = 1});
    EXPECT_EQ(n, 1);
}

TEST_F(Events, PublishFromOtherThreadArrivesOnScriptThreadNextFrame) {
    std::vector<std::string> seen;
    graft::on<ping>          a{[&](const ping& p) { seen.push_back(std::format("{}@{}", p.n, tid())); }};
    std::thread([] { EXPECT_TRUE(graft::publish(ping{.n = 7})); }).join();
    EXPECT_TRUE(seen.empty());
    frame();
    EXPECT_EQ(seen, (std::vector<std::string>{std::format("7@{}", tid())}));
}

TEST_F(Events, DestroyedSubscriptionStopsDelivery) {
    int n = 0;
    {
        graft::on<ping> a{[&](const ping&) { ++n; }};
        graft::publish(ping{});
    }
    graft::publish(ping{});
    EXPECT_EQ(n, 1);
    EXPECT_EQ(hub.subscribers("test.ping"), 0u);
}

TEST_F(Events, UnsubscribeInsideOwnHandlerKeepsClosureAliveUntilReturn) {
    std::optional<graft::on<ping>> self;
    int                            calls = 0;
    self.emplace([&, kept = std::string("замыкание")](const ping&) {
        ++calls;
        self->reset();
        EXPECT_EQ(kept, "замыкание");
    });
    graft::publish(ping{});
    graft::publish(ping{});
    EXPECT_EQ(calls, 1);
}

TEST_F(Events, SubscriberAddedDuringDeliveryMissesCurrentEvent) {
    std::vector<int>               late_seen;
    std::optional<graft::on<ping>> late;
    graft::on<ping>                first{[&](const ping&) {
        if (!late) {
            late.emplace([&](const ping& p) { late_seen.push_back(p.n); });
        }
    }};
    graft::publish(ping{.n = 1});
    graft::publish(ping{.n = 2});
    EXPECT_EQ(late_seen, (std::vector<int>{2}));
}

TEST_F(Events, ResetRemovesOnlyItsOwnSubscription) {
    int                            b_calls = 0;
    std::optional<graft::on<ping>> a;
    a.emplace([](const ping&) {});
    graft::on<ping> b{[&](const ping&) { ++b_calls; }};
    a.reset();
    graft::on<ping> c{[](const ping&) {}};
    c.reset(); // снять свою, а не чужую
    graft::publish(ping{});
    EXPECT_EQ(b_calls, 1);
    EXPECT_EQ(hub.subscribers(ping::name), 1u);
}

TEST_F(Events, StateReachesLateSubscriberOnNextFrame) {
    EXPECT_TRUE(graft::publish_state(mood{.v = 5}));
    std::vector<int> seen;
    graft::on<mood>  s{[&](const mood& m) { seen.push_back(m.v); }};
    EXPECT_TRUE(seen.empty()); // не внутри подписки: объект подписчика может быть недостроен
    frame();
    EXPECT_EQ(seen, (std::vector<int>{5}));
    graft::publish_state(mood{.v = 6});
    EXPECT_EQ(seen, (std::vector<int>{5, 6}));
}

TEST_F(Events, LateSubscriberRemovedBeforeFrameGetsNoState) {
    graft::publish_state(mood{.v = 1});
    int             n = 0;
    graft::on<mood> s{[&](const mood&) { ++n; }};
    s.reset();
    frame();
    EXPECT_EQ(n, 0);
}

TEST_F(Events, SizeMismatchIsRefused) {
    int                  n = 0;
    graft::on<ping>      a{[&](const ping&) { ++n; }};
    graft::on<ping_wide> b{[](const ping_wide&) {}};
    EXPECT_TRUE(a);
    EXPECT_FALSE(b);
    EXPECT_FALSE(graft::publish(ping_wide{}));
    EXPECT_TRUE(graft::publish(ping{}));
    EXPECT_EQ(n, 1);
}

TEST_F(Events, ThrowingHandlerDoesNotStopOthers) {
    const auto      faults_were = graft::loader::fault_count();
    int             n           = 0;
    graft::on<ping> a{[](const ping&) { throw std::runtime_error("обработчик упал"); }};
    graft::on<ping> b{[&](const ping&) { ++n; }};
    graft::publish(ping{});
    EXPECT_EQ(n, 1);
    EXPECT_EQ(graft::loader::fault_count(), faults_were + 1);
}

TEST_F(Events, HardwareFaultInHandlerIsCaughtAndOffThreadResetDoesNotHang) {
    const auto faults_were = graft::loader::fault_count();
    int* volatile nowhere  = nullptr;
    graft::on<ping> a{[&](const ping&) { *nowhere = 1; }};
    graft::publish(ping{});
    EXPECT_EQ(graft::loader::fault_count(), faults_were + 1);
    std::thread([&] { a.reset(); }).join(); // busy снят __finally — ждать нечего
    EXPECT_FALSE(a);
}

TEST_F(Events, ResetFromOtherThreadWaitsForRunningHandler) {
    std::atomic<int> stage{0};
    graft::on<ping>  s{[&](const ping&) {
        stage.store(1);
        stage.notify_all();
        std::this_thread::sleep_for(50ms);
        stage.store(2);
    }};
    int              stage_after_reset = -1;
    std::thread      other([&] {
        stage.wait(0);
        s.reset();
        stage_after_reset = stage.load();
    });
    graft::publish(ping{});
    other.join();
    EXPECT_EQ(stage_after_reset, 2);
}

// ── Неиспользуемое ничего не стоит ───────────────────────────────────────────

TEST_F(Events, PublishWithoutListenersQueuesNothing) {
    EXPECT_FALSE(graft::wanted<ping>());
    std::thread([] { EXPECT_TRUE(graft::publish(ping{.n = 1})); }).join();
    EXPECT_EQ(hub.queued(), 0u); // ни копии, ни задания
    {
        graft::on<ping> a{[](const ping&) {}};
        EXPECT_TRUE(graft::wanted<ping>());
        std::thread([] { graft::publish(ping{.n = 2}); }).join();
        EXPECT_EQ(hub.queued(), 1u);
        frame();
    }
    EXPECT_FALSE(graft::wanted<ping>());
}

TEST_F(Events, HostTickIsNotBuiltWithoutListeners) {
    graft::events::frame(0.016f, 1);
    EXPECT_FALSE(graft::wanted<graft::tick>());
    std::vector<std::uint64_t> ticks;
    graft::on<graft::tick>     s{[&](const graft::tick& t) { ticks.push_back(t.number); }};
    EXPECT_TRUE(graft::wanted<graft::tick>());
    graft::events::frame(0.016f, 2);
    EXPECT_EQ(ticks, (std::vector<std::uint64_t>{2}));
}

TEST_F(Events, SourceIsToldWhenFirstListenerComesAndLastLeaves) {
    std::vector<bool>   told;
    graft::source<mood> feed{[&](bool wanted) { told.push_back(wanted); }};
    frame();
    EXPECT_TRUE(told.empty()); // никого — молчит
    {
        graft::on<mood> a{[](const mood&) {}};
        graft::on<mood> b{[](const mood&) {}};
        EXPECT_TRUE(told.empty()); // не внутри подписки
        frame();
        EXPECT_EQ(told, (std::vector<bool>{true}));
    }
    frame();
    EXPECT_EQ(told, (std::vector<bool>{true, false}));
}

TEST_F(Events, SourceIsNotBotheredByListenerThatCameAndWentWithinAFrame) {
    std::vector<bool>   told;
    graft::source<mood> feed{[&](bool wanted) { told.push_back(wanted); }};
    {
        graft::on<mood> a{[](const mood&) {}};
    }
    frame();
    EXPECT_TRUE(told.empty());
}

TEST_F(Events, LateSourceHearsAboutExistingListeners) {
    graft::on<mood>     a{[](const mood&) {}};
    std::vector<bool>   told;
    graft::source<mood> feed{[&](bool wanted) { told.push_back(wanted); }};
    frame();
    EXPECT_EQ(told, (std::vector<bool>{true}));
    feed.reset();
    a.reset();
    frame();
    EXPECT_EQ(told, (std::vector<bool>{true})); // снятому — ничего
}

// ── Задания на поток скрипта ─────────────────────────────────────────────────

TEST_F(Events, PostRunsOnScriptThreadInOrder) {
    std::vector<std::string> seen;
    std::thread([&] {
        for (int i = 0; i < 3; ++i) {
            EXPECT_TRUE(graft::post([&seen, i] { seen.push_back(std::format("{}@{}", i, tid())); }));
        }
    }).join();
    EXPECT_TRUE(seen.empty());
    frame();
    const std::string me = tid();
    EXPECT_EQ(seen, (std::vector<std::string>{"0@" + me, "1@" + me, "2@" + me}));
}

TEST_F(Events, PostFromScriptThreadWaitsForFrame) {
    int n = 0;
    graft::post([&] { ++n; });
    EXPECT_EQ(n, 0);
    frame();
    EXPECT_EQ(n, 1);
}

TEST_F(Events, BudgetLeavesTheRestForNextFrame) {
    int n = 0;
    for (int i = 0; i < 5; ++i) {
        graft::post([&] {
            ++n;
            fake_now += 1ms;
        });
    }
    frame(2ms);
    EXPECT_EQ(n, 2);
    frame(2ms);
    EXPECT_EQ(n, 4);
    frame(2ms);
    EXPECT_EQ(n, 5);
}

TEST_F(Events, AtLeastOneTaskPerFrame) {
    int n = 0;
    graft::post([&] { ++n; });
    graft::post([&] { ++n; });
    frame(0ms);
    EXPECT_EQ(n, 1);
    frame(0ms);
    EXPECT_EQ(n, 2);
}

TEST_F(Events, TaskPostedDuringDrainRunsNextFrame) {
    std::vector<int> seen;
    graft::post([&] {
        seen.push_back(1);
        graft::post([&] { seen.push_back(2); });
    });
    frame();
    EXPECT_EQ(seen, (std::vector<int>{1}));
    frame();
    EXPECT_EQ(seen, (std::vector<int>{1, 2}));
}

TEST_F(Events, ClosureIsReleasedAfterRun) {
    auto held = std::make_shared<int>(1);
    graft::post([held] {});
    EXPECT_EQ(held.use_count(), 2);
    frame();
    EXPECT_EQ(held.use_count(), 1);
}

TEST_F(Events, FaultInTaskDoesNotStopTheQueue) {
    int n = 0;
    graft::post([] { throw std::runtime_error("задание упало"); });
    graft::post([&] { ++n; });
    frame();
    EXPECT_EQ(n, 1);
}

// ── Таймеры ──────────────────────────────────────────────────────────────────

TEST_F(Events, AfterFiresOnceWhenDue) {
    int        n = 0;
    const auto t = graft::after(100ms, [&] { ++n; });
    ASSERT_TRUE(t);
    frame();
    EXPECT_EQ(n, 0);
    fake_now += 100ms;
    frame();
    EXPECT_EQ(n, 1);
    EXPECT_FALSE(t); // отработал
    fake_now += 1s;
    frame();
    EXPECT_EQ(n, 1);
}

TEST_F(Events, DroppedTimerHandleKeepsTheTimer) {
    int n = 0;
    graft::every(10ms, [&] { ++n; });
    fake_now += 10ms;
    frame();
    fake_now += 10ms;
    frame();
    EXPECT_EQ(n, 2);
    EXPECT_EQ(hub.timers(), 1u);
}

TEST_F(Events, HandleOfFinishedTimerIsSafeToCancel) {
    auto held = std::make_shared<int>(1);
    auto t    = graft::after(1ms, [held] {});
    fake_now += 1ms;
    frame();
    EXPECT_EQ(held.use_count(), 1); // замыкание отпущено
    t.cancel();                     // коробки уже нет — и не надо
    EXPECT_FALSE(t);
}

TEST_F(Events, EveryRepeatsWithoutCatchUpBurst) {
    int n = 0;
    graft::every(10ms, [&] { ++n; });
    fake_now += 10ms;
    frame();
    EXPECT_EQ(n, 1);
    fake_now += 100ms; // простой: десять сроков пропущено
    frame();
    EXPECT_EQ(n, 2);
    fake_now += 10ms;
    frame();
    EXPECT_EQ(n, 3);
}

TEST_F(Events, CancelledTimerStops) {
    int  n = 0;
    auto t = graft::every(10ms, [&] { ++n; });
    t.cancel();
    fake_now += 50ms;
    frame();
    EXPECT_EQ(n, 0);
    EXPECT_EQ(hub.timers(), 0u);
}

TEST_F(Events, TimerCancelledInsideItsCallback) {
    int          n = 0;
    graft::timer t;
    t = graft::every(10ms, [&] {
        ++n;
        t.cancel();
    });
    for (int i = 0; i < 3; ++i) {
        fake_now += 10ms;
        frame();
    }
    EXPECT_EQ(n, 1);
}

TEST_F(Events, TimerCancelFromOtherThreadWaitsForRunningCall) {
    std::atomic<int> stage{0};
    graft::timer     t                  = graft::every(1ms, [&] {
        stage.store(1);
        stage.notify_all();
        std::this_thread::sleep_for(50ms);
        stage.store(2);
    });
    int              stage_after_cancel = -1;
    std::thread      other([&] {
        stage.wait(0);
        t.cancel();
        stage_after_cancel = stage.load();
    });
    fake_now += 1ms;
    frame();
    other.join();
    EXPECT_EQ(stage_after_cancel, 2);
}

TEST_F(Events, TimerStartedFromOtherThreadRunsOnScriptThread) {
    std::string where;
    std::thread([&] { graft::after(5ms, [&] { where = tid(); }); }).join();
    frame(); // заявка таймера дошла
    fake_now += 5ms;
    frame();
    EXPECT_EQ(where, tid());
}

// ── Журнал с чужого потока ───────────────────────────────────────────────────

TEST_F(Events, PrintFromOtherThreadWaitsForScriptThread) {
    bool said = false;
    std::thread([&] { said = graft::print("строка с чужого потока"); }).join();
    EXPECT_TRUE(said);
    EXPECT_EQ(hub.queued(), 1u);
    frame(); // движка в тесте нет — строка уйдёт в системный журнал, но уже с потока скрипта
    EXPECT_EQ(hub.queued(), 0u);
}

// ── Кадр хоста ───────────────────────────────────────────────────────────────

TEST_F(Events, HostFramePublishesTickAndDrainsQueue) {
    std::vector<std::uint64_t> ticks;
    graft::on<graft::tick>     s{[&](const graft::tick& t) { ticks.push_back(t.number); }};
    int                        n = 0;
    graft::post([&] { ++n; });
    graft::events::frame(0.016f, 41);
    EXPECT_EQ(ticks, (std::vector<std::uint64_t>{41}));
    EXPECT_EQ(n, 1);
}

TEST(EventsBudget, KeyFromCommandLine) {
    EXPECT_EQ(graft::plugins::frame_budget_ms("DayZServer_x64.exe -config=a.cfg"), 2u);
    EXPECT_EQ(graft::plugins::frame_budget_ms("DayZServer_x64.exe -graftBudget=5"), 5u);
    EXPECT_EQ(graft::plugins::frame_budget_ms("DayZServer_x64.exe -graftBudget 7"), 7u);
    EXPECT_EQ(graft::plugins::frame_budget_ms("DayZServer_x64.exe -graftBudget=мусор"), 2u);
    EXPECT_EQ(graft::plugins::profile_seconds("DayZServer_x64.exe -config=a.cfg"), 0u);
    EXPECT_EQ(graft::plugins::profile_seconds("DayZServer_x64.exe -graftProfile=10"), 10u);
}

// ── Совместимость ────────────────────────────────────────────────────────────
// Раскладка graft_host_api 0.5 (ABI 7), замороженная копией: новый хост обязан её продолжать,
// иначе уже собранные плагины прочтут чужие поля.
struct host_api_05 {
    std::uint32_t size, abi, layout;
    void (*log)(const char*);
    void* (*find_class)(const char*);
    void (*find_method)(void*, const char*, graft_method_info*);
    void (*find_method_named)(const char*, const char*, graft_method_info*);
    std::uint8_t (*register_method_late)(const char*, const char*, void*, std::uint8_t, std::uint8_t);
    void (*note_call_miss)(const char*, const char*, void*, const graft_method_info*);
    void (*add_tick)(void (*)(float));
    void* (*script_root)();
    void (*note_fault)(void*, std::uint32_t, const void*, const char*);
    void (*watch_object)(void*, void (*)(void*));
    void* (*find_global)(const char*);
    std::uint8_t (*install_hook)(void*, void*, void**);
    std::uint8_t (*remove_hook)(void*);
    std::uint8_t (*install_hooks)(const graft_hook_request*, std::uint32_t);
};

TEST(EventsAbi, NewHostApiExtendsTheOldLayout) {
    static_assert(GRAFT_ABI_VERSION == 7u, "события дописаны без смены ABI");
    static_assert(offsetof(graft_host_api, log) == offsetof(host_api_05, log));
    static_assert(offsetof(graft_host_api, add_tick) == offsetof(host_api_05, add_tick));
    static_assert(offsetof(graft_host_api, watch_object) == offsetof(host_api_05, watch_object));
    static_assert(offsetof(graft_host_api, install_hooks) == offsetof(host_api_05, install_hooks));
    static_assert(offsetof(graft_host_api, post) == sizeof(host_api_05), "новое — только в хвосте");
    SUCCEED();
}

} // namespace
