// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "combat/combat_world.h"
#include "session/abilities.h"
#include "session/ability_pipeline.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace aoc::worldd;

#define CHECK(expr)                                                                                 \
    do {                                                                                            \
        if (!(expr)) {                                                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr);                  \
            std::exit(1);                                                                           \
        }                                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= (eps))

class EmptyGearCatalog final : public stats::GearCatalog {
public:
    const stats::ItemGear* item(int64_t) const override { return nullptr; }
    const stats::SetBonus* set(int64_t) const override { return nullptr; }
    std::string slot_name(int) const override { return ""; }
};

int32_t add_const_program(stats::StatBundle& b, double val) {
    const uint16_t idx = static_cast<uint16_t>(b.consts.size());
    b.consts.push_back(val);
    stats::Program p;
    p.locals = 0;
    p.code = {
        stats::op::Const,
        static_cast<uint8_t>(idx & 0xFFu),
        static_cast<uint8_t>((idx >> 8) & 0xFFu),
        stats::op::Ret,
    };
    const int32_t prog_id = static_cast<int32_t>(b.programs.size());
    b.programs.push_back(std::move(p));
    return prog_id;
}

void add_stat_def(stats::StatBundle& b, int64_t guid, double base_val, uint8_t flags, double min_val,
                  bool has_min) {
    const uint32_t stat_idx = static_cast<uint32_t>(b.stats.size());
    stats::Stat s;
    s.guid = guid;
    s.flags = flags;
    s.base = add_const_program(b, base_val);
    if (has_min) {
        s.min = add_const_program(b, min_val);
    }
    b.stats.push_back(s);
    b.by_guid[guid] = stat_idx;
    b.order.push_back((stat_idx << 1u) | stats::kNodeInit);
    b.order.push_back((stat_idx << 1u) | stats::kNodeValue);
}

content::AbilityRules build_ability_rules() {
    content::AbilityRules rules;
    rules.stat_regen_time = 1.0;
    rules.regen_rate = 5.0;

    rules.sprint.guid = content::kGuidSprint;
    rules.sprint.held = content::Held::Smart;
    rules.sprint.min_stamina = 1.0;
    rules.sprint.cost_stat_guid = content::kGuidStamina;
    rules.sprint.cost_amount = 1.25;
    rules.sprint.charge_interval = 0.25;
    rules.sprint.effect_guid = 9101;

    rules.dodge.guid = content::kGuidDodge;
    rules.dodge.cost_stat_guid = content::kGuidStamina;
    rules.dodge.cost_amount = 50.0;
    rules.dodge.cooldown = 0.65;
    rules.dodge.triggers_cooldown = true;
    rules.dodge.disable_during_cooldown = true;
    rules.dodge.effect_guid = 9102;

    rules.rest.guid = content::kGuidRest;
    rules.rest.held = content::Held::Toggle;
    rules.rest.requires_out_of_combat = true;
    rules.rest.effect_guid = 9103;

    content::EffectRule sprint_eff;
    sprint_eff.guid = 9101;
    sprint_eff.infinite = true;
    sprint_eff.stops_regen_moving = true;
    sprint_eff.end_effect_guid = 9104;
    sprint_eff.end_effect_duration = 1.0;
    rules.effects[9101] = sprint_eff;

    content::EffectRule evasion_eff;
    evasion_eff.guid = 9102;
    evasion_eff.duration = 0.50;
    evasion_eff.stops_regen_standing = true;
    evasion_eff.stops_regen_moving = true;
    evasion_eff.end_effect_guid = 9104;
    evasion_eff.end_effect_duration = 1.0;
    rules.effects[9102] = evasion_eff;

    content::EffectRule rest_eff;
    rest_eff.guid = 9103;
    rest_eff.infinite = true;
    rules.effects[9103] = rest_eff;

    content::EffectRule pause_eff;
    pause_eff.guid = 9104;
    pause_eff.duration = 1.0;
    pause_eff.stops_regen_standing = true;
    pause_eff.stops_regen_moving = true;
    rules.effects[9104] = pause_eff;

    return rules;
}

struct PipelineFixture {
    static constexpr int64_t kStatCcScale = 8801;
    static constexpr int64_t kStatProcBonus = 8802;

    stats::StatBundle bundle;
    EmptyGearCatalog gear;
    content::AbilityRules rules;

