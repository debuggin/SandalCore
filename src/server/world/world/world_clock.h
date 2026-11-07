// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ClientSetTime day/night clock: [06:00, 18:00) runs 6x, [18:00, 06:00) runs 9x (12,000 real s / 86,400 game s).
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace aoc::worldd::world {

inline constexpr double kDefaultStartHour = 7.0;

class WorldClock {
public:
    static constexpr double kGameDay = 86400.0;
    static constexpr double kDayStart = 6.0 * 3600.0;
    static constexpr double kNightStart = 18.0 * 3600.0;
    static constexpr double kDayRate = 6.0;
    static constexpr double kNightRate = 9.0;
    static constexpr double kDayGameSeconds = kNightStart - kDayStart;
    static constexpr double kDayReal = kDayGameSeconds / kDayRate;
    static constexpr double kNightReal = (kGameDay - kDayGameSeconds) / kNightRate;
    static constexpr double kCycle = kDayReal + kNightReal;
    static constexpr double kStartDay = 1858.0;

    WorldClock() : WorldClock(starting_at(kDefaultStartHour)) {}

    static WorldClock reading(double game_time, double at = 0.0) {
        const double day = std::floor(game_time / kGameDay);
        const double second = game_time - day * kGameDay;
        const bool after_start = second >= kDayStart;
        const double cycle_day = after_start ? day : day - 1.0;
        const double since = after_start ? second - kDayStart : second + (kGameDay - kDayStart);
        const double p = since < kDayGameSeconds ? since / kDayRate : kDayReal + (since - kDayGameSeconds) / kNightRate;
        return WorldClock((cycle_day - kStartDay) * kCycle + p - at);
    }

    static WorldClock starting_at(double hour) {
        const double h = hour - 24.0 * std::floor(hour / 24.0);
        return reading(kStartDay * kGameDay + h * 3600.0, 0.0);
    }

    double game_time(double world_seconds) const {
        const double c = cycle_ + world_seconds;
        const double k = std::floor(c / kCycle);
        const double p = c - k * kCycle;
        const double into = p < kDayReal ? kDayRate * p : kDayGameSeconds + kNightRate * (p - kDayReal);
        return (kStartDay + k) * kGameDay + kDayStart + into;
    }

    double hour(double world_seconds) const {
        double h = std::fmod(game_time(world_seconds), kGameDay) / 3600.0;
        if (h < 0.0) h += 24.0;
        if (h >= 24.0) h -= 24.0;
        return h;
    }

    double hour_at(double world_seconds) const { return hour(world_seconds); }

    static bool is_night(double hour) { return hour >= 18.0 || hour < 6.0; }

private:
    explicit WorldClock(double cycle) : cycle_(cycle) {}
    double cycle_ = 0;
};

inline WorldClock default_world_clock() { return WorldClock::starting_at(kDefaultStartHour); }

inline std::string hour_text(double hour) {
    const int minutes = static_cast<int>(std::floor(hour * 60.0 + 1e-6));
    const int h = (minutes / 60) % 24, m = minutes % 60;
    const int h12 = h % 12 == 0 ? 12 : h % 12;
    char text[32];
    std::snprintf(text, sizeof(text), "%d:%02d %s", h12, m, h >= 12 ? "PM" : "AM");
    return text;
}

}  // namespace aoc::worldd::world
