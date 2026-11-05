// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_ability_npc_common.h"

namespace {

struct PetRulesEnv {
    Rig                            r{6, 25};
    combat_rig::World              w;
    std::string                    wolf_path;
    double                         now = 1.0;
    combat::CombatWorld*           cw = nullptr;
    combat_rig::FakePlayer*        player = nullptr;

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

    void put(uint64_t id, const char* name, double seconds) {
        effects::Applied e;
        e.def = r.catalog->effect_named(name);
        e.duration = seconds;
        CHECK(e.def != nullptr, name);
        if (e.def) cw->apply_effects(id, 77, {e}, now);
    }

    int count_of(uint64_t id, const char* name) const {
        const auto* def = r.catalog->effect_named(name);
        return def ? cw->effect_stacks(id, def->row.guid) : -1;
    }

    static combat::Resolved loss(double amount) {
        combat::Resolved hurt;
        hurt.mods.push_back({0, content::kGuidHealth, -amount});
        return hurt;
    }
};

void verify_pet_rules_cc_hex_shock(PetRulesEnv& env, std::vector<uint64_t>& spawned) {
    const uint64_t a = env.w.spawn(env.wolf_path, 900, 0, 25, 1.0);
    spawned.push_back(a);
    env.cw->fighter_of(a);
    env.put(a, "Status_Tripped", 0.5);
    CHECK(env.count_of(a, "Status_Tripped") == 0, "a trip stays off a creature");

    const abilities::HitDef* stunning = nullptr;
    for (const content::AbilityHitRow& row : env.r.catalog->ability(6064633748157319575ll)->hits) {
        const auto* h = env.r.catalog->hit(row.hit_guid);
        if (h && h->row.name == "Summoner_SoulFlare_Damage") stunning = h;
    }
    CHECK(stunning != nullptr, "(Soul Flare's damage hit)");
    const uint64_t stun_target = env.w.spawn(env.wolf_path, 900, 300, 25, 1.0);
    spawned.push_back(stun_target);
    combat::Fighter* stun_fighter = env.cw->fighter_of(stun_target);
    if (stunning && stun_fighter) {
        combat::Resolved gate;
        combat::HitMeta meta;
        std::string why;
        CHECK(env.cw->resolver().resolve_effects(env.player->fighter(), *stun_fighter, *stunning, meta, gate, why), "(Soul Flare's effects run)");
        CHECK(gate.effects.size() == 1 && gate.effects[0].def == env.r.catalog->effect_named("Status_Stunned"), "the data's Soul Flare asks for a stun");
        env.cw->apply_effects(stun_target, 77, gate.effects, env.now);
        CHECK(env.count_of(stun_target, "Status_Stunned") == 0, "... which stays off the creature");
    }

    const uint64_t c = env.w.spawn(env.wolf_path, 900, 600, 25, 1.0);
    spawned.push_back(c);
    env.cw->fighter_of(c);
    env.put(c, "Summoner_Hex", 1.5);
    double before = env.cw->health_of(c);
    env.run(2.0);
    std::printf("  [info] Hex: %.1f -> %.1f, Exposed %d\n", before, env.cw->health_of(c), env.count_of(c, "Status_Exposed"));
    CHECK(env.count_of(c, "Summoner_Hex") == 0, "a Hex of a second and a half is over");
    CHECK(env.cw->health_of(c) < before - 1.5, "... and its end detonates on the creature");

    const uint64_t d = env.w.spawn(env.wolf_path, 900, -600, 25, 1.0);
    spawned.push_back(d);
    env.cw->fighter_of(d);
    env.put(d, "Status_Shocked", 6.0);
    combat::HitOrigin strike;
    strike.hit = env.r.catalog->hit(stunning ? stunning->row.guid : 0);
    strike.source_type = content::kSourceTypeAbility;
    before = env.cw->health_of(d);
    env.player->dealt.clear();
    env.cw->apply_hit_to_npc(d, 77, PetRulesEnv::loss(10.0), env.now, 0, &strike);
    std::printf("  [info] Shocked: a hit of 10 took %.1f, %zu further hits\n", before - env.cw->health_of(d), env.player->dealt.size());
    CHECK(env.player->dealt.size() == 1 && before - env.cw->health_of(d) > 10.5, "Status_Shocked answers a hit with its own");
}

void verify_pet_rules_reconstruct_flare_brace(PetRulesEnv& env, uint64_t pet, std::vector<uint64_t>& spawned) {
    const pets::Definition& guardian = pets::kDefinitions[0];
    CHECK(env.cw->pet_summoned(pet, 77), "the pet joins its owner's side");
    combat::TargetInfo seen = env.cw->inspect(pet, env.player->fighter());
    CHECK(seen.relation != combat::Relation::Hostile, "to its owner the pet is no enemy");
    const double pet_full = env.cw->max_health_of(pet);
    env.cw->apply_hit_to_npc(pet, 77, PetRulesEnv::loss(pet_full * 0.5), env.now);
    CHECK(std::abs(env.cw->health_of(pet) - pet_full * 0.5) < 1.0, "the owner's Extract takes the pet's Health");
    CHECK(!env.cw->ai_of(pet) || env.cw->ai_state(pet) == npc::ai::State::Idle, "... and the pet does not turn on its owner");
    env.put(pet, "Summoner_Reconstruct", 10.0);
    const double hurt_at = env.cw->health_of(pet);
    env.run(4.5);
    std::printf("  [info] Reconstruct: pet %.0f -> %.0f of %.0f\n", hurt_at, env.cw->health_of(pet), pet_full);
    CHECK(env.cw->health_of(pet) > hurt_at, "Reconstruct's ticks heal the pet");

    const uint64_t prey = env.w.spawn(env.wolf_path, 350, 0, 25, 1.0);
    spawned.push_back(prey);
    env.cw->fighter_of(prey);
    const double prey_before = env.cw->max_health_of(prey);
    env.put(pet, "Summoner_SoulFlare", 1.0);
    env.run(0.2);
    std::printf("  [info] Soul Flare: creature %.0f of %.0f\n", env.cw->health_of(prey), prey_before);
    CHECK(env.cw->health_of(prey) < prey_before, "Soul Flare's Apply hit hurts what stands around the pet");

    const double pet_shield = env.cw->shield_of(pet, env.now);
    CHECK(env.cw->pet_use(pet, 77, 6064634403600334848ll, 0, guardian.reach_cm), "the pet braces");
    env.run(1.5);
    CHECK(env.cw->effect_stacks(pet, first_effect_of(*env.r.catalog, 6064634403610624001ll)) == 1, "Brace lands on the Guardian");
    std::printf("  [info] Brace: shield %.0f -> %.0f\n", pet_shield, env.cw->shield_of(pet, env.now));
    const uint64_t striker = env.w.spawn(env.wolf_path, 300, 0, 25, 1.0);
    spawned.push_back(striker);
    env.cw->fighter_of(striker);
    const double striker_full = env.cw->max_health_of(striker);
    env.cw->add_threat(striker, pet, 100000.0, env.now);
    for (int tries = 0; tries < 10 && env.cw->health_of(striker) >= striker_full; ++tries) env.run(6.0);
    std::printf("  [info] Brace: the creature that struck %.0f of %.0f\n", env.cw->health_of(striker), striker_full);
    CHECK(env.cw->health_of(striker) < striker_full, "Brace sends the hit back at the creature");
}

void verify_pet_rules_taunt_follow_lunge(PetRulesEnv& env, uint64_t pet, const std::vector<uint64_t>& spawned) {
    for (const uint64_t other : spawned) {
        env.cw->forget(other);
        env.w.registry.remove(other);
    }
    const uint64_t on_owner = env.w.spawn(env.wolf_path, 1600, 0, 25, 1.0);
    env.cw->fighter_of(on_owner);
    env.cw->add_threat(on_owner, 77, 5000.0, env.now);
    env.run(0.3);
    CHECK(env.cw->creature_target(on_owner) == 77, "(the creature is on the owner)");
    CHECK(env.cw->owner_attacker(77, pet, 3000.0) == on_owner, "a Defensive pet finds what is attacking its owner");
    CHECK(env.cw->pet_can_act(pet), "the Guardian can act");
    CHECK(env.cw->pet_use(pet, 77, pets::kGuardianTaunt, on_owner, pets::kGuardianTauntReach), "the Guardian takes Taunt");
    const double guardian_x = env.w.registry.find(pet)->x;
    env.run(2.5);
    CHECK(std::abs(env.w.registry.find(pet)->x - guardian_x) < 200.0, "it taunts from where it stands");
    CHECK(env.count_of(on_owner, "Status_Demoralized") == 1, "Taunt lands at range");
    CHECK(env.cw->creature_target(on_owner) == pet, "the creature turns on the Guardian");

    const pets::Definition& mystic = pets::kDefinitions[2];
    const uint64_t follower = env.w.spawn(mystic.archetype, 0, 2500, 25, 1.0);
    auto owner_at = std::make_shared<nav::Vec3>(nav::Vec3{0, 2000, 1010});
    CHECK(env.w.npcs->order_pursue(follower, [owner_at](nav::Vec3& out) { out = *owner_at; return true; }, 720.0, 250.0), "the pet sets off after its owner");
    int stops = 0;
    bool was_moving = true;
    double last_y = env.w.registry.find(follower)->y;
    for (int i = 0; i < 40; ++i) {
        owner_at->y -= 40.0;
        env.now += 0.1;
        env.w.npcs->note_player(77, 0, 0, env.now);
        env.w.npcs->tick(env.now);
        const double y = env.w.registry.find(follower)->y;
        const bool moving = std::abs(y - last_y) > 0.5;
        if (i > 8 && was_moving && !moving) ++stops;
        was_moving = moving;
        last_y = y;
    }
    std::printf("  [info] following a walking owner: %d stops in 4 s, %.0f cm behind\n", stops, last_y - owner_at->y);
    CHECK(stops <= 2, "a pet following a walking owner keeps walking");
    env.w.npcs->halt(follower);

    const pets::Definition& hunter = pets::kDefinitions[1];
    const uint64_t lunger = env.w.spawn(hunter.archetype, -800, 0, 25, 1.0);
    const uint64_t far_prey = env.w.spawn(env.wolf_path, -200, 0, 1, 1.0);
    env.cw->pet_summoned(lunger, 77);
    CHECK(env.cw->pet_use(lunger, 77, pets::kHunterLunge, far_prey, 999.0), "the Hunter takes Lunge");
    const double from = env.w.registry.find(lunger)->x;
    env.run(0.4);
    CHECK(env.w.registry.find(lunger)->x > from + 300.0, "it covers the ground faster than it could run");
    env.run(2.0);
}

void test_pet_rules() {
    PetTestData shared;
    if (!shared.ok) return;
    std::printf("the rules of a creature's effects, from the data\n");
    PetRulesEnv env;
    env.wolf_path = shared.wolf_path;
    std::string error;
    const auto rows = env.r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *env.r.ctx->bundle, error);
    CHECK(env.r.ok() && data && env.w.problem.empty(), "(the world of the rules)");
    if (!env.r.ok() || !data || !env.w.problem.empty()) return;
    combat::NpcStatBook book(*env.r.ctx->bundle, *env.r.ctx->gear);
    combat::CombatWorld cw(env.w.registry, env.w.npcs.get(), &navfix::shared_nav(), *data, *env.r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(env.r, 77, {0, 0, 1098});
    cw.add_player(&player);
    env.cw = &cw;
    env.player = &player;
    std::vector<uint64_t> spawned;
    verify_pet_rules_cc_hex_shock(env, spawned);
    const uint64_t pet = env.w.spawn(pets::kDefinitions[0].archetype, 200, 0, 25, 1.0);
    verify_pet_rules_reconstruct_flare_brace(env, pet, spawned);
    verify_pet_rules_taunt_follow_lunge(env, pet, spawned);
}

void check_pet_ability_outcome(const std::string& name, combat::CombatWorld& cw, uint64_t prey, double full, const combat_rig::FakePlayer& player) {
    if (name.find("Stomp") != std::string::npos) {
        CHECK(cw.health_of(prey) < full && !player.dealt.empty(), "Stomp hurts what stands around the pet, and the owner is told");
    }
    if (name.find("Taunt") != std::string::npos) CHECK(cw.effect_stacks(prey, 6064630758760448234ll) == 1, "Taunt demoralizes the creature");
    if (name.find("Incapacitate") != std::string::npos) CHECK(cw.effect_stacks(prey, pets::kHunterIncapMelee) == 0, "Incapacitate plays; its crowd control stays off the creature");
    if (name.find("Eviscerate") != std::string::npos || name.find("Lunge") != std::string::npos) CHECK(cw.health_of(prey) < full, "its hit hurts the creature");
    if (name.find("Eruption") != std::string::npos) CHECK(cw.health_of(prey) < full, "Essence Eruption hurts around its target");
    if (name.find("Recreate") != std::string::npos || name.find("Cleanse") != std::string::npos) CHECK(!player.from_pet.empty(), "the Mystic's help lands on its owner");
}

bool run_one_pet_ability(const PetTestData& shared, const pets::PetAbility& use) {
    const pets::Definition* def = pets::for_effect(use.pet_effect);
    Rig r(6, 25);
    std::string error;
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty() && def, "(the world of the pet's ability)");
    if (!r.ok() || !data || !w.problem.empty() || !def) return false;
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    const uint64_t pet = w.spawn(def->archetype, 200, 0, 25, 1.0);
    const uint64_t prey = w.spawn(shared.wolf_path, 500, 0, 25, 1.0);
    const auto* a = r.catalog->ability(use.ability);
    CHECK(pet && prey && a, "(the pet, a creature and the ability)");
    if (!pet || !prey || !a) return false;
    CHECK(cw.pet_use(pet, 77, use.ability, prey, def->reach_cm) && cw.pet_casting(pet), "the pet takes the ability");
    double now = 1.0;
    cw.fighter_of(prey);
    const double full = cw.max_health_of(prey);
    for (int i = 0; i < 80 && cw.pet_casting(pet); ++i) {
        now += 0.1;
        w.npcs->note_player(77, 0, 0, now);
        w.npcs->tick(now);
        cw.tick(now);
    }
    CHECK(!cw.pet_casting(pet), "the ability ends");
    std::printf("  [info] %s: creature %.0f of %.0f Health, %zu dealt, %zu on the owner\n", a->row.name.c_str(), cw.health_of(prey), full, player.dealt.size(), player.from_pet.size());
    check_pet_ability_outcome(a->row.name, cw, prey, full, player);
    if (a->row.name.find("Brace") != std::string::npos) CHECK(cw.effect_stacks(pet, first_effect_of(*r.catalog, 6064634403610624001ll)) == 1, "Brace lands on the Guardian");
    CHECK(!cw.pet_use(pet, 77, 987654, prey, def->reach_cm), "a pet plays no ability that is not there");
    return true;
}

