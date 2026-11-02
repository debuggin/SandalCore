#include "combat_test_fixture.h"

namespace sandal::worldd::tests {
namespace {

namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;

struct MoveWire {
    double t = 0;
    int    rep_movement = -1;
    bool   place = false;
    bool   lunge = false;
    double yaw = 0;
};

void read_move_wire(world::WorldActor& actor, repl::ActorShadow& shadow, double now, std::vector<MoveWire>& out) {
    std::vector<repl::ContentBlock> blocks;
    if (!repl::collect_update_blocks(actor.actor, shadow, blocks)) return;
    const schema::ClassSchema& sc = *actor.actor.obj.schema;
    for (const repl::ContentBlock& b : blocks) {
        if (b.sub || !b.has_rep_layout) continue;
        const auto st = repl::load_object_state(sc, b.payload.data(), b.payload.bit_pos(), true);
        if (!st) continue;
        MoveWire w;
        w.t = now;
        if (const auto* v = st->state.get(sc, "bReplicateMovement")) {
            if (v->get_if<bool>()) w.rep_movement = *v->get_if<bool>() ? 1 : 0;
        }
        if (const auto* v = st->state.get(sc, "RepRootMotion.bIsActive")) {
            if (v->get_if<bool>()) w.lunge = *v->get_if<bool>();
        }
        if (const auto* v = st->state.get(sc, "ReplicatedMovement")) {
            if (const auto* m = v->get_if<repl::RepMovement>()) {
                w.place = true;
                w.yaw = m->rotation.yaw * 360.0 / (st->opts.rep_movement_short_rotation ? 65536.0 : 256.0);
            }
        }
        out.push_back(w);
    }
}

struct AlphaMoveCast {
    double start = 0;
    bool   in_place = false;
    int    held = -1;
};

struct AlphaMoveSim {
    uint64_t                   wolf = 0;
    std::vector<MoveWire>      wire;
    std::vector<AlphaMoveCast> casts;
    bool                       behind = false;
    double                     behind_at = 0;
    double                     yaw_before_turn = 0;
};

bool CastIsInPlace(const Fixture& f, const combat::NpcAbilityData& tables, const GremCastView& view) {
    const abilities::AbilityDef* def = f.catalog->ability(view.ability);
    if (!def) return false;
    for (const content::MontageRow& m : def->montages) {
        const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
        if (md && md->guid == view.montage) return md->root_motion.empty();
    }
    return false;
}

void run_alpha_wolf_move_loop(Fixture& f, const combat::NpcAbilityData& tables, combat_rig::World& w,
                              combat::CombatWorld& world, AlphaMoveSim& sim) {
    constexpr double kPi = 3.14159265358979323846;
    const uint64_t wolf = sim.wolf;
    world::WorldActor* actor = w.registry.find(wolf);
    combat::Fighter* fighter = world.fighter_of(wolf);
    double px = 0, py = 800;
    Rig rig(4, 20, {});
    combat_rig::FakePlayer player(rig, 77, {px, py, 1098});
    world.add_player(&player);
    repl::ActorShadow shadow;
    repl::ExportAcks acks;
    bool failed = false;
    (void)repl::open_actor(actor->actor, shadow, acks, &failed);
    GremCastView last_view;
    int held = 0;
    const stats::StatValue* wolf_max = fighter->stats().find(content::kGuidMaxHealth);
    for (double now = 1.0 / 15.0; now < 22.0 + 1e-6; now += 1.0 / 15.0) {
        if (!sim.behind && now >= 12.0) {
            sim.behind = true;
            sim.behind_at = now;
            const world::WorldActor* g = w.registry.find(wolf);
            sim.yaw_before_turn = g->yaw_degrees;
            px = g->x - 300.0 * std::cos(g->yaw_degrees * kPi / 180.0);
            py = g->y - 300.0 * std::sin(g->yaw_degrees * kPi / 180.0);
            player.rig_fighter().move_to({px, py, 1098});
        }
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
        if (wolf_max) fighter->stats().set_current(content::kGuidHealth, static_cast<float>(wolf_max->value * 0.6));
        w.npcs->note_player(77, px, py, now);
        w.npcs->tick(now);
        world.tick(now);
        const size_t first_new = sim.wire.size();
        read_move_wire(*actor, shadow, now, sim.wire);
        for (size_t i = first_new; i < sim.wire.size(); ++i) {
            if (sim.wire[i].rep_movement >= 0) held = sim.wire[i].rep_movement;
            sim.wire[i].rep_movement = held;
        }
        const GremCastView view = grem_cast_view(w.registry, wolf);
        if (view.montage != 0 && (view.element != last_view.element || last_view.montage == 0)) {
            sim.casts.push_back({now, CastIsInPlace(f, tables, view), held});
        }
        last_view = view;
    }
}

void check_alpha_wolf_move_results(TestHarness& h, const AlphaMoveSim& sim) {
    size_t places = 0, places_ignored = 0, lunges = 0, lunges_ignored = 0, turns = 0, turns_ignored = 0;
    for (const MoveWire& m : sim.wire) {
        if (m.place) {
            ++places;
            if (m.rep_movement != 1) ++places_ignored;
            const double dyaw = std::fabs(m.yaw - sim.yaw_before_turn);
            if (sim.behind && m.t >= sim.behind_at && dyaw > 45.0 && dyaw < 315.0) {
                ++turns;
                if (m.rep_movement != 1) ++turns_ignored;
            }
        }
        if (m.lunge) {
            ++lunges;
            if (m.rep_movement != 1) ++lunges_ignored;
        }
    }
    size_t in_place = 0, in_place_ignored = 0;
    for (const AlphaMoveCast& c : sim.casts) {
        if (!c.in_place) continue;
        ++in_place;
        if (c.held != 1) ++in_place_ignored;
    }
    std::printf("  %zu casts in 22 s, %zu of them in place; %zu place updates, %zu with a montage carrying the creature, %zu that turn it\n",
                sim.casts.size(), in_place, places, lunges, turns);
    CHECKF(places >= 20, "the creature's places reach the client while it walks, fights and turns (%zu updates)", places);
    CHECKF(places_ignored == 0, "every place update reaches a client that applies it: the bit is on from the first one (%zu of %zu would be ignored)", places_ignored, places);
    CHECKF(in_place >= 1 && in_place_ignored == 0, "an attack that plays in place begins with the bit on (%zu such casts, %zu begin with it off)", in_place, in_place_ignored);
    CHECKF(turns >= 1 && turns_ignored == 0, "its turn to the player at its back reaches a client that applies it (%zu such updates, %zu ignored)", turns, turns_ignored);
    CHECKF(lunges_ignored == 0, "and so does every update of a lunge (%zu of %zu ignored)", lunges_ignored, lunges);
}

void test_creature_movement_reaches_client(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a creature whose open has the client ignore its movement: it walks, plays an in-place attack and turns, and every one of those updates reaches a client that applies them\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string alpha_path = creature_path(f, "/Beast/Canine/AlphaWolf.Default__AlphaWolf_C");
    const auto bp = f.motion.find(alpha_path);
    CHECK(!alpha_path.empty() && bp && tables.abilities_of(alpha_path) != nullptr, "the Blueprint table has the Alpha Wolf and the creature tables list its abilities");
    if (alpha_path.empty() || !bp || !tables.abilities_of(alpha_path)) return;
    const abilities::AbilityDef* growl = f.catalog->ability_named("AlphaWolf_Growl");
    CHECK(growl != nullptr, "the ability data has the Growl");
    if (!growl) return;

    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t wolf = w.spawn(alpha_path, 0, 0, 7, 900, bp->half_height);
    CHECK(wolf != 0, "an Alpha Wolf is made from its open");
    if (wolf == 0) return;
    world::WorldActor* actor = w.registry.find(wolf);
    CHECK(combat::npc_knows_ability(*actor, growl->row.guid), "its open lists the Growl, so the world plays its animation");
    const schema::ClassSchema& sc = *actor->actor.obj.schema;
    CHECK(sc.command("bReplicateMovement") != nullptr, "its actor has the bReplicateMovement property");
    actor->actor.obj.state.set(sc, "bReplicateMovement", repl::Value::boolean(false));
    combat::Fighter* fighter = world.fighter_of(wolf);
    CHECK(fighter != nullptr, "the world makes it a fighter");
    if (!fighter) return;
    repl::ActorShadow shadow_check;
    repl::ExportAcks acks;
    bool failed = false;
    const auto open = repl::open_actor(actor->actor, shadow_check, acks, &failed);
    CHECK(open.has_value() && !failed, "the Alpha Wolf's open builds (the shadow of the connection that watches the fight)");

    AlphaMoveSim sim;
    sim.wolf = wolf;
    run_alpha_wolf_move_loop(f, tables, w, world, sim);
    check_alpha_wolf_move_results(h, sim);
}

struct GrowlGateSpec {
    std::string path;
    double      hurt_to = 1.0;
    double      seconds = 16.0;
    bool        needs_growl = false;
    int64_t     growl_guid = 0;
    int64_t     bite_guid = 0;
};

bool run_growl_gate_fight(Fixture& f, const combat::NpcAbilityData& tables, const GrowlGateSpec& spec, int& growls, int& bites) {
    Rig rig(4, 20, {});
    if (!rig.ok()) return false;
    combat_rig::World w;
    if (!w.problem.empty()) return false;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const auto bp = f.motion.find(spec.path);
    if (!bp) return false;
    const uint64_t wolf = w.spawn(spec.path, 0, 0, 7, 900, bp->half_height);
    if (wolf == 0) return false;
    world::WorldActor* actor = w.registry.find(wolf);
    if ((spec.needs_growl && !combat::npc_knows_ability(*actor, spec.growl_guid)) || !combat::npc_knows_ability(*actor, spec.bite_guid)) return false;
    combat_rig::FakePlayer player(rig, 77, {0, 800, 1098});
    world.add_player(&player);
    combat::Fighter* fighter = world.fighter_of(wolf);
    if (!fighter) return false;
    GremCastView last;
    bool hurt = false;
    for (double now = 1.0 / 15.0; now < spec.seconds; now += 1.0 / 15.0) {
        if (!hurt && spec.hurt_to < 1.0 && now >= 2.0) {
            const stats::StatValue* hp = fighter->stats().find(content::kGuidMaxHealth);
            if (hp) fighter->stats().set_current(content::kGuidHealth, static_cast<float>(hp->value * spec.hurt_to));
            hurt = true;
        }
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
        w.npcs->note_player(77, 0, 800, now);
        w.npcs->tick(now);
        world.tick(now);
        const GremCastView view = grem_cast_view(w.registry, wolf);
        if (view.montage != 0 && (view.element != last.element || last.montage == 0)) {
            if (view.ability == spec.growl_guid) ++growls;
            if (view.ability == spec.bite_guid) ++bites;
        }
        last = view;
    }
    return true;
}

const combat::CreatureAbility* find_ability_task(const combat::NpcAbilityData& tables, const std::string& path, int64_t guid) {
    for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
        if (t.ability == guid) return &t;
    }
    return nullptr;
}

void check_growl_coverage(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& alpha_path, const std::string& wolf_path) {
    std::string alpha_line, wolf_line;
    for (const std::string& line : tables.coverage_lines()) {
        if (line.rfind("AlphaWolf: ", 0) == 0) alpha_line = line;
        if (line.rfind("Wolf: ", 0) == 0) wolf_line = line;
    }
    std::printf("  load log: %s\n            %s\n", alpha_line.c_str(), wolf_line.c_str());
    const auto alpha_cover = tables.coverage().at(alpha_path), wolf_cover = tables.coverage().at(wolf_path);
    CHECKF(alpha_cover.usable > wolf_cover.usable && wolf_cover.off > alpha_cover.off && !alpha_line.empty() && !wolf_line.empty(),
           "the log counts the Alpha Wolf's usable tasks (%zu) above the Wolf's (%zu), and the Wolf's switched-off ones (%zu) above the Alpha's (%zu)",
           alpha_cover.usable, wolf_cover.usable, wolf_cover.off, alpha_cover.off);
}

void test_creature_parameter_and_health_gates(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the Alpha Wolf's Growl is gated by a tree parameter (the plain Wolf's is switched off) and by its own health (under 75 %%): the same fight, a full-health Alpha, a hurt Alpha and a hurt Wolf\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string alpha_path = creature_path(f, "/Beast/Canine/AlphaWolf.Default__AlphaWolf_C");
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    CHECK(!alpha_path.empty() && !wolf_path.empty() && tables.abilities_of(alpha_path) && tables.abilities_of(wolf_path),
          "the Blueprint table has the Alpha Wolf and the Wolf and the creature tables list their abilities");
    if (alpha_path.empty() || wolf_path.empty() || !tables.abilities_of(alpha_path) || !tables.abilities_of(wolf_path)) return;
    const abilities::AbilityDef* growl = f.catalog->ability_named("AlphaWolf_Growl");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(growl && bite, "the ability data has the Growl and the Bite");
    if (!growl || !bite) return;
    const combat::CreatureAbility* alpha_task = find_ability_task(tables, alpha_path, growl->row.guid);
    const combat::CreatureAbility* wolf_task = find_ability_task(tables, wolf_path, growl->row.guid);
    CHECK(alpha_task && wolf_task, "both Blueprints' trees have the Growl");
    if (!alpha_task || !wolf_task) return;
    CHECKF(!alpha_task->off && !alpha_task->unknown && alpha_task->fight && !alpha_task->groups.empty(),
           "the Alpha Wolf's Growl is usable in the fight and carries a condition on its health (off %d, unknown %d, groups %zu)",
           alpha_task->off ? 1 : 0, alpha_task->unknown ? 1 : 0, alpha_task->groups.size());
    CHECK(wolf_task->off, "the plain Wolf's Growl is switched off by its tree's parameters");
    check_growl_coverage(h, tables, alpha_path, wolf_path);

