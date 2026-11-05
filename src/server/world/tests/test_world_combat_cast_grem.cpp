// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "combat_test_fixture.h"

#include <filesystem>

namespace sandal::worldd::tests {
namespace {

std::string wolf_archetype(Fixture& f) {
    return creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
}

void check_player_cast_hit(TestHarness& h, Rig& rig, combat::CombatWorld& world, uint64_t wolf, const abilities::AbilityDef& axe) {
    const int64_t ability = axe.row.guid;
    const abilities::HitDef* hit1 = h.f.catalog->hit(axe.hits[0].hit_guid);
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::TargetingRow& t : axe.targetings) {
        if (t.key == axe.hits[0].key) targeting = t.guid;
    }
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {wolf};
    double t = 1.0;
    CHECK(rig.pipe->trigger(ability, 10, t, info) == Refusal::None, "a cast at the Wolf is accepted");
    std::vector<Seen> seen = rig.run_to(t + 0.4);
    const Seen* item = nullptr;
    for (const Seen& s : seen) {
        if (!s.change.abilities_added.empty()) item = &s;
    }
    CHECK(item != nullptr, "its ability item comes");
    if (item) {
        const bootstrap::ActiveAbilityItem& it = item->change.abilities_added[0];
        CHECK(!it.targets_self && it.target_objects.size() == 1 && it.target_objects[0].ObjectId == wolf + 4,
              "the item's Targets is the Wolf's collision cylinder (the actor's + 4, inside its block), not the caster's");
    }

    t += 0.5;
    const auto verdict = rig.pipe->hit_request(ability, targeting, hit1->row.guid, 1, 0, t, {wolf});
    CHECK(verdict == AbilityPipeline::HitVerdict::Scheduled, "the hit request is scheduled");
    seen = rig.run_to(t + 0.5);
    const bootstrap::HitConfirm* conf = nullptr;
    for (const Seen& s : seen) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid == hit1->row.guid && !c.targets.empty()) conf = &c;
        }
    }
    CHECK(conf != nullptr, "the hit is confirmed with its targets");
    if (conf) {
        CHECK(conf->targets.size() == 1 && conf->targets[0].component.ObjectId == wolf && conf->targets[0].cylinder.ObjectId == wolf + 4 &&
                  conf->targets[0].has_impact && conf->mods.empty() && conf->response,
              "one FHitInst with the Wolf actor as its target (its cylinder to export, the impact at it); the pawn is none");
        const bootstrap::HitTargetOut& tg = conf->targets[0];
        if ((tg.flags & stats::kHitMiss) == 0) {
            CHECK(tg.mods.size() == 1 && tg.mods[0].stat_mod_guid == hit1->mods[0].stat_mod_guid && tg.mods[0].value < 0 && tg.mods[0].effective < 0 &&
                      std::fabs(tg.mods[0].effective) <= std::fabs(tg.mods[0].value) + 1e-3,
                  "the target's stat mod: the formula's value and the part that took effect (both a loss)");
            CHECKF(world.health_of(wolf) < world.max_health_of(wolf), "the Wolf lost Health (%.1f of %.1f)", world.health_of(wolf), world.max_health_of(wolf));
        } else {
            CHECK(tg.mods.empty(), "a miss has no stat mods");
        }
    }
    const stats::StatValue* hp = rig.cs->find(content::kGuidHealth);
    const stats::StatValue* hp_max = rig.cs->find(content::kGuidMaxHealth);
    CHECK(hp && hp_max && hp->value == hp_max->value, "the caster's Health is untouched");
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}

void check_player_cast_refusals(TestHarness& h, Rig& rig, combat_rig::World& w, combat::CombatWorld& world, uint64_t wolf) {
    Fixture& f = h.f;
    const abilities::AbilityDef* axe = f.catalog->ability_named("WeaponCombo_Melee_1H_Axe");
    if (!axe) return;
    const int64_t ability = axe->row.guid;
    const double t = 1.5;
    const uint64_t far_wolf = w.spawn(wolf_archetype(f), 9000, 0, 8, 100);
    TriggerInfo far_info;
    far_info.target_count = 1;
    far_info.target_actors = {far_wolf};
    CHECK(rig.pipe->trigger(ability, 11, t + 3.0, far_info) == Refusal::OutOfRange, "a Wolf 90 m away is out of range");
    TriggerInfo nobody;
    nobody.target_count = 1;
    CHECK(rig.pipe->trigger(ability, 12, t + 3.1, nobody) == Refusal::None, "a cast naming a target the session could not resolve (no actor) is a cast at the air");
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    world.apply_hit_to_npc(wolf, 77, kill, t + 3.2);
    TriggerInfo dead_info;
    dead_info.target_count = 1;
    dead_info.target_actors = {wolf};
    CHECK(rig.pipe->trigger(ability, 13, t + 3.3, dead_info) == Refusal::BadTarget, "a dead Wolf is no target");
    TriggerInfo strange;
    strange.target_count = 1;
    strange.target_actors = {123456789};
    CHECK(rig.pipe->trigger(ability, 14, t + 3.4, strange) == Refusal::BadTarget, "an actor that is not in the world is refused");
    const abilities::AbilityDef* flourish = f.catalog->ability_named("Bard_Misc_Flourish");
    if (flourish) {
        TriggerInfo at_enemy;
        at_enemy.target_count = 1;
        at_enemy.target_actors = {far_wolf};
        const Refusal r = rig.pipe->trigger(flourish->row.guid, 15, t + 3.5, at_enemy);
        CHECK(r == Refusal::BadTarget || r == Refusal::OutOfRange, "an ability that only helps allies is not cast at an enemy");
    }
}