void test_pet_abilities() {
    PetTestData shared;
    if (!shared.ok) return;
    std::printf("a pet's abilities\n");
    for (const pets::PetAbility& use : pets::kPetAbilities) {
        if (use.ability == pets::kHunterStanceSwap) continue;
        if (!run_one_pet_ability(shared, use)) break;
    }
}

struct PetOwnerEnv {
    Rig                            r{6, 25};
    combat_rig::World              w;
    std::string                    wolf_path;
    combat::CombatWorld*           cw = nullptr;
    uint64_t                       pet = 0;
    uint64_t                       prey = 0;
    double                         t = 2.0;
    int32_t                        id = 100;

    static constexpr int64_t kExtract = 6064633645783911914ll, kExtractHit = 6064633645898797957ll;
    static constexpr int64_t kSiphon = 6064633662084304872ll, kSiphonHit = 6064633662084304873ll;
    static constexpr int64_t kBond = 6064633814845161472ll, kBondHit = 6064633815783636993ll;
    static constexpr int64_t kBondPet = 6064633815887904770ll, kBondCaster = 6064633821403750774ll;
    static constexpr int64_t kShred = 6064633271239453745ll, kShredHit = 6064633271360225904ll;
    static constexpr int64_t kFortitude = 6064634608804770959ll;
    static constexpr int64_t kHex = 6064633493499835793ll, kHexKillReset = 6064634568783891085ll;
    static constexpr int64_t kReconstruct = 6064633663176909411ll, kReconstructHit = 6064633663176909412ll;
    static constexpr int64_t kAmplify = 6064633844273079586ll, kAmplifyHit = 6064633861412726145ll;
    static constexpr int64_t kPhaseOut = 6064633866966605140ll, kPhaseOutPet = 6064633867155810140ll;