    int full_growls = 0, full_bites = 0, hurt_growls = 0, hurt_bites = 0, wolf_growls = 0, wolf_bites = 0;
    const bool a = run_growl_gate_fight(f, tables, {alpha_path, 1.0, 16.0, true, growl->row.guid, bite->row.guid}, full_growls, full_bites);
    const bool b = run_growl_gate_fight(f, tables, {alpha_path, 0.6, 16.0, true, growl->row.guid, bite->row.guid}, hurt_growls, hurt_bites);
    const bool c = run_growl_gate_fight(f, tables, {wolf_path, 0.6, 16.0, false, growl->row.guid, bite->row.guid}, wolf_growls, wolf_bites);
    CHECK(a && b && c, "the three fights run (the creatures are made from their opens and know the Bite, the Alpha the Growl too)");
    if (!(a && b && c)) return;
    std::printf("  Alpha Wolf at full health: %d bites, %d growls; hurt to 60 %%: %d bites, %d growls; plain Wolf hurt to 60 %%: %d bites, %d growls\n",
                full_bites, full_growls, hurt_bites, hurt_growls, wolf_bites, wolf_growls);
    CHECKF(full_bites >= 3 && full_growls == 0, "the Alpha Wolf at full health bites and never growls (%d bites, %d growls)", full_bites, full_growls);
    CHECKF(hurt_growls >= 1 && hurt_bites >= 1, "hurt under 75 %% it growls (%d growls, %d bites)", hurt_growls, hurt_bites);
    CHECKF(wolf_bites >= 3 && wolf_growls == 0, "the plain Wolf, hurt as much, still never growls: its tree switches the Growl off (%d bites, %d growls)", wolf_bites, wolf_growls);
}

