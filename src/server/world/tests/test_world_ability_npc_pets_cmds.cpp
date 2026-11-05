// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_ability_npc_common.h"

namespace {

struct PetGmEnv {
    Rig               r{6, 25};
    combat_rig::World w;
    std::string       wolf_path;
    double            now = 1.0;
    combat::CombatWorld* cw = nullptr;
    combat::Aftermath*   after = nullptr;

    void run(double seconds) {
        for (double t = 0; t < seconds; t += 0.1) {
            now += 0.1;
            r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));
            r.take();
            w.npcs->note_player(77, 0, 0, now);
            w.npcs->tick(now);
            cw->tick(now);
        }
    }
};

void verify_pet_gm_kill_and_fighting_despawn(PetGmEnv& env, uint64_t other) {
    const pets::Definition& guardian = pets::kDefinitions[0];
    double seconds = 0;
    const uint64_t pet2 = env.w.spawn(guardian.archetype, 200, 100, 25, 1.0);
    CHECK(pet2 && env.cw->pet_summoned(pet2, 77) && port_of_actor(*env.cw, pet2), "(a second pet)");
    CHECK(env.after->despawn(other, env.now, true, &seconds), "an ordinary creature is taken out of the world");
    CHECK(env.cw->is_pet(pet2) && port_of_actor(*env.cw, pet2) && env.cw->pet_of(77) == pet2, "... and the pet is still on its owner's side");
    env.run(0.5);

    std::string error;
    CHECK(env.cw->gm_kill(pet2, env.now, error), "the game master's kill takes the pet's Health");
    CHECK(env.cw->dead(pet2) && env.after->corpse_of(pet2) == nullptr, "the pet is dead and leaves no corpse");
    env.run(2.0);
    CHECK(env.cw->is_pet(pet2) && env.cw->dead(pet2), "(until its session sees it dead, it stands in the world's tables)");
    env.cw->pet_gone(pet2);
    CHECK(!env.cw->is_pet(pet2) && !port_of_actor(*env.cw, pet2) && env.cw->pet_of(77) == 0, "the session's dismissal takes its port");
    env.run(1.0);

    const uint64_t prey = env.w.spawn(env.wolf_path, 1500, 300, 25, 0.0);
    env.cw->fighter_of(prey);
    CHECK(env.cw->gm_kill(prey, env.now, error) && env.after->corpse_of(prey) != nullptr, "an ordinary creature a game master kills still leaves its corpse");
    env.run(0.5);

    const uint64_t pet3 = env.w.spawn(guardian.archetype, 200, -100, 25, 1.0);
    const uint64_t enemy = env.w.spawn(env.wolf_path, 600, -100, 25, 1.0);
    CHECK(pet3 && enemy && env.cw->pet_summoned(pet3, 77) && env.cw->pet_attack(pet3, 77, guardian.attack, enemy) && env.cw->pet_fighting(pet3), "(a pet that fights an enemy)");
    CHECK(env.after->despawn(pet3, env.now, true, &seconds), "the game master takes the fighting pet out of the world");
    CHECK(!env.cw->pet_fighting(pet3) && !env.cw->is_pet(pet3) && !port_of_actor(*env.cw, pet3), "its fight is over and its port is gone");
    env.run(3.0);
    CHECK(env.cw->health_of(enemy) > 0.0, "the enemy it fought lives on, and the world ticks on");
}