    Refusal cast(int64_t ability, int64_t hit, const std::vector<uint64_t>& aim, int64_t branch = 0) {
        const bool channelled = branch != 0;
        TriggerInfo info;
        info.target_count = static_cast<uint32_t>(aim.size());
        info.target_actors = aim;
        const Refusal why = r.pipe->trigger(ability, ++id, t, info);
        r.take();
        const size_t notes_from = r.log.size();
        t += channelled ? 1.6 : 0.3;
        if (channelled) r.pipe->branch_request(id, branch, t);
        t += channelled ? 0.2 : 0.0;
        r.pipe->refresh(t, "(the cost step)");
        r.take();
        const auto verdict = r.pipe->hit_request(ability, 0, hit, 1, aim.empty() ? 1 : 0, t, aim);
        std::printf("  [info] %s: trigger %d, request %s, targets %zu\n", r.catalog->ability(ability)->row.name.c_str(), static_cast<int>(why), AbilityPipeline::verdict_name(verdict), aim.size());
        r.take();
        t += 1.5;
        r.pipe->refresh(t, "(the hit lands)");
        r.take();
        for (size_t i = notes_from; i < r.log.size(); ++i) std::printf("    [note] %s\n", r.log[i].note.c_str());
        t += 40.0;
        return why;
    }

