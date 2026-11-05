// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_ability_session_common.h"
#include "capture_blocks.h"
#include "combat_rig.h"
#include "session/player_session.h"
#include "session/summoner_pets.h"
#include "spawns/area.h"

using namespace aoc::worldd;

namespace {

size_t count_lines(const std::string& text, const char* a, const char* b) {
    size_t n = 0, pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) ++n;
        pos = end + 1;
    }
    return n;
}

BunchWriter actor_bunch(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params) {
    BunchWriter out, field;
    const schema::RpcSchema* rpc = cls.rpc(name);
    bool failed = false;
    if (!rpc || (repl::write_rpc_field(cls, *rpc, params, field, client_options(), &failed) == 0 && !params.values.empty()) || failed) return out;
    repl::write_content_block(out, repl::BlockHeader{}, field, false);
    return out;
}

void send_ready_to_phase_in(Run& r) {
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
}

void test_ability_log_window() {
    std::printf("the ability log: the first 100 transitions of each minute are at info, so a long session is not blind after its first 100 lines\n");
    Run r;
    r.play(0.3, true);
    g_log.str("");
    const auto sprints = [&](int rounds) {
        for (int i = 0; i < rounds; ++i) {
            const int32_t id = r.trigger(kSprint);
            r.play(0.1, true);
            r.cancel(id);
            r.play(0.1, true);
        }
    };
    sprints(60);
    spdlog::default_logger()->flush();
    const size_t first = count_lines(g_log.str(), "[info]", "(Stamina ");
    r.play(61.0, true);
    g_log.str("");
    sprints(10);
    spdlog::default_logger()->flush();
    const size_t later = count_lines(g_log.str(), "[info]", "(Stamina ");
    CHECKF(first == PlayerSession::kLoggedAbilityTransitions, "120 transitions in the first minute: the first 100 are at info (%zu)", first);
    CHECKF(later >= 20, "a minute later 20 more transitions are at info again, not at debug (%zu)", later);
    CHECK(r.h.s->ability_triggers() == 70 && r.h.s->ability_triggers_dropped() == 0 && !r.h.s->abilities_failed(), "(all 70 triggers were handled)");
}

BunchWriter pet_command_bunch(int64_t command, const repl::Vec3d* place) {
    const auto& pawn = class_named("PlayerPawn_C");
    const auto* rpc = pawn.rpc("ServerSendPetCommand");
    repl::RpcParams params;
    bool ok = rpc && repl::set_rpc_param(*rpc, params, "PetCommandId", repl::Value::i64(command));
    if (ok && place) {
        for (const char* word : {"TargetData.TargetGuid.A", "TargetData.TargetGuid.B", "TargetData.TargetGuid.C", "TargetData.TargetGuid.D"}) {
            ok = ok && repl::set_rpc_param(*rpc, params, word, repl::Value::i64(0));
        }
        ok = ok && repl::set_rpc_param(*rpc, params, "TargetData.TargetLocation", repl::Value(*place)) && repl::set_rpc_param(*rpc, params, "TargetData.bHasLocation", repl::Value::boolean(true));
    }
    if (!ok) {
        std::printf("  [FAIL] the pet command's parameters do not fit\n");
        return BunchWriter();
    }
    return actor_bunch(pawn, "ServerSendPetCommand", params);
}

std::unique_ptr<nav::AreaNav> ground_under_the_player() {
    nav::Geometry g = navfix::test_geometry();
    g.placements.clear();
    g.center_x = kX;
    g.center_y = kY;
    for (nav::TerrainGrid& t : g.terrain) {
        t.x0 += kX;
        t.y0 += kY;
        t.z_base = kZ - 88.0;
        std::fill(t.raw.begin(), t.raw.end(), static_cast<uint16_t>(32768));
    }
    nav::BuildOptions opt;
    opt.threads = 2;
    opt.nav_margin_cm = 3000;
    nav::NavFile file;
    nav::BuildStats stats;
    std::string err;
    if (!nav::build_nav(g, 7, opt, &file, &stats, &err)) {
        std::printf("  [FAIL] the ground under the player: %s\n", err.c_str());
        return nullptr;
    }
    return nav::AreaNav::from_file(file, &err);
}

void play_with_world(Run& r, double seconds) {
    const int n = static_cast<int>(std::llround(seconds * 10));
    for (int i = 0; i < n; ++i) {
        r.tick(false);
        r.h.fx.world->tick_world(r.now());
    }
}

void walk_with_world(Run& r, double cm_per_s, int steps) {
    for (int i = 0; i < steps; ++i) {
        r.tick_at(cm_per_s);
        r.h.fx.world->tick_world(r.now());
    }
}