void test_pet_despawn_and_kill() {
    PetTestData shared;
    if (!shared.ok) return;
    std::string error;
    std::printf("a game master takes a pet out of the world, or kills it\n");
    PetGmEnv env;
    env.wolf_path = shared.wolf_path;
    const auto rows = env.r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *env.r.ctx->bundle, error);
    CHECK(env.r.ok() && data && env.w.problem.empty(), "(the world of the game master's hands)");
    if (!env.r.ok() || !data || !env.w.problem.empty()) return;
    combat::NpcStatBook book(*env.r.ctx->bundle, *env.r.ctx->gear);
    combat::CombatWorld cw(env.w.registry, env.w.npcs.get(), &navfix::shared_nav(), *data, *env.r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(env.r, 77, {0, 0, 1098});
    cw.add_player(&player);
    uint64_t next_guid = 1;
    combat::Aftermath after(env.w.registry, cw, nullptr, [&] {
        aoc::protocol::emit::FIntrepidNetworkGUID g;
        g.ObjectId = 9000000 + next_guid++;
        return g;
    });
    after.install();
    env.cw = &cw;
    env.after = &after;
    const pets::Definition& guardian = pets::kDefinitions[0];
    const uint64_t other = env.w.spawn(shared.wolf_path, 1500, 0, 25, 0.0);
    cw.fighter_of(other);

    const uint64_t pet = env.w.spawn(guardian.archetype, 200, 0, 25, 1.0);
    CHECK(pet && cw.pet_summoned(pet, 77) && cw.is_pet(pet) && port_of_actor(cw, pet) && cw.pet_of(77) == pet, "(a pet on its owner's side, with its port in the players' list)");
    env.run(0.5);
    double seconds = 0;
    CHECK(after.despawn(pet, env.now, true, &seconds), "the game master's despawn takes the pet out of the world");
    CHECK(!cw.is_pet(pet) && !port_of_actor(cw, pet) && cw.pet_of(77) == 0, "its port is gone from the players' list, with its owner and its fight");
    CHECK(cw.health_of(pet) == -1.0, "... and its combatant is forgotten");
    env.run(2.0);
    CHECK(port_of_actor(cw, 77) && cw.health_of(other) > 0.0, "the world ticks on: the owner and the other creature are as they were");
    verify_pet_gm_kill_and_fighting_despawn(env, other);
}

struct TargetSideActors {
    uint64_t pet = 0;
    uint64_t owner = 77;
    uint64_t enemy = 0;
    uint64_t friendly = 0;
    uint64_t far_enemy = 0;
    uint64_t dead_enemy = 0;
    uint64_t missing = 987654321;
};

void verify_pet_target_side_table(combat::CombatWorld& cw, const TargetSideActors& a) {
    struct Row {
        int64_t     ability;
        const char* name;
        bool        at_enemy, at_friend, at_owner, at_pet, at_none, at_dead, at_far, at_missing;
    };
    const Row table[] = {
        {pets::kGuardianTaunt, "Taunt",                 true,  false, false, false, true,  false, false, false},
        {6064633577430779319ll, "Eviscerate",           true,  false, false, false, true,  false, false, false},
        {pets::kHunterLunge, "Lunge",                   true,  false, false, false, true,  false, false, false},
        {pets::kHunterIncapacitate, "Incapacitate",     true,  false, false, false, true,  false, false, false},
        {6064633577433793980ll, "Essence Eruption",     true,  false, false, false, true,  false, false, false},
        {6064633577432614330ll, "Recreate",             false, true,  true,  true,  true,  false, false, false},
        {6064634103529603072ll, "Cleanse",              false, true,  true,  true,  true,  false, false, false},
        {6064633577427633586ll, "Stomp",                true,  true,  true,  true,  true,  false, false, false},
        {6064634403600334848ll, "Brace",                false, true,  true,  true,  true,  false, false, false},
    };
    for (const Row& row : table) {
        const struct { const char* at; uint64_t target; bool expected; } cases[] = {
            {"an enemy", a.enemy, row.at_enemy},      {"a friend", a.friendly, row.at_friend}, {"its owner", a.owner, row.at_owner},   {"the pet", a.pet, row.at_pet},
            {"no one", 0, row.at_none},               {"a dead enemy", a.dead_enemy, row.at_dead}, {"a far enemy", a.far_enemy, row.at_far}, {"no actor", a.missing, row.at_missing},
        };
        for (const auto& c : cases) {
            std::string refused;
            const bool ok = cw.pet_can_use(a.pet, a.owner, row.ability, c.target, &refused);
            CHECKF(ok == c.expected, "%s at %s is %s (it %s)%s%s", row.name, c.at, c.expected ? "played" : "refused", ok ? "was played" : "was refused", ok ? "" : ": ", ok ? "" : refused.c_str());
        }
    }
}