    void refill() {
        r.pipe->refresh(t, "(refill)");
        r.take();
        r.set_current(pets::kEssence, pets::kEssenceMax);
        r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
        r.take();
    }
};

void verify_owner_extract_siphon_bond(PetOwnerEnv& env) {
    env.refill();
    const double full = env.r.pawn.stat(content::kGuidMaxHealth);
    env.r.set_current(content::kGuidHealth, full * 0.5);
    env.r.take();
    const double pet_before = env.cw->health_of(env.pet);
    CHECK(env.cast(PetOwnerEnv::kExtract, PetOwnerEnv::kExtractHit, {env.pet}, 6064633645892571971ll) == Refusal::None, "Extract is taken with a pet out");
    std::printf("  [info] Extract: Health %.0f -> %.0f of %.0f, pet %.0f -> %.0f\n", full * 0.5, env.r.pawn.stat(content::kGuidHealth), full, pet_before, env.cw->health_of(env.pet));
    CHECK(env.r.pawn.stat(content::kGuidHealth) > full * 0.5 + 1.0, "Extract heals its owner (the reaction its hit carries)");
    CHECK(env.cw->health_of(env.pet) < pet_before, "and takes it from the pet");
    env.refill();
    env.r.set_current(content::kGuidMana, 1.0);
    env.r.take();
    CHECK(env.cast(PetOwnerEnv::kSiphon, PetOwnerEnv::kSiphonHit, {env.pet}, 6064633662084370415ll) == Refusal::None, "Siphon is taken with a pet out");
    std::printf("  [info] Siphon: Mana 1 -> %.0f\n", env.r.pawn.stat(content::kGuidMana));
    CHECK(env.r.pawn.stat(content::kGuidMana) > 2.0, "Siphon gives its owner Mana (the reaction its hit carries)");

    env.refill();
    CHECK(env.cast(PetOwnerEnv::kBond, PetOwnerEnv::kBondHit, {}) == Refusal::None, "Bond is taken");
    env.r.pipe->refresh(env.t, "(Bond ticks)");
    env.r.take();
    std::printf("  [info] Bond: on the pet %d, on the owner %d\n", env.cw->effect_stacks(env.pet, PetOwnerEnv::kBondPet), env.r.pipe->stacks_of(PetOwnerEnv::kBondCaster));
    CHECK(env.cw->effect_stacks(env.pet, PetOwnerEnv::kBondPet) == 0, "Mutual Bond, the pet's own effect, asks for its talent");
    CHECK(env.r.pipe->stacks_of(PetOwnerEnv::kBondCaster) > 0, "Bond's tick reaches the pet, whose hit gives its owner its side (the reaction it carries)");
}