void test_player_cast(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a player's cast and its hits on an NPC\n");
    Rig rig(4, 20, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = wolf_archetype(f);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 600, 0, 8, 100);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    rig.know({"WeaponCombo_Melee_1H_Axe", "Bard_Misc_Flourish"});
    const abilities::AbilityDef* axe = f.catalog->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && wolf != 0, "the 1H axe combo and a Wolf 6 m away");
    if (!axe || wolf == 0) return;
    check_player_cast_hit(h, rig, world, wolf, *axe);
    check_player_cast_refusals(h, rig, w, world, wolf);
}

void test_npc_hits_player(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("an NPC's hit on the player: Health through the shields, the confirmation\n");
    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = wolf_archetype(f);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 300, 0, 10);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    const double max = rig.cs->find(content::kGuidMaxHealth)->value;
    combat::IncomingHit in;
    in.instigator = wolf;
    in.hit_guid = 109234434627193;
    in.source_type = content::kSourceTypeAbility;
    in.source_guid = 109184795825810;
    in.mods.push_back({109234434627194, content::kGuidHealth, -50.0});
    rig.pipe->receive_hit(in, 2.0);
    std::vector<Seen> seen = rig.take();
    CHECK(rig.cs->find(content::kGuidHealth)->value == static_cast<float>(max - 50.0), "Health fell by 50");
    const Seen* conf = nullptr;
    for (const Seen& s : seen) {
        if (!s.confirmations.empty()) conf = &s;
    }
    CHECK(conf && conf->confirmations.size() == 1 && conf->confirmations[0].instigator && conf->confirmations[0].instigator->ObjectId == wolf &&
              conf->confirmations[0].targets.empty() && conf->confirmations[0].mods.size() == 1 && conf->confirmations[0].mods[0].value == -50.0f &&
              conf->confirmations[0].mods[0].effective == -50.0f,
          "the confirmation names the Wolf as the instigator and the pawn as the target, with the value and the effective loss");
    const float before_miss = rig.cs->find(content::kGuidHealth)->value;
    in.flags = stats::kHitMiss;
    rig.pipe->receive_hit(in, 2.0001);
    seen = rig.take();
    CHECK(rig.cs->find(content::kGuidHealth)->value >= before_miss, "a miss costs no Health");
    in.flags = 0;
    in.mods[0].value = -1.0e9;
    rig.pipe->receive_hit(in, 2.0002);
    seen = rig.take();
    bool lethal = false;
    for (const Seen& s : seen) {
        for (const bootstrap::HitConfirm& c : s.confirmations) lethal = lethal || (c.flags & stats::kHitLethal);
    }
    CHECK(rig.cs->find(content::kGuidHealth)->value == 0.0f && lethal, "a killing blow takes Health to 0 and is flagged Lethal");
    CHECK(!player.rig_fighter().alive(), "the player is dead to the AI");
    combat::IncomingHit bad = in;
    bad.mods[0].value = std::nan("");
    rig.pipe->receive_hit(bad, 2.0003);
    rig.take();
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}

struct GremCast {
    double              start = 0, duration = 0;
    int64_t             ability = 0;
    uint64_t            montage_id = 0;
    std::string         name;
    int                 bites = 0;
    double              surface_before = 0, angle_before = 0;
    double              facing_error = 0;
    double              centre_start = 0, centre_end = 0;
    int                 delivered = 0;
    double              stepped_back = 0;
    double              closest = 0;
    bool                settled = true;
    int                 mode_at_start = -1, mode_after = -1;
    std::vector<double> notifies;
};

struct GremPose {
    double t = 0, x = 0, y = 0, yaw = 0, px = 0, py = 0;
    int    mode = -1;
};

struct GremWire {
    double   t = 0;
    int      active = -1;
    double   position = -1;
    bool     has_animation = false;
    uint64_t animation = 0;
    bool     has_rrm_loc = false, has_mv_loc = false;
    int64_t  rrm_loc[3] = {0, 0, 0}, mv_loc[3] = {0, 0, 0};
    bool     has_mv_rot = false, has_rrm_rot = false, has_rrm_vel = false;
    double   mv_yaw = 0, rrm_yaw = 0;
    int64_t  mv_vel[2] = {0, 0};
    int64_t  rrm_vel[2] = {0, 0};
    int      rep_movement = -1;
};

struct GremSim {
    Rig                            rig{4, 20, {}};
    combat_rig::World              w;
    combat::NpcStatBook            book;
    combat::CombatWorld            world;
    combat_rig::FakePlayer         player;
    uint64_t                       grem = 0;
    double                         r_grem = 0;
    double                         r_player = 34.0;
    double                         touch = 0;
    double                         reach_cm = 0;
    int                            mode_before = -1;
    int                            open_rep_movement = -1;
    double                         min_centre = 1e9;
    double                         farthest_in_fight = 0;
    double                         first_cast_at = -1;
    std::vector<GremPose>          trace;
    std::vector<GremCast>          casts;
    std::vector<double>            moves;
    std::vector<GremWire>          wire;
    aoc::protocol::repl::ActorShadow shadow;

