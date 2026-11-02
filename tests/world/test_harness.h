#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace sandal::test {

class ManualClock {
public:
    explicit constexpr ManualClock(int64_t initial_ns = 0) noexcept
        : now_ns_(initial_ns) {}

    [[nodiscard]] constexpr int64_t now_ns() const noexcept {
        return now_ns_;
    }

    [[nodiscard]] constexpr int64_t now_ms() const noexcept {
        return now_ns_ / 1'000'000LL;
    }

    [[nodiscard]] constexpr double now_seconds() const noexcept {
        return static_cast<double>(now_ns_) * 1e-9;
    }

    constexpr void advance_ns(int64_t delta_ns) noexcept {
        if (delta_ns > 0) {
            now_ns_ += delta_ns;
        }
    }

    constexpr void advance_ms(int64_t delta_ms) noexcept {
        if (delta_ms > 0) {
            now_ns_ += delta_ms * 1'000'000LL;
        }
    }

    constexpr void advance_ticks(int64_t ticks, int64_t tick_hz = 30) noexcept {
        if (ticks > 0 && tick_hz > 0) {
            now_ns_ += (ticks * 1'000'000'000LL) / tick_hz;
        }
    }

private:
    int64_t now_ns_{0};
};

struct DataFreeConfig {
    bool data_free{true};
    std::string_view case_filter{};
};

[[nodiscard]] inline DataFreeConfig parse_test_cli(int argc, char** argv) noexcept {
    DataFreeConfig cfg{};
    if (argv == nullptr) {
        return cfg;
    }
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        const std::string_view arg{argv[i]};
        if (arg == "--data-free") {
            cfg.data_free = true;
        } else if (arg == "--no-data-free") {
            cfg.data_free = false;
        } else if (arg == "--case" && (i + 1) < argc && argv[i + 1] != nullptr) {
            cfg.case_filter = std::string_view{argv[++i]};
        }
    }
    return cfg;
}

[[nodiscard]] inline bool should_run_case(const DataFreeConfig& cfg, std::string_view name) noexcept {
    return cfg.case_filter.empty() || cfg.case_filter == name;
}

struct StandinAbilityRow {
    uint32_t ability_id;
    int32_t base_damage;
    int32_t cooldown_ms;
    float range_m;
};

inline constexpr std::array<StandinAbilityRow, 4> kStandinAbilities{{
    {1001U, 120, 1500, 5.0F},
    {1002U, 250, 6000, 25.0F},
    {1003U, 85, 3000, 15.0F},
    {1004U, 410, 12000, 30.0F},
}};

[[nodiscard]] constexpr const StandinAbilityRow* find_standin_ability(uint32_t ability_id) noexcept {
    for (const auto& row : kStandinAbilities) {
        if (row.ability_id == ability_id) {
            return &row;
        }
    }
    return nullptr;
}

}  // namespace sandal::test

#define SANDAL_ASSERT(cond)                                                        \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                              \
        }                                                                          \
    } while (false)
