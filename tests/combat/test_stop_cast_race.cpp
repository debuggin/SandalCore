// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "combat/npc_ability_replicator.h"
#include "protocol/schema/generated/UAoCAbilityComponent.h"
#include "session/abilities.h"
#include "session/ability_pipeline.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace aoc::worldd;
namespace repl = ::aoc::protocol::repl;
namespace emit = ::aoc::protocol::emit;
namespace schema = ::aoc::protocol::schema;

void check_true(bool cond, const char* expr, int line) {
    if (!cond) {
        std::fprintf(stderr, "FAIL(%d): %s\n", line, expr);
        std::exit(1);
    }
}

#define ASSERT_TRUE(c) check_true((c), #c, __LINE__)

class EmptyGearCatalog final : public stats::GearCatalog {
public:
    const stats::ItemGear* item(int64_t) const override { return nullptr; }
    const stats::SetBonus* set(int64_t) const override { return nullptr; }
    std::string slot_name(int) const override { return ""; }
};

void seed_npc_actor(world::ActorRegistry& reg, uint64_t actor_id) {
    world::WorldActor actor{};
    actor.id = actor_id;
    actor.kind = world::ActorKind::Npc;
    actor.actor.guid = emit::FIntrepidNetworkGUID::from_u64(actor_id);

    repl::Subobject sub{};
    sub.name = "AbilityComponent";
    sub.obj.schema = &schema::gen::kUAoCAbilityComponent;
    actor.actor.subobjects.push_back(std::move(sub));
    ASSERT_TRUE(reg.add(std::move(actor)));
}

content::AbilityRules build_sprint_rest_rules() {
    content::AbilityRules rules{};
    rules.stat_regen_time = 1.0;
    rules.regen_rate = 5.0;

    rules.sprint.guid = content::kGuidSprint;
    rules.sprint.held = content::Held::Smart;
    rules.sprint.min_stamina = 1.0;
    rules.sprint.cost_stat_guid = content::kGuidStamina;
    rules.sprint.cost_amount = 1.25;
    rules.sprint.charge_interval = 0.25;
    rules.sprint.effect_guid = 9101;

    content::EffectRule sprint_eff{};
    sprint_eff.guid = 9101;
    sprint_eff.infinite = true;
    sprint_eff.stops_regen_moving = true;
    rules.effects[9101] = sprint_eff;

    rules.rest.guid = content::kGuidRest;
    rules.rest.held = content::Held::Toggle;
    rules.rest.requires_out_of_combat = true;
    rules.rest.effect_guid = 9103;

    content::EffectRule rest_eff{};
    rest_eff.guid = 9103;
    rest_eff.infinite = true;
    rules.effects[9103] = rest_eff;
    return rules;
}

void verify_npc_replicator_start_and_stop_cast() {
    world::ActorRegistry reg;
    constexpr uint64_t kNpcId = 5000u;
    seed_npc_actor(reg, kNpcId);

    combat::NpcCastState state{};
    combat::NpcCast cast{};
    cast.ability = 77001;
    cast.target = 8000u;
    cast.montage = 3301u;
    cast.play_rate = 1.25f;

    repl::ExportChain chain{};
    ASSERT_TRUE(combat::start_npc_cast(reg, kNpcId, state, cast, chain));
    ASSERT_TRUE(state.playing);
    ASSERT_TRUE(state.element == 1);
    ASSERT_TRUE(state.counter == 1);
    ASSERT_TRUE(state.next_instance == 1);

    const auto* live = reg.find(kNpcId);
    ASSERT_TRUE(live != nullptr);
    ASSERT_TRUE(!live->ability_exports.empty());

    ASSERT_TRUE(combat::end_npc_cast(reg, kNpcId, state));
    ASSERT_TRUE(!state.playing);
    ASSERT_TRUE(state.counter == 2);
    ASSERT_TRUE(live->ability_exports.empty());
    ASSERT_TRUE(!combat::end_npc_cast(reg, kNpcId, state));
}

