// Copyright (C) 2025-2026 6wingSerap
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "graft/hub.hpp"

// Базовые события мира — ТОЛЬКО ХОСТ (что обещано плагину — graft/events.hpp: mission_ready,
// players_online, player_joined, player_left). Источники стоят на хабе и спят: таймера и вызовов
// движка нет, пока на тему никто не подписан; ушёл последний — опрос снова встаёт.
namespace graft::world_events {

inline constexpr std::chrono::milliseconds mission_every{250};
inline constexpr std::chrono::milliseconds players_every{1000};

// Что знает движок. В игре — engine_probe(), в тестах — подделка. Ник — только вошедшим: раз в секунду
// спрашивать имена всех незачем.
struct probe {
    std::move_only_function<std::optional<std::string>()>              mission; // имя мира, когда миссия есть
    std::move_only_function<std::optional<std::vector<std::string>>()> players; // uid; пусто — игры ещё нет
    std::move_only_function<std::string(std::size_t)>                  nick;    // ник i-го из последнего players()
    std::move_only_function<std::int32_t()>                            slots;   // maxPlayers
};

// Кто вошёл и вышел — разница списков uid подряд. Пустой uid и повтор не считаются.
class roster {
public:
    struct change {
        std::vector<std::string> joined;
        std::vector<std::string> left;
    };

    change update(std::vector<std::string> now) {
        std::erase(now, std::string{});
        std::ranges::sort(now);
        now.erase(std::ranges::unique(now).begin(), now.end());
        change c;
        std::ranges::set_difference(now, online_, std::back_inserter(c.joined));
        std::ranges::set_difference(online_, now, std::back_inserter(c.left));
        online_ = std::move(now);
        return c;
    }

    std::size_t size() const { return online_.size(); }

    void clear() { online_.clear(); }

private:
    std::vector<std::string> online_; // по возрастанию
};

// Поставить источники на хаб. Живут до конца процесса, как и хаб хоста.
void install(events::hub& h, probe p);

// Движок: GetGame().GetMission(), GetPlayerIndentities, ServerConfigGetInt("maxPlayers").
probe engine_probe();

} // namespace graft::world_events
