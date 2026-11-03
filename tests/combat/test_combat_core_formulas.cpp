// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "combat/hit_resolver.h"
#include "combat/npc_ability_data.h"
#include "combat/npc_ability_replicator.h"
#include "combat/npc_kits.h"
#include "protocol/schema/generated/UAoCAbilityComponent.h"
#include "session/ability_catalog.h"
#include "stats/damage.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace aoc::worldd;
namespace repl = ::aoc::protocol::repl;
namespace emit = ::aoc::protocol::emit;
namespace schema = ::aoc::protocol::schema;

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

void add_stat_def(stats::StatBundle& b, int64_t guid, const char* name, double base_val) {
    const uint32_t stat_idx = static_cast<uint32_t>(b.stats.size());
    const uint32_t name_idx = static_cast<uint32_t>(b.strings.size());
    b.strings.emplace_back(name);
    stats::Stat s;
    s.guid = guid;
    s.name = name_idx;
    s.base = add_const_program(b, base_val);
    b.stats.push_back(s);
    b.by_guid[guid] = stat_idx;
    b.order.push_back((stat_idx << 1u) | stats::kNodeInit);
    b.order.push_back((stat_idx << 1u) | stats::kNodeValue);
}

struct CombatFixture {
    stats::StatBundle bundle;
    EmptyGearCatalog gear;
    content::CombatRows rows;

    CombatFixture() {
        const double profile_defaults[21] = {
            1.10,  0.05, 1.20, 0.10, 0.00, 120.0, 1.25, 0.15, 0.70, 0.12, 1.75,
            0.80,  0.50, 95.0, 1.15, 0.08, 0.78,  0.08, 1.60, 0.85, 0.60,
        };
        for (size_t i = 0; i < 21; ++i) {
            const int64_t guid = static_cast<int64_t>(5000 + i);
            add_stat_def(bundle, guid, stats::kCombatProfileKeys[i], profile_defaults[i]);
            rows.profile[stats::kCombatProfileKeys[i]] = guid;
        }
        add_stat_def(bundle, 5101, "PhysicalCriticalChance", 0.25);
        add_stat_def(bundle, 5102, "MagicalCriticalChance", 0.15);
        add_stat_def(bundle, 5103, "PhysicalCriticalAvoidance", 0.06);
        add_stat_def(bundle, 5104, "MagicalCriticalAvoidance", 0.03);
        add_stat_def(bundle, 5105, "PhysicalAccuracy", 0.10);
        add_stat_def(bundle, 5106, "PhysicalEvasion", 0.28);
        add_stat_def(bundle, 5107, "PhysicalBlockChance", 0.18);

        stats::ClassDef cls;
        cls.enum_value = 1;
        for (uint32_t i = 0; i < static_cast<uint32_t>(bundle.stats.size()); ++i) {
            cls.members.push_back(i);
        }
        bundle.classes.push_back(std::move(cls));
    }
};

void verify_damage_profile_math(const stats::DamageProfile& prof, const stats::StatView& instigator,
                                const stats::StatView& target) {
    stats::DamageHit phys_hit;
    phys_hit.tag = "Element.Physical.Slash";
    phys_hit.physical = true;
    phys_hit.flags = stats::kHitCrit;

    CHECK_NEAR(prof.calculate_damage(instigator, phys_hit), 120.0, 1e-5);
    CHECK_NEAR(prof.calculate_damage_multiplier(instigator, phys_hit), 1.10 * 1.20 * 1.25, 1e-5);
    CHECK_NEAR(prof.calculate_damage_additive_multiplier(instigator, phys_hit), 1.0 + 0.05 + 0.10 + 0.15, 1e-5);
    CHECK_NEAR(prof.calculate_damage_critical(instigator, phys_hit), 1.75, 1e-5);
    CHECK_NEAR(prof.calculate_damage_penetration(instigator, phys_hit), 0.88, 1e-5);
    CHECK_NEAR(prof.calculate_damage_mitigation(target, phys_hit), 1.0 - (0.80 * 0.70), 1e-5);
    CHECK_NEAR(prof.calculate_damage_dampening(instigator, target, phys_hit), 1.0, 1e-5);

    CHECK_NEAR(prof.calculate(stats::kHostCalcDamage, instigator, target, phys_hit), 120.0, 1e-5);
    CHECK_NEAR(prof.calculate(stats::kHostCalcLast + 1, instigator, target, phys_hit), 0.0, 1e-5);

    stats::DamageHit mag_hit;
    mag_hit.tag = "Element.Magical.Fire";
    mag_hit.physical = false;
    mag_hit.flags = 0;
    CHECK_NEAR(prof.calculate_damage(instigator, mag_hit), 95.0, 1e-5);
    CHECK_NEAR(prof.calculate_damage_critical(instigator, mag_hit), 1.0, 1e-5);
    CHECK_NEAR(prof.calculate_damage_mitigation(target, mag_hit), 0.22, 1e-5);
}