void verify_owner_shred_buffs_phase(PetOwnerEnv& env) {
    env.refill();
    env.w.npcs->note_player(77, 0, 0, env.t);
    env.w.npcs->tick(env.t);
    env.cw->tick(env.t);
    TriggerInfo shred;
    shred.target_count = 1;
    shred.target_actors = {env.prey};
    CHECK(env.r.pipe->trigger(PetOwnerEnv::kShred, ++env.id, env.t, shred) == Refusal::None, "Spirit Shred is taken");
    env.r.take();
    env.t += 0.3;
    env.r.pipe->refresh(env.t, "(its cost)");
    env.r.take();
    const std::vector<uint64_t> on_prey = {env.prey};
    env.r.pipe->hit_request(PetOwnerEnv::kShred, 0, PetOwnerEnv::kShredHit, 1, 0, env.t, on_prey);
    for (int step = 0; step < 16; ++step) {
        env.t += 0.5;
        env.r.pipe->refresh(env.t, "(it ticks)");
        env.r.take();
        env.w.npcs->note_player(77, 0, 0, env.t);
        env.w.npcs->tick(env.t);
        env.cw->tick(env.t);
    }
    CHECK(env.cw->effect_stacks(env.prey, first_effect_of(*env.r.catalog, PetOwnerEnv::kShredHit)) == 1 || env.cw->health_of(env.prey) < env.cw->max_health_of(env.prey), "(it landed on the creature)");
    env.t += 40.0;
    std::printf("  [info] Spirit Shred: Fortitude on the Guardian %d\n", env.cw->effect_stacks(env.pet, PetOwnerEnv::kFortitude));
    CHECK(env.cw->effect_stacks(env.pet, PetOwnerEnv::kFortitude) >= 1, "its damage stacks Fortitude on the Guardian (the reaction its hit carries)");

    env.refill();
    CHECK(env.cast(PetOwnerEnv::kReconstruct, PetOwnerEnv::kReconstructHit, {env.pet}) == Refusal::None, "Reconstruct is taken");
    env.refill();
    CHECK(env.cast(PetOwnerEnv::kAmplify, PetOwnerEnv::kAmplifyHit, {env.pet}) == Refusal::None, "Amplify is taken");
    CHECK(env.cw->effect_stacks(env.pet, first_effect_of(*env.r.catalog, PetOwnerEnv::kAmplifyHit)) == 1, "Amplify lands on the pet");
    env.refill();
    CHECK(env.cast(PetOwnerEnv::kPhaseOut, PetOwnerEnv::kPhaseOutPet, {env.pet}) == Refusal::None, "Phase Out is taken");
    CHECK(first_effect_of(*env.r.catalog, PetOwnerEnv::kPhaseOutPet) != 0, "(Phase Out's hit puts an effect on the pet)");
    CHECK(env.cw->effect_stacks(env.pet, first_effect_of(*env.r.catalog, PetOwnerEnv::kPhaseOutPet)) == 0, "Phase Out's effect on the pet is a crowd control: it stays off");
}

void verify_owner_pay_hex_and_regen(PetOwnerEnv& env) {
    env.refill();
    double seconds = 0;
    const double essence = env.r.pawn.stat(pets::kEssence);
    CHECK(env.r.pipe->pay_pet_ability(pets::kGuardianTaunt, env.t, seconds), "Taunt is paid for");
    env.r.take();
    std::printf("  [info] Taunt: Essence %.1f -> %.1f, cooldown %.1f s\n", essence, env.r.pawn.stat(pets::kEssence), seconds);
    CHECK(env.r.pawn.stat(pets::kEssence) < essence, "it costs Essence");
    CHECK(std::abs(seconds - 10.0) < 0.01, "its cooldown is the record's 10 s");
    CHECK(env.r.pipe->charges_left(pets::kGuardianTaunt) == 0, "the cooldown runs on the owner, where the bar reads it");
    env.t += seconds + 1.0;
    env.r.pipe->refresh(env.t, "(the cooldown is over)");
    env.r.take();
    CHECK(env.r.pipe->charges_left(pets::kGuardianTaunt) == -1, "and ends");
    env.r.set_current(pets::kEssence, 0.0);
    env.r.take();
    CHECK(!env.r.pipe->pay_pet_ability(pets::kGuardianTaunt, env.t, seconds), "with no Essence it is refused");
    env.r.take();
    CHECK(env.r.pipe->charges_left(pets::kGuardianTaunt) == -1, "and starts no cooldown");

    env.refill();
    TriggerInfo hex;
    hex.target_count = 1;
    hex.target_actors = {env.prey};
    CHECK(env.r.pipe->trigger(PetOwnerEnv::kHex, ++env.id, env.t, hex) == Refusal::None, "Hex is taken");
    env.r.take();
    env.t += 0.6;
    env.r.pipe->refresh(env.t, "(its cost)");
    env.r.take();
    CHECK(env.r.pipe->charges_left(PetOwnerEnv::kHex) != -1, "Hex's cooldown runs");
    env.r.pipe->land_pet_hit(PetOwnerEnv::kHexKillReset, PetOwnerEnv::kHex, env.t);
    env.r.take();
    CHECK(env.r.pipe->charges_left(PetOwnerEnv::kHex) == -1, "the kill gives Hex back (Summoner_Hex_KillReset)");

    const double hurt = env.cw->health_of(env.pet);
    for (int step = 0; step < 50; ++step) {
        env.t += 0.1;
        env.w.npcs->note_player(77, 0, 0, env.t);
        env.w.npcs->tick(env.t);
        env.cw->tick(env.t);
    }
    std::printf("  [info] a pet's regeneration: %.1f -> %.1f in 5 s of %.0f\n", hurt, env.cw->health_of(env.pet), env.cw->max_health_of(env.pet));
    CHECK(env.cw->health_of(env.pet) > hurt, "a hurt pet regenerates");
    env.r.set_current(content::kGuidHealth, env.r.pawn.stat(content::kGuidMaxHealth));
    const uint64_t beside = env.w.spawn(env.wolf_path, 350, 0, 25, 1.0);
    env.cw->fighter_of(beside);
    const double whole = env.cw->max_health_of(beside);
    effects::Applied flare;
    flare.def = env.r.catalog->effect_named("Summoner_SoulFlare");
    flare.duration = 1.0;
    for (int tries = 0; tries < 10 && env.cw->health_of(beside) >= whole; ++tries) {
        env.cw->apply_effects(env.pet, 77, {flare}, env.t);
        for (int step = 0; step < 15; ++step) {
            env.t += 0.1;
            env.w.npcs->note_player(77, 0, 0, env.t);
            env.w.npcs->tick(env.t);
            env.cw->tick(env.t);
        }
    }
    CHECK(env.cw->health_of(beside) < whole, "Soul Flare still hurts what stands around the pet");
}