    PipelineFixture() : rules(build_ability_rules()) {
        add_stat_def(bundle, content::kGuidHealth, 500.0, stats::kStatSpawnMax, 0.0, false);
        add_stat_def(bundle, content::kGuidMaxHealth, 500.0, 0, 0.0, false);
        add_stat_def(bundle, content::kGuidStamina, 100.0, stats::kStatSpawnMax, 0.0, false);
        add_stat_def(bundle, content::kGuidMaxStamina, 100.0, 0, 0.0, false);
        add_stat_def(bundle, kStatCcScale, 1.0, stats::kStatMulMods | stats::kStatMulCompl, 0.20, true);
        add_stat_def(bundle, kStatProcBonus, 10.0, 0, 0.0, false);

        stats::ClassDef cls;
        cls.enum_value = 1;
        for (uint32_t i = 0; i < static_cast<uint32_t>(bundle.stats.size()); ++i) {
            cls.members.push_back(i);
        }
        bundle.classes.push_back(std::move(cls));
    }
};

void verify_proc_icd_and_prng_boundaries() {
    combat::ProcGateState gate;
    constexpr int64_t kProcGuid = 9001;
    constexpr int64_t kIcdMs = 3000;

    CHECK(!gate.can_fire(0, kIcdMs, 1000));
    CHECK(!gate.try_fire(0, kIcdMs, 1000, 0.0, 1.0));
    CHECK(gate.can_fire(kProcGuid, kIcdMs, 1000));
    CHECK(!gate.try_fire(kProcGuid, kIcdMs, 1000, 0.25, 0.25));
    CHECK(!gate.try_fire(kProcGuid, kIcdMs, 1000, 0.50, 0.25));
    CHECK(!gate.try_fire(kProcGuid, kIcdMs, 1000, -0.01, 0.25));
    CHECK(!gate.try_fire(kProcGuid, kIcdMs, 1000, 0.0, 0.0));
    CHECK(gate.try_fire(kProcGuid, kIcdMs, 1000, 0.2499, 0.25));

    CHECK(!gate.can_fire(kProcGuid, kIcdMs, 1000));
    CHECK(!gate.can_fire(kProcGuid, kIcdMs, 3999));
    CHECK(!gate.try_fire(kProcGuid, kIcdMs, 3999, 0.0, 1.0));
    CHECK(gate.can_fire(kProcGuid, kIcdMs, 4000));

    {
        combat::ProcDepthScope active_proc(gate.proc_depth);
        CHECK(active_proc.allowed());
        CHECK(!gate.can_fire(kProcGuid, kIcdMs, 4000));
        CHECK(!gate.try_fire(kProcGuid, kIcdMs, 4000, 0.0, 1.0));
    }
    CHECK(gate.try_fire(kProcGuid, kIcdMs, 4000, 0.0, 1.0));

    stats::SplitMix rng_a{42u};
    stats::SplitMix rng_b{42u};
    for (int i = 0; i < 16; ++i) {
        const double u_a = rng_a.rand_double();
        const double u_b = rng_b.rand_double();
        CHECK_NEAR(u_a, u_b, 1e-15);
        CHECK(u_a >= 0.0 && u_a < 1.0);
    }
}

void case_proc_chain_depth_guard() {
    PipelineFixture fx;
    std::string err;

    CHECK(AbilityPipeline::kMaxApplyDepth == 4);
    CHECK(AbilityPipeline::kMaxEffects == 256u);
    CHECK(AbilityPipeline::kMaxStepsPerAdvance == 100000u);
    CHECK(stats::kMaxEffectStacks == 4096);

    stats::CharacterStats cstats(fx.bundle, fx.gear, 1, 10);
    std::vector<stats::WornItem> empty_worn;
    CHECK(cstats.start(empty_worn, err));

    std::map<int64_t, double> init_stats = {
        {content::kGuidStamina, 100.0},
        {content::kGuidMaxStamina, 100.0},
    };
    AbilityRuntime a7(fx.rules, init_stats, 0.0);
    CHECK(a7.ok());

    AbilityPipeline pipeline(nullptr, cstats, a7, [&]() { return empty_worn; }, 0.0, 77u);
    CHECK(pipeline.reactions_enabled());
    CHECK(pipeline.reactions_fired() == 0u);

    pipeline.set_reaction_carriers({6001, 6002});
    pipeline.set_reactions_enabled(false);
    CHECK(!pipeline.reactions_enabled());

    combat::IncomingHit hit;
    hit.instigator = 500;
    hit.source_type = content::kSourceTypeReaction;
    hit.source_guid = 6001;
    hit.hit_guid = 7001;
    hit.mods.push_back(combat::ResolvedMod{7101, content::kGuidHealth, -50.0});
    pipeline.receive_hit(hit, 1.0);
    CHECK(pipeline.reactions_fired() == 0u);

    const auto* hp_after_hit = cstats.find(content::kGuidHealth);
    CHECK(hp_after_hit != nullptr);
    CHECK_NEAR(hp_after_hit->value, 450.0f, 1e-4);

    combat::IncomingHit miss_hit = hit;
    miss_hit.flags = stats::kHitMiss;
    pipeline.receive_hit(miss_hit, 1.5);
    CHECK_NEAR(cstats.find(content::kGuidHealth)->value, 450.0f, 1e-4);

    pipeline.set_dead(true);
    CHECK(pipeline.dead());
    verify_proc_icd_and_prng_boundaries();
}

