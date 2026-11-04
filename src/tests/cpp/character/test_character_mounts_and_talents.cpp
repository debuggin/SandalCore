// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "content/content_types_world.h"
#include "protocol/repl/skill_rpc.h"
#include "session/mounts.h"
#include "session/talents.h"
#include "spawns/captured_spawns.h"
#include "spawns/mount_spawn.h"
#include "world/actor_registry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace aoc::worldd;

constexpr int64_t kItemWolfMount = 8100001;
constexpr int64_t kSummonAbility = 8200001;
constexpr int64_t kDashAbility = 8200002;

spawns::MountBook build_mount_book() {
    content::MountData md;
    content::MountRow row;
    row.item = kItemWolfMount;
    row.item_name = "Swiftclaw";
    row.kind = "ground";
    row.blueprint_path = "/Game/Characters/Mounts/Mount_Ground_TerrorBird_1.Mount_Ground_TerrorBird_1_C";
    row.blueprint = "Mount_Ground_TerrorBird_1";
    row.ground_pct = 150;
    row.health_pct = 100;
    md.mounts.push_back(row);
    md.stats.push_back({2, content::kGuidMaxHealth, "Mount_Base_HP", 500.0});
    md.abilities.push_back({kSummonAbility, "Summon_Mount", "summon", false, false});
    md.abilities.push_back({kDashAbility, "MountAbility_Sprint", "dash", true, false});

    std::string err;
    auto donors = spawns::MountDonors::load(err);
    spawns::MountBook book(std::move(md), donors ? std::move(*donors) : spawns::MountDonors{});
    book.set_speeds("Mount_Ground_TerrorBird_1", {200.0, 950.0, 0.0, 300.0});
    return book;
}

MountController::Request make_mount_request(int64_t lock_exp, int64_t now_unix, bool dead) {
    MountController::Request req;
    req.item_record = kItemWolfMount;
    req.item_guid = {101, 202, 303, 404};
    req.client_instance = 7;
    req.lock_expiration = lock_exp;
    req.now_unix = now_unix;
    req.place = {1000.0, 2000.0, 300.0, 45.0};
    req.owner_level = 2;
    req.dead = dead;
    req.world_time = 10.0;
    return req;
}

bool test_mount_refusals(MountController& mc) {
    auto dead_req = make_mount_request(0, 1000, true);
    if (mc.summon(dead_req, 1.0) != MountController::Outcome::Refused) return false;
    (void)mc.advance(3.0);

    auto locked_req = make_mount_request(1050, 1000, false);
    if (mc.summon(locked_req, 3.0) != MountController::Outcome::Refused) return false;
    const auto eff = mc.advance(3.2);
    if (eff.callouts.empty() || mc.stats().refused != 2) return false;
    (void)mc.advance(10.0);
    return true;
}

bool test_mount_lifecycle(MountController& mc, world::ActorRegistry& reg) {
    auto req = make_mount_request(0, 1000, false);
    if (mc.summon(req, 10.0) != MountController::Outcome::Started) return false;
    if (mc.phase() != MountController::Phase::Summoning || mc.actor_id() != 0) return false;

    mc.on_damage(10.05);
    if (mc.phase() != MountController::Phase::Idle || mc.stats().cancelled != 1) return false;

    if (mc.summon(req, 20.0) != MountController::Outcome::Started) return false;
    const auto open_eff = mc.advance(20.2);
    if (!open_eff.bar || open_eff.bar_ability != kDashAbility || !reg.find(mc.actor_id())) {
        return false;
    }
    (void)mc.advance(23.0);
    if (mc.phase() != MountController::Phase::Out || !mc.mount()) return false;

    if (!mc.desummon(23.5) || mc.phase() != MountController::Phase::Desummoning) return false;
    (void)mc.advance(28.0);
    if (mc.phase() != MountController::Phase::Idle || mc.actor_id() != 0) return false;

    if (mc.summon(req, 30.0) != MountController::Outcome::Started) return false;
    (void)mc.advance(33.0);
    if (!mc.die(33.5, 2000)) return false;
    const auto die_eff = mc.advance(33.5);
    if (!die_eff.persist || !die_eff.persist->died ||
        die_eff.persist->lock_expiration != 2000 + MountController::kLockSeconds) {
        return false;
    }
    (void)mc.advance(40.0);
    return mc.phase() == MountController::Phase::Idle && mc.stats().deaths == 1;
}

int run_mount_cooldown_manual_clock() {
    world::ActorRegistry reg;
    const spawns::MountBook book = build_mount_book();
    uint64_t next_guid = 50000;
    MountController::Owner owner;
    owner.player_id = 1;
    owner.guid = {11, 22, 33, 44};
    owner.name = "Tester";
    owner.pawn = aoc::protocol::emit::FIntrepidNetworkGUID{100};
    owner.server_id = 1;

    MountController mc(reg, book, [&]() { return aoc::protocol::emit::FIntrepidNetworkGUID{next_guid += 2}; }, owner);
    mc.set_ground([](double, double, double z_hint) -> std::optional<double> { return z_hint; });
    if (!test_mount_refusals(mc)) return 1;
    if (!test_mount_lifecycle(mc, reg)) return 1;
    return 0;
}