void verify_go_to_and_leash_moves(Run& r, npc::NpcWorld& npcs, uint64_t pet) {
    const auto where = [&]() -> std::pair<double, double> {
        const world::WorldActor* a = r.h.fx.world->registry().find(pet);
        return a ? std::make_pair(a->x, a->y) : std::make_pair(0.0, 0.0);
    };
    const auto metres_between = [](std::pair<double, double> a, std::pair<double, double> b) { return std::hypot(a.first - b.first, a.second - b.second) / 100.0; };
    const auto owner_at = [&]() { return std::make_pair(r.x, kY); };

    const repl::Vec3d far_place{kX, kY + 40000.0, kZ - 88.0};
    const uint64_t moves = npcs.stats().moves_started;
    r.send_reliable(pet_command_bunch(pets::kGoTo, &far_place));
    play_with_world(r, 0.3);
    CHECK(r.h.s->pet_following() && npcs.stats().moves_started == moves, "Go To 400 m away: refused, the pet still follows and no walk is started");
    const repl::Vec3d nowhere{std::nan(""), kY, kZ - 88.0};
    r.send_reliable(pet_command_bunch(pets::kGoTo, &nowhere));
    play_with_world(r, 0.3);
    CHECK(r.h.s->pet_following() && npcs.stats().moves_started == moves, "Go To a place that is no number: refused too");

    const repl::Vec3d near_place{kX, kY + 3000.0, kZ - 88.0};
    r.send_reliable(pet_command_bunch(pets::kGoTo, &near_place));
    play_with_world(r, 0.3);
    CHECK(!r.h.s->pet_following() && npcs.stats().moves_started == moves + 1, "Go To 30 m away: the pet sets off, and no longer follows");
    play_with_world(r, 6.0);
    CHECK(metres_between(where(), {kX, kY + 3000.0}) < 3.0, "... and it stands at the place");
    CHECK(!r.h.s->pet_following(), "... where it stays");

    const auto stood = where();
    walk_with_world(r, 500.0, 90);
    CHECK(metres_between(stood, owner_at()) > 40.0, "(the owner is more than 40 m from where the pet was sent)");
    play_with_world(r, 1.0);
    CHECK(r.h.s->pet_following() && metres_between(where(), owner_at()) < 10.0, "the leash brings the sent pet back to its owner, and it follows again");

    r.send_reliable(pet_command_bunch(pets::kStay, nullptr));
    play_with_world(r, 0.5);
    CHECK(!r.h.s->pet_following(), "Stay: the pet no longer follows");
    const auto stays = where();
    walk_with_world(r, 500.0, 60);
    CHECK(metres_between(stays, owner_at()) > 25.0 && metres_between(stays, owner_at()) < 40.0, "(the owner walked 30 m from the stayed pet)");
    CHECK(!r.h.s->pet_following() && metres_between(where(), stays) < 1.0, "within 40 m a stayed pet stays where it is");
    walk_with_world(r, 500.0, 30);
    play_with_world(r, 1.0);
    CHECK(metres_between(stays, owner_at()) > 40.0, "(the owner is more than 40 m from the stayed pet)");
    CHECK(r.h.s->pet_following() && metres_between(where(), owner_at()) < 10.0, "the leash brings the stayed pet back to its owner, and it follows again");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "all owner replication updates read successfully");
}