void run_bear_roll_sim(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& bear_path, int64_t right_guid, int64_t left_guid) {
    Fixture& f = h.f;
    const auto bp = f.motion.find(bear_path);
    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t bear = w.spawn(bear_path, 0, 0, 7, 900, bp->half_height);
    CHECK(bear != 0, "a Bear is made from its open");
    if (bear == 0) return;
    CHECK(combat::npc_knows_ability(*w.registry.find(bear), right_guid) && combat::npc_knows_ability(*w.registry.find(bear), left_guid),
          "its open lists both swings, so the world plays their animations");
    combat_rig::FakePlayer player(rig, 77, {0, 800, 1098});
    world.add_player(&player);
    GremCastView last;
    int rights = 0, lefts = 0, others = 0;
    for (double now = 1.0 / 15.0; now < 60.0; now += 1.0 / 15.0) {
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
        w.npcs->note_player(77, 0, 800, now);
        w.npcs->tick(now);
        world.tick(now);
        const GremCastView view = grem_cast_view(w.registry, bear);
        if (view.montage != 0 && (view.element != last.element || last.montage == 0)) {
            if (view.ability == right_guid) ++rights;
            else if (view.ability == left_guid) ++lefts;
            else ++others;
        }
        last = view;
    }
    std::printf("  60 s against a standing player: %d right swings, %d left swings, %d other attacks\n", rights, lefts, others);
    CHECKF(rights >= 3 && lefts >= 3, "the swings come in both hands: the roll lets the right one through about half the time (%d right, %d left)", rights, lefts);
    CHECKF(rights + lefts >= 10, "and the Bear keeps attacking while it rolls (%d swings in 60 s)", rights + lefts);
}