    explicit GremSim(Fixture& f)
        : book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, [] {
              combat::CombatWorldConfig cfg;
              cfg.seed = 5;
              return cfg;
          }()),
          player(rig, 77, {0, 800, 1098}) {
        world.add_player(&player);
    }
};

int movement_mode_of(const world::WorldActor& g) {
    const repl::ObjectState& obj = g.actor.obj;
    const repl::Value* v = obj.schema ? obj.state.get(*obj.schema, "ReplicatedMovementMode") : nullptr;
    return v && v->get_if<uint64_t>() ? static_cast<int>(*v->get_if<uint64_t>()) : -1;
}

GremWire decode_grem_wire(const aoc::protocol::schema::ClassSchema& sc, const repl::ObjectState& st, double now) {
    GremWire ws;
    ws.t = now;
    if (const auto* v = st.state.get(sc, "RepRootMotion.bIsActive")) {
        if (v->get_if<bool>()) ws.active = *v->get_if<bool>() ? 1 : 0;
    }
    if (const auto* v = st.state.get(sc, "RepRootMotion.Position")) {
        if (v->get_if<float>()) ws.position = *v->get_if<float>();
    }
    if (const auto* v = st.state.get(sc, "RepRootMotion.Animation")) {
        if (const auto* o = v->get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()) {
            ws.has_animation = true;
            ws.animation = o->ObjectId;
        }
    }
    if (const auto* v = st.state.get(sc, "RepRootMotion.Location")) {
        if (const auto* q = v->get_if<repl::QuantVec>()) {
            ws.has_rrm_loc = true;
            ws.rrm_loc[0] = q->x, ws.rrm_loc[1] = q->y, ws.rrm_loc[2] = q->z;
        }
    }
    if (const auto* v = st.state.get(sc, "ReplicatedMovement")) {
        if (const auto* m = v->get_if<repl::RepMovement>()) {
            ws.has_mv_loc = true;
            ws.mv_loc[0] = m->location.x, ws.mv_loc[1] = m->location.y, ws.mv_loc[2] = m->location.z;
            ws.has_mv_rot = true;
            ws.mv_yaw = m->rotation.yaw * 360.0 / (st.opts.rep_movement_short_rotation ? 65536.0 : 256.0);
            ws.mv_vel[0] = m->linear_velocity.x, ws.mv_vel[1] = m->linear_velocity.y;
        }
    }
    if (const auto* v = st.state.get(sc, "RepRootMotion.Rotation")) {
        if (const auto* r = v->get_if<repl::Rot>()) {
            ws.has_rrm_rot = true;
            ws.rrm_yaw = r->yaw * 360.0 / 65536.0;
        }
    }
    if (const auto* v = st.state.get(sc, "RepRootMotion.LinearVelocity")) {
        if (const auto* q = v->get_if<repl::QuantVec>()) {
            ws.has_rrm_vel = true;
            ws.rrm_vel[0] = q->x, ws.rrm_vel[1] = q->y;
        }
    }
    if (const auto* v = st.state.get(sc, "bReplicateMovement")) {
        if (v->get_if<bool>()) ws.rep_movement = *v->get_if<bool>() ? 1 : 0;
    }
    return ws;
}

void collect_grem_wire_tick(world::WorldActor& actor, repl::ActorShadow& shadow, double now, std::vector<GremWire>& wire) {
    std::vector<repl::ContentBlock> blocks;
    if (!repl::collect_update_blocks(actor.actor, shadow, blocks)) return;
    for (const repl::ContentBlock& b : blocks) {
        if (b.sub || !b.has_rep_layout) continue;
        const auto st = repl::load_object_state(*actor.actor.obj.schema, b.payload.data(), b.payload.bit_pos(), true);
        if (!st) continue;
        wire.push_back(decode_grem_wire(*actor.actor.obj.schema, *st, now));
    }
}

void populate_grem_cast_montage(const abilities::AbilityDef& def, const combat::NpcAbilityData& tables, uint64_t montage_guid, GremCast& c) {
    for (const content::MontageRow& m : def.montages) {
        const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
        if (!md || md->guid != montage_guid) continue;
        c.duration = md->seconds(1.0);
        for (const combat::MontageData::Hit& mh : md->hits) {
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source != "hit" || r.key != mh.key) continue;
                ++c.bites;
                c.notifies.push_back(mh.time / md->rate_scale);
                break;
            }
        }
    }
}