void test_summoner_pet_go_to_and_leash() {
    std::printf("Summoner pets: Go To within the ability's range, the commands of a refused ability, and the leash in every mode\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    bool ground = false;
    Run r(0, true, &rec, [&](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        auto nav = ground_under_the_player();
        ground = nav && world.adopt_ground(std::move(nav), error);
        CHECK(ground, "the ground under the player is adopted");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    });
    CHECK(ground && r.h.fx.world->npcs() != nullptr, "(a world with ground under the player)");
    if (!ground || !r.h.fx.world->npcs()) return;
    npc::NpcWorld& npcs = *r.h.fx.world->npcs();
    send_ready_to_phase_in(r);
    play_with_world(r, 1.0);
    const pets::Definition& guardian = pets::kDefinitions[0];
    r.h.s->teach_ability(guardian.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(guardian.ability);
    play_with_world(r, 8.8);
    const uint64_t pet = r.h.s->summoned_pet();
    CHECK(pet != 0, "the Guardian comes");
    if (!pet) return;
    play_with_world(r, 3.0);
    const world::WorldActor* a = r.h.fx.world->registry().find(pet);
    const double dist_m = a ? std::hypot(a->x - r.x, a->y - kY) / 100.0 : 999.0;
    CHECK(r.h.s->pet_following() && dist_m < 6.0, "the pet follows its owner and stands near");
    verify_go_to_and_leash_moves(r, npcs, pet);
}

void test_summoner_pet_command_needs_its_ability() {
    std::printf("Summoner pets: the pet obeys a command only when the pipeline took the ability\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    Run r(0, true, &rec, [&](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    });
    send_ready_to_phase_in(r);
    r.play(1.0, false);
    const pets::Definition& guardian = pets::kDefinitions[0];
    r.h.s->teach_ability(guardian.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(guardian.ability);
    r.play(8.8, false);
    CHECK(r.h.s->summoned_pet() != 0 && r.h.s->pet_following(), "the Guardian comes, and follows");
    constexpr int64_t kStayAbility = 6064633576956297816ll, kFollowAbility = 6064633576846066210ll;
    const int32_t first_stay = r.trigger(kStayAbility);
    r.play(1.5, false);
    CHECK(!r.h.s->pet_following(), "Stay, taken: the pet no longer follows");
    r.trigger(kFollowAbility);
    r.play(1.5, false);
    CHECK(r.h.s->pet_following(), "Follow, taken: it follows again");
    r.send_trigger(kStayAbility, first_stay);
    r.play(1.5, false);
    CHECK(r.h.s->pet_following(), "Stay, refused as a replay: the pet is not told to stay");
    r.trigger(kStayAbility);
    r.play(1.5, false);
    CHECK(!r.h.s->pet_following(), "Stay, taken again: the pet stays");
    r.trigger(kFollowAbility);
    r.play(1.5, false);
    CHECK(r.h.s->pet_following(), "(it follows)");
    r.trigger(kStayAbility);
    r.play(0.5, false);
    CHECK(!r.h.s->pet_following(), "Stay, taken: the pet stays");
    r.trigger(kFollowAbility);
    r.play(0.2, false);
    CHECK(!r.h.s->pet_following(), "Follow, refused on the commands' cooldown: the pet is not told to follow");
}

BunchWriter trigger_naming(const bs::PlayerSetup& setup, int64_t guid, int32_t id, const emit::FIntrepidNetworkGUID* target) {
    const schema::RpcSchema* rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    if (ok && target) {
        repl::ArrayValue targets;
        repl::ElementState e;
        e.values[1] = repl::Value(*target);
        targets.elements.push_back(std::move(e));
        ok = repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    }
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (!ok) {
        std::printf("  [FAIL] the trigger's parameters do not fit\n");
        return BunchWriter();
    }
    return ability_rpc(setup.guids.ability, "ServerTriggerAbility", p);
}

AbilityPipeline& pipeline_of(Run& r) { return *const_cast<AbilityPipeline*>(r.h.s->pipeline()); }

size_t cooldown_items_of(const Run& r, int64_t ability) {
    size_t n = 0;
    for (const auto& item : r.m.cooldowns) n += item.second == ability ? 1 : 0;
    return n;
}

struct PetTargetWorld {
    uint64_t wolf = 0, second_wolf = 0, friendly = 0;
    uint64_t next_actor_id = 5000000;
    bool     ground = false;
    void prepare(PlayerWorld& w) {
        std::string err;
        if (!w.load_blueprints(NPC_MOTION_PATH, err)) {
            std::printf("  [FAIL] the Blueprint table: %s\n", err.c_str());
            return;
        }
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
        ground = nav && w.adopt_ground(std::move(nav), err);
        if (!ground) {
            std::printf("  [FAIL] the ground: %s\n", err.c_str());
            return;
        }
        spawns::AreaConfig area;
        area.radius_cm = 1;
        if (!w.load_area(area, err)) std::printf("  [FAIL] the world's actor templates: %s\n", err.c_str());
        auto captured = spawns::CapturedData::load(err);
        if (!captured) {
            std::printf("  [FAIL] the captured data: %s\n", err.c_str());
            return;
        }
        std::string wolf_path, friend_path;
        for (const auto& [p, row] : w.blueprints().rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = p;
            if (p.find("Quest_NPC_Master.Default__Quest_NPC_Master_C") != std::string::npos) friend_path = p;
        }
        wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, wolf_path, kX + 120, kY, kZ, 8, 0);
        second_wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, wolf_path, kX + 120, kY + 100, kZ, 8, 0);
        if (!friend_path.empty()) friendly = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, friend_path, kX + 200, kY, kZ, 8, 0);
    }
    bool ok() const { return ground && wolf != 0 && second_wolf != 0 && friendly != 0; }
};

void verify_pet_ability_targets_exec(Run& r, const PetTargetWorld& pw, combat::CombatWorld& cw, uint64_t pet) {
    const auto guid_of = [&](uint64_t actor) {
        const world::WorldActor* a = r.h.fx.world->registry().find(actor);
        return a ? a->actor.guid : emit::FIntrepidNetworkGUID{};
    };
    const emit::FIntrepidNetworkGUID wolf_guid = guid_of(pw.wolf), friend_guid = guid_of(pw.friendly), dead_guid = guid_of(pw.second_wolf);
    emit::FIntrepidNetworkGUID missing_guid = wolf_guid;
    missing_guid.ObjectId = 987654321;
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const int64_t taunt = pets::kGuardianTaunt;
    const int64_t stomp = 6064633577427633586ll;
    r.h.s->set_current_value(pets::kEssence, 100.0f);
    r.play(0.2, false);
    const float full = r.m.stats[pets::kEssence];

    const auto refused_at = [&](const char* what, const emit::FIntrepidNetworkGUID* named) {
        r.send_reliable(trigger_naming(setup, taunt, r.next_id++, named));
        r.play(0.3, false);
        CHECKF(std::abs(r.m.stats[pets::kEssence] - full) < 1.0f, "Taunt at %s costs no Essence (%.1f of %.1f left)", what, r.m.stats[pets::kEssence], full);
        CHECKF(cooldown_items_of(r, taunt) == 0 && !cw.pet_casting(pet), "Taunt at %s starts no cooldown and no cast", what);
    };
    refused_at("a friendly questgiver", &friend_guid);
    CHECK(cw.health_of(pw.friendly) == -1.0, "... and makes no combatant of the questgiver");
    refused_at("a dead Wolf", &dead_guid);
    refused_at("an actor that is not there", &missing_guid);
    refused_at("the caster itself", &setup.guids.collision_cylinder);

    r.send_reliable(trigger_naming(setup, taunt, r.next_id++, &wolf_guid));
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[pets::kEssence] - (full - 15.0f)) < 1.5f, "Taunt at a hostile Wolf takes its 15 Essence, once");
    CHECK(cooldown_items_of(r, taunt) == 1, "... and starts its cooldown, once");
    for (int i = 0; i < 100 && cw.pet_casting(pet); ++i) play_with_world(r, 0.1);
    std::printf("  [info] after Taunt: the pet casts %d, the Wolf holds %zu effect(s), Demoralized %d, Health %.0f of %.0f\n", cw.pet_casting(pet) ? 1 : 0, cw.effect_count(pw.wolf),
                cw.effect_stacks(pw.wolf, 6064630758760448234ll), cw.health_of(pw.wolf), cw.max_health_of(pw.wolf));
    CHECK(cw.effect_stacks(pw.wolf, 6064630758760448234ll) == 1, "... and the Wolf is demoralized");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "all owner replication updates read successfully");

    r.h.s->set_current_value(pets::kEssence, 100.0f);
    r.play(0.2, false);
    const pets::Definition& guardian = pets::kDefinitions[0];
    pipeline_of(r).set_known({guardian.ability});
    const float before_unknown = r.m.stats[pets::kEssence];
    r.trigger(stomp);
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[pets::kEssence] - before_unknown) < 1.0f && cooldown_items_of(r, stomp) == 0, "Stomp, not learned, costs nothing and starts no cooldown");
    pipeline_of(r).set_known({guardian.ability, stomp});
    r.trigger(stomp);
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[pets::kEssence] - (before_unknown - 30.0f)) < 1.5f, "... and learned, it takes its 30 Essence");
}

