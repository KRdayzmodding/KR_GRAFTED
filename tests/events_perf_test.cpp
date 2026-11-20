// Цена событий (graft/events.hpp) без игры: сколько graft добавляет к кадру движка. Числа печатаются ([замер]);
// вентиль — с запасом в разы против замера на машине разработки: CI медленнее и шумнее, а ловить надо порядок
// (лишний замок, системный вызов, выделение памяти на кадр), не проценты.
//
// Для масштаба: сервер 404 без игроков — ~22 000 кадров в секунду, 45 мкс на кадр; с игроками — 30–60 кадров, 16–33 мс.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "graft/events.hpp"
#include "graft/frame.hpp"
#include "graft/guard.hpp"
#include "graft/hub.hpp"
#include "graft/loader.hpp"
#include "graft/profile.hpp"

namespace {

using namespace std::chrono_literals;

struct ping {
    static constexpr std::string_view name = "perf.ping";
    int                               n    = 0;
};

class EventsPerf : public ::testing::Test {
protected:
    graft::events::hub    hub; // настоящие часы: QPC в кадре — тоже цена
    graft::events::scoped use{hub};

    void SetUp() override { hub.bind_here(); }
};

// Наносекунд на вызов f, лучшее из пяти прогонов: шум планировщика только прибавляет.
template <class F>
double ns_per(int rounds, F&& f) {
    double best = 1e300;
    for (int pass = 0; pass < 5; ++pass) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < rounds; ++i) {
            f(i);
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / rounds;
        best            = ns < best ? ns : best;
    }
    return best;
}

void report(std::string_view what, double ns) {
    std::cout << "  [замер] " << what << ": " << ns << " нс" << std::endl;
}

constexpr int rounds = 200'000;

std::atomic<int> sink{0};

void plain_tick(float) {
    sink.fetch_add(1, std::memory_order_relaxed);
}

TEST_F(EventsPerf, IdleFrameCostsNothing) {
    const double ns = ns_per(rounds, [](int i) { graft::events::frame(0.016f, static_cast<std::uint64_t>(i)); });
    report("кадр без подписчиков, таймеров и заданий", ns);
    EXPECT_LT(ns, 100.0);
}

TEST_F(EventsPerf, WaitingTimersCostOneClockRead) {
    for (int i = 0; i < 16; ++i) {
        graft::every(1h, [] {});
    }
    graft::events::frame(0.016f, 0); // заявки дошли
    const double ns = ns_per(rounds, [](int i) { graft::events::frame(0.016f, static_cast<std::uint64_t>(i)); });
    report("кадр с 16 таймерами, ни один не пора", ns);
    EXPECT_LT(ns, 200.0);
}

TEST_F(EventsPerf, TickToSubscribers) {
    const double baseline = ns_per(rounds, [](int i) {
        const graft::loader::fault_place place{reinterpret_cast<void*>(&plain_tick), "GRAFT_ON_TICK"};
        graft::detail::guarded<void>(reinterpret_cast<void*>(&plain_tick), [i] { plain_tick(static_cast<float>(i)); });
    });
    report("для сравнения: один старый GRAFT_ON_TICK под ловушкой", baseline);
    double one = 0;
    {
        graft::on<graft::tick> a{[] { sink.fetch_add(1, std::memory_order_relaxed); }};
        one = ns_per(rounds, [](int i) { graft::events::frame(0.016f, static_cast<std::uint64_t>(i)); });
    }
    report("кадр: graft::tick одному подписчику", one);
    std::vector<std::unique_ptr<graft::on<graft::tick>>> many;
    for (int i = 0; i < 8; ++i) {
        many.push_back(std::make_unique<graft::on<graft::tick>>([] { sink.fetch_add(1, std::memory_order_relaxed); }));
    }
    const double eight = ns_per(rounds, [](int i) { graft::events::frame(0.016f, static_cast<std::uint64_t>(i)); });
    report("кадр: graft::tick восьми подписчикам", eight);
    EXPECT_LT(one, 300.0);
    EXPECT_LT(eight, 8 * 150.0);
}