void test_pet_owner_abilities() {
    PetTestData shared;
    if (!shared.ok) return;
    std::printf("a Summoner's abilities on its pet, as the client asks for them\n");
    PetOwnerEnv env;
    env.wolf_path = shared.wolf_path;
    std::string error;
    const auto rows = env.r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *env.r.ctx->bundle, error);
    CHECK(env.r.ok() && data && env.w.problem.empty(), "(the world of the pet)");
    if (!env.r.ok() || !data || !env.w.problem.empty()) return;
    combat::NpcStatBook book(*env.r.ctx->bundle, *env.r.ctx->gear);
    combat::CombatWorld cw(env.w.registry, env.w.npcs.get(), &navfix::shared_nav(), *data, *env.r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(env.r, 77, {0, 0, 1098});
    cw.add_player(&player);
    combat::WorldHitPort port(cw, env.w.registry, player.fighter());
    env.r.pipe->set_combat(&port);
    env.r.pipe->set_reactions_enabled(true);
    env.cw = &cw;
    const pets::Definition& guardian = pets::kDefinitions[0];
    env.pet = env.w.spawn(guardian.archetype, 200, 0, 25, 1.0);
    env.prey = env.w.spawn(shared.wolf_path, 900, 0, 25, 1.0);
    cw.fighter_of(env.prey);
    CHECK(env.pet && env.prey && cw.pet_summoned(env.pet, 77), "(the Guardian is on its owner's side)");
    CHECK(env.r.pipe->grant_effect(guardian.effect, 1.0), "(its summon's effect is on the owner)");
    env.r.take();
    env.r.pipe->set_known({PetOwnerEnv::kExtract, PetOwnerEnv::kSiphon, PetOwnerEnv::kBond, PetOwnerEnv::kShred, PetOwnerEnv::kHex, PetOwnerEnv::kReconstruct, PetOwnerEnv::kAmplify, PetOwnerEnv::kPhaseOut});
    verify_owner_extract_siphon_bond(env);
    verify_owner_shred_buffs_phase(env);
    verify_owner_pay_hex_and_regen(env);
    env.r.pipe->set_combat(nullptr);
}

void verify_pet_bolt_mark(Rig& r, combat::CombatWorld& cw, uint64_t prey, const TriggerInfo& info, double& t) {
    const auto press = [&](int32_t id, double at) {
        const Refusal why = r.pipe->trigger(pets::kBolt, id, at, info);
        r.take();
        return why;
    };
    const abilities::EffectDef* mark = r.catalog->effect(pets::kBoltMark);
    CHECK(mark != nullptr, "the bolt's mark is in the data");
    if (mark) {
        effects::Applied landed;
        landed.def = mark;
        landed.duration = 5.0;
        t = 3.0;
        cw.apply_effects(prey, 77, {landed}, t);
        r.pipe->refresh(t, "the first bolt is paid for");
        r.take();
        const double mana = r.pawn.stat(content::kGuidMana);
        CHECK(press(3, t) == Refusal::None, "the next press on the marked creature, with a pet out, is taken");
        const auto recall = r.pipe->take_bolt_recall();
        CHECK(recall.target == prey && !recall.behind, "it sends the Guardian to the creature");
        CHECK(cw.effect_stacks(prey, pets::kBoltMark) == 0, "the press spends the mark");
        CHECK(r.pipe->stacks_of(pets::kBoltLockout) == 0 && r.pipe->stacks_of(pets::kBoltLockout2) == 0, "and the lockouts");
        CHECK(r.pawn.stat(content::kGuidMana) >= mana, "and costs no Mana");
        CHECK(r.pipe->charges_left(pets::kBolt) == 1, "it starts the cooldown, and spends no charge (retail's CD says 1)");
        CHECK(press(4, t + 0.02) == Refusal::Charges, "the next trigger of the held key is refused");
        CHECK(r.pipe->take_bolt_recall().target == 0, "and sends nothing");
        CHECK(press(5, t + 13.0) == Refusal::None, "12 s on the bolt can be thrown again");
        t += 32.0;
    }
    CHECK(r.pipe->grant_effect(pets::kBoltLockout2, t), "(the bolt's second lockout)");
    r.take();
    CHECK(press(6, t + 0.1) == Refusal::Busy, "under the lockout, with no mark, nothing is thrown");
    r.pipe->refresh(t + 6.0, "the lockout is over");
    r.take();
    CHECK(r.pipe->charges_left(pets::kBolt) == 1, "the lockout's end starts the cooldown");
}