void test_summoner_pet_ability_targets() {
    std::printf("Summoner pets: a cast at the wrong side or at what is not there costs nothing\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    PetTargetWorld pw;
    Run r(0, true, &rec, [&](PlayerWorld& world) { pw.prepare(world); });
    CHECK(pw.ok() && r.h.fx.world->combat() != nullptr, "(a world with fights, two Wolves and a friendly questgiver)");
    if (!pw.ok() || !r.h.fx.world->combat()) return;
    combat::CombatWorld& cw = *r.h.fx.world->combat();
    send_ready_to_phase_in(r);
    r.play(1.0, false);
    const pets::Definition& guardian = pets::kDefinitions[0];
    r.h.s->teach_ability(guardian.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(guardian.ability);
    r.play(8.8, false);
    const uint64_t pet = r.h.s->summoned_pet();
    CHECK(pet != 0, "the Guardian comes");
    if (!pet) return;
    std::string why;
    CHECK(cw.gm_kill(pw.second_wolf, 1.0, why), "(the second Wolf is dead)");
    verify_pet_ability_targets_exec(r, pw, cw, pet);
}

void test_summoner_pet_healing_targets() {
    std::printf("Summoner pets: the Mystic's help reaches its owner, not an enemy\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    PetTargetWorld pw;
    Run r(0, true, &rec, [&](PlayerWorld& world) { pw.prepare(world); });
    CHECK(pw.ok() && r.h.fx.world->combat() != nullptr, "(a world with fights, two Wolves and a friendly questgiver)");
    if (!pw.ok() || !r.h.fx.world->combat()) return;
    send_ready_to_phase_in(r);
    r.play(1.0, false);
    const pets::Definition& mystic = pets::kDefinitions[2];
    r.h.s->teach_ability(mystic.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(mystic.ability);
    r.play(8.8, false);
    CHECK(r.h.s->pet_definition() == &mystic, "the Mystic comes");
    const world::WorldActor* wolf = r.h.fx.world->registry().find(pw.wolf);
    CHECK(wolf != nullptr, "(the Wolf is in the world)");
    if (!wolf) return;
    const emit::FIntrepidNetworkGUID wolf_guid = wolf->actor.guid;
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const int64_t recreate = 6064633577432614330ll;
    r.h.s->set_current_value(pets::kEssence, 100.0f);
    r.play(0.2, false);
    const float full = r.m.stats[pets::kEssence];
    r.send_reliable(trigger_naming(setup, recreate, r.next_id++, &wolf_guid));
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[pets::kEssence] - full) < 1.0f && cooldown_items_of(r, recreate) == 0, "Recreate at an enemy costs no Essence and starts no cooldown");
    r.send_reliable(trigger_naming(setup, recreate, r.next_id++, &setup.guids.collision_cylinder));
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[pets::kEssence] - (full - 20.0f)) < 1.5f, "Recreate at its owner takes its 20 Essence, once");
}