void test_pet_target_sides() {
    PetTestData shared;
    if (!shared.ok) return;
    std::string error;
    std::printf("whom a pet's ability may be played at, asked before the owner pays\n");
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the pet's targets)");
    if (!r.ok() || !data || !w.problem.empty()) return;
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    const std::string friend_path = blueprint_path_holding(shared.motion, "Quest_NPC_Master.Default__Quest_NPC_Master_C");
    CHECK(!friend_path.empty(), "(a questgiver, who is no enemy of a player, is in the Blueprint table)");
    if (friend_path.empty()) return;
    const pets::Definition& guardian = pets::kDefinitions[0];
    TargetSideActors a;
    a.pet = w.spawn(guardian.archetype, 200, 0, 25, 1.0);
    a.enemy = w.spawn(shared.wolf_path, 900, 0, 25, 1.0);
    a.friendly = w.spawn(friend_path, 0, 900, 25, 0.0);
    a.far_enemy = w.spawn(shared.wolf_path, 6500, 0, 25, 1.0);
    a.dead_enemy = w.spawn(shared.wolf_path, 0, -900, 25, 1.0);
    CHECK(a.pet && a.enemy && a.friendly && a.far_enemy && a.dead_enemy && cw.pet_summoned(a.pet, 77), "(the pet, an enemy, a friend, a far enemy and one that will be dead)");
    if (!a.pet || !a.enemy || !a.friendly || !a.far_enemy || !a.dead_enemy) return;
    std::string why;
    double now = 1.0;
    CHECK(cw.gm_kill(a.dead_enemy, now, why), "(a dead enemy)");
    verify_pet_target_side_table(cw, a);
    CHECK(cw.health_of(a.friendly) == -1.0 && cw.health_of(a.far_enemy) == -1.0 && cw.health_of(a.enemy) == -1.0, "asking made no combatant of a friend, a far enemy or an enemy");
    CHECK(!cw.pet_casting(a.pet) && !cw.pet_fighting(a.pet), "... and the pet started nothing");
    CHECK(!cw.pet_can_use(a.pet, a.owner, 987654, a.enemy) && !cw.pet_can_use(a.pet, 5555, pets::kGuardianTaunt, a.enemy), "an ability that is not there, or an owner that is not there, is refused");
    CHECK(!cw.pet_can_use(a.dead_enemy, a.owner, pets::kGuardianTaunt, a.enemy), "a dead pet plays nothing");

    cw.fighter_of(a.enemy);
    CHECK(cw.pet_can_use(a.pet, a.owner, pets::kGuardianTaunt, a.enemy) && cw.pet_use(a.pet, a.owner, pets::kGuardianTaunt, a.enemy, pets::kGuardianTauntReach) && cw.pet_casting(a.pet), "Taunt at the enemy is played");
    for (int i = 0; i < 80 && cw.pet_casting(a.pet); ++i) {
        now += 0.1;
        w.npcs->note_player(77, 0, 0, now);
        w.npcs->tick(now);
        cw.tick(now);
    }
    CHECK(cw.effect_stacks(a.enemy, 6064630758760448234ll) == 1, "... and demoralizes the enemy");

    const int64_t attack = guardian.attack;
    CHECK(cw.pet_attack(a.pet, a.owner, attack, a.enemy), "Attack at an enemy: the pet goes");
    cw.pet_stop(a.pet);
    CHECK(!cw.pet_attack(a.pet, a.owner, attack, a.friendly), "Attack at a friend: the pet does not go");
    CHECK(!cw.pet_attack(a.pet, a.owner, attack, a.missing), "Attack at no actor: the pet does not go");
    CHECK(!cw.pet_attack(a.pet, a.owner, attack, a.dead_enemy), "Attack at a dead enemy: the pet does not go");
    CHECK(!cw.pet_fighting(a.pet), "... and fights nothing");
    CHECK(cw.health_of(a.friendly) == -1.0, "Attack at a friend made no combatant of it");
}