void log_gate_census(TestHarness& h, const combat::CombatWorld& world) {
    const combat::CombatWorld::GateCensus census = world.gate_census();
    std::printf("  %s\n", census.text().c_str());
    const auto list = [](const char* what, const std::vector<std::string>& names) {
        std::string s;
        for (const std::string& n : names) s += (s.empty() ? "" : ", ") + n;
        std::printf("    %s: %s\n", what, s.c_str());
    };
    list("on the target wherever it stands", census.any_names);
    list("inside a cone", census.validity_names);
    list("by volumes", census.volume_names);
    list("keep the single swing", census.kept_names);
    list("no hit of the montage's to decide by", census.unplayed_names);
    CHECKF(census.any >= 1 && census.validity >= 1 && census.volumes >= 1,
           "the tables hold attacks of every kind of rule: %zu on the target wherever it stands, %zu inside a cone, %zu by volumes (%zu keep the swing)",
           census.any, census.validity, census.volumes, census.kept_swing);
}

void run_grem_sim_loop(Fixture& f, const combat::NpcAbilityData& tables, GremSim& sim) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kDt = 1.0 / 15.0, kAway = 20.0, kBehind = 28.0, kEnd = 36.0;
    double px = 0, py = 800, now = 0;
    bool away = false, behind = false;
    GremCastView last_view;
    world::WorldActor* actor = sim.w.registry.find(sim.grem);
    while (now < kEnd) {
        now += kDt;
        if (!away && now >= kAway) {
            away = true;
            py += 600;
            sim.player.rig_fighter().move_to({px, py, 1098});
            sim.moves.push_back(now);
        }
        if (!behind && now >= kBehind) {
            behind = true;
            const world::WorldActor* g = sim.w.registry.find(sim.grem);
            px = g->x - 300.0 * std::cos(g->yaw_degrees * kPi / 180.0);
            py = g->y - 300.0 * std::sin(g->yaw_degrees * kPi / 180.0);
            sim.player.rig_fighter().move_to({px, py, 1098});
            sim.moves.push_back(now);
        }
        sim.w.npcs->note_player(77, px, py, now);
        sim.w.npcs->tick(now);
        const world::WorldActor* before_actor = sim.w.registry.find(sim.grem);
        const double bx = before_actor->x, by = before_actor->y, byaw = before_actor->yaw_degrees;
        sim.world.tick(now);
        const world::WorldActor* g = sim.w.registry.find(sim.grem);
        sim.trace.push_back({now, g->x, g->y, g->yaw_degrees, px, py, movement_mode_of(*g)});
        collect_grem_wire_tick(*actor, sim.shadow, now, sim.wire);
        const double centre = std::hypot(g->x - px, g->y - py);
        sim.min_centre = std::min(sim.min_centre, centre);
        if (sim.first_cast_at >= 0 && now < kAway) sim.farthest_in_fight = std::max(sim.farthest_in_fight, centre);
        const GremCastView view = grem_cast_view(sim.w.registry, sim.grem);
        if (view.montage != 0 && (view.element != last_view.element || last_view.montage == 0)) {
            GremCast c;
            c.start = now;
            c.ability = view.ability;
            c.montage_id = view.montage;
            const abilities::AbilityDef* def = f.catalog->ability(view.ability);
            c.name = def ? def->row.name : "?";
            c.surface_before = std::hypot(bx - px, by - py) - sim.r_grem - sim.r_player;
            c.angle_before = std::fabs(wrap180(std::atan2(py - by, px - bx) * 180.0 / kPi - byaw));
            c.facing_error = std::fabs(wrap180(std::atan2(py - g->y, px - g->x) * 180.0 / kPi - g->yaw_degrees));
            c.centre_start = centre;
            if (def) populate_grem_cast_montage(*def, tables, view.montage, c);
            if (sim.first_cast_at < 0) sim.first_cast_at = now;
            sim.casts.push_back(std::move(c));
        }
        last_view = view;
    }
}

void summarize_grem_casts(GremSim& sim) {
    constexpr double kDt = 1.0 / 15.0, kEnd = 36.0;
    for (GremCast& c : sim.casts) {
        c.centre_end = c.centre_start;
        double closest = c.centre_start;
        for (const GremPose& p : sim.trace) {
            if (p.t < c.start) continue;
            const double d = std::hypot(p.x - p.px, p.y - p.py);
            if (p.t <= c.start + c.duration) {
                closest = std::min(closest, d);
                c.centre_end = d;
            }
            if (std::fabs(p.t - c.start) < 0.5 * kDt) c.mode_at_start = p.mode;
            if (p.t >= c.start + c.duration - 0.1 && c.mode_after < 0) c.mode_after = p.mode;
        }
        c.closest = closest;
        c.stepped_back = c.centre_end - closest;
        for (const combat::IncomingHit& ih : sim.player.hits) {
            if (ih.source_guid == c.ability && ih.at >= c.start - 0.05 && ih.at <= c.start + c.duration + 0.25) ++c.delivered;
        }
        c.settled = c.start + c.duration + 0.25 <= kEnd;
        for (const double m : sim.moves) c.settled = c.settled && !(m > c.start - 0.01 && m < c.start + c.duration + 0.25);
    }
}