void verify_pipeline_death_and_mount_interrupt_race() {
    stats::StatBundle bundle{};
    EmptyGearCatalog gear{};
    stats::ClassDef cls{};
    cls.enum_value = 1;
    bundle.classes.push_back(cls);

    stats::CharacterStats cstats(bundle, gear, 1, 10);
    std::vector<stats::WornItem> empty_worn;
    std::string err;
    ASSERT_TRUE(cstats.start(empty_worn, err));

    const content::AbilityRules rules = build_sprint_rest_rules();
    std::map<int64_t, double> init_stats = {
        {content::kGuidStamina, 100.0},
        {content::kGuidMaxStamina, 100.0},
    };
    AbilityRuntime a7(rules, init_stats, 0.0);
    ASSERT_TRUE(a7.ok());

    AbilityPipeline pipe(nullptr, cstats, a7, [&]() { return empty_worn; }, 0.0, 42u);
    ASSERT_TRUE(a7.trigger(content::kGuidSprint, 41, 1.0) == Refusal::None);
    ASSERT_TRUE(a7.sprinting());

    a7.set_mounted(true, 1.2);
    ASSERT_TRUE(a7.mounted());
    ASSERT_TRUE(!a7.sprinting());
    ASSERT_TRUE(a7.trigger(content::kGuidSprint, 42, 1.3) == Refusal::Mounted);

    pipe.set_dead(true);
    ASSERT_TRUE(pipe.dead());
    ASSERT_TRUE(pipe.trigger(9001, 43, 1.5) == Refusal::Dead);
}

void verify_generation_token_stale_stop_immunity() {
    stats::StatBundle bundle{};
    EmptyGearCatalog gear{};
    stats::ClassDef cls{};
    cls.enum_value = 1;
    bundle.classes.push_back(cls);

    stats::CharacterStats cstats(bundle, gear, 1, 10);
    std::vector<stats::WornItem> empty_worn;
    std::string err;
    ASSERT_TRUE(cstats.start(empty_worn, err));

    const content::AbilityRules rules = build_sprint_rest_rules();
    std::map<int64_t, double> init_stats = {
        {content::kGuidStamina, 100.0},
        {content::kGuidMaxStamina, 100.0},
    };
    AbilityRuntime a7(rules, init_stats, 0.0);
    ASSERT_TRUE(a7.ok());
    AbilityPipeline pipe(nullptr, cstats, a7, [&]() { return empty_worn; }, 0.0, 99u);

    ASSERT_TRUE(a7.trigger(content::kGuidSprint, 100, 5.0) == Refusal::None);
    const auto token_a = a7.instance_of(100);
    ASSERT_TRUE(token_a.has_value());
    ASSERT_TRUE(a7.cancel(100, 5.5));
    ASSERT_TRUE(!a7.sprinting());

    ASSERT_TRUE(a7.trigger(content::kGuidSprint, 101, 6.0) == Refusal::None);
    const auto token_b = a7.instance_of(101);
    ASSERT_TRUE(token_b.has_value());
    ASSERT_TRUE(*token_b > *token_a);

    ASSERT_TRUE(!a7.cancel(100, 6.2));
    ASSERT_TRUE(!pipe.end_request(static_cast<int32_t>(*token_a), 6.2));
    ASSERT_TRUE(a7.sprinting());
    ASSERT_TRUE(a7.cancel(101, 6.5));
    ASSERT_TRUE(!a7.sprinting());
}

void verify_npc_cast_overlap_replaces_prior_element() {
    world::ActorRegistry reg;
    constexpr uint64_t kNpcId = 6002u;
    seed_npc_actor(reg, kNpcId);

    combat::NpcCastState state{};
    combat::NpcCast cast1{};
    cast1.ability = 77001;
    cast1.montage = 3301u;
    repl::ExportChain chain{};

    ASSERT_TRUE(combat::start_npc_cast(reg, kNpcId, state, cast1, chain));
    const int32_t first_elem = state.element;
    const int64_t first_gen = state.next_instance;

    combat::NpcCast cast2{};
    cast2.ability = 77002;
    cast2.montage = 3302u;
    ASSERT_TRUE(combat::start_npc_cast(reg, kNpcId, state, cast2, chain));
    ASSERT_TRUE(state.element > first_elem);
    ASSERT_TRUE(state.next_instance == first_gen + 1);

    world::WorldActor* live = reg.find(kNpcId);
    ASSERT_TRUE(live != nullptr);
    repl::FastArrayState* arr = combat::npc_ability_detail::active_abilities(*live);
    ASSERT_TRUE(arr != nullptr);
    ASSERT_TRUE(arr->find(first_elem) == nullptr);
    ASSERT_TRUE(arr->find(state.element) != nullptr);
}

void test_mob_stop_cast_target_death_and_cc_race() {
    verify_npc_replicator_start_and_stop_cast();
    verify_pipeline_death_and_mount_interrupt_race();
}

void test_mob_stop_cast_generation_token_immunity() {
    verify_generation_token_stale_stop_immunity();
    verify_npc_cast_overlap_replaces_prior_element();
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view selected_case;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            selected_case = argv[++i];
        }
    }

    if (selected_case.empty() || selected_case == "mob_stop_cast_target_death_and_cc_race") {
        test_mob_stop_cast_target_death_and_cc_race();
    }
    if (selected_case.empty() || selected_case == "mob_stop_cast_generation_token_immunity") {
        test_mob_stop_cast_generation_token_immunity();
    }
    return 0;
}