TEST_F(EventsPerf, PublishWithoutListenersIsOneLoad) {
    const double ns = ns_per(rounds * 10, [](int i) { graft::publish(ping{.n = i}); });
    report("publish без подписчиков", ns);
    EXPECT_LT(ns, 10.0);
}

TEST_F(EventsPerf, PublishToOneListener) {
    graft::on<ping> a{[](const ping& p) { sink.fetch_add(p.n, std::memory_order_relaxed); }};
    const double    ns = ns_per(rounds, [](int i) { graft::publish(ping{.n = i}); });
    report("publish одному подписчику на потоке скрипта", ns);
    EXPECT_LT(ns, 200.0);
}

TEST_F(EventsPerf, PostAndRunOnNextFrame) {
    const double ns = ns_per(rounds / 10, [](int) {
        graft::post([] { sink.fetch_add(1, std::memory_order_relaxed); });
        graft::events::frame(0.016f, 0);
    });
    report("post + кадр, который его выполнил", ns);
    EXPECT_LT(ns, 1000.0);
}

} // namespace

// Отчёт -graftProfile: доля, средний кадр и места по убыванию цены.
TEST(EventsProfile, ReportNamesCostliestCallsFirst) {
    graft::profile::meter m;
    const std::string     a = "a.cpp:1"; // место — по адресу строки у записи хаба, как в hub::call
    const std::string     b = "b.cpp:2";
    m.frame(1000);
    m.frame(3000);
    m.call(nullptr, a, 100);
    m.call(nullptr, b, 2500);
    m.call(nullptr, a, 100);
    const std::string text = m.report(1.0, 1.0); // 1 нс на такт, окно — секунда
    EXPECT_NE(text.find("кадров 2"), std::string::npos) << text;
    EXPECT_NE(text.find("среднее 2.000 мкс (сам graft 0.650, обработчики 1.350)"), std::string::npos) << text;
    EXPECT_NE(text.find("худший 3.0 мкс"), std::string::npos) << text;
    EXPECT_LT(text.find("b.cpp:2"), text.find("a.cpp:1")) << text;
    EXPECT_NE(text.find("a.cpp:1 — вызовов 2"), std::string::npos) << text;
    EXPECT_FALSE(graft::profile::on()); // без ключа выключен
}

// Запись умерла, адрес строки достался другой: место — другое.
TEST(EventsProfile, ReusedAddressWithOtherTextIsOtherPlace) {
    graft::profile::meter m;
    std::string           where = "a.cpp:1";
    m.call(nullptr, where, 10);
    where.replace(0, 1, "b");
    m.call(nullptr, where, 10);
    const std::string text = m.report(1.0, 1.0);
    EXPECT_NE(text.find("a.cpp:1 — вызовов 1"), std::string::npos) << text;
    EXPECT_NE(text.find("b.cpp:1 — вызовов 1"), std::string::npos) << text;
}

// Счётчик кадров без подписки на graft::tick.
TEST(EventsFrames, CountsEngineFrames) {
    const std::uint64_t was = graft::frames();
    graft::frame::detail::note_frame(0.016f);
    EXPECT_EQ(graft::frames(), was + 1);
}

// Кадр с таймерами, которым не пора, часы не читает: «сейчас» — по сумме dt кадров, часы — раз в 64 кадра.
std::atomic<int> clock_reads{0};

graft::events::clock::time_point counted_now() {
    clock_reads.fetch_add(1, std::memory_order_relaxed);
    return graft::events::clock::now();
}

TEST(EventsClock, WaitingTimersReadTheClockRarely) {
    graft::events::hub    hub{&counted_now};
    graft::events::scoped use{hub};
    hub.bind_here();
    graft::every(1h, [] {});
    clock_reads = 0;
    for (int i = 0; i < 6400; ++i) {
        graft::events::frame(0.016f, static_cast<std::uint64_t>(i));
    }
    EXPECT_LE(clock_reads.load(), 6400 / 64 + 1);
}
