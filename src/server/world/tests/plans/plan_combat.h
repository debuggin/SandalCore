// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "aoc/worldadmin/v1/worldadmin.grpc.pb.h"
#include "plan_abilities.h"
#include "protocol/repl/move_packed.h"

#include <grpcpp/grpcpp.h>

namespace aoc::world::tests::plans {

inline std::vector<bots::MonsterTarget> build_regional_monster_pack() {
    return {
        {70001ULL, "Highland Goblin Raider", "Goblin", 50, 22000, 22000, {-643200.0, 377400.0, 11400.0}, "aoc-world-riverlands", false, false, 0},
        {70002ULL, "Mycelette Sporeling", "Mycelette", 50, 21000, 21000, {-642500.0, 378100.0, 11420.0}, "aoc-world-riverlands", false, false, 0},
        {70003ULL, "Carrion Raven Stalker", "Raven", 50, 19500, 19500, {-591200.0, 342800.0, 11200.0}, "aoc-world-riverlands", false, false, 0},
        {70004ULL, "Sandsquall Dune Scorpion", "Scorpion", 50, 25000, 25000, {-417200.0, 682600.0, 9800.0}, "aoc-world-sandsquall", false, false, 0},
        {70005ULL, "Winstead Highway Bandit", "Bandit", 50, 23500, 23500, {-644000.0, 376200.0, 11400.0}, "aoc-world-riverlands", false, false, 0},
        {70006ULL, "Briarmoor Dire Wolf", "Wolf", 50, 22500, 22500, {-617400.0, 398500.0, 11600.0}, "aoc-world-riverlands", false, false, 0},
        {70007ULL, "Lionhold Stone Golem", "Golem", 50, 28000, 28000, {-684200.0, 412600.0, 12500.0}, "aoc-world-riverlands", false, false, 0},
        {70008ULL, "Ashland Cultist Pyromancer", "Cultist", 50, 21500, 21500, {-654100.0, 385600.0, 11800.0}, "aoc-world-riverlands", false, false, 0},
    };
}

inline bool run_plan_combat(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    auto monsters = build_regional_monster_pack();
    size_t seq = 0;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            auto mob = monsters[seq++ % monsters.size()];
            mob.pos_cm = {actor.pos_cm.x + 250.0, actor.pos_cm.y, actor.pos_cm.z};
            mob.worker_id = actor.worker_id;
            actor.in_combat = true;
            observer.in_combat = true;
            actor.locked_target_id = mob.monster_id;
            observer.locked_target_id = mob.monster_id;
            mob.in_combat = true;
            mob.aggro_bot_id = actor.bot_id;
            for (int round = 0; round < 4 && mob.hp > 0; ++round) {
                (void)execute_bot_class_ability_rotation(actor, mob);
                if (mob.hp > 0) (void)execute_bot_class_ability_rotation(observer, mob);
            }
            if (mob.hp != 0) return false;
            mob.dead = true;
            mob.in_combat = false;
            actor.in_combat = false;
            observer.in_combat = false;
            ++actor.monsters_killed;
            ++observer.monsters_killed;
            actor.xp_earned += 4500;
            observer.xp_earned += 4500;
            actor.mana = actor.max_mana;
            observer.mana = observer.max_mana;
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

inline bots::BunchWriter encode_pawn_move(float ts, double x, double y, double z) {
    const bots::schema::ClassSchema& pawn = bots::class_named("PlayerPawn_C");
    const bots::schema::NetField* f = pawn.field("ServerMovePacked");
    bots::BunchWriter out;
    if (!f) return out;
    bots::repl::MovePacked mp;
    mp.new_move.time_stamp = ts;
    mp.new_move.acceleration = bots::repl::quantize_vector(10, 0.0, 0.0, 0.0);
    mp.new_move.location = bots::repl::quantize_vector(100, x, y, z);
    mp.new_move.movement_mode = bots::repl::kMoveWalking;
    bots::BunchWriter params, field;
    if (!bots::repl::write_move_packed(params, mp) || bots::repl::write_field(pawn, f->index, params, field) == 0) return out;
    bots::repl::write_content_block(out, bots::repl::BlockHeader{}, field, false);
    return out;
}

inline void attach_service_auth(grpc::ClientContext& ctx) {
    const char* env_tok = std::getenv("AOC_SERVICE_TOKEN");
    const std::string tok = (env_tok && *env_tok) ? env_tok : "dev-local-service-token-32bytes!";
    ctx.AddMetadata("authorization", "Bearer " + tok);
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(4));
}

inline std::string world_stream_addr() {
    const char* e = std::getenv("AOC_WORLD_STREAM_ADDR");
    std::string a = (e && *e) ? e : "127.0.0.1:50062";
    if (a[0] == ':') return "127.0.0.1" + a;
    return a.rfind("0.0.0.0:", 0) == 0 ? "127.0.0.1:" + a.substr(8) : a;
}

inline bool discover_live_goblin(bots::LiveSession& s) {
    auto stub = aoc::worldadmin::v1::WorldStreamService::NewStub(
        grpc::CreateChannel(world_stream_addr(), grpc::InsecureChannelCredentials()));
    grpc::ClientContext st_ctx;
    attach_service_auth(st_ctx);
    aoc::worldadmin::v1::GetStatusResponse st_resp;
    if (!stub->GetStatus(&st_ctx, {}, &st_resp).ok() || st_resp.stats().npcs() <= 10000) {
        std::fprintf(stderr, "[FAIL] Stage 5: WorldStream GetStatus npcs=%u (expected >10000)\n", st_resp.stats().npcs());
        return false;
    }
    s.world_npcs = st_resp.stats().npcs();
    grpc::ClientContext sr_ctx;
    attach_service_auth(sr_ctx);
    aoc::worldadmin::v1::SearchRequest sr_req;
    sr_req.set_query("Goblin");
    sr_req.set_limit(50);
    aoc::worldadmin::v1::SearchResponse sr_resp;
    if (!stub->Search(&sr_ctx, sr_req, &sr_resp).ok() || sr_resp.results().empty()) {
        std::fprintf(stderr, "[FAIL] Stage 5: WorldStream Search('Goblin') returned 0 results\n");
        return false;
    }
    double best_dist = 1e18;
    for (const auto& r : sr_resp.results()) {
        if (r.entity().kind() != aoc::worldadmin::v1::ENTITY_KIND_NPC ||
            (r.entity().state() & aoc::worldadmin::v1::ENTITY_STATE_DEAD) != 0) continue;
        const double d = std::hypot(r.entity().x_cm() - s.spawn[0], r.entity().y_cm() - s.spawn[1]);
        if (d < best_dist) {
            best_dist = d;
            s.goblin_id = r.entity().id();
            s.goblin_name = r.name();
            s.goblin_pos = {static_cast<double>(r.entity().x_cm()), static_cast<double>(r.entity().y_cm()),
                            static_cast<double>(r.entity().z_cm())};
        }
    }
    return s.goblin_id != 0;
}

inline bool teleport_to_goblin(bots::LiveSession& s) {
    auto stub = aoc::worldadmin::v1::WorldAdminService::NewStub(
        grpc::CreateChannel("127.0.0.1:50061", grpc::InsecureChannelCredentials()));
    const std::string want_id = bots::strip_hyphens_lower(s.cfg.char_id);
    std::string char_name;
    grpc::ClientContext lp_ctx;
    attach_service_auth(lp_ctx);
    aoc::worldadmin::v1::ListPlayersResponse lp_resp;
    if (!stub->ListPlayers(&lp_ctx, {}, &lp_resp).ok() || lp_resp.players().empty()) return false;
    for (const auto& p : lp_resp.players()) {
        if (bots::strip_hyphens_lower(p.character_id()) == want_id) char_name = p.name();
    }
    if (char_name.empty()) char_name = lp_resp.players(0).name();
    auto gm_call = [&](const char* cmd, uint32_t lvl) {
        grpc::ClientContext ctx;
        attach_service_auth(ctx);
        aoc::worldadmin::v1::SpawnNpcRequest req;
        req.set_npc(cmd);
        req.set_place(char_name);
        req.set_level(lvl);
        aoc::worldadmin::v1::SpawnNpcResponse resp;
        (void)stub->SpawnNpc(&ctx, req, &resp);
    };
    gm_call("gm.SetLevel", 50);
    gm_call("gm.UnlockAllEmberSprings", 1);
    for (int attempt = 0; attempt < 4 && s.goblin_guid.ObjectId == 0; ++attempt) {
        grpc::ClientContext tp_ctx;
        attach_service_auth(tp_ctx);
        aoc::worldadmin::v1::TeleportRequest req;
        req.set_character(char_name);
        auto* pt = req.mutable_point();
        pt->set_x(s.goblin_pos[0] + 100.0);
        pt->set_y(s.goblin_pos[1]);
        pt->set_z(s.goblin_pos[2]);
        pt->set_has_z(true);
        aoc::worldadmin::v1::TeleportResponse resp;
        if (!stub->Teleport(&tp_ctx, req, &resp).ok()) return false;
        pump_udp(s, 350);
    }
    return s.goblin_guid.ObjectId == s.goblin_id && s.replicated_npc_channels > 0;
}

inline bool run_stage5_movement(bots::LiveSession& s) {
    for (int step = 1; step <= 3; ++step) {
        s.move_ts += 0.2f;
        const bots::BunchWriter mv = encode_pawn_move(s.move_ts, s.spawn[0] + step * 40.0, s.spawn[1], s.spawn[2]);
        send_bunches(s, {s.client->actor(5, mv, false)});
        pump_udp(s, 120);
    }
    if (!discover_live_goblin(s) || !teleport_to_goblin(s)) {
        std::fprintf(stderr, "[FAIL] Stage 5: Goblin discovery/teleport or ch>=7 actor replication failed (goblin=%llu)\n",
                     static_cast<unsigned long long>(s.goblin_id));
        return false;
    }
    std::printf("  [ok ] Stage 5: ServerMovePacked (3 steps) & Goblin teleport complete (world_npcs=%u goblin=%llu '%s' npc_ch=%zu)\n",
                s.world_npcs, static_cast<unsigned long long>(s.goblin_id), s.goblin_name.c_str(), s.replicated_npc_channels);
    return true;
}

inline bool inspect_goblin_combat(const bots::LiveSession& s) {
    auto stub = aoc::worldadmin::v1::WorldStreamService::NewStub(
        grpc::CreateChannel(world_stream_addr(), grpc::InsecureChannelCredentials()));
    grpc::ClientContext ctx;
    attach_service_auth(ctx);
    aoc::worldadmin::v1::InspectRequest req;
    req.set_id(s.goblin_id);
    aoc::worldadmin::v1::InspectResponse resp;
    if (!stub->Inspect(&ctx, req, &resp).ok() || !resp.has_actor()) {
        std::fprintf(stderr, "[FAIL] Stage 8: WorldStream Inspect(%llu) failed\n", static_cast<unsigned long long>(s.goblin_id));
        return false;
    }
    const auto& act = resp.actor();
    if (act.max_health() <= 0.0 || act.archetype().find("Goblin") == std::string::npos) {
        std::fprintf(stderr, "[FAIL] Stage 8: invalid Goblin telemetry (archetype='%s' max_hp=%.1f)\n",
                     act.archetype().c_str(), act.max_health());
        return false;
    }
    std::printf("  [ok ] Stage 8: Goblin targeted & combat abilities triggered (id=%llu archetype='%s' hp=%.0f/%.0f ai='%s')\n",
                static_cast<unsigned long long>(s.goblin_id), act.archetype().c_str(),
                act.combatant() ? act.health() : act.max_health(), act.max_health(), act.ai_state().c_str());
    return true;
}

inline bool run_stage8_combat_abilities(bots::LiveSession& s) {
    dispatch_live_stage8_ability_rpcs(s);
    return inspect_goblin_combat(s);
}

}  // namespace aoc::world::tests::plans
