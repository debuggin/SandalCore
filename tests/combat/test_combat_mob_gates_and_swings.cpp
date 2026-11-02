#include "../world/test_harness.h"

#include <cstdint>

namespace {

struct MobState {
    int32_t level{10};
    int32_t health{500};
    int32_t max_health{500};
    bool in_combat{false};
    bool evading{false};
    bool casting{false};
    int64_t next_swing_ms{0};
    uint32_t swings_landed{0};
};

[[nodiscard]] bool evaluate_mob_aggro_or_leash(
    MobState& mob,
    int32_t player_level,
    float dist_from_mob_m,
    float dist_from_anchor_m,
    bool has_los) noexcept {
    constexpr float kFootstepRadiusM = 8.0F;
    constexpr float kSightRadiusM = 20.0F;
    constexpr float kLeashDistanceM = 140.0F;
    if (mob.in_combat && dist_from_anchor_m > kLeashDistanceM) {
        mob.in_combat = false;
        mob.evading = true;
        mob.casting = false;
        mob.health = mob.max_health;
        return false;
    }
    if (!mob.in_combat && (player_level - mob.level) <= 9) {
        if (dist_from_mob_m <= kFootstepRadiusM || (has_los && dist_from_mob_m <= kSightRadiusM)) {
            mob.in_combat = true;
            mob.evading = false;
        }
    }
    return mob.in_combat;
}

[[nodiscard]] bool step_mob_swing_or_stop_cast(
    MobState& mob,
    bool stop_cast_signal,
    int64_t swing_cadence_ms,
    int64_t now_ms) noexcept {
    if (stop_cast_signal && mob.casting) {
        mob.casting = false;
        mob.next_swing_ms = now_ms + swing_cadence_ms;
        return false;
    }
    if (!mob.in_combat || mob.casting || now_ms < mob.next_swing_ms) {
        return false;
    }
    ++mob.swings_landed;
    mob.next_swing_ms = now_ms + swing_cadence_ms;
    return true;
}

int test_mob_aggro_and_leash_evade() {
    sandal::test::ManualClock clock{0};
    MobState mob{};

    SANDAL_ASSERT(!evaluate_mob_aggro_or_leash(mob, 20, 5.0F, 5.0F, true));
    SANDAL_ASSERT(!mob.in_combat);

    SANDAL_ASSERT(!evaluate_mob_aggro_or_leash(mob, 19, 15.0F, 15.0F, false));
    clock.advance_ms(2'000);
    SANDAL_ASSERT(evaluate_mob_aggro_or_leash(mob, 19, 19.5F, 19.5F, true));
    SANDAL_ASSERT(mob.in_combat && !mob.evading);

    mob.health = 120;
    mob.casting = true;
    SANDAL_ASSERT(evaluate_mob_aggro_or_leash(mob, 19, 10.0F, 140.0F, true));
    SANDAL_ASSERT(!evaluate_mob_aggro_or_leash(mob, 19, 10.0F, 140.1F, true));
    SANDAL_ASSERT(!mob.in_combat && mob.evading && !mob.casting);
    SANDAL_ASSERT(mob.health == mob.max_health);
    return 0;
}

int test_mob_swing_timer_cadence() {
    sandal::test::ManualClock clock{0};
    const auto* melee = sandal::test::find_standin_ability(1001U);
    SANDAL_ASSERT(melee != nullptr);

    MobState mob{};
    mob.in_combat = true;
    mob.next_swing_ms = melee->cooldown_ms;

    clock.advance_ticks(44, 30);
    SANDAL_ASSERT(!step_mob_swing_or_stop_cast(mob, false, melee->cooldown_ms, clock.now_ms()));
    SANDAL_ASSERT(mob.swings_landed == 0U);

    clock.advance_ms(34);
    SANDAL_ASSERT(step_mob_swing_or_stop_cast(mob, false, melee->cooldown_ms, clock.now_ms()));
    SANDAL_ASSERT(mob.swings_landed == 1U && mob.next_swing_ms == 3000);

    mob.casting = true;
    clock.advance_ms(500);
    SANDAL_ASSERT(!step_mob_swing_or_stop_cast(mob, true, melee->cooldown_ms, clock.now_ms()));
    SANDAL_ASSERT(!mob.casting && mob.next_swing_ms == 3500);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "mob_aggro_and_leash_evade")) {
        if (test_mob_aggro_and_leash_evade() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "mob_swing_timer_cadence")) {
        if (test_mob_swing_timer_cadence() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
