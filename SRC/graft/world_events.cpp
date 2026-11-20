// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#include "graft/world_events.hpp"

#include <functional>
#include <utility>

#include "graft/events.hpp"
#include "graft/native.hpp"
#include "graft/world.hpp"

// Базовые события мира от хоста. Опрос, а не врезка: движок о входе игрока скрипту говорит
// через DayZGame.OnEvent, места этого вызова мы пока не перехватываем (план — README «События»).
// Опрос один на процесс и только пока есть подписчики: раз в секунду список PlayerIdentity.
namespace graft::world_events {
namespace {

using namespace std::chrono_literals;

struct world {
    events::hub& h;
    probe        p;

    const graft_topic* mission_t = nullptr;
    const graft_topic* online_t  = nullptr;
    const graft_topic* joined_t  = nullptr;
    const graft_topic* left_t    = nullptr;

    std::uint64_t mission_timer = 0;
    bool          mission_found = false;

    int            players_wanted = 0; // тем игроков с подписчиками: 0..3
    std::uint64_t  players_timer  = 0;
    roster         online;
    players_online last{.count = -1, .slots = -1};
};

void look_mission(world& w) {
    const std::optional<std::string> name = w.p.mission ? w.p.mission() : std::nullopt;
    if (!name) {
        return;
    }
    w.mission_found = true;
    w.h.unsubscribe(std::exchange(w.mission_timer, 0)); // миссия одна на процесс: больше не спрашиваем
    mission_ready m;
    put(m.world, *name);
    w.h.publish(w.mission_t, &m, true);
}

void look_players(world& w) {
    const std::optional<std::vector<std::string>> list = w.p.players ? w.p.players() : std::nullopt;
    if (!list) {
        return;
    }
    const roster::change change = w.online.update(*list);
    for (const std::string& uid : change.left) {
        player_left e;
        put(e.uid, uid);
        w.h.publish(w.left_t, &e, false);
    }
    for (const std::string& uid : change.joined) {
        player_joined e;
        put(e.uid, uid);
        if (const auto at = std::ranges::find(*list, uid); at != list->end() && w.p.nick) {
            put(e.nick, w.p.nick(static_cast<std::size_t>(at - list->begin())));
        }
        w.h.publish(w.joined_t, &e, false);
    }
    if (w.last.slots < 0) {
        w.last.slots = w.p.slots ? w.p.slots() : 0;
    }
    const players_online now{.count = static_cast<std::int32_t>(w.online.size()), .slots = w.last.slots};
    if (now.count != w.last.count) {
        w.last = now;
        w.h.publish(w.online_t, &now, true);
    }
}

void players_wanted(world& w, bool wanted) {
    w.players_wanted += wanted ? 1 : -1;
    if (w.players_wanted > 0 && w.players_timer == 0) {
        w.players_timer = w.h.timer_own(0ms, players_every, [&w] { look_players(w); }, "graft: игроки");
    } else if (w.players_wanted == 0 && w.players_timer != 0) {
        w.h.unsubscribe(std::exchange(w.players_timer, 0));
        // Снова позовут — первая проверка назовёт вошедшими всех, кто на сервере.
        w.online.clear();
        w.last = {.count = -1, .slots = w.last.slots};
    }
}

} // namespace

void install(events::hub& h, probe p) {
    world& w    = *new world{h, std::move(p)}; // до конца процесса, как хаб хоста
    w.mission_t = h.topic(mission_ready::name, sizeof(mission_ready), "graft");
    w.online_t  = h.topic(players_online::name, sizeof(players_online), "graft");
    w.joined_t  = h.topic(player_joined::name, sizeof(player_joined), "graft");
    w.left_t    = h.topic(player_left::name, sizeof(player_left), "graft");
    h.watch_own(mission_ready::name, sizeof(mission_ready), [&w](bool wanted) {
        if (wanted && !w.mission_found && w.mission_timer == 0) {
            w.mission_timer = w.h.timer_own(0ms, mission_every, [&w] { look_mission(w); }, "graft: миссия");
        } else if (!wanted) {
            w.h.unsubscribe(std::exchange(w.mission_timer, 0));
        } }, "graft: миссия");
    h.watch_own(players_online::name, sizeof(players_online), [&w](bool wanted) { players_wanted(w, wanted); }, "graft: игроки");
    h.watch_own(player_joined::name, sizeof(player_joined), [&w](bool wanted) { players_wanted(w, wanted); }, "graft: игроки");
    h.watch_own(player_left::name, sizeof(player_left), [&w](bool wanted) { players_wanted(w, wanted); }, "graft: игроки");
}

probe engine_probe() {
    return probe{
        .mission = []() -> std::optional<std::string> {
            const auto game = graft::game();
            if (!game || !game.call<ref<"Mission">, "GetMission">()) {
                return std::nullopt;
            }
            std::string name;
            game.proto<void, "GetWorldName">(std::ref(name));
            return name;
        },
        // Ники — по тем же PlayerIdentity, пока жив вызов: массив scratch до следующего players() не трогается.
        .players = []() -> std::optional<std::vector<std::string>> {
            const auto game = graft::game();
            const auto ids  = scratch<array<ref<"PlayerIdentity">>>();
            if (!game || !ids) {
                return std::nullopt;
            }
            ids.clear();
            game.call<void, "GetPlayerIndentities">(ids); // опечатка — движковая
            std::vector<std::string> out;
            out.reserve(ids.size());
            for (const auto id : ids) {
                out.push_back(id ? id.proto<std::string, "GetPlainId">() : std::string{});
            }
            return out;
        },
        .nick = [](std::size_t i) -> std::string {
            const auto ids = scratch<array<ref<"PlayerIdentity">>>();
            const auto id  = ids && i < static_cast<std::size_t>(ids.size()) ? ids[static_cast<i32>(i)] : ref<"PlayerIdentity">{};
            return id ? id.proto<std::string, "GetName">() : std::string{};
        },
        .slots = []() -> std::int32_t {
            const auto game = graft::game();
            return game ? game.call<i32, "ServerConfigGetInt">(str{"maxPlayers"}) : 0;
        },
    };
}

} // namespace graft::world_events