void test_pet_payment_split() {
    std::printf("what a pet's ability costs its owner: asked first, paid after\n");
    Rig r(6, 25);
    CHECK(r.ok(), "(the Summoner rig loads)");
    if (!r.ok()) return;
    double t = 5.0;
    r.pipe->refresh(t, "(the start)");
    r.take();
    r.set_current(pets::kEssence, 100.0);
    r.take();
    const double full = r.pawn.stat(pets::kEssence);
    AbilityPipeline::PetPayment pay;
    CHECK(r.pipe->can_pay_pet_ability(pets::kGuardianTaunt, t, pay), "Taunt can be paid for with 100 Essence");
    r.take();
    CHECK(pay.costs.size() == 1 && pay.costs[0].first == pets::kEssence && pay.costs[0].second < 0.0, "its cost is Essence, and one line of it");
    CHECK(std::abs(pay.cooldown - 10.0) < 0.01, "its cooldown is the record's 10 s");
    CHECK(r.pawn.stat(pets::kEssence) == full && r.pipe->charges_left(pets::kGuardianTaunt) == -1, "asking pays nothing and starts no cooldown");
    CHECK(r.pipe->can_pay_pet_ability(pets::kGuardianTaunt, t, pay), "asked again, it can still be paid for");
    r.take();
    CHECK(r.pawn.stat(pets::kEssence) == full && r.pipe->charges_left(pets::kGuardianTaunt) == -1, "... and nothing is paid then either");
    r.pipe->commit_pet_payment(pets::kGuardianTaunt, pay, t);
    r.take();
    CHECK(std::abs(r.pawn.stat(pets::kEssence) - (full + pay.costs[0].second)) < 0.01, "the commit takes the Essence once");
    CHECK(r.pipe->charges_left(pets::kGuardianTaunt) == 0, "... and starts the cooldown, where the owner's bar reads it");
    r.set_current(pets::kEssence, 5.0);
    r.take();
    t += 20.0;
    r.pipe->refresh(t, "(the cooldown is over)");
    r.take();
    const double little = r.pawn.stat(pets::kEssence);
    AbilityPipeline::PetPayment none;
    CHECK(little < 15.0 && !r.pipe->can_pay_pet_ability(pets::kGuardianTaunt, t, none), "with less than 15 Essence Taunt cannot be paid for");
    r.take();
    CHECK(r.pawn.stat(pets::kEssence) == little && r.pipe->charges_left(pets::kGuardianTaunt) == -1, "... and nothing was paid or started");
    r.set_current(pets::kEssence, 100.0);
    r.take();
    double seconds = 0;
    CHECK(r.pipe->pay_pet_ability(pets::kGuardianTaunt, t, seconds) && std::abs(seconds - 10.0) < 0.01, "pay_pet_ability pays and says the cooldown");
    r.take();
    CHECK(r.pawn.stat(pets::kEssence) < 100.0 && r.pipe->charges_left(pets::kGuardianTaunt) == 0, "... Essence taken and the cooldown running");
    const auto total = pets::sum_by_stat({{pets::kEssence, -30.0}, {pets::kEssence, -30.0}, {111, -1.0}});
    CHECK(total.size() == 2 && total[0].first == pets::kEssence && total[0].second == -60.0 && total[1].first == 111 && total[1].second == -1.0, "two cost lines of 30 on one stat are a cost of 60");
    CHECK(50.0 + total[0].second < 0.0, "so that 50 Essence does not pay for two costs of 30");
    CHECK(pets::sum_by_stat({}).empty(), "no cost lines are no cost");
}

}  // namespace

void run_ability_npc_pets_cmds_tests() {
    test_pet_despawn_and_kill();
    test_pet_target_sides();
    test_pet_payment_split();
}