void test_creature_roll_gate(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the Bear's right swing is gated by a roll of one half, asked afresh at every try: over a long fight it swings right and left, neither always\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string bear_path = creature_path(f, "/Beast/Bear/Bear.Default__Bear_C");
    const auto bp = f.motion.find(bear_path);
    CHECK(!bear_path.empty() && bp && tables.abilities_of(bear_path) != nullptr, "the Blueprint table has the Bear and the creature tables list its abilities");
    if (bear_path.empty() || !bp || !tables.abilities_of(bear_path)) return;
    const abilities::AbilityDef* right = f.catalog->ability_named("Bear_RightSwing");
    const abilities::AbilityDef* left = f.catalog->ability_named("Bear_LeftSwing");
    CHECK(right && left, "the ability data has the Bear's swings");
    if (!right || !left) return;
    const combat::CreatureAbility* right_task = find_ability_task(tables, bear_path, right->row.guid);
    bool has_roll = false;
    if (right_task) {
        for (const combat::CondGroup& g : right_task->groups) {
            for (const combat::CondToken& t : g) has_roll = has_roll || t.atom.kind == combat::CondAtom::Kind::Random;
        }
    }
    CHECK(right_task && !right_task->unknown && !right_task->off && has_roll, "the tables carry the right swing with a roll among its conditions");
    if (!right_task) return;
    run_bear_roll_sim(h, tables, bear_path, right->row.guid, left->row.guid);
}