constexpr int64_t kPtArch = 61001;
constexpr int64_t kPtStam = 61002;
constexpr int64_t kPtWeap = 61003;
constexpr int64_t kTreeUniv = 62001;
constexpr int64_t kTreeFighter = 62002;
constexpr int64_t kClassFighter = 63001;
constexpr int64_t kLvlArch = 64001;
constexpr int64_t kNodeRoot = 65001;
constexpr int64_t kNodeStrike = 65002;
constexpr int64_t kNodeCleave = 65003;
constexpr int64_t kRankRoot = 66001;
constexpr int64_t kRankStrike1 = 66002;
constexpr int64_t kRankStrike2 = 66003;
constexpr int64_t kRankCleave1 = 66004;
constexpr int64_t kAbilRoot = 67001;
constexpr int64_t kAbilStrike1 = 67002;
constexpr int64_t kAbilStrike2 = 67003;
constexpr int64_t kAbilCleave = 67004;
constexpr int64_t kStatPower = 68001;

content::TalentData build_talent_data() {
    content::TalentData td;
    td.point_types[content::kPointArchetype] = kPtArch;
    td.point_types[content::kPointStamina] = kPtStam;
    td.point_types[content::kPointWeapon] = kPtWeap;
    td.points[kLvlArch][3][kPtArch] = 3;

    content::SkillTreeRow univ;
    univ.guid = kTreeUniv;
    univ.name = "Universal";
    univ.kind = "universal";
    td.trees.push_back(univ);

    content::SkillTreeRow fighter;
    fighter.guid = kTreeFighter;
    fighter.name = "Fighter";
    fighter.kind = "class";
    fighter.leveling_type_guid = kLvlArch;
    fighter.class_guid = kClassFighter;

    content::SkillNodeRow n0;
    n0.node = kNodeRoot;
    n0.skill = 7001;
    n0.auto_granted = true;
    n0.level_requirement = 1;
    n0.ranks.push_back({kRankRoot, 0, "Root", kAbilRoot, "RootAbil", 0, "", false, 0.0, {}, {}});

    content::SkillNodeRow n1;
    n1.node = kNodeStrike;
    n1.skill = 7002;
    n1.level_requirement = 1;
    n1.ranks.push_back({kRankStrike1, 0, "S1", kAbilStrike1, "S1", 0, "", false, 0.0,
                        {{kPtArch, 1}}, {{kStatPower, "Power", 10.0}}});
    n1.ranks.push_back({kRankStrike2, 1, "S2", kAbilStrike2, "S2", 0, "", false, 0.0,
                        {{kPtArch, 1}}, {{kStatPower, "Power", 15.0}}});

    content::SkillNodeRow n2;
    n2.node = kNodeCleave;
    n2.skill = 7003;
    n2.level_requirement = 2;
    n2.point_requirement = 2;
    n2.requires_nodes = {kNodeStrike};
    n2.ranks.push_back({kRankCleave1, 0, "C1", kAbilCleave, "C1", 0, "", false, 0.0,
                        {{kPtArch, 1}}, {}});

    fighter.nodes = {n0, n1, n2};
    td.trees.push_back(fighter);
    return td;
}

aoc::protocol::repl::SkillRankChange make_rank_change(int64_t rank, bool purchase) {
    aoc::protocol::repl::SkillRankChange c;
    c.rank = {rank, content::kTypeSkillRank};
    c.tree = {kTreeFighter, content::kTypeSkillTree};
    c.leveling_type = {kLvlArch, 0};
    c.purchase = purchase;
    return c;
}

int run_talent_tree_rank_allocation() {
    std::string err;
    const auto cat = talents::Catalog::from(build_talent_data(), err);
    if (!cat) return 1;
    const auto* tree = cat->class_tree(kClassFighter);
    if (!tree) return 1;

    talents::Book book(*cat, kClassFighter, 3);
    if (book.points_left(*tree, kPtArch) != 3) return 1;

    aoc::protocol::repl::SkillBuild bad_prereq;
    bad_prereq.changes = {make_rank_change(kRankCleave1, true)};
    if (book.plan(bad_prereq).ok) return 1;

    aoc::protocol::repl::SkillBuild valid_build;
    valid_build.changes = {make_rank_change(kRankStrike1, true),
                           make_rank_change(kRankStrike2, true),
                           make_rank_change(kRankCleave1, true)};
    const auto p1 = book.plan(valid_build);
    if (!p1.ok) return 1;
    book.apply(p1);
    if (book.points_left(*tree, kPtArch) != 0) return 1;

    const auto passives = book.passives(0);
    if (passives.size() != 2 || passives[1].mods.empty() || passives[1].mods[0].value != 15.0) {
        return 1;
    }

    aoc::protocol::repl::SkillBuild respec_empty;
    respec_empty.changes = {make_rank_change(kRankCleave1, false),
                            make_rank_change(kRankStrike2, false),
                            make_rank_change(kRankStrike1, false)};
    const auto p_reset = book.plan(respec_empty);
    if (!p_reset.ok) return 1;
    book.apply(p_reset);
    return book.points_left(*tree, kPtArch) == 3 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const char* case_name = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            case_name = argv[++i];
        }
    }
    if (!case_name) {
        if (run_mount_cooldown_manual_clock() != 0) return 1;
        if (run_talent_tree_rank_allocation() != 0) return 1;
        return 0;
    }
    if (std::strcmp(case_name, "mount_cooldown_manual_clock") == 0) {
        return run_mount_cooldown_manual_clock();
    }
    if (std::strcmp(case_name, "talent_tree_rank_allocation") == 0) {
        return run_talent_tree_rank_allocation();
    }
    std::fprintf(stderr, "Unknown --case %s\n", case_name);
    return 2;
}