void check_grem_wire_yaw_and_heading(TestHarness& h, const GremSim& sim) {
    constexpr double kPi = 3.14159265358979323846;
    size_t samples = 0, off8 = 0, off16 = 0;
    double worst8 = 0, worst16 = 0;
    for (const GremCast& c : sim.casts) {
        double yaw8 = 0, yaw16 = 0;
        bool have8 = false, have16 = false;
        for (const GremWire& ws : sim.wire) {
            if (ws.t > c.start + 1e-6) break;
            if (ws.has_mv_rot) yaw8 = ws.mv_yaw, have8 = true;
            if (ws.has_rrm_rot) yaw16 = ws.rrm_yaw, have16 = true;
        }
        const GremPose* at = nullptr;
        for (const GremPose& p : sim.trace) {
            if (std::fabs(p.t - c.start) < 1e-6) at = &p;
        }
        if (!at || !have8) continue;
        const double bearing = at->yaw;
        const double d8 = std::fabs(wrap180(bearing - yaw8)), d16 = have16 ? std::fabs(wrap180(bearing - yaw16)) : 0.0;
        ++samples;
        worst8 = std::max(worst8, d8);
        worst16 = std::max(worst16, d16);
        if (d8 > 0.75 * 360.0 / 256.0) ++off8;
        if (have16 && d16 > 0.75 * 360.0 / 65536.0 + 0.01) ++off16;
    }
    std::printf("  the wire's yaw against the server's, as the client holds it at the start of each cast (%zu): ReplicatedMovement (8 bits) at most %.2f deg off, RepRootMotion.Rotation (16 bits) at most %.3f deg off\n",
                samples, worst8, worst16);
    CHECKF(samples >= 8 && off8 == 0 && off16 == 0,
           "the Grem's yaw on the wire, read as the client reads it, is the yaw the server bites with: %zu casts sampled, %zu off in ReplicatedMovement, %zu in RepRootMotion.Rotation",
           samples, off8, off16);

    size_t walking = 0, wrong = 0;
    double worst = 0;
    for (const GremWire& ws : sim.wire) {
        if (ws.t > sim.first_cast_at || ws.t < 0.55 || !ws.has_mv_rot) continue;
        if (std::hypot(static_cast<double>(ws.mv_vel[0]), static_cast<double>(ws.mv_vel[1])) < 200.0) continue;
        const double heading = std::atan2(static_cast<double>(ws.mv_vel[1]), static_cast<double>(ws.mv_vel[0])) * 180.0 / kPi;
        const double d = std::fabs(wrap180(heading - ws.mv_yaw));
        ++walking;
        worst = std::max(worst, d);
        if (d > 10.0) ++wrong;
    }
    CHECKF(walking >= 3 && wrong == 0, "a Grem that walks up to the player faces the way it walks, on the wire (%zu updates sampled, the worst %.1f deg off)", walking, worst);

    size_t lunging = 0, missing = 0;
    for (const GremWire& ws : sim.wire) {
        if (ws.position < 0) continue;
        ++lunging;
        if (!ws.has_rrm_vel && std::hypot(static_cast<double>(ws.mv_vel[0]), static_cast<double>(ws.mv_vel[1])) > 1.0) ++missing;
    }
    CHECKF(missing == 0, "every update of a lunge that moves the Grem carries RepRootMotion.LinearVelocity (%zu of %zu do not)", missing, lunging);
}

void check_grem_rrm_and_freeze(TestHarness& h, const combat::NpcAbilityData& tables, const abilities::AbilityDef& fury, const GremSim& sim) {
    constexpr double kEnd = 36.0;
    if (const combat::RootMotionFreezeRule* rule = tables.freeze_of(fury.row.guid)) {
        const double edge = rule->distance + rule->radius_scale * sim.r_grem + sim.r_player;
        size_t outside = 0, inside = 0, stopped_wrong = 0, moved_inside = 0;
        double lowest = 1e9, highest = 0;
        for (const GremCast& c : sim.casts) {
            if (c.ability != fury.row.guid || !c.settled) continue;
            if (c.centre_start > edge) {
                ++outside;
                lowest = std::min(lowest, c.centre_end);
                highest = std::max(highest, c.centre_end);
                if (c.centre_end < 235.0 || c.centre_end > 274.0) ++stopped_wrong;
            } else {
                ++inside;
                if (c.centre_start - c.closest > 2.0) ++moved_inside;
            }
        }
        std::printf("  the Fury: its freeze box reaches %.0f cm ahead of the Grem's centre (%.0f + %.0f x %.0f + %.0f); %zu began outside it and stopped at %.1f to %.1f cm, %zu began inside it\n",
                    edge, rule->distance, rule->radius_scale, sim.r_grem, sim.r_player, outside, outside ? lowest : 0.0, highest, inside);
        CHECKF(rule->kind == combat::RootMotionFreezeRule::Kind::Stop && outside >= 1, "a Fury begins outside its freeze box, as the Grem walks up to the player (%zu did)", outside);
        CHECKF(stopped_wrong == 0, "a Fury that begins outside its freeze box stops at a centre distance of 235 to 274 cm from a player who stands (%zu of %zu did not, %.1f to %.1f cm)",
               stopped_wrong, outside, outside ? lowest : 0.0, highest);
        CHECKF(moved_inside == 0, "a Fury that begins inside its freeze box does not move (%zu of %zu did)", moved_inside, inside);
    }

    size_t no_start = 0, no_end = 0, thin = 0, places_differ = 0, complete_casts = 0;
    for (const GremCast& c : sim.casts) {
        if (c.start + c.duration > kEnd) continue;
        ++complete_casts;
        bool started = false, ended = false;
        size_t positions = 0;
        for (const GremWire& ws : sim.wire) {
            if (ws.t < c.start - 1e-6 || ws.t > c.start + c.duration + 1e-6) continue;
            if (ws.active == 1 && ws.has_animation && ws.animation == c.montage_id && ws.t <= c.start + 1e-6) started = true;
            if (ws.active == 0 && ws.has_animation && ws.animation == 0) ended = true;
            if (ws.position >= 0) ++positions;
            if (ws.has_rrm_loc && ws.has_mv_loc && (ws.rrm_loc[0] != ws.mv_loc[0] || ws.rrm_loc[1] != ws.mv_loc[1] || ws.rrm_loc[2] != ws.mv_loc[2])) ++places_differ;
        }
        if (!started) ++no_start;
        if (!ended) ++no_end;
        if (positions < 6) ++thin;
    }
    CHECKF(no_start == 0 && no_end == 0 && thin == 0 && places_differ == 0,
           "the updates of a lunge carry RepRootMotion: its first names the montage and says it carries the Grem (%zu of %zu casts lack it), every one the montage's position (%zu casts have fewer than 6), the last "
           "its end (%zu lack it), and the place is ReplicatedMovement's (%zu updates differ)",
           no_start, complete_casts, thin, no_end, places_differ);
}