bool run_wolf_bite_where_fight(Fixture& f, const combat::NpcAbilityData& tables,
                               int64_t bite_guid, int where, int* counts_out) {
    constexpr double kPi = 3.14159265358979323846;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    double flip_at = -1;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> got;
    LungeFightSpec spec;
    spec.path = wolf_path;
    spec.seconds = 30.0;
    spec.player_y = 800.0;
    spec.hits = &got;
    spec.leash_cm = 40000.0;
    spec.steer = [&](double now, const LungeSample* prev, double& px, double& py, const world::WorldActor& wolf) {
        if (where != 0 && prev && prev->cast_began && prev->ability == bite_guid) flip_at = now + 0.1;
        if (flip_at < 0 || now < flip_at) return false;
        flip_at = -1;
        const double d = where == 1 ? -150.0 : 2500.0;
        px = wolf.x + d * std::cos(wolf.yaw_degrees * kPi / 180.0);
        py = wolf.y + d * std::sin(wolf.yaw_degrees * kPi / 180.0);
        return true;
    };
    if (!run_lunge_fight(f, tables, spec, samples)) return false;
    counts_out[0] = 0;
    counts_out[1] = 0;
    for (const LungeSample& s : samples) {
        if (s.cast_began && s.ability == bite_guid) ++counts_out[0];
    }
    for (const combat::IncomingHit& ih : got) {
        if (ih.source_guid == bite_guid) ++counts_out[1];
    }
    return true;
}

void test_bite_lands_wherever_the_player_stands(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a Wolf's bite, whose targeting record measures nothing (validity off): at its notify it lands on its target wherever he stands, in front of it, behind it or far away\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path) && bite,
          "the Blueprint table has the Wolf, the creature tables list its abilities and the ability data has the Bite");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !tables.abilities_of(wolf_path) || !bite) return;
    const combat::BiteArea* record = tables.area_of(bite->row.guid);
    const combat::RootMotionFreezeRule* freeze = tables.freeze_of(bite->row.guid);
    CHECK(record && record->gate() == combat::BiteArea::Gate::Any && record->discovery == combat::BiteArea::Discovery::PrefersActive &&
              !record->check_validity && record->volume_count == 0 && freeze && freeze->kind == combat::RootMotionFreezeRule::Kind::Pause && freeze->distance > 0,
          "the Bite's targeting record finds the creature's own target, measures nothing and sweeps no volume (an Any record), and its lunge pauses on contact");
    if (!record || !freeze) return;

    int front[2] = {}, behind[2] = {}, far_away[2] = {};
    const bool a = run_wolf_bite_where_fight(f, tables, bite->row.guid, 0, front);
    const bool b = run_wolf_bite_where_fight(f, tables, bite->row.guid, 1, behind);
    const bool c = run_wolf_bite_where_fight(f, tables, bite->row.guid, 2, far_away);
    CHECK(a && b && c, "the three fights run (the Wolf is made from its open and knows the Bite)");
    if (!a || !b || !c) return;
    std::printf("  30 s against a player who stands in front: %d bites, %d reached him; who steps behind the Wolf as each bite begins: %d bites, %d reached him; who is put 25 m away as each bite begins: %d bites, %d reached him\n",
                front[0], front[1], behind[0], behind[1], far_away[0], far_away[1]);
    CHECKF(front[0] >= 5 && front[1] >= front[0] - 1,
           "every bite of the Wolf reaches a player who stands in front of it (%d of %d; the last one may be still in the air when the fight ends)", front[1], front[0]);
    CHECKF(behind[0] >= 4 && behind[1] >= behind[0] - 1, "... and one who has stepped behind it when the bite lands (%d of %d bites reached him)", behind[1], behind[0]);
    CHECKF(far_away[0] >= 2 && far_away[1] >= far_away[0] - 1, "... and one who is 25 m away when it lands (%d of %d bites reached him)", far_away[1], far_away[0]);
}

combat::HitSubject make_hit_body(double dist, double bearing, double yaw = 0.0, double radius = 34.0) {
    constexpr double kPi = 3.14159265358979323846;
    combat::HitSubject s;
    s.cx = 1000.0;
    s.cy = -500.0;
    s.yaw = yaw;
    const double a = (yaw + bearing) * kPi / 180.0;
    s.tx = s.cx + dist * std::cos(a);
    s.ty = s.cy + dist * std::sin(a);
    s.tradius = radius;
    return s;
}