void verify_hit_context_and_host(const combat::CombatData& data, const stats::StatView& instigator,
                                 const stats::StatView& target) {
    CHECK(data.ids().physical_crit_chance == 5101);
    CHECK(data.ids().physical_accuracy == 5105);
    CHECK(data.ids().physical_evasion == 5106);
    CHECK(data.ids().physical_block_chance == 5107);
    CHECK_NEAR(data.in_combat_timer(), 6.0, 1e-6);

    combat::HitContext ctx;
    ctx.profile = &data.profile();
    ctx.hit = {"Element.Physical.Slash", true, stats::kHitCrit};
    ctx.tags = {"Element.Physical.Slash", "ability.Melee"};
    ctx.instigator = &instigator;
    ctx.target = &target;
    ctx.targets = 3.0;
    ctx.distance_cm = 250.0;
    ctx.enemy = true;

    CHECK(ctx.has_tag("Element.Physical"));
    CHECK(!ctx.has_tag("Element.Magical"));
    CHECK(ctx.has_flag("Crit"));
    CHECK(!ctx.has_flag("Miss"));
    CHECK(combat::aspect_of(ctx.tags).physical);
    CHECK(!combat::aspect_of({"Element.Magical.Fire"}).physical);

    combat::ContextHost host;
    host.set_context(&ctx);
    CHECK_NEAR(host.hit_targets(), 3.0, 1e-6);
    const std::string caster_target = std::to_string(stats::kRoleCaster) + "," + std::to_string(stats::kRoleTarget);
    CHECK_NEAR(host.host_fn(stats::kHostCalcDamage, "", nullptr, 0), 120.0, 1e-5);
    CHECK_NEAR(host.host_fn(stats::kHostIsEnemy, caster_target, nullptr, 0), 1.0, 1e-5);
    CHECK_NEAR(host.host_fn(stats::kHostDistanceBetween, caster_target, nullptr, 0), 250.0, 1e-5);
}

void case_mitigation_and_crit_scaling() {
    CombatFixture fx;
    std::string err;

    content::CombatRows incomplete_rows = fx.rows;
    incomplete_rows.profile.erase("Physical.Mitigation");
    CHECK(!combat::CombatData::build(incomplete_rows, fx.bundle, err).has_value());
    CHECK(!err.empty());

    err.clear();
    auto data = combat::CombatData::build(fx.rows, fx.bundle, err);
    CHECK(data.has_value());

    stats::CharacterStats attacker_stats(fx.bundle, fx.gear, 1, 10);
    stats::CharacterStats target_stats(fx.bundle, fx.gear, 1, 10);
    std::vector<stats::WornItem> empty_worn;
    CHECK(attacker_stats.start(empty_worn, err));
    CHECK(target_stats.start(empty_worn, err));

    auto attacker_eval = attacker_stats.evaluation(empty_worn, err);
    auto target_eval = target_stats.evaluation(empty_worn, err);
    CHECK(attacker_eval && attacker_eval->ok());
    CHECK(target_eval && target_eval->ok());

    const combat::EvalView instigator_view(*attacker_eval);
    const combat::EvalView target_view(*target_eval);
    verify_damage_profile_math(data->profile(), instigator_view, target_view);
    verify_hit_context_and_host(*data, instigator_view, target_view);
}

world::WorldActor make_npc_with_loaded_records(const std::vector<int64_t>& ability_guids) {
    world::WorldActor npc;
    npc.id = 200;
    npc.actor.guid = emit::FIntrepidNetworkGUID::from_u64(200);

    repl::Subobject sub;
    sub.name = "AbilityComponent";
    sub.obj.schema = &schema::gen::kUAoCAbilityComponent;

    repl::ArrayValue list;
    for (int64_t g : ability_guids) {
        repl::ElementState elem;
        elem.values[1] = repl::Value::i64(g);
        elem.values[2] = repl::Value::i64(0);
        elem.values[3] = repl::Value(repl::NameValue::plain("Ability"));
        list.elements.push_back(std::move(elem));
    }
    const bool ok = sub.obj.state.set(schema::gen::kUAoCAbilityComponent, "ReplicatedRecordsToLoad",
                                      repl::Value(std::move(list)));
    CHECK(ok);
    npc.actor.subobjects.push_back(std::move(sub));
    return npc;
}

void case_npc_ability_known_table_standin() {
    CombatFixture fx;
    std::string err;
    const auto missing_cat = abilities::Catalog::load("nonexistent_content_file.db", fx.bundle, err);
    CHECK(!missing_cat.has_value());
    CHECK(!err.empty());

    const world::WorldActor npc = make_npc_with_loaded_records({700101, 700102});
    CHECK(combat::npc_knows_ability(npc, 700101));
    CHECK(combat::npc_knows_ability(npc, 700102));
    CHECK(!combat::npc_knows_ability(npc, 700999));

    const world::WorldActor empty_npc;
    CHECK(!combat::npc_knows_ability(empty_npc, 700101));

    CHECK(combat::npc_kits::kKitCount == 123u);
    CHECK(combat::npc_kits::kKits[0].count > 0u);
    CHECK(combat::npc_kits::kKits[0].abilities[0].windup_ms > 0);
    CHECK(combat::npc_kits::kKits[0].abilities[0].range_cm > 0);

    combat::MontageData montage;
    montage.guid = 8801;
    montage.length = 2.4;
    montage.rate_scale = 1.2;
    montage.anim_end = 1.8;
    CHECK_NEAR(montage.seconds(1.0), 2.0, 1e-6);
    CHECK_NEAR(montage.task_done_after(1.0), 1.5, 1e-6);
    CHECK_NEAR(montage.task_done_after(0.0), 0.0, 1e-6);
}

}  // namespace

int main(int argc, char** argv) {
    const char* selected_case = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            selected_case = argv[++i];
        }
    }

    if (!selected_case || std::strcmp(selected_case, "mitigation_and_crit_scaling") == 0) {
        case_mitigation_and_crit_scaling();
    }
    if (!selected_case || std::strcmp(selected_case, "npc_ability_known_table_standin") == 0) {
        case_npc_ability_known_table_standin();
    }
    if (selected_case && std::strcmp(selected_case, "mitigation_and_crit_scaling") != 0 &&
        std::strcmp(selected_case, "npc_ability_known_table_standin") != 0) {
        std::fprintf(stderr, "Unknown --case %s\n", selected_case);
        return 2;
    }

    std::printf("[PASS] test_combat_core_formulas\n");
    return 0;
}
