#include "test_harness.h"

#include <cmath>
#include <cstdint>

namespace {

int test_manual_clock_tick_barriers() {
    sandal::test::ManualClock clock{0};
    SANDAL_ASSERT(clock.now_ns() == 0);
    SANDAL_ASSERT(clock.now_ms() == 0);

    clock.advance_ns(-100);
    clock.advance_ms(0);
    clock.advance_ticks(-3, 30);
    clock.advance_ticks(5, 0);
    SANDAL_ASSERT(clock.now_ns() == 0);

    const auto* ability = sandal::test::find_standin_ability(1001U);
    SANDAL_ASSERT(ability != nullptr);
    const int64_t ready_at_ms = clock.now_ms() + ability->cooldown_ms;

    clock.advance_ticks(44, 30);
    SANDAL_ASSERT(clock.now_ms() == 1466);
    SANDAL_ASSERT(clock.now_ms() < ready_at_ms);

    clock.advance_ticks(1, 30);
    SANDAL_ASSERT(clock.now_ms() == 1499);
    SANDAL_ASSERT(clock.now_ms() < ready_at_ms);

    clock.advance_ns(1LL);
    SANDAL_ASSERT(clock.now_ms() == 1500);
    SANDAL_ASSERT(clock.now_ms() >= ready_at_ms);
    SANDAL_ASSERT(std::fabs(clock.now_seconds() - 1.5) < 1e-6);
    return 0;
}

int test_data_free_standin_tooltip_lookup(const sandal::test::DataFreeConfig& cfg) {
    SANDAL_ASSERT(cfg.data_free);
    SANDAL_ASSERT(sandal::test::kStandinAbilities.size() == 4U);

    for (const auto& row : sandal::test::kStandinAbilities) {
        SANDAL_ASSERT(row.ability_id >= 1001U);
        SANDAL_ASSERT(row.base_damage > 0);
        SANDAL_ASSERT(row.cooldown_ms >= 1500);
        SANDAL_ASSERT(row.range_m > 0.0F);
    }

    const auto* melee = sandal::test::find_standin_ability(1001U);
    SANDAL_ASSERT(melee != nullptr);
    SANDAL_ASSERT(melee->base_damage == 120);
    SANDAL_ASSERT(melee->cooldown_ms == 1500);

    const auto* finisher = sandal::test::find_standin_ability(1004U);
    SANDAL_ASSERT(finisher != nullptr);
    SANDAL_ASSERT(finisher->base_damage == 410);
    SANDAL_ASSERT(finisher->range_m == 30.0F);

    SANDAL_ASSERT(sandal::test::find_standin_ability(0U) == nullptr);
    SANDAL_ASSERT(sandal::test::find_standin_ability(9999U) == nullptr);

    char arg0[] = "test_world_clock_and_data_free";
    char arg1[] = "--data-free";
    char arg2[] = "--case";
    char arg3[] = "manual_clock_tick_barriers";
    char* argv[] = {arg0, arg1, arg2, arg3};
    const auto parsed = sandal::test::parse_test_cli(4, argv);
    SANDAL_ASSERT(parsed.data_free);
    SANDAL_ASSERT(sandal::test::should_run_case(parsed, "manual_clock_tick_barriers"));
    SANDAL_ASSERT(!sandal::test::should_run_case(parsed, "data_free_standin_tooltip_lookup"));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "manual_clock_tick_barriers")) {
        if (test_manual_clock_tick_barriers() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "data_free_standin_tooltip_lookup")) {
        if (test_data_free_standin_tooltip_lookup(cfg) != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