void check_grem_rrm_tail_and_flag(TestHarness& h, const GremSim& sim) {
    constexpr double kEnd = 36.0;
    size_t ends = 0, unsent = 0, wrong_place = 0, moving = 0, thin_tail = 0;
    double slowest = 0;
    for (const GremCast& c : sim.casts) {
        if (!c.settled || c.start + c.duration > kEnd) continue;
        const GremWire* end = nullptr;
        for (const GremWire& ws : sim.wire) {
            if (ws.t < c.start - 1e-6 || ws.t > c.start + c.duration + 1e-6) continue;
            if (ws.active == 0 && ws.has_animation && ws.animation == 0) {
                end = &ws;
                break;
            }
        }
        if (!end) continue;
        ++ends;
        const GremWire* again = nullptr;
        size_t tail = 0;
        for (const GremWire& ws : sim.wire) {
            if (ws.t <= end->t + 1e-6 || ws.t > end->t + 0.35 || !ws.has_mv_loc) continue;
            if (!again) again = &ws;
            ++tail;
        }
        if (tail < 3) ++thin_tail;
        if (!again || again->t - end->t > 0.25 + 1e-6) {
            ++unsent;
            continue;
        }
        slowest = std::max(slowest, again->t - end->t);
        const GremPose* at = nullptr;
        for (const GremPose& p : sim.trace) {
            if (std::fabs(p.t - again->t) < 1e-6) at = &p;
        }
        if (!at || std::fabs(static_cast<double>(again->mv_loc[0]) - at->x * 100.0) > 100.0 || std::fabs(static_cast<double>(again->mv_loc[1]) - at->y * 100.0) > 100.0) ++wrong_place;
        if (again->mv_vel[0] != 0 || again->mv_vel[1] != 0) ++moving;
    }
    std::printf("  after a montage lets go of the Grem (%zu ends), ReplicatedMovement goes out again within %.3f s at the longest\n", ends, slowest);
    CHECKF(ends >= 6 && unsent == 0, "after every cast's end a ReplicatedMovement goes out within 0.25 s (%zu of %zu ends have none)", unsent, ends);
    CHECKF(wrong_place == 0 && moving == 0, "... with the Grem's place and no velocity (%zu with another place, %zu with a velocity)", wrong_place, moving);
    CHECKF(thin_tail == 0, "... and it goes on at the following updates for a few more (%zu of %zu ends are followed by fewer than 3 in 0.35 s)", thin_tail, ends);

    int held = sim.open_rep_movement;
    size_t lunge = 0, lunge_off = 0, place = 0, place_off = 0;
    for (const GremWire& ws : sim.wire) {
        if (ws.rep_movement >= 0) held = ws.rep_movement;
        if (sim.first_cast_at < 0 || ws.t < sim.first_cast_at - 1e-6) continue;
        if (ws.active == 1) {
            ++lunge;
            if (held != 1) ++lunge_off;
        }
        if (ws.has_mv_loc) {
            ++place;
            if (held != 1) ++place_off;
        }
    }
    std::printf("  bReplicateMovement: %d in the open (-1: not in it), %d at the end of the fight\n", sim.open_rep_movement, held);
    CHECKF(lunge > 0 && lunge_off == 0, "every RepRootMotion update of a lunge reaches a client that replicates the Grem's movement (%zu of %zu do not)", lunge_off, lunge);
    CHECKF(place_off == 0, "... and so does every ReplicatedMovement from the first cast on (%zu of %zu do not)", place_off, place);
}