void verify_proc_depth_scope_guard() {
    CHECK(combat::kMaxProcChainDepth == 1);
    CHECK(combat::can_trigger_proc_at_depth(0));
    CHECK(!combat::can_trigger_proc_at_depth(1));
    CHECK(!combat::can_trigger_proc_at_depth(2));
    CHECK(!combat::can_trigger_proc_at_depth(-1));

    int depth = 0;
    {
        combat::ProcDepthScope outer(depth);
        CHECK(outer.allowed());
        CHECK(depth == 1);
        CHECK(!combat::can_trigger_proc_at_depth(depth));
        {
            combat::ProcDepthScope inner(depth);
            CHECK(!inner.allowed());
            CHECK(depth == 1);
        }
        CHECK(depth == 1);
    }
    CHECK(depth == 0);
}

void verify_four_stage_cc_dr_window() {
    CHECK(combat::kCcDrWindowMs == 15000);
    CHECK(combat::kCcDrMaxStage == 3);
    CHECK_NEAR(combat::CcDrTracker::stage_scale(0), 1.0, 1e-9);
    CHECK_NEAR(combat::CcDrTracker::stage_scale(1), 0.5, 1e-9);
    CHECK_NEAR(combat::CcDrTracker::stage_scale(2), 0.25, 1e-9);
    CHECK_NEAR(combat::CcDrTracker::stage_scale(3), 0.0, 1e-9);
    CHECK_NEAR(combat::CcDrTracker::stage_scale(4), 0.0, 1e-9);

    combat::CcDrTracker dr;
    const auto invalid = dr.apply(0, 500);
    CHECK(invalid.effective_duration_ms == 0);
    CHECK(dr.stage == 0);

    const auto s0 = dr.apply(4000, 1000);
    CHECK(s0.stage == 0 && !s0.immune);
    CHECK_NEAR(s0.scale, 1.0, 1e-9);
    CHECK(s0.effective_duration_ms == 4000);

    const auto s1 = dr.apply(4000, 5000);
    CHECK(s1.stage == 1 && !s1.immune);
    CHECK_NEAR(s1.scale, 0.5, 1e-9);
    CHECK(s1.effective_duration_ms == 2000);

    const auto s2 = dr.apply(4000, 9000);
    CHECK(s2.stage == 2 && !s2.immune);
    CHECK_NEAR(s2.scale, 0.25, 1e-9);
    CHECK(s2.effective_duration_ms == 1000);

    const auto s3 = dr.apply(4000, 12000);
    CHECK(s3.stage == 3 && s3.immune);
    CHECK_NEAR(s3.scale, 0.0, 1e-9);
    CHECK(s3.effective_duration_ms == 0);

    const auto s3_before_expiry = dr.apply(4000, 12000 + combat::kCcDrWindowMs - 1);
    CHECK(s3_before_expiry.stage == 3 && s3_before_expiry.immune);
    CHECK(s3_before_expiry.effective_duration_ms == 0);

    const auto s_reset = dr.apply(4000, s3_before_expiry.effective_duration_ms + 12000 +
                                            combat::kCcDrWindowMs - 1 + combat::kCcDrWindowMs);
    CHECK(s_reset.stage == 0 && !s_reset.immune);
    CHECK_NEAR(s_reset.scale, 1.0, 1e-9);
    CHECK(s_reset.effective_duration_ms == 4000);
}