void test_pet_bolt() {
    PetTestData shared;
    if (!shared.ok) return;
    std::printf("Dimensional Bolt: its presses, a held key's triggers and its cooldown\n");
    std::string error;
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the bolt)");
    if (!r.ok() || !data || !w.problem.empty()) return;
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    combat::WorldHitPort port(cw, w.registry, player.fighter());
    r.pipe->set_combat(&port);
    const uint64_t prey = w.spawn(shared.wolf_path, 900, 0, 25, 1.0);
    cw.fighter_of(prey);
    r.pipe->set_known({pets::kBolt});
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {prey};
    double t = 1.0;
    CHECK(r.pipe->grant_effect(pets::kDefinitions[0].effect, t), "(a Guardian is out)");
    r.take();
    CHECK(r.pipe->trigger(pets::kBolt, 1, t, info) == Refusal::None, "the first press throws the bolt");
    r.take();
    CHECK(r.pipe->trigger(pets::kBolt, 2, t + 0.02, info) == Refusal::Busy, "a held key's next trigger, 20 ms on, throws no second bolt");
    r.take();
    CHECK(r.pipe->charges_left(pets::kBolt) == -1, "the bolt itself starts no cooldown");
    verify_pet_bolt_mark(r, cw, prey, info, t);
    r.pipe->set_combat(nullptr);
}

struct PetFightEnv {
    Rig                            r{6, 25};
    combat_rig::World              w;
    std::string                    wolf_path;
    combat::CombatWorld*           cw = nullptr;
    combat_rig::FakePlayer*        player = nullptr;
    uint64_t                       pet = 0;
    double                         now = 1.0;
};

void verify_pet_fight_second_and_third(PetFightEnv& env, const pets::Definition& def) {
    const uint64_t second = env.w.spawn(env.wolf_path, 3600, 0, 25, 1.0);
    CHECK(second && env.cw->pet_attack(env.pet, 77, def.attack, second), "it takes another order");
    double even = 0, lowest = second ? env.cw->max_health_of(second) : 0.0, lowest_at = 0;
    for (; even < 15.0 && second && !env.cw->dead(second) && env.cw->pet_fighting(env.pet); even += 0.1) {
        env.now += 0.1;
        env.w.npcs->note_player(77, 0, 0, env.now);
        env.w.npcs->tick(env.now);
        env.cw->tick(env.now);
        if (env.cw->health_of(second) < lowest) {
            lowest = env.cw->health_of(second);
            lowest_at = even;
        }
    }
    const double taken = second ? env.cw->max_health_of(second) - lowest : 0.0;
    std::printf("  [info] %s against a creature of its own level: %.0f of %.0f Health by %.1f s\n", def.name, taken, second ? env.cw->max_health_of(second) : 0.0, lowest_at);
    CHECK(taken > 0, "its hits count as its owner's and hurt a creature of its level");
    CHECK(taken < 0.9 * env.cw->max_health_of(second) || lowest_at > 5.0, "a pet does not cut a creature of its level down in a few seconds");
    if (second) {
        env.cw->forget(second);
        env.w.registry.remove(second);
    }
    env.r.set_current(content::kGuidHealth, env.r.pawn.stat(content::kGuidMaxHealth));
    env.r.take();
    const double pet_full = env.cw->health_of(env.pet);
    const size_t hits_before = env.player->hits.size();
    const uint64_t foe = env.w.spawn(env.wolf_path, 3600, 0, 25, 1.0);
    CHECK(foe && env.cw->pet_attack(env.pet, 77, def.attack, foe), "the pet goes for a third creature");
    for (double t = 0; t < 180.0 && foe && !env.cw->dead(env.pet) && !env.cw->dead(foe); t += 0.1) {
        env.now += 0.1;
        env.w.npcs->note_player(77, 0, 0, env.now);
        env.w.npcs->tick(env.now);
        env.cw->tick(env.now);
    }
    std::printf("  [info] %s after the fight: pet %.0f of %.0f Health (%s), creature %.0f of %.0f\n", def.name, env.cw->health_of(env.pet), pet_full, env.cw->dead(env.pet) ? "killed" : "alive", foe ? env.cw->health_of(foe) : 0.0, foe ? env.cw->max_health_of(foe) : 0.0);
    CHECK(env.cw->health_of(env.pet) < pet_full, "the creature hurts the pet");
    CHECK(env.player->hits.size() == hits_before, "while the pet stands the creature leaves the owner alone");
    if (env.cw->dead(env.pet)) {
        CHECK(!env.cw->pet_fighting(env.pet), "a killed pet's fight is over");
        CHECK(foe && !env.cw->dead(foe) && env.cw->health_of(foe) < env.cw->max_health_of(foe), "(the creature lived, hurt)");
        for (int i = 0; i < 5; ++i) {
            env.now += 0.1;
            env.w.npcs->note_player(77, 0, 0, env.now);
            env.w.npcs->tick(env.now);
            env.cw->tick(env.now);
        }
        const auto state = env.cw->ai_state(foe);
        CHECK(state && *state != npc::ai::State::Reset && *state != npc::ai::State::Idle, "the creature does not go home healed: it goes for the owner");
    }
    env.cw->pet_stop(env.pet);
    CHECK(!env.cw->pet_fighting(env.pet), "a stop ends the fight");
    env.cw->pet_gone(env.pet);
    CHECK(!env.cw->is_combatant(env.pet), "a pet that leaves is forgotten");
}