void check_grem_bounds_and_modes(TestHarness& h, const GremSim& sim) {
    constexpr double kAway = 20.0, kEnd = 36.0;
    size_t before_away = 0;
    for (const GremCast& c : sim.casts) {
        if (c.start + c.duration <= kAway) ++before_away;
    }
    CHECKF(before_away >= 6, "the Grem attacks a player who stands in front of it again and again (%zu casts in the first %.0f s)", before_away, kAway);
    CHECKF(sim.min_centre >= sim.touch - 1.0, "its centre never comes closer to the player's than the two radii (%.1f cm), the closest was %.1f cm", sim.touch, sim.min_centre);
    const double bound = sim.touch + sim.reach_cm * 1.15 + 1.0;
    CHECKF(sim.first_cast_at >= 0 && sim.farthest_in_fight <= bound,
           "while the player stands it stays within %.0f cm (the radii and the tree's reach with the AI's slack) of the player, the farthest was %.1f cm", bound, sim.farthest_in_fight);
    size_t backed = 0;
    double most_back = 0;
    for (const GremCast& c : sim.casts) {
        if (!c.settled || c.closest > sim.touch + 1.0) continue;
        if (c.stepped_back > 1.5) ++backed;
        most_back = std::max(most_back, c.stepped_back);
    }
    CHECKF(backed == 0, "once it is at the player's body it does not step back when the animation winds back (%zu casts did, the most %.1f cm)", backed, most_back);
    size_t wrong_mode = 0;
    for (const GremCast& c : sim.casts) {
        if (c.start + c.duration > kEnd) continue;
        if (c.mode_at_start != repl::kMoveWalking || c.mode_after != sim.mode_before) ++wrong_mode;
    }
    CHECKF(wrong_mode == 0, "its movement mode is Walking while a montage carries it and what it was (%d) when the montage is over (%zu of %zu casts differ)",
           sim.mode_before, wrong_mode, sim.casts.size());
}

void check_grem_tree_and_reengage(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& rabid_path, const GremSim& sim) {
    constexpr double kAway = 20.0, kBehind = 28.0;
    size_t unfaced = 0, outside = 0, short_casts = 0;
    double worst_facing = 0, first_outside_at = -1, first_surface = 0, first_angle = 0, first_short_at = -1;
    int missing_bites = 0;
    for (const GremCast& c : sim.casts) {
        if (c.facing_error > 3.0) ++unfaced;
        worst_facing = std::max(worst_facing, c.facing_error);
        bool ok = false;
        for (const combat::CreatureAbility& t : *tables.abilities_of(rabid_path)) {
            if (t.unknown || t.ability != c.ability) continue;
            const bool d_ok = (t.max_distance < 0 || c.surface_before <= t.max_distance + 0.5) && (t.min_distance < 0 || c.surface_before >= t.min_distance - 0.5);
            ok = ok || (d_ok && (t.max_angle < 0 || c.angle_before <= t.max_angle + 0.5));
        }
        if (!ok && outside++ == 0) { first_outside_at = c.start; first_surface = c.surface_before; first_angle = c.angle_before; }
        if (c.settled && c.delivered < c.bites) {
            if (short_casts++ == 0) first_short_at = c.start;
            missing_bites += c.bites - c.delivered;
        }
    }
    CHECKF(unfaced == 0, "every cast faces the player when it begins (%zu of %zu do not, the worst %.1f deg off)", unfaced, sim.casts.size(), worst_facing);
    CHECKF(outside == 0, "no cast begins while the tree's conditions fail (%zu of %zu did; the first at %.2f s, %.1f cm from the player's surface and %.1f deg off its facing)",
           outside, sim.casts.size(), first_outside_at, first_surface, first_angle);
    CHECKF(short_casts == 0, "every bite of every cast lands while the player stands in its area (%zu casts lost %d bites; the first at %.2f s)",
           short_casts, missing_bites, first_short_at);
    double t_back = -1;
    for (const GremPose& p : sim.trace) {
        if (p.t >= kAway && p.t < kBehind && std::hypot(p.x - p.px, p.y - p.py) <= sim.touch + sim.reach_cm) { t_back = p.t; break; }
    }
    CHECKF(t_back >= 0 && t_back - kAway <= 7.0, "when the player steps 6 m away the Grem is back within its reach in time (%.1f s after the step)", t_back < 0 ? -1.0 : t_back - kAway);
    int bites_after = 0, bites_behind = 0;
    size_t casts_between = 0, after_behind = 0;
    for (const GremCast& c : sim.casts) {
        if (c.start >= kAway && c.start < kBehind) { ++casts_between; bites_after += c.delivered; }
        if (c.start >= kBehind) { ++after_behind; bites_behind += c.delivered; }
    }
    CHECKF(casts_between >= 1 && bites_after >= 1, "and it lands bites again (%zu casts and %d bites between the step and the next)", casts_between, bites_after);
    CHECKF(after_behind >= 1 && bites_behind >= 1, "when the player is at its back it turns and bites (%zu casts and %d bites after it)", after_behind, bites_behind);
}