void check_hit_area_any_and_cone(TestHarness& h, combat::BiteArea& fury_out) {
    using combat::BiteArea;
    const std::function<bool()> clear = [] { return true; };
    const std::function<bool()> blocked = [] { return false; };
    int asked = 0;
    const std::function<bool()> counted = [&asked] { ++asked; return true; };

    BiteArea any;
    any.discovery = BiteArea::Discovery::PrefersActive;
    any.distance = 500;
    any.angle = 120;
    any.cone_check = true;
    any.los_check = true;
    CHECK(any.gate() == BiteArea::Gate::Any && any.usable(), "a record that finds a target and measures nothing is an Any record");
    CHECK(combat::hit_lands(any, make_hit_body(30, 0), true, clear) && combat::hit_lands(any, make_hit_body(5000, 180), true, clear) &&
              combat::hit_lands(any, make_hit_body(900, 90), true, blocked),
          "an Any record hits its target at 30 cm, 50 m behind it and 9 m to its side with no line of sight");
    CHECK(!combat::hit_lands(any, make_hit_body(30, 0), false, clear), "... and nobody else (it has no volume)");
    asked = 0;
    combat::hit_lands(any, make_hit_body(300, 20), true, counted);
    CHECK(asked == 0, "... without asking for a line of sight");

    BiteArea fury = any;
    fury.discovery = BiteArea::Discovery::OnlyLocked;
    fury.check_validity = true;
    fury.distance = 300;
    fury.angle = 60;
    fury_out = fury;
    CHECK(fury.gate() == BiteArea::Gate::Validity, "a record whose validity is on is a Validity record");
    CHECK(combat::hit_lands(fury, make_hit_body(333, 0), true, clear) && !combat::hit_lands(fury, make_hit_body(335, 0), true, clear),
          "its reach is 300 cm to the body's surface: a body 34 cm wide lands at 333 cm and not at 335");
    CHECK(combat::hit_lands(fury, make_hit_body(200, 39), true, clear) && !combat::hit_lands(fury, make_hit_body(200, 41), true, clear),
          "its cone is 30 degrees either side (half the 60), plus the body's own width: lands 39 degrees off at 200 cm, not 41");
    CHECK(combat::hit_lands(fury, make_hit_body(200, -39), true, clear) && !combat::hit_lands(fury, make_hit_body(200, -41), true, clear) &&
              !combat::hit_lands(fury, make_hit_body(200, 180), true, clear),
          "... to either side, and not behind");
    CHECK(combat::hit_lands(fury, make_hit_body(200, 0, 90), true, clear) && !combat::hit_lands(fury, make_hit_body(200, 90, 90), true, clear),
          "the cone goes the way the creature faces (facing 90 degrees: ahead lands, 90 degrees off does not)");
    CHECK(!combat::hit_lands(fury, make_hit_body(200, 0), true, blocked) && combat::hit_lands(fury, make_hit_body(200, 0), true, clear), "it needs a line of sight");
    asked = 0;
    combat::hit_lands(fury, make_hit_body(400, 0), true, counted);
    combat::hit_lands(fury, make_hit_body(200, 80), true, counted);
    CHECK(asked == 0, "... which it asks for only once the body is inside the cone");
    CHECK(!combat::hit_lands(fury, make_hit_body(200, 0), false, clear), "it hits its target and nobody else");
}

void check_hit_area_cone_variants(TestHarness& h, const combat::BiteArea& fury) {
    using combat::BiteArea;
    const std::function<bool()> clear = [] { return true; };
    const std::function<bool()> blocked = [] { return false; };

    BiteArea round = fury, none = fury, shifted = fury, ahead = fury, near_area = fury, no_reach = fury, sight_only = fury;
    round.angle = 360;
    none.angle = 0;
    shifted.angle_center = 90;
    ahead.distance_center = 100;
    near_area.min_distance = 100;
    no_reach.distance = 0;
    sight_only.cone_check = false;
    CHECK(combat::hit_lands(round, make_hit_body(200, 180), true, clear) && combat::hit_lands(none, make_hit_body(200, 180), true, clear) &&
              !combat::hit_lands(round, make_hit_body(340, 180), true, clear),
          "an angle of 360 or 0 is all round, still inside the reach");
    CHECK(combat::hit_lands(shifted, make_hit_body(200, 90), true, clear) && !combat::hit_lands(shifted, make_hit_body(200, 0), true, clear),
          "the angle centre turns the cone (90: it points to the creature's right)");
    CHECK(combat::hit_lands(ahead, make_hit_body(433, 0), true, clear) && !combat::hit_lands(ahead, make_hit_body(436, 0), true, clear),
          "the distance centre moves the start of the cone ahead of the creature (100 cm: the reach ends 100 cm farther)");
    CHECK(!combat::hit_lands(near_area, make_hit_body(120, 0), true, clear) && combat::hit_lands(near_area, make_hit_body(180, 0), true, clear),
          "a body nearer than the minimum distance to its surface (100 cm) is not hit");
    CHECK(!combat::hit_lands(no_reach, make_hit_body(0, 0), true, clear), "a cone with no reach holds nothing");
    CHECK(combat::hit_lands(sight_only, make_hit_body(4000, 180), true, clear) && !combat::hit_lands(sight_only, make_hit_body(100, 0), true, blocked),
          "validity on with the cone switch off asks only for the line of sight");
}

