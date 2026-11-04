#include "test_harness.h"

#include "bootstrap/captured_abilities.h"
#include "session/abilities.h"
#include "session/ability_catalog.h"
#include "session/move_checker.h"
#include "tests/tooltip_key.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>

namespace {

namespace content = aoc::worldd::content;
namespace repl = aoc::protocol::repl;

constexpr int64_t kMoveSpeedMultGuid = 109228544644729LL;
constexpr int64_t kSprintEffectGuid = 6064631641826394197LL;
constexpr int64_t kEvasionEffectGuid = 106223481149050LL;
constexpr int64_t kRestEffectGuid = 109394092054139LL;
constexpr int64_t kDisableRegenGuid = 6064631668308443138LL;

content::AbilityRules make_hermetic_ability_rules() {
    content::AbilityRules rules{};
    rules.stat_regen_time = 1.0;
    rules.regen_rate = 5.0;

    rules.sprint.guid = content::kGuidSprint;
    rules.sprint.name = "General_Sprint";
    rules.sprint.held = content::Held::Smart;
    rules.sprint.min_stamina = 1.0;
    rules.sprint.cost_stat_guid = content::kGuidStamina;
    rules.sprint.cost_amount = 1.25;
    rules.sprint.cost_requires_moving = true;
    rules.sprint.charge_interval = 0.25;
    rules.sprint.effect_guid = kSprintEffectGuid;

    rules.dodge.guid = content::kGuidDodge;
    rules.dodge.name = "General_Dodge";
    rules.dodge.cost_stat_guid = content::kGuidStamina;
    rules.dodge.cost_amount = 50.0;
    rules.dodge.cooldown = 0.65;
    rules.dodge.triggers_cooldown = true;
    rules.dodge.disable_during_cooldown = true;
    rules.dodge.effect_guid = kEvasionEffectGuid;

    rules.rest.guid = content::kGuidRest;
    rules.rest.name = "General_Rest";
    rules.rest.held = content::Held::Toggle;
    rules.rest.requires_out_of_combat = true;
    rules.rest.effect_guid = kRestEffectGuid;

    content::EffectRule disable_regen{};
    disable_regen.guid = kDisableRegenGuid;
    disable_regen.name = "General_Stamina_DisableRegen";
    disable_regen.duration = 1.0;
    disable_regen.stops_regen_standing = true;
    disable_regen.stops_regen_moving = true;
    rules.effects[kDisableRegenGuid] = disable_regen;

    content::EffectRule sprint_fx{};
    sprint_fx.guid = kSprintEffectGuid;
    sprint_fx.name = "Effect_Sprint";
    sprint_fx.infinite = true;
    sprint_fx.stops_regen_moving = true;
    sprint_fx.end_effect_guid = kDisableRegenGuid;
    sprint_fx.end_effect_duration = 1.0;
    sprint_fx.stat_mods.push_back({kMoveSpeedMultGuid, 0, 0.0, 0.2});
    rules.effects[kSprintEffectGuid] = sprint_fx;

    content::EffectRule evasion_fx{};
    evasion_fx.guid = kEvasionEffectGuid;
    evasion_fx.name = "Effect_Evasion";
    evasion_fx.duration = 0.5;
    evasion_fx.stops_regen_standing = true;
    evasion_fx.stops_regen_moving = true;
    evasion_fx.end_effect_guid = kDisableRegenGuid;
    evasion_fx.end_effect_duration = 1.0;
    rules.effects[kEvasionEffectGuid] = evasion_fx;

    content::EffectRule rest_fx{};
    rest_fx.guid = kRestEffectGuid;
    rest_fx.name = "Effect_Rest";
    rest_fx.infinite = true;
    rest_fx.stat_mods.push_back({content::kGuidHealthRegeneration, content::kGuidMaxHealth, 0.05, 1.0});
    rest_fx.stat_mods.push_back({content::kGuidManaRegeneration, content::kGuidMaxMana, 0.05, 1.0});
    rules.effects[kRestEffectGuid] = rest_fx;
    return rules;
}

std::map<int64_t, double> make_initial_stats() {
    return {
        {content::kGuidStamina, 100.0},
        {content::kGuidMaxStamina, 100.0},
        {content::kGuidStaminaRegeneration, 5.0},
        {content::kGuidHealthRegeneration, 0.6},
        {content::kGuidManaRegeneration, 0.6},
        {content::kGuidMaxHealth, 100.0},
        {content::kGuidMaxMana, 100.0},
        {kMoveSpeedMultGuid, 1.0},
    };
}

int verify_move_checker_clock_barriers() {
    aoc::worldd::MoveLimits limits{};
    aoc::worldd::GoodPosition start{-777664.5, 616578.7, 13685.0, 0.0, repl::kMoveWalking, 0, false, 0.0F};
    aoc::worldd::MoveChecker checker(limits, start);
    const auto t0 = std::chrono::steady_clock::time_point{};

    repl::NetworkMove step{};
    step.time_stamp = 1.0F;
    step.location = repl::quantize_vector(100, start.x + 500.0, start.y, start.z);
    step.movement_mode = repl::kMoveWalking;
    const auto ok_verdict = checker.check(step, t0);
    SANDAL_ASSERT(ok_verdict.accepted && ok_verdict.fault == aoc::worldd::MoveFault::None);
    SANDAL_ASSERT(checker.is_stale(1.0F) && !checker.is_stale(1.05F));

    repl::NetworkMove stale = step;
    stale.time_stamp = 0.95F;
    const auto stale_verdict = checker.check(stale, t0 + std::chrono::milliseconds(50));
    SANDAL_ASSERT(!stale_verdict.accepted && stale_verdict.fault == aoc::worldd::MoveFault::TimeStampNotRising);

    checker.restart_clock();
    SANDAL_ASSERT(!checker.is_stale(0.25F));

    repl::NetworkMove nan_move = step;
    nan_move.time_stamp = std::numeric_limits<float>::quiet_NaN();
    const auto nan_verdict = checker.check(nan_move, t0 + std::chrono::milliseconds(100));
    SANDAL_ASSERT(!nan_verdict.accepted && nan_verdict.fault == aoc::worldd::MoveFault::NotFinite);
    return 0;
}

int test_manual_clock_tick_barriers() {
    sandal::test::ManualClock clock{0};
    clock.advance_ns(-100);
    clock.advance_ms(0);
    clock.advance_ticks(-3, 30);
    clock.advance_ticks(5, 0);
    SANDAL_ASSERT(clock.now_ns() == 0 && clock.now_ms() == 0);

    aoc::worldd::AbilityRuntime rt(make_hermetic_ability_rules(), make_initial_stats(), 1000.0);
    SANDAL_ASSERT(rt.ok());
    SANDAL_ASSERT(rt.enabled(content::kGuidSprint) && rt.enabled(content::kGuidDodge));
    SANDAL_ASSERT(!rt.enabled(999999LL));

    SANDAL_ASSERT(rt.trigger(content::kGuidDodge, 10, clock.now_seconds()) == aoc::worldd::Refusal::None);
    SANDAL_ASSERT(std::fabs(rt.stamina() - 50.0) < 1e-6);
    SANDAL_ASSERT(rt.evading() && rt.dash_allowed(clock.now_seconds()));
    SANDAL_ASSERT(std::fabs(rt.dodge_cooldown_until() - 0.65) < 1e-6);
    SANDAL_ASSERT(rt.trigger(content::kGuidDodge, 10, clock.now_seconds()) == aoc::worldd::Refusal::Duplicate);

    clock.advance_ms(300);
    rt.tick(clock.now_seconds());
    SANDAL_ASSERT(rt.trigger(content::kGuidDodge, 11, clock.now_seconds()) == aoc::worldd::Refusal::Cooldown);

    clock.advance_ms(200);
    rt.tick(clock.now_seconds());
    SANDAL_ASSERT(!rt.evading() && rt.dash_allowed(clock.now_seconds()));
    SANDAL_ASSERT(std::fabs(rt.regen_pause_until() - 1.50) < 1e-6);

    clock.advance_ms(350);
    rt.tick(clock.now_seconds());
    SANDAL_ASSERT(!rt.dash_allowed(clock.now_seconds()));
    SANDAL_ASSERT(std::fabs(rt.stamina() - 50.0) < 1e-6);

    clock.advance_ms(1150);
    rt.tick(clock.now_seconds());
    SANDAL_ASSERT(clock.now_ms() == 2000);
    SANDAL_ASSERT(!rt.regeneration_blocked(clock.now_seconds()));
    SANDAL_ASSERT(std::fabs(rt.stamina() - 55.0) < 1e-6);
    SANDAL_ASSERT(!rt.take_events().empty());
    return verify_move_checker_clock_barriers();
}

const tooltip_key::Key* find_tooltip_by_guid(int64_t guid) noexcept {
    for (const auto& entry : tooltip_key::kKeys) {
        if (entry.guid == guid) {
            return &entry;
        }
    }
    return nullptr;
}

int test_data_free_standin_tooltip_lookup(const sandal::test::DataFreeConfig& cfg) {
    SANDAL_ASSERT(cfg.data_free);
    SANDAL_ASSERT(std::size(tooltip_key::kKeys) == static_cast<size_t>(tooltip_key::kKeyCount));
    SANDAL_ASSERT(std::size(aoc::worldd::bootstrap::captured::kKnownAbilities) == 57U);
    SANDAL_ASSERT(std::size(aoc::worldd::bootstrap::captured::kRecordsToLoad) == 58U);

    const auto* dodge_tip = find_tooltip_by_guid(content::kGuidDodge);
    const auto* sprint_tip = find_tooltip_by_guid(content::kGuidSprint);
    SANDAL_ASSERT(dodge_tip != nullptr && std::strcmp(dodge_tip->name, "General_Dodge") == 0);
    SANDAL_ASSERT(std::fabs(dodge_tip->cooldown_s - 0.65) < 1e-6);
    SANDAL_ASSERT(sprint_tip != nullptr && std::strcmp(sprint_tip->held, "Smart") == 0);
    SANDAL_ASSERT(find_tooltip_by_guid(0LL) == nullptr);

    size_t matched_known = 0;
    for (const int64_t guid : aoc::worldd::bootstrap::captured::kKnownAbilities) {
        SANDAL_ASSERT(guid != 0LL);
        if (find_tooltip_by_guid(guid) != nullptr) {
            ++matched_known;
        }
    }
    SANDAL_ASSERT(matched_known >= 2U);
    SANDAL_ASSERT(aoc::worldd::bootstrap::captured::kRecordsToLoad[0].guid ==
                  aoc::worldd::bootstrap::captured::kKnownAbilities[0]);

    const auto* melee = sandal::test::find_standin_ability(1001U);
    const auto* finisher = sandal::test::find_standin_ability(1004U);
    SANDAL_ASSERT(melee != nullptr && melee->base_damage == 120 && melee->cooldown_ms == 1500);
    SANDAL_ASSERT(finisher != nullptr && finisher->base_damage == 410 && finisher->range_m == 30.0F);
    SANDAL_ASSERT(sandal::test::find_standin_ability(0U) == nullptr);
    SANDAL_ASSERT(sandal::test::find_standin_ability(9999U) == nullptr);
    SANDAL_ASSERT(std::strlen(aoc::worldd::refusal_name(aoc::worldd::Refusal::Cooldown)) > 0U);
    SANDAL_ASSERT(std::strlen(aoc::worldd::fault_name(aoc::worldd::MoveFault::TooFast)) > 0U);

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