void test_summoner_pet_automation_needs_the_ability() {
    std::printf("Summoner pets: the auto mode's toggle is for a character that has learned it\n");
    {
        const svc::Character mage = class25(8, "Mage", 17750);
        Run r(0, true, &mage);
        r.play(0.5, false);
        CHECK(r.h.s->pipeline() && !r.h.s->pipeline()->known(pets::kAutomationAbility), "(a Mage has not learned the toggle)");
        r.trigger(pets::kAutomationAbility);
        r.play(0.3, false);
        CHECK(!r.h.s->pet_automation() && !r.m.has_effect(pets::kAutomationEffect), "a Mage cannot turn the pet's auto mode on");
    }
    {
        const svc::Character rec = class25(6, "Summoner", 17753);
        Run r(0, true, &rec);
        r.play(0.5, false);
        CHECK(r.h.s->pipeline() && r.h.s->pipeline()->known(pets::kAutomationAbility), "(a Summoner knows the toggle without being taught)");
        r.trigger(pets::kAutomationAbility);
        r.play(0.3, false);
        CHECK(r.h.s->pet_automation() && r.m.has_effect(pets::kAutomationEffect), "a Summoner turns it on");
    }
}

const repl::ArrayValue* summoned_pet_array(const PlayerSession& s) {
    for (const auto& component : s.bootstrap().pawn().subobjects) {
        if (component.name != "InteractInfo" || !component.obj.schema) continue;
        const auto* value = component.obj.state.get(*component.obj.schema, "PetArray");
        return value ? value->get_if<repl::ArrayValue>() : nullptr;
    }
    return nullptr;
}

void verify_summoned_pet_alignments(const Run& r, const world::WorldActor& pet) {
    const repl::AlignmentContainer* inherited = nullptr;
    const repl::AlignmentContainer* own = nullptr;
    for (const auto& s : pet.actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UAlignmentComponent") continue;
        const auto* i = s.obj.state.get(*s.obj.schema, "InheritedAlignments");
        const auto* a = s.obj.state.get(*s.obj.schema, "Alignments");
        inherited = i ? i->get_if<repl::AlignmentContainer>() : nullptr;
        own = a ? a->get_if<repl::AlignmentContainer>() : nullptr;
    }
    const repl::Subobject* owner_alignment = r.h.s->bootstrap().pawn().find(r.h.s->bootstrap().setup().guids.alignment);
    const repl::Value* owner_value = owner_alignment ? owner_alignment->obj.state.get(*owner_alignment->obj.schema, "Alignments") : nullptr;
    const auto* owner_set = owner_value ? owner_value->get_if<repl::AlignmentContainer>() : nullptr;
    CHECK(inherited && owner_set && *inherited == *owner_set, "the pet inherits its owner's alignments");
    const auto& words = r.h.s->bootstrap().setup().character_guid;
    const repl::GuidValue owner{words[0], words[1], words[2], words[3]};
    bool names_owner = false;
    if (own) {
        for (const auto& inst : own->instances) {
            if (inst.key == bootstrap::kAlignmentKeyCharacter) names_owner = names_owner || std::find(inst.guids.begin(), inst.guids.end(), owner) != inst.guids.end();
        }
    }
    CHECK(names_owner, "the pet's Instigator row names its owner");
}

