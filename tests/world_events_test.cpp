// Базовые события мира (graft/world_events.hpp) без игры: движок подменён подделкой, хаб — свой,
// часы — поддельные. Главное здесь — что источник спит без подписчиков: ни таймера, ни вопроса.
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "graft/events.hpp"
#include "graft/hub.hpp"
#include "graft/world_events.hpp"

namespace {

using namespace std::chrono_literals;
using graft::events::clock;

clock::time_point fake_now{};

clock::time_point now_for_tests() {
    return fake_now;
}

using names = std::vector<std::string>;

class WorldEvents : public ::testing::Test {
protected:
    graft::events::hub    hub{&now_for_tests};
    graft::events::scoped use{hub};

    int                                              asked_mission = 0;
    int                                              asked_players = 0;
    int                                              asked_nicks   = 0;
    std::optional<std::string>                       mission;
    std::vector<std::pair<std::string, std::string>> online; // uid, ник

    void SetUp() override {
        fake_now = clock::time_point{} + 1h;
        hub.bind_here();
        graft::world_events::install(hub, graft::world_events::probe{
                                              .mission = [this] { ++asked_mission; return mission; },
                                              .players = [this] {
                                                  ++asked_players;
                                                  std::vector<std::string> uids;
                                                  for (const auto& [uid, nick] : online) {
                                                      uids.push_back(uid);
                                                  }
                                                  return std::optional{uids}; },
                                              .nick    = [this](std::size_t i) { ++asked_nicks; return online[i].second; },
                                              .slots   = [] { return 60; },
                                          });
    }

    // Кадр через step: таймеры источников и задания хаба.
    void frame(clock::duration step = 0ms) {
        fake_now += step;
        hub.frame(1h);
    }
};

TEST_F(WorldEvents, SleepWithoutListeners) {
    for (int i = 0; i < 5; ++i) {
        frame(1s);
    }
    EXPECT_EQ(asked_mission, 0);
    EXPECT_EQ(asked_players, 0);
    EXPECT_EQ(hub.timers(), 0u);
}

TEST_F(WorldEvents, PlayersPolledOnlyWhileSomeoneListens) {
    names joined;
    {
        graft::on<graft::player_joined> j{[&](const graft::player_joined& e) { joined.emplace_back(graft::as_text(e.uid)); }};
        online = {{"76561198000000002", "b"}, {"76561198000000001", "a"}};
        frame(); // источнику сказали — встал таймер
        EXPECT_EQ(hub.timers(), 1u);
        frame(); // первая проверка — сразу
        EXPECT_EQ(joined, (names{"76561198000000001", "76561198000000002"}));
        frame(1s);
        EXPECT_EQ(asked_players, 2);
    }
    frame(); // ушёл последний — таймер снят
    EXPECT_EQ(hub.timers(), 0u);
    const int was = asked_players;
    frame(5s);
    EXPECT_EQ(asked_players, was);
}

TEST_F(WorldEvents, JoinLeaveAndCount) {
    names                            joined;
    names                            nicks;
    names                            left;
    std::vector<int>                 counts;
    graft::on<graft::player_joined>  j{[&](const graft::player_joined& e) {
        joined.emplace_back(graft::as_text(e.uid));
        nicks.emplace_back(graft::as_text(e.nick));
    }};
    graft::on<graft::player_left>    l{[&](const graft::player_left& e) { left.emplace_back(graft::as_text(e.uid)); }};
    graft::on<graft::players_online> c{[&](const graft::players_online& p) {
        counts.push_back(p.count);
        EXPECT_EQ(p.slots, 60);
    }};
    frame();
    frame(); // никого: состояние 0
    online = {{"1", "Пук"}};
    frame(1s);
    online = {{"2", "b"}};
    frame(1s);
    frame(1s); // без перемен — ничего
    EXPECT_EQ(joined, (names{"1", "2"}));
    EXPECT_EQ(nicks, (names{"Пук", "b"}));
    EXPECT_EQ(left, (names{"1"}));
    EXPECT_EQ(counts, (std::vector<int>{0, 1}));
    EXPECT_EQ(asked_nicks, 2);   // только вошедшим
    EXPECT_EQ(hub.timers(), 1u); // три темы — один опрос
}

TEST_F(WorldEvents, MissionReadyIsStateAndPollingStops) {
    names seen;
    {
        graft::on<graft::mission_ready> m{[&](const graft::mission_ready& e) { seen.emplace_back(graft::as_text(e.world)); }};
        frame();
        frame(250ms);
        EXPECT_TRUE(seen.empty());
        mission = "dz_map";
        frame(250ms);
        EXPECT_EQ(seen, (names{"dz_map"}));
        const int was = asked_mission;
        frame(1s);
        EXPECT_EQ(asked_mission, was); // миссия одна: больше не спрашиваем
        EXPECT_EQ(hub.timers(), 0u);
    }
    names                           late;
    graft::on<graft::mission_ready> m{[&](const graft::mission_ready& e) { late.emplace_back(graft::as_text(e.world)); }};
    frame();
    frame();
    EXPECT_EQ(late, (names{"dz_map"}));
}

TEST(Roster, JoinedAndLeftBetweenLists) {
    graft::world_events::roster r;
    auto                        c = r.update({"b", "a"});
    EXPECT_EQ(c.joined, (names{"a", "b"}));
    EXPECT_TRUE(c.left.empty());
    c = r.update({"a", "b"});
    EXPECT_TRUE(c.joined.empty() && c.left.empty());
    c = r.update({"c", "a"});
    EXPECT_EQ(c.joined, (names{"c"}));
    EXPECT_EQ(c.left, (names{"b"}));
    c = r.update({"a", "a", ""}); // повтор и пустой uid не считаются
    EXPECT_TRUE(c.joined.empty());
    EXPECT_EQ(c.left, (names{"c"}));
    EXPECT_EQ(r.size(), 1u);
}

TEST(EventFields, TextIsCutAtUtf8Boundary) {
    graft::player_joined j;
    graft::put(j.uid, "76561198000000001");
    EXPECT_EQ(graft::as_text(j.uid), "76561198000000001");
    char tiny[6];
    graft::put(tiny, "абв"); // 6 байт: влезает 5 — третья буква целиком не входит
    EXPECT_EQ(graft::as_text(tiny), "аб");
}

} // namespace