void verify_dr_complement_scaling(const PipelineFixture& fx, stats::CharacterStats& cstats) {
    std::string err;
    std::vector<stats::WornItem> empty_worn;
    std::vector<stats::StatValue> changed;

    auto make_dr_effect = [](int stacks) {
        stats::ActiveEffect eff;
        eff.effect = {stats::kRefTypeEffect, 4001};
        eff.stacks = stacks;
        eff.tags = {"CC.Stun.Diminishing"};
        eff.mods = {{PipelineFixture::kStatCcScale, 0.50}};
        return eff;
    };

    cstats.set_effects({make_dr_effect(1)});
    CHECK(cstats.update(empty_worn, changed, err));
    CHECK_NEAR(cstats.find(PipelineFixture::kStatCcScale)->value, 0.50f, 1e-5);

    cstats.set_effects({make_dr_effect(2)});
    CHECK(cstats.update(empty_worn, changed, err));
    CHECK_NEAR(cstats.find(PipelineFixture::kStatCcScale)->value, 0.25f, 1e-5);

    cstats.set_effects({make_dr_effect(3)});
    CHECK(cstats.update(empty_worn, changed, err));
    CHECK_NEAR(cstats.find(PipelineFixture::kStatCcScale)->value, 0.20f, 1e-5);

    auto eval = cstats.evaluation(empty_worn, err);
    CHECK(eval && eval->ok());
    CHECK(eval->inputs().effects.size() == 1u);
    CHECK(eval->inputs().effects[0].effect == (stats::RecordRef{stats::kRefTypeEffect, 4001}));
    CHECK(eval->inputs().effects[0].stacks == 3);
    CHECK(stats::tag_matches(eval->inputs().effects[0].tags[0], "CC.Stun"));
}

void case_cc_diminishing_returns_clock() {
    PipelineFixture fx;
    std::string err;
    stats::CharacterStats cstats(fx.bundle, fx.gear, 1, 10);
    std::vector<stats::WornItem> empty_worn;
    CHECK(cstats.start(empty_worn, err));

    verify_proc_icd_and_prng_boundaries();
    verify_four_stage_cc_dr_window();
    verify_dr_complement_scaling(fx, cstats);

    std::map<int64_t, double> init_stats = {
        {content::kGuidStamina, 100.0},
        {content::kGuidMaxStamina, 100.0},
    };
    AbilityRuntime a7(fx.rules, init_stats, 0.0);
    CHECK(a7.ok());

    CHECK(a7.trigger(content::kGuidDodge, 10, 1.0) == Refusal::None);
    CHECK(a7.evading());
    CHECK(a7.regeneration_blocked(1.1));
    CHECK_NEAR(a7.stamina(), 50.0, 1e-5);
    CHECK_NEAR(a7.dodge_cooldown_until(), 1.65, 1e-5);

    CHECK(a7.trigger(content::kGuidDodge, 11, 1.3) == Refusal::Cooldown);
    CHECK(a7.refused() == 1u);

    a7.tick(1.55);
    CHECK(!a7.evading());
    CHECK(a7.regeneration_blocked(1.55));
    CHECK_NEAR(a7.regen_pause_until(), 2.50, 1e-5);

    a7.tick(2.60);
    CHECK(!a7.regeneration_blocked(2.60));
    CHECK(a7.trigger(content::kGuidDodge, 12, 2.60) == Refusal::None);
    CHECK(a7.evading());
    CHECK_NEAR(a7.stamina(), 0.0, 1e-5);

    a7.tick(3.50);
    CHECK(a7.trigger(content::kGuidDodge, 13, 3.50) == Refusal::Stamina);
}

}  // namespace

int main(int argc, char** argv) {
    const char* selected_case = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            selected_case = argv[++i];
        }
    }

    if (!selected_case || std::strcmp(selected_case, "proc_chain_depth_guard") == 0) {
        case_proc_chain_depth_guard();
        verify_proc_depth_scope_guard();
    }
    if (!selected_case || std::strcmp(selected_case, "cc_diminishing_returns_clock") == 0) {
        case_cc_diminishing_returns_clock();
    }
    if (selected_case && std::strcmp(selected_case, "proc_chain_depth_guard") != 0 &&
        std::strcmp(selected_case, "cc_diminishing_returns_clock") != 0) {
        std::fprintf(stderr, "Unknown --case %s\n", selected_case);
        return 2;
    }

    std::printf("[PASS] test_combat_procs_and_cc\n");
    return 0;
}