bool verify_summon_each_pet(Run& r, const pets::Definition& d, uint64_t& previous) {
    r.h.s->teach_ability(d.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(d.ability);
    r.play(7.0, false);
    CHECK(r.h.s->summoned_pet() == previous, "a new pet is not created before completion");
    r.play(1.8, false);
    const uint64_t id = r.h.s->summoned_pet();
    const auto* pet = r.h.fx.world->registry().find(id);
    CHECK(id && id != previous && pet, "completion creates the pet actor");
    CHECK(!previous || !r.h.fx.world->registry().find(previous), "replacement removes the previous pet");
    if (!pet) return false;
    CHECK(pet->npc.archetype == d.archetype && pet->npc.level == 25 && pet->never_dormant, "the pet has the right Blueprint and owner level and stays awake");
    const auto& object = pet->actor.obj;
    const auto* instigator = object.state.get(*object.schema, "Instigator");
    CHECK(instigator && instigator->get_if<emit::FIntrepidNetworkGUID>() && *instigator->get_if<emit::FIntrepidNetworkGUID>() == r.h.s->bootstrap().setup().guids.pawn,
          "the pet names its summoner as instigator");
    const auto* array = summoned_pet_array(*r.h.s);
    CHECK(array && array->elements.size() == 1, "the player's replicated PetArray contains one pet");
    if (array && array->elements.size() == 1) {
        const auto& values = array->elements[0].values;
        const auto ptr = values.find(1);
        CHECK(ptr != values.end() && ptr->second.get_if<emit::FIntrepidNetworkGUID>() && *ptr->second.get_if<emit::FIntrepidNetworkGUID>() == pet->actor.guid, "PetArray points to the spawned actor");
    }
    CHECK(r.m.has_effect(d.effect), "the client received the pet's summon effect");
    size_t on_bar = 0, the_players = 0;
    for (const repl::ActionbarSlot& s : r.h.s->action_bar().slots()) {
        if (s.bank_index != repl::ActionbarBank::Pet) continue;
        ++on_bar;
        if (s.use.instigator == repl::ActionbarInstigator::Player) ++the_players;
    }
    CHECK(on_bar > 0 && the_players == on_bar, "every slot of the pet bar names the Player as its instigator");
    verify_summoned_pet_alignments(r, *pet);
    CHECK(r.h.s->actor_view()->channel_of(id) != 0, "the pet opened on an actor channel for the client");
    previous = id;
    return true;
}

void verify_pet_session_commands_and_stances(Run& r, uint64_t previous) {
    r.h.s->set_current_value(kManaStat, 10.0f);
    r.trigger(pets::kDefinitions[0].ability);
    r.play(8.8, false);
    CHECK(r.h.s->summoned_pet() == previous && r.h.fx.world->registry().find(previous), "a summon the Summoner cannot pay for leaves the current pet");
    CHECK(r.h.s->pet_attack_ability() == pets::kDefinitions[2].attack, "the session finds the attack of the pet in the world");
    const auto cast = [&](int64_t ability) {
        r.h.s->teach_ability(ability);
        r.trigger(ability);
        r.play(1.0, false);
    };
    CHECK(r.h.s->pet_following() && r.h.s->pet_stance() == pets::Stance::Aggressive, "a new pet follows, aggressive");
    const float essence = r.m.stats.count(6064633668368219158ll) ? r.m.stats[6064633668368219158ll] : 0.0f;
    r.trigger(6064633577427633586ll);
    r.play(0.3, false);
    const float after_refusal = r.m.stats.count(6064633668368219158ll) ? r.m.stats[6064633668368219158ll] : 0.0f;
    CHECK(after_refusal == essence, "another pet's ability costs nothing");
    cast(6064633576956297816ll);
    CHECK(!r.h.s->pet_following(), "Stay, cast: the pet no longer follows");
    cast(6064633576846066210ll);
    CHECK(r.h.s->pet_following(), "Follow, cast: it follows again");
    cast(6064633724850470923ll);
    const auto owner_stance = [&]() -> int64_t {
        for (const auto& component : r.h.s->bootstrap().pawn().subobjects) {
            if (component.name != "InteractInfo" || !component.obj.schema) continue;
            const auto* value = component.obj.state.get(*component.obj.schema, "PetStance");
            const uint64_t* stance = value ? value->get_if<uint64_t>() : nullptr;
            return stance ? static_cast<int64_t>(*stance) : -1;
        }
        return -1;
    };
    CHECK(r.h.s->pet_stance() == pets::Stance::Passive && owner_stance() == 3, "the Passive stance, cast, is on the owner's InteractInfo");
    cast(6064633724849553418ll);
    CHECK(r.h.s->pet_stance() == pets::Stance::Defensive && owner_stance() == 2, "the Defensive stance, cast, is on the owner's InteractInfo");
    cast(6064633724848504841ll);
    CHECK(r.h.s->pet_stance() == pets::Stance::Aggressive && owner_stance() == 1, "the Aggressive stance, cast, is on the owner's InteractInfo");
    CHECK(r.h.s->summoned_pet() == previous, "the commands leave the pet in the world");
}

void verify_pet_session_dismiss_and_death(Run& r, const svc::Character& rec, uint64_t previous) {
    repl::RpcParams params;
    const auto& pawn = class_named("PlayerPawn_C");
    const auto* command = pawn.rpc("ServerSendPetCommand");
    CHECK(command && repl::set_rpc_param(*command, params, "PetCommandId", repl::Value::i64(pets::kDismiss)), "the dismiss command writes");
    r.send_reliable(actor_bunch(pawn, "ServerSendPetCommand", params));
    r.play(0.5, false);
    CHECK(!r.h.s->summoned_pet() && !r.h.fx.world->registry().find(previous), "dismiss removes the pet from the world");
    const auto* array = summoned_pet_array(*r.h.s);
    CHECK(array && array->elements.empty(), "dismiss clears the player's replicated PetArray");
    CHECK(!r.m.has_effect(pets::kDefinitions[2].effect), "dismiss clears the client's summon effect");

    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(pets::kDefinitions[0].ability);
    r.play(8.8, false);
    const uint64_t final_pet = r.h.s->summoned_pet();
    CHECK(final_pet != 0, "a dismissed pet can be summoned again");
    r.h.s->finish("pet cleanup test");
    CHECK(!r.h.fx.world->registry().find(final_pet), "logout removes the summoned pet");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "all owner replication updates read successfully");

    const auto prepare = [](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    };
    Run death(0, true, &rec, prepare);
    death.h.s->teach_ability(pets::kDefinitions[0].ability);
    death.play(0.5, false);
    death.trigger(pets::kDefinitions[0].ability);
    death.play(8.8, false);
    const uint64_t dead_pet = death.h.s->summoned_pet();
    CHECK(dead_pet != 0, "the death test starts with a summoned pet");
    death.h.s->take_damage(death.h.conn, 1.0e9, death.h.t);
    death.play(0.5, false);
    CHECK(!death.h.s->summoned_pet() && !death.h.fx.world->registry().find(dead_pet), "the owner's death removes the pet");
    CHECK(!death.m.has_effect(pets::kDefinitions[0].effect), "the owner's death clears the summon effect");
}