bool run_one_pet_fight(const PetTestData& shared, const pets::Definition& def) {
    PetFightEnv env;
    env.wolf_path = shared.wolf_path;
    std::string error;
    const auto rows = env.r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *env.r.ctx->bundle, error);
    CHECK(env.r.ok() && data && env.w.problem.empty(), "(the world of the pet's fight)");
    if (!env.r.ok() || !data || !env.w.problem.empty()) return false;
    combat::NpcStatBook book(*env.r.ctx->bundle, *env.r.ctx->gear);
    combat::CombatWorld cw(env.w.registry, env.w.npcs.get(), &navfix::shared_nav(), *data, *env.r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(env.r, 77, {0, 0, 1098});
    cw.add_player(&player);
    env.cw = &cw;
    env.player = &player;
    env.pet = env.w.spawn(def.archetype, 200, 0, 25, 1.0);
    const uint64_t prey = env.w.spawn(shared.wolf_path, 900, 0, 1, 1.0);
    CHECK(env.pet && prey, "(the pet and a creature stand in the world)");
    if (!env.pet || !prey) return false;
    CHECK(env.r.catalog->ability(def.attack) != nullptr, "the pet's attack is in the data");
    CHECK(!cw.pet_attack(env.pet, 77, def.attack, env.pet) && !cw.pet_attack(env.pet, 77, def.attack, 987654), "a pet does not attack itself or nothing");
    CHECK(cw.pet_attack(env.pet, 77, def.attack, prey) && cw.pet_fighting(env.pet), "the pet takes the order to attack");
    const double full = cw.max_health_of(prey);
    size_t steps = 0;
    for (; steps < 900 && !cw.dead(prey); ++steps) {
        env.now += 0.1;
        env.w.npcs->note_player(77, 0, 0, env.now);
        env.w.npcs->tick(env.now);
        cw.tick(env.now);
    }
    std::printf("  [info] %s killed the creature (%.0f Health) in %.1f s\n", def.name, full, 0.1 * static_cast<double>(steps));
    CHECK(cw.dead(prey), "the pet's attacks kill the creature");
    for (int i = 0; i < 5; ++i) {
        env.now += 0.1;
        cw.tick(env.now);
    }
    CHECK(!cw.pet_fighting(env.pet), "the fight ends with the creature's death");
    verify_pet_fight_second_and_third(env, def);
    return true;
}

void test_pets_fight() {
    PetTestData shared;
    if (!shared.ok) return;
    std::printf("a Summoner's pets fight\n");
    for (const pets::Definition& def : pets::kDefinitions) {
        if (!run_one_pet_fight(shared, def)) break;
    }
}

void test_pet_taunt_second_landing() {
    PetTestData shared;
    if (!shared.ok) return;
    std::printf("Taunt: the requirement of its effects reads what the creature holds\n");
    std::string error;
    Rig r(6, 25);
    const auto rows = r.db->combat_rows(error);
    std::optional<combat::CombatData> data;
    if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
    combat_rig::World w;
    CHECK(r.ok() && data && w.problem.empty(), "(the world of the Taunt)");
    if (!r.ok() || !data || !w.problem.empty()) return;
    combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
    combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, shared.motion);
    combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
    cw.add_player(&player);
    const pets::Definition& guardian = pets::kDefinitions[0];
    const uint64_t pet = w.spawn(guardian.archetype, 200, 0, 25, 1.0);
    const uint64_t prey = w.spawn(shared.wolf_path, 500, 0, 25, 1.0);
    cw.fighter_of(prey);
    const auto* demoralized = r.catalog->effect_named("Status_Demoralized");
    const auto* humiliated = r.catalog->effect_named("Status_Humiliated");
    CHECK(pet && prey && demoralized && humiliated, "(the Guardian, a creature and the two effects)");
    if (!pet || !prey || !demoralized || !humiliated) return;
    double now = 1.0;
    const auto taunt = [&] {
        CHECK(cw.pet_use(pet, 77, pets::kGuardianTaunt, prey, pets::kGuardianTauntReach), "the Guardian takes Taunt");
        for (int i = 0; i < 40 && cw.pet_casting(pet); ++i) {
            now += 0.1;
            r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));
            r.take();
            w.npcs->note_player(77, 0, 0, now);
            w.npcs->tick(now);
            cw.tick(now);
        }
    };
    taunt();
    CHECK(cw.effect_stacks(prey, demoralized->row.guid) == 1 && cw.effect_stacks(prey, humiliated->row.guid) == 0, "its first landing demoralizes");
    taunt();
    CHECK(cw.effect_stacks(prey, humiliated->row.guid) == 1, "its second humiliates");
}

}  // namespace

void run_ability_npc_pets_tests() {
    test_pet_rules();
    test_pet_abilities();
    test_pet_owner_abilities();
    test_pet_bolt();
    test_pets_fight();
    test_pet_taunt_second_landing();
}
