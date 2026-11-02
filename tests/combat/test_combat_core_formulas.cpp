#include "../world/test_harness.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

struct HitInput {
    int32_t base_damage;
    int32_t attack_power;
    int32_t target_armor;
    float crit_multiplier;
    bool is_crit;
};

[[nodiscard]] int32_t resolve_mitigated_hit(const HitInput& hit, bool magical_scaling) noexcept {
    if (hit.base_damage <= 0) {
        return 0;
    }
    const int32_t power_bonus = magical_scaling ? hit.attack_power : (hit.attack_power * 4) / 5;
    const float raw = static_cast<float>(hit.base_damage + std::max(0, power_bonus));
    const float clamped_armor = static_cast<float>(std::max(0, hit.target_armor));
    const float mitigation = 1000.0F / (1000.0F + clamped_armor);
    const float crit_mult = hit.is_crit ? std::max(1.0F, hit.crit_multiplier) : 1.0F;
    return static_cast<int32_t>(std::lround(raw * mitigation * crit_mult));
}

struct KnownNpcCastGap {
    uint32_t ability_id;
    uint32_t capture_id;
    int32_t anim_end_ms;
};

inline constexpr std::array<KnownNpcCastGap, 3> kKnownNpcCastGaps{{
    {1001U, 13U, 800},
    {1002U, 24U, 1200},
    {1004U, 60U, 1750},
}};

[[nodiscard]] constexpr const KnownNpcCastGap* find_known_npc_gap(
    uint32_t ability_id,
    uint32_t capture_id) noexcept {
    for (const auto& entry : kKnownNpcCastGaps) {
        if (entry.ability_id == ability_id && entry.capture_id == capture_id) {
            return &entry;
        }
    }
    return nullptr;
}

int test_mitigation_and_crit_scaling() {
    sandal::test::ManualClock clock{0};
    const auto* melee = sandal::test::find_standin_ability(1001U);
    SANDAL_ASSERT(melee != nullptr);

    const HitInput normal_hit{melee->base_damage, 100, 1000, 1.5F, false};
    const int32_t phys_dmg = resolve_mitigated_hit(normal_hit, false);
    const int32_t mag_dmg = resolve_mitigated_hit(normal_hit, true);
    SANDAL_ASSERT(phys_dmg == 100);
    SANDAL_ASSERT(mag_dmg == 110);

    const HitInput crit_hit{melee->base_damage, 100, 1000, 1.5F, true};
    SANDAL_ASSERT(resolve_mitigated_hit(crit_hit, true) == 165);

    const HitInput zero_hit{0, 100, 500, 2.0F, true};
    SANDAL_ASSERT(resolve_mitigated_hit(zero_hit, false) == 0);

    const HitInput neg_armor{melee->base_damage, 0, -500, 0.5F, true};
    SANDAL_ASSERT(resolve_mitigated_hit(neg_armor, false) == melee->base_damage);

    clock.advance_ms(melee->cooldown_ms - 1);
    SANDAL_ASSERT(clock.now_ms() < melee->cooldown_ms);
    clock.advance_ms(1);
    SANDAL_ASSERT(clock.now_ms() == melee->cooldown_ms);
    return 0;
}

int test_npc_ability_known_table_standin(const sandal::test::DataFreeConfig& cfg) {
    SANDAL_ASSERT(cfg.data_free);
    sandal::test::ManualClock clock{0};

    const auto* gap = find_known_npc_gap(1002U, 24U);
    SANDAL_ASSERT(gap != nullptr);
    const auto* ability = sandal::test::find_standin_ability(gap->ability_id);
    SANDAL_ASSERT(ability != nullptr);
    SANDAL_ASSERT(ability->cooldown_ms == 6000);

    clock.advance_ms(gap->anim_end_ms);
    const int64_t cooldown_ready_ms = clock.now_ms() + ability->cooldown_ms;
    SANDAL_ASSERT(clock.now_ms() == 1200);

    clock.advance_ticks(180, 30);
    SANDAL_ASSERT(clock.now_ms() == cooldown_ready_ms);

    SANDAL_ASSERT(find_known_npc_gap(1002U, 25U) == nullptr);
    SANDAL_ASSERT(find_known_npc_gap(9999U, 24U) == nullptr);
    SANDAL_ASSERT(sandal::test::find_standin_ability(9999U) == nullptr);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "mitigation_and_crit_scaling")) {
        if (test_mitigation_and_crit_scaling() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "npc_ability_known_table_standin")) {
        if (test_npc_ability_known_table_standin(cfg) != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