void log_grem_summary(const GremSim& sim) {
    std::printf("  %zu casts in 36 s (%llu attacks, %llu hits and %llu misses reached the player); the first cast at %.1f s\n", sim.casts.size(),
                static_cast<unsigned long long>(sim.world.stats().npc_attacks), static_cast<unsigned long long>(sim.world.stats().npc_hits),
                static_cast<unsigned long long>(sim.world.stats().npc_misses), sim.first_cast_at);
    size_t with_rrm = 0, starts = 0, ends = 0;
    for (const GremWire& ws : sim.wire) {
        if (ws.position >= 0) ++with_rrm;
        if (ws.active == 1) ++starts;
        if (ws.active == 0) ++ends;
    }
    std::printf("  the Grem's own block was sent %zu times; %zu carried RepRootMotion's position, %zu said a montage carries the Grem, %zu said it let go\n",
                sim.wire.size(), with_rrm, starts, ends);
    for (size_t i = 0; i < sim.casts.size() && i < 60; ++i) {
        const GremCast& c = sim.casts[i];
        std::printf("    t %5.2f  %-11s  before: surface %6.1f cm, %5.1f deg off   facing error %5.1f deg   centre %6.1f -> %6.1f cm (back %4.1f)   bites %d of %d%s\n",
                    c.start, c.name.c_str(), c.surface_before, c.angle_before, c.facing_error, c.centre_start, c.centre_end, c.stepped_back, c.delivered, c.bites,
                    c.settled ? "" : "   (the player moved during it)");
    }
}

bool load_creature_tables(TestHarness& h, combat::NpcAbilityData& tables) {
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    const std::string files[3] = {dir + "npc_abilities.tsv", dir + "npc_montages.tsv", dir + "npc_targeting.tsv"};
    std::string err;
    if (tables.load(files[0], files[1], files[2], err)) return true;
    std::error_code ec;
    bool any_there = false;
    for (const std::string& file : files) any_there = any_there || std::filesystem::exists(file, ec);
    if (!any_there) {
        std::printf("  [skip] the creature tables (tools/npcabilities, the npcabilities extractor step): %s\n", err.c_str());
        return false;
    }
    CHECKF(false, "the creature tables are there but do not read: %s", err.c_str());
    return false;
}

void test_grem_fight(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a Grem against a player who stands still: it lunges to the player, bites in reach, never goes through or drifts away, turns, and comes back when the player steps away\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(h, tables)) return;
    const std::string rabid_path = creature_path(f, "/Beast/Rabid/Rabid.Default__Rabid_C");
    const auto bp = f.motion.find(rabid_path);
    CHECK(!rabid_path.empty() && bp && tables.abilities_of(rabid_path) != nullptr, "the Blueprint table has the Rabid and the creature tables list its abilities");
    if (rabid_path.empty() || !bp || !tables.abilities_of(rabid_path)) return;
    const abilities::AbilityDef* fury = f.catalog->ability_named("Rabid_Fury");
    const abilities::AbilityDef* snap = f.catalog->ability_named("Rabid_Snap");
    CHECK(fury && snap, "the ability data has the Grem's Fury and Snap");
    if (!fury || !snap) return;

    GremSim sim(f);
    if (!sim.rig.ok() || !sim.w.problem.empty()) return;
    sim.world.set_ability_data(&tables);
    sim.grem = sim.w.spawn(rabid_path, 0, 0, 7, 900, bp->half_height);
    CHECK(sim.grem != 0, "a Grem is made from the Rabid's capture");
    if (sim.grem == 0) return;
    sim.r_grem = bp->radius;
    sim.touch = sim.r_grem + sim.r_player;
    log_gate_census(h, sim.world);
    world::WorldActor* actor = sim.w.registry.find(sim.grem);
    CHECK(actor && combat::npc_knows_ability(*actor, fury->row.guid) && combat::npc_knows_ability(*actor, snap->row.guid),
          "its open lists the Fury and the Snap, so the world plays their animations");
    for (const combat::CreatureAbility& t : *tables.abilities_of(rabid_path)) {
        if (!t.unknown && t.max_distance > sim.reach_cm) sim.reach_cm = t.max_distance;
    }
    CHECK(sim.reach_cm > 0, "the tree gives its basic attacks a reach");
    sim.mode_before = movement_mode_of(*actor);
    if (const auto* v = actor->actor.obj.state.get(*actor->actor.obj.schema, "bReplicateMovement")) {
        if (v->get_if<bool>()) sim.open_rep_movement = *v->get_if<bool>() ? 1 : 0;
    }
    repl::ExportAcks acks;
    bool failed = false;
    const auto open = repl::open_actor(actor->actor, sim.shadow, acks, &failed);
    CHECK(open.has_value() && !failed, "the Grem's open builds (the shadow of the connection that watches the fight)");
    run_grem_sim_loop(f, tables, sim);
    const stats::StatValue* hp = sim.rig.cs->find(content::kGuidHealth);
    CHECK(hp && hp->value > 0, "the player is still alive at the end (the bites went on)");
    summarize_grem_casts(sim);
    log_grem_summary(sim);
    check_grem_bounds_and_modes(h, sim);
    check_grem_wire_yaw_and_heading(h, sim);
    check_grem_rrm_and_freeze(h, tables, *fury, sim);
    check_grem_rrm_tail_and_flag(h, sim);
    check_grem_tree_and_reengage(h, tables, rabid_path, sim);
}

}  // namespace

void run_combat_cast_grem_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db) {
    (void)db;
    test_player_cast(h);
    test_npc_hits_player(h);
    test_grem_fight(h);
}

}  // namespace sandal::worldd::tests