void check_hit_area_volumes(TestHarness& h, const combat::BiteArea& fury) {
    using combat::BiteArea;
    using combat::HitVolume;
    const std::function<bool()> clear = [] { return true; };
    const std::function<bool()> blocked = [] { return false; };

    BiteArea sphere;
    HitVolume ball;
    ball.shape = HitVolume::Shape::Sphere;
    ball.ox = 75;
    ball.radius = 64;
    sphere.volumes.push_back(ball);
    sphere.volume_count = 1;
    CHECK(sphere.gate() == BiteArea::Gate::Volumes && sphere.usable() && !sphere.has_target_part(), "a record with a volume and no target part is a Volumes record");
    CHECK(combat::hit_lands(sphere, make_hit_body(173, 0), false, clear) && !combat::hit_lands(sphere, make_hit_body(175, 0), false, clear) &&
              !combat::hit_lands(sphere, make_hit_body(300, 180), false, clear),
          "a sphere 75 cm ahead with a radius of 64 hits a body 34 cm wide out to 173 cm ahead, and nothing behind it, whoever the body is");
    CHECK(combat::hit_lands(sphere, make_hit_body(100, 0, 90), false, clear) && !combat::hit_lands(sphere, make_hit_body(100, 90, 90), false, clear),
          "the sphere goes the way the creature faces (facing 90 degrees: ahead lands, 90 degrees off does not)");
    CHECK(!combat::hit_lands(sphere, make_hit_body(100, 0), false, blocked), "a volume asks for a line of sight too (its switch is on)");

    BiteArea crate;
    HitVolume box;
    box.shape = HitVolume::Shape::Box;
    box.ox = 130;
    box.hx = 100;
    box.hy = 80;
    box.los = false;
    crate.volumes.push_back(box);
    crate.volume_count = 1;
    CHECK(combat::hit_lands(crate, make_hit_body(264, 0), false, clear) && !combat::hit_lands(crate, make_hit_body(266, 0), false, clear) &&
              combat::hit_lands(crate, make_hit_body(60, 0), false, clear) && combat::hit_lands(crate, make_hit_body(-3, 0), false, clear) &&
              !combat::hit_lands(crate, make_hit_body(-5, 0), false, clear),
          "a box from 30 to 230 cm ahead hits a body 34 cm wide from 3 cm behind the creature's centre to 264 cm ahead of it");
    CHECK(combat::hit_lands(crate, make_hit_body(150, 35), false, clear) && !combat::hit_lands(crate, make_hit_body(200, 90), false, clear),
          "... and 80 cm to each side of its centre line, plus the body's width");
    CHECK(combat::hit_lands(crate, make_hit_body(150, 0), false, blocked), "a volume whose line-of-sight switch is off hits through a wall");

    BiteArea both = fury;
    both.volumes.push_back(ball);
    both.volume_count = 1;
    CHECK(both.gate() == BiteArea::Gate::Volumes, "a record with a target part and a volume counts as a Volumes record");
    CHECK(!combat::hit_lands(both, make_hit_body(400, 180), true, clear) && combat::hit_lands(both, make_hit_body(100, 0), false, clear) &&
              combat::hit_lands(both, make_hit_body(200, 0), true, clear),
          "... and hits what either part reaches: its target inside the cone, and anybody in the volume");
    BiteArea odd = sphere;
    odd.readable = false;
    CHECK(!odd.usable(), "a record with a volume the world cannot place is not usable");
}

void test_hit_area_rules(TestHarness& h) {
    std::printf("where a hit lands, by its targeting record: a record that measures nothing hits its target wherever he stands, one that measures holds its cone, volumes hit everyone they overlap\n");
    combat::BiteArea fury;
    check_hit_area_any_and_cone(h, fury);
    check_hit_area_cone_variants(h, fury);
    check_hit_area_volumes(h, fury);
}

struct FuryPlace {
    const char* name;
    double      distance;
    double      bearing;
    bool        fury_lands;
};