void test_summoner_pet_session() {
    std::printf("Summoner pets: cast completion creates and replicates the owned pet\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    const auto prepare = [](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    };
    Run r(0, true, &rec, prepare);
    send_ready_to_phase_in(r);
    r.play(1.0, false);
    uint64_t previous = 0;
    for (const auto& d : pets::kDefinitions) {
        if (!verify_summon_each_pet(r, d, previous)) return;
    }
    verify_pet_session_commands_and_stances(r, previous);
    verify_pet_session_dismiss_and_death(r, rec, previous);
}

void test_summoner_hunter_stance() {
    std::printf("Summoner pets: the Hunter's stance swaps with its attack\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    const auto prepare = [](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    };
    Run r(0, true, &rec, prepare);
    send_ready_to_phase_in(r);
    r.play(1.0, false);
    const pets::Definition& hunter = pets::kDefinitions[1];
    r.h.s->teach_ability(hunter.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(hunter.ability);
    r.play(8.8, false);
    CHECK(r.h.s->summoned_pet() != 0, "the Hunter comes");
    CHECK(r.h.s->pet_attack_ability() == hunter.attack && !r.h.s->hunter_ranged(), "it comes in melee");
    CHECK(r.m.has_effect(pets::kHunterRefMelee), "its owner holds the melee reference");
    r.h.s->teach_ability(pets::kHunterStanceSwap);
    r.trigger(pets::kHunterStanceSwap);
    r.play(0.3, false);
    CHECK(r.h.s->hunter_ranged() && r.h.s->pet_attack_ability() == pets::kHunterRangedAttack, "the swap gives it its ranged attack");
    CHECK(r.m.has_effect(pets::kHunterRefRanged) && !r.m.has_effect(pets::kHunterRefMelee), "the reference follows");
    r.trigger(pets::kHunterStanceSwap);
    r.play(0.3, false);
    CHECK(!r.h.s->hunter_ranged() && r.h.s->pet_attack_ability() == hunter.attack, "a second swap brings it back");
}

struct WolfWorld {
    uint64_t wolf = 0;
    uint64_t next_actor_id = 5000000;
    bool     ground = false;
    int      level = 8;
    void prepare(PlayerWorld& w) {
        std::string err;
        if (!w.load_blueprints(NPC_MOTION_PATH, err)) return;
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
        ground = nav && w.adopt_ground(std::move(nav), err);
        auto captured = spawns::CapturedData::load(err);
        if (!ground || !captured) return;
        std::string path;
        for (const auto& [p, row] : w.blueprints().rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) path = p;
        }
        wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + 120, kY, kZ, level, 800);
    }
};

void verify_summoner_pet_auto_mystic(Run& r) {
    const int64_t kEssence = 6064633668368219158ll;
    CHECK(!r.h.s->pet_automation(), "the auto mode starts off");
    r.h.s->teach_ability(pets::kAutomationAbility);
    r.trigger(pets::kAutomationAbility);
    r.play(0.2, false);
    CHECK(r.h.s->pet_automation() && r.m.has_effect(pets::kAutomationEffect), "cast, it turns on");
    r.trigger(pets::kAutomationAbility);
    r.play(0.2, false);
    CHECK(!r.h.s->pet_automation() && !r.m.has_effect(pets::kAutomationEffect), "cast again, it turns off");

    const pets::Definition& mystic = pets::kDefinitions[2];
    r.h.s->teach_ability(mystic.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(mystic.ability);
    r.play(8.8, false);
    CHECK(r.h.s->pet_definition() == &mystic, "(the Mystic comes)");
    r.trigger(pets::kAutomationAbility);
    r.h.s->set_current_value(kEssence, 80.0f);
    r.play(1.2, false);
    const float rested = r.m.stats[kEssence];
    CHECK(rested >= 79.0f, "at full Health it spends nothing");
    r.h.s->set_current_value(content::kGuidHealth, r.m.stats[content::kGuidMaxHealth] * 0.5f);
    r.play(1.0, false);
    std::printf("  [info] auto Recreate: Essence %.1f -> %.1f\n", rested, r.m.stats[kEssence]);
    CHECK(r.m.stats[kEssence] < rested - 10.0f, "below 80% Health the Mystic casts Recreate by itself");
}

void test_summoner_pet_ability_session() {
    std::printf("Summoner pets: an ability cast from the bar pays Essence and the pet plays it\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    WolfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld& world) {
        ww.prepare(world);
        std::string error;
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    });
    CHECK(ww.ground && r.h.fx.world->combat() != nullptr, "(a world with fights)");
    if (!ww.ground || !r.h.fx.world->combat()) return;
    send_ready_to_phase_in(r);
    r.play(1.0, false);
    const pets::Definition& hunter = pets::kDefinitions[1];
    r.h.s->teach_ability(hunter.ability);
    r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]);
    r.trigger(hunter.ability);
    r.play(8.8, false);
    CHECK(r.h.s->summoned_pet() != 0, "the Hunter comes");
    const int64_t kEssence = 6064633668368219158ll;
    const int64_t eviscerate = 6064633577430779319ll;
    r.h.s->set_current_value(kEssence, 100.0f);
    r.play(0.2, false);
    const float before = r.m.stats[kEssence];
    const float mana = r.m.stats[kManaStat];
    r.trigger(eviscerate);
    r.play(0.3, false);
    CHECK(std::abs(r.m.stats[kEssence] - (before - 30.0f)) < 2.0f, "Eviscerate takes 30 Essence");
    CHECK(r.m.stats[kManaStat] >= mana, "the character pays no Mana for its pet's ability");
    const auto& bar = r.h.s->action_bar();
    const auto* first = bar.at(repl::ActionbarBank::Pet, 0);
    CHECK(first && first->use_type == repl::ActionbarUseType::PetCommand && pets::pet_of_ability(first->ability_id) == hunter.effect, "the pet bar's first slot holds one of the Hunter's abilities");
    bool has_follow = false;
    for (int i = 0; i < 12; ++i) {
        const auto* s = bar.at(repl::ActionbarBank::Pet, i);
        has_follow = has_follow || (s && s->pet_command_id == pets::kFollow);
    }
    CHECK(has_follow, "... and Follow is on it");
    r.play(3.0, false);
    r.h.s->set_current_value(kEssence, 5.0f);
    r.play(0.2, false);
    const float little = r.m.stats[kEssence];
    r.trigger(eviscerate);
    r.play(0.3, false);
    CHECK(r.m.stats[kEssence] >= little, "with too little Essence nothing is paid");
    r.play(3.0, false);
    verify_summoner_pet_auto_mystic(r);
}

void test_summoner_spawn_failure() {
    const svc::Character rec = class25(6, "Summoner", 17753);
    Run r(0, true, &rec);
    const auto& d = pets::kDefinitions[0];
    r.h.s->teach_ability(d.ability);
    r.play(0.5, false);
    r.trigger(d.ability);
    r.play(8.8, false);
    CHECK(!r.h.s->summoned_pet() && !r.m.has_effect(d.effect), "a missing pet template leaves no phantom pet effect");
    CHECK(!r.h.s->abilities_failed(), "a missing pet template does not disable abilities");
}

}  // namespace

void run_ability_session_pets_tests() {
    test_ability_log_window();
    if (!ability_rig::real_db_present()) return;
    test_summoner_pet_go_to_and_leash();
    test_summoner_pet_command_needs_its_ability();
    test_summoner_pet_ability_targets();
    test_summoner_pet_healing_targets();
    test_summoner_pet_automation_needs_the_ability();
    test_summoner_pet_session();
    test_summoner_hunter_stance();
    test_summoner_pet_ability_session();
    test_summoner_spawn_failure();
}