void run_fury_place_case(TestHarness& h, const combat::NpcAbilityData& tables,
                         const FuryPlace& place, int64_t fury_guid, int64_t snap_guid) {
    constexpr double kPi = 3.14159265358979323846;
    Fixture& f = h.f;
    const std::string rabid_path = creature_path(f, "/Beast/Rabid/Rabid.Default__Rabid_C");
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> got;
    LungeFightSpec spec;
    spec.path = rabid_path;
    spec.seconds = 40.0;
    spec.player_y = 800.0;
    spec.hits = &got;
    spec.leash_cm = 40000.0;
    spec.steer = [&](double, const LungeSample* prev, double& px, double& py, const world::WorldActor& grem) {
        if (!prev || prev->montage == 0 || (prev->ability != fury_guid && prev->ability != snap_guid)) return false;
        const double a = (grem.yaw_degrees + place.bearing) * kPi / 180.0;
        px = grem.x + place.distance * std::cos(a);
        py = grem.y + place.distance * std::sin(a);
        return true;
    };
    CHECKF(run_lunge_fight(f, tables, spec, samples), "the fight against a player put %s runs", place.name);
    size_t fury_casts = 0, fury_due = 0, fury_landed = 0, snap_casts = 0, snap_due = 0, snap_landed = 0;
    for (const LungeSample& s : samples) {
        if (!s.cast_began || (s.ability != fury_guid && s.ability != snap_guid)) continue;
        const bool is_fury = s.ability == fury_guid;
        (is_fury ? fury_casts : snap_casts) += 1;
        const combat::MontageData* md = montage_of_cast(f, tables, s.ability, s.montage);
        for (const double at : cast_bites(f, tables, s.ability, s.montage)) {
            if (s.t + at <= 40.0 - 0.2) (is_fury ? fury_due : snap_due) += 1;
        }
        for (const combat::IncomingHit& ih : got) {
            if (ih.source_guid == s.ability && ih.at >= s.t - 0.05 && ih.at <= s.t + (md ? md->seconds(1.0) : 1.0) + 0.25) {
                (is_fury ? fury_landed : snap_landed) += 1;
            }
        }
    }
    std::printf("  the player put %-18s: Fury %zu casts, %zu bites due, %zu landed;  Snap %zu casts, %zu bites due, %zu landed\n",
                place.name, fury_casts, fury_due, fury_landed, snap_casts, snap_due, snap_landed);
    CHECKF(fury_casts >= 2 && fury_due >= 6, "the Grem casts the Fury at a player put %s (%zu casts, %zu bites due)", place.name, fury_casts, fury_due);
    if (place.fury_lands) {
        CHECKF(fury_landed >= fury_due, "every bite of its Fury lands on a player put %s, inside the cone (%zu of %zu)", place.name, fury_landed, fury_due);
    } else {
        CHECKF(fury_landed == 0, "no bite of its Fury lands on a player put %s, outside the cone (%zu of %zu landed)", place.name, fury_landed, fury_due);
    }
    CHECKF(snap_casts == 0 || snap_landed >= snap_due, "every bite of its Snap lands on a player put %s (%zu of %zu)", place.name, snap_landed, snap_due);
}

void test_fury_lands_only_inside_its_cone(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a Grem's Fury (validity on: 300 cm, 60 degrees, line of sight) lands each of its four bites at its notify only while the player is inside its cone, and its Snap (validity off) wherever he stands\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string rabid_path = creature_path(f, "/Beast/Rabid/Rabid.Default__Rabid_C");
    const abilities::AbilityDef* fury = f.catalog->ability_named("Rabid_Fury");
    const abilities::AbilityDef* snap = f.catalog->ability_named("Rabid_Snap");
    CHECK(!rabid_path.empty() && f.motion.find(rabid_path) && tables.abilities_of(rabid_path) && fury && snap,
          "the Blueprint table has the Rabid, the creature tables list its abilities and the ability data has the Fury and the Snap");
    if (rabid_path.empty() || !f.motion.find(rabid_path) || !tables.abilities_of(rabid_path) || !fury || !snap) return;
    const combat::BiteArea* fury_area = tables.area_of(fury->row.guid, 1);
    const combat::BiteArea* snap_area = tables.area_of(snap->row.guid, 0);
    CHECK(fury_area && fury_area->gate() == combat::BiteArea::Gate::Validity && fury_area->discovery == combat::BiteArea::Discovery::OnlyLocked &&
              fury_area->cone_check && fury_area->los_check && fury_area->distance == 300 && fury_area->angle == 60 && fury_area->volume_count == 0,
          "the Fury's record finds the locked target and measures: 300 cm, 60 degrees, the cone and the line of sight");
    CHECK(snap_area && snap_area->gate() == combat::BiteArea::Gate::Any && snap_area->distance == 500 && snap_area->angle == 120 && !snap_area->check_validity,
          "the Snap's record states 500 cm and 120 degrees and has its validity off (an Any record)");
    if (!fury_area || !snap_area) return;

    const FuryPlace places[] = {
        {"220 cm ahead", 220, 0, true},
        {"220 cm, 15 deg off", 220, 15, true},
        {"220 cm, 50 deg off", 220, 50, false},
        {"220 cm behind", 220, 180, false},
        {"520 cm ahead", 520, 0, false},
    };
    for (const FuryPlace& place : places) {
        run_fury_place_case(h, tables, place, fury->row.guid, snap->row.guid);
    }
}

}  // namespace

void run_combat_gates_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db) {
    (void)db;
    test_creature_movement_reaches_client(h);
    test_creature_parameter_and_health_gates(h);
    test_creature_roll_gate(h);
    test_bite_lands_wherever_the_player_stands(h);
    test_hit_area_rules(h);
    test_fury_lands_only_inside_its_cone(h);
}

}  // namespace sandal::worldd::tests
