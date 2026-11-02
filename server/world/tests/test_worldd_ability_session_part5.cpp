#include "test_worldd_ability_session_common.h"
#include "capture_blocks.h"
#include "combat_rig.h"
#include "spawns/area.h"
using namespace aoc::worldd;
static BunchWriter trigger_at_value(const bs::PlayerSetup &setup, int64_t guid, int32_t id, const repl::Value &target, int anim_index = 0) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    if (ok && anim_index != 0) ok = repl::set_rpc_param(*rpc, p, "AnimationIndex", repl::Value::i64(anim_index));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    if (ok) {
        repl::ArrayValue targets;
        repl::ElementState e;
        e.values[1] = target;
        targets.elements.push_back(std::move(e));
        ok = repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    }
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    if (!ok) return BunchWriter();
    return ability_rpc(setup.guids.ability, "ServerTriggerAbility", p);
}
static BunchWriter hit_request_with(const bs::PlayerSetup &setup, int64_t ability, int64_t targeting, int64_t hit, repl::HitComponentRef c) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("RequestHitVerification_Server");
    repl::HitRequest req;
    c.server_timestamp = 1.0f;
    req.components.push_back(c);
    req.ability_guid = ability;
    req.targeting_guid = targeting;
    req.hit_guid = hit;
    const auto params = rpc ? repl::hit_request_params(*rpc, req) : std::nullopt;
    if (!params) return BunchWriter();
    return ability_rpc(setup.guids.ability, "RequestHitVerification_Server", *params);
}
static BunchWriter hit_request_at_object(const bs::PlayerSetup &setup, int64_t ability, int64_t targeting, int64_t hit, const emit::FIntrepidNetworkGUID &object) {
    repl::HitComponentRef c;
    c.object = object;
    return hit_request_with(setup, ability, targeting, hit, c);
}
struct WolfWorld {
    uint64_t wolf = 0;
    uint64_t next_actor_id = 5000000;
    bool ground = false;
    int level = 8;
    int extra = 0;
    std::vector<uint64_t> wolves;
    void prepare(PlayerWorld &w) {
        std::string err;
        if (!w.load_blueprints(NPC_MOTION_PATH, err)) return;
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
        ground = nav && w.adopt_ground(std::move(nav), err);
        auto captured = spawns::CapturedData::load(err);
        if (!ground || !captured) return;
        std::string path;
        for (const auto &[p, row] : w.blueprints().rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) path = p;
        }
        wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + 120, kY, kZ, level, 800);
        for (int i = 0; i < extra; ++i) {
            const uint64_t id = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + 120, kY + 100 * (i + 1), kZ, level, 800);
            if (id != 0) wolves.push_back(id);
        }
    }
};
static void world_play(Run &r, double seconds) {
    const int steps = std::max(1, static_cast<int>(std::llround(seconds * 10.0)));
    for (int i = 0; i < steps; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
}
static bool exports_carry(const fake::Delivered &d, const emit::FIntrepidNetworkGUID &guid) {
    for (const BunchWriter &e : d.exports) {
        const size_t bits = e.bit_pos();
        for (size_t start = 0; start + 128 <= bits; ++start) {
            size_t pos = start;
            if (emit::read_intrepid_guid(e.data(), e.byte_size() + 1, pos) == guid) return true;
        }
    }
    return false;
}
static repl::InlineObject inline_cylinder_of(const world::WorldActor &wa) {
    repl::InlineObject cyl;
    cyl.flags = 5;
    cyl.outer = wa.actor.guid;
    cyl.path = "CollisionCylinder";
    cyl.checksum = 0xb876932f;
    return cyl;
}
static emit::FIntrepidNetworkGUID cylinder_guid_of(const world::WorldActor &wa) {
    emit::FIntrepidNetworkGUID g = wa.actor.guid;
    g.ObjectId = wa.npc.collision_cylinder;
    return g;
}
struct ExportRecord {
    emit::FIntrepidNetworkGUID guid;
    uint8_t flags = 0;
    emit::FIntrepidNetworkGUID outer;
    std::string path;
    uint32_t checksum = 0;
};
struct ExportCursor {
    const uint8_t *data = nullptr;
    size_t len = 0;
    size_t pos = 0;
};
static bool read_export_entry(ExportCursor &cur, std::vector<ExportRecord> &out, emit::FIntrepidNetworkGUID *guid_out, int depth) {
    if (depth > 8) return false;
    ExportRecord r;
    r.guid = emit::read_intrepid_guid(cur.data, cur.len, cur.pos);
    if (guid_out) *guid_out = r.guid;
    if (r.guid.ObjectId == 0) return true;
    r.flags = static_cast<uint8_t>(ue5::read_bits(cur.data, cur.len, cur.pos, 8));
    if (r.flags & 1) {
        if (!read_export_entry(cur, out, &r.outer, depth + 1)) return false;
        const int32_t n = static_cast<int32_t>(ue5::read_bits(cur.data, cur.len, cur.pos, 32));
        if (n < 0 || n > 512) return false;
        for (int32_t i = 0; i < n; ++i) {
            const char c = static_cast<char>(ue5::read_bits(cur.data, cur.len, cur.pos, 8));
            if (c != 0) r.path.push_back(c);
        }
        if (r.flags & 4) r.checksum = static_cast<uint32_t>(ue5::read_bits(cur.data, cur.len, cur.pos, 32));
    }
    out.push_back(std::move(r));
    return true;
}
static std::vector<ExportRecord> export_records(const fake::Delivered &d) {
    std::vector<ExportRecord> out;
    for (const BunchWriter &e : d.exports) {
        const size_t bits = e.bit_pos();
        ExportCursor cur{e.data(), e.byte_size() + 1, 0};
        (void)ue5::read_bits(cur.data, cur.len, cur.pos, 1);
        const uint32_t count = static_cast<uint32_t>(ue5::read_bits(cur.data, cur.len, cur.pos, 32));
        for (uint32_t i = 0; i < count && cur.pos < bits; ++i) {
            if (!read_export_entry(cur, out, nullptr, 0)) break;
        }
    }
    return out;
}
struct GuidCache {
    std::map<uint64_t, std::pair<std::string, uint64_t>> held;
    size_t mismatches = 0;
    std::vector<std::string> described;
    uint64_t find_path(const std::string &path, uint64_t outer) const {
        for (const auto &[id, po] : held) {
            if (po.first == path && po.second == outer) return id;
        }
        return 0;
    }
    void record_entry(const ExportRecord &r) {
        if (!(r.flags & 1)) return;
        const auto it = held.find(r.guid.ObjectId);
        if (it == held.end()) {
            held.emplace(r.guid.ObjectId, std::make_pair(r.path, r.outer.ObjectId));
            return;
        }
        if (it->second.first != r.path || it->second.second != r.outer.ObjectId) {
            ++mismatches;
            if (described.size() < 4) {
                described.push_back("GUID " + std::to_string(r.guid.ObjectId) + " exported as " + r.path + " of " + std::to_string(r.outer.ObjectId) + " but held as " + it->second.first + " of " +
                                    std::to_string(it->second.second));
            }
        }
    }
    void take(const fake::Delivered &d) {
        for (const ExportRecord &r : export_records(d)) record_entry(r);
    }
};
static void verify_inline_axe_swing(Run &r, const bs::PlayerSetup &setup, uint64_t wolf, const repl::InlineObject &cyl) {
    combat::CombatWorld &cw = *r.h.fx.world->combat();
    const abilities::AbilityDef *axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    int64_t targeting = 0;
    for (const content::TargetingRow &t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(axe->row.guid);
    r.h.fx.world->tick_world(r.now());
    const double max_hp = cw.max_health_of(wolf);
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_value(setup, axe->row.guid, id, repl::Value(cyl));
    r.send_trigger(axe->row.guid, id, &trig);
    r.play(0.6, false);
    const std::vector<int32_t> ends = r.m.end_ids();
    CHECK(r.m.has_ability(axe->row.guid) && std::find(ends.begin(), ends.end(), id) == ends.end(), "the swing at the Wolf named in place is accepted");
    repl::HitComponentRef comp;
    comp.inline_object = cyl;
    r.send_reliable(hit_request_with(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, comp));
    world_play(r, 0.5);
    const repl::HitInstance *conf = nullptr;
    for (const repl::HitInstance &h : r.m.hits) {
        if (!h.targets.empty()) conf = &h;
    }
    CHECK(conf != nullptr, "the hit request naming the cylinder in place is "
                           "confirmed with a target");
    const double hp1 = cw.health_of(wolf);
    const bool missed = conf && (conf->targets[0].flags & stats::kHitMiss);
    std::printf("  (the Wolf: %.1f of %.1f Health after the swing)\n", hp1, max_hp);
    CHECK(conf && (missed || hp1 < max_hp), "the Wolf lost Health to the swing (or the hit missed)");
}
static void verify_inline_mock(Run &r, const bs::PlayerSetup &setup, uint64_t wolf, const repl::InlineObject &cyl) {
    combat::CombatWorld &cw = *r.h.fx.world->combat();
    constexpr int64_t kMock = 6064632575971298233ll;
    constexpr int64_t kMockeryApply = 6064630758756712681ll;
    constexpr int64_t kQuickWitApply = 6064632501410136576ll;
    r.h.s->teach_ability(kMock);
    const float health_before = r.m.stats[content::kGuidHealth];
    const size_t hits_before = r.m.hits.size();
    const int32_t mock_id = r.next_id++;
    const BunchWriter mock = trigger_at_value(setup, kMock, mock_id, repl::Value(cyl));
    r.send_trigger(kMock, mock_id, &mock);
    world_play(r, 2.0);
    const std::vector<int32_t> ends2 = r.m.end_ids();
    CHECK(std::find(ends2.begin(), ends2.end(), mock_id) == ends2.end(), "Mock at the Wolf is accepted");
    bool mockery = false;
    bool quickwit = false;
    for (size_t i = hits_before; i < r.m.hits.size(); ++i) {
        if (r.m.hits[i].record_guid == kMockeryApply && !r.m.hits[i].targets.empty()) mockery = true;
        if (r.m.hits[i].record_guid == kQuickWitApply) quickwit = true;
    }
    const double hp2 = cw.health_of(wolf);
    std::printf("  (the Wolf: %.1f Health after Mock)\n", hp2);
    CHECK(mockery, "Bard_Mockery_Apply is confirmed on the Wolf");
    CHECK(!quickwit && !r.m.has_effect(kGleeEffect), "Bard_QuickWit_Apply is not confirmed and Glee is not on the caster: "
                                                     "Mock heals nobody");
    (void)health_before;
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}
static void test_e1_inline_target_session() {
    std::printf("E1 stage 3 fix: a target named in place is the Wolf; Mock harms "
                "it and heals nobody\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld &w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr, "a world with ground, a Wolf next to the player and its fights");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return;
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor *wa = r.h.fx.world->registry().find(ww.wolf);
    CHECK(wa != nullptr, "(the Wolf is in the registry)");
    if (!wa) return;
    const repl::InlineObject cyl = inline_cylinder_of(*wa);
    verify_inline_axe_swing(r, setup, ww.wolf, cyl);
    verify_inline_mock(r, setup, ww.wolf, cyl);
}
static void verify_unacked_cylinder_swing(Run &r, const bs::PlayerSetup &setup, const world::WorldActor &wa, const abilities::AbilityDef &axe, int64_t targeting) {
    const emit::FIntrepidNetworkGUID cylinder = cylinder_guid_of(wa);
    const repl::InlineObject cyl = inline_cylinder_of(wa);
    r.h.hold = true;
    const size_t first = r.h.got.size();
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_value(setup, axe.row.guid, id, repl::Value(cyl));
    r.send_trigger(axe.row.guid, id, &trig);
    r.play(0.6, false);
    repl::HitComponentRef comp;
    comp.inline_object = cyl;
    r.send_reliable(hit_request_with(setup, axe.row.guid, targeting, axe.hits[0].hit_guid, comp));
    world_play(r, 0.5);
    r.h.release();
    r.absorb();
    bool item_named = false;
    bool item_exported = false;
    bool conf_named = false;
    bool conf_exported = false;
    for (size_t i = first; i < r.h.got.size(); ++i) {
        const fake::Delivered &d = r.h.got[i];
        if (d.channel == r.h.pawn_ch && exports_carry(d, cylinder)) item_exported = true;
        if (d.channel == r.h.pc_ch && exports_carry(d, cylinder)) conf_exported = true;
    }
    for (const auto &[key, a] : r.m.abilities) item_named = item_named || (a.ability == axe.row.guid && a.has_target);
    bool conf_at_wolf = false;
    for (const repl::HitInstance &h : r.m.hits) {
        for (const repl::HitTarget &t : h.targets) {
            conf_named = conf_named || t.hit_comp == wa.actor.guid;
            const double dx = t.impact_location.x - wa.x;
            const double dy = t.impact_location.y - wa.y;
            const double nlen = std::sqrt(t.impact_normal.x * t.impact_normal.x + t.impact_normal.y * t.impact_normal.y + t.impact_normal.z * t.impact_normal.z);
            if (t.hit_comp == wa.actor.guid && std::sqrt(dx * dx + dy * dy) < 100.0 && std::fabs(nlen - 1.0) < 1e-6) conf_at_wolf = true;
        }
    }
    CHECK(item_named, "the swing's item names a target (the Wolf's cylinder)");
    CHECK(item_exported, "the pawn update that carries the item exports the Wolf's collision "
                         "cylinder (the actor's GUID + 4, its ServerId and Randomizer)");
    CHECK(conf_named, "the hit's confirmation names the Wolf ACTOR as its "
                      "HitComp (retail: the actor, never the cylinder)");
    CHECK(conf_at_wolf, "its ImpactLocation is at the Wolf and its ImpactNormal a unit vector "
                        "(retail: the target's position, a unit normal)");
    CHECK(conf_exported, "the confirmation's chain exports the cylinder too (the "
                         "client had not acknowledged the item's export)");
}
static void verify_acked_cylinder_swing(Run &r, const bs::PlayerSetup &setup, const world::WorldActor &wa, const abilities::AbilityDef &axe, int64_t targeting) {
    const emit::FIntrepidNetworkGUID cylinder = cylinder_guid_of(wa);
    r.absorb();
    const size_t second = r.h.got.size();
    const int32_t id2 = r.next_id++;
    const BunchWriter trig2 = trigger_at_value(setup, axe.row.guid, id2, repl::Value(cylinder));
    r.send_trigger(axe.row.guid, id2, &trig2);
    r.play(0.6, false);
    r.send_reliable(hit_request_at_object(setup, axe.row.guid, targeting, axe.hits[0].hit_guid, cylinder));
    world_play(r, 0.5);
    bool again = false;
    size_t confirmed_by_guid = 0;
    for (size_t i = second; i < r.h.got.size(); ++i) again = again || exports_carry(r.h.got[i], cylinder);
    for (const repl::HitInstance &h : r.m.hits) {
        for (const repl::HitTarget &t : h.targets) confirmed_by_guid += t.hit_comp == wa.actor.guid ? 1 : 0;
    }
    CHECK(!again, "once acknowledged the cylinder is not exported again (UE's "
                  "ShouldSendFullPath)");
    CHECK(confirmed_by_guid >= 2, "a request naming the cylinder by its GUID is confirmed on the Wolf too");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}
static void verify_wolf_cylinder_subobject(const world::WorldActor &wa) {
    const emit::FIntrepidNetworkGUID cylinder = cylinder_guid_of(wa);
    const repl::Subobject *alignment = nullptr;
    for (const repl::Subobject &s : wa.actor.subobjects) {
        if (s.name == "AlignmentComponent") alignment = &s;
    }
    CHECK(cylinder.ObjectId == wa.id + 4 && alignment && alignment->guid.ObjectId == cylinder.ObjectId + 2 && cylinder.ServerId == wa.actor.guid.ServerId,
          "the Wolf's cylinder GUID is its actor's + 4 = its "
          "AlignmentComponent's - 2, with the actor's ServerId and Randomizer "
          "[R: 211 of 211 retail exports]");
}
static void test_e1_npc_cylinder_export() {
    std::printf("E1 stage 3 fix 2: the Wolf's collision cylinder is exported "
                "with the item and the confirmation that name it\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld &w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr, "a world with ground, a Wolf next to the player and its fights");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return;
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor *wa = r.h.fx.world->registry().find(ww.wolf);
    CHECK(wa != nullptr, "(the Wolf is in the registry)");
    if (!wa) return;
    verify_wolf_cylinder_subobject(*wa);
    const abilities::AbilityDef *axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    int64_t targeting = 0;
    for (const content::TargetingRow &t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(axe->row.guid);
    r.h.fx.world->tick_world(r.now());
    verify_unacked_cylinder_swing(r, setup, *wa, *axe, targeting);
    verify_acked_cylinder_swing(r, setup, *wa, *axe, targeting);
}
static void verify_one_clock_kill_and_corpse(Run &r, const bs::PlayerSetup &setup, uint64_t wolf, const abilities::AbilityDef &axe, int64_t targeting) {
    combat::CombatWorld &cw = *r.h.fx.world->combat();
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    const world::WorldActor *wa = r.h.fx.world->registry().find(wolf);
    const repl::InlineObject cyl = inline_cylinder_of(*wa);
    const auto swing = [&] {
        const int32_t id = r.next_id++;
        const BunchWriter trig = trigger_at_value(setup, axe.row.guid, id, repl::Value(cyl));
        r.send_trigger(axe.row.guid, id, &trig);
        r.play(0.6, false);
        repl::HitComponentRef comp;
        comp.inline_object = cyl;
        r.send_reliable(hit_request_with(setup, axe.row.guid, targeting, axe.hits[0].hit_guid, comp));
        world_play(r, 0.4);
    };
    const double max_hp = cw.max_health_of(wolf);
    swing();
    world_play(r, 1.0);
    CHECK(cw.health_of(wolf) < max_hp, "the swing hurt the Wolf");
    CHECK(r.h.s->character_stats() && r.h.s->character_stats()->in_combat(), "a second after the swing the player is still in combat (the 6 s "
                                                                             "window is on the world's clock)");
    double died_at_world = -1;
    for (int i = 0; i < 400 && !cw.dead(wolf); ++i) {
        swing();
        if (cw.dead(wolf)) died_at_world = r.now();
    }
    std::printf("  (the level-1 Wolf: %.1f Health at the start, dead %d)\n", max_hp, cw.dead(wolf) ? 1 : 0);
    CHECK(cw.dead(wolf), "the Wolf dies of the swings");
    if (!cw.dead(wolf)) return;
    const combat::Corpse *corpse = after.corpse_of(wolf);
    CHECK(corpse != nullptr && std::fabs(corpse->died_at - died_at_world) < 1.5, "the death is stamped on the world's clock (the aftermath's corpse has "
                                                                                 "the world time of the kill)");
    if (corpse) std::printf("  (died_at %.1f, the world's clock %.1f)\n", corpse->died_at, died_at_world);
    world_play(r, 2.0);
    CHECK(r.h.fx.world->registry().find(wolf) != nullptr && after.stats().corpses_closed == 0, "two seconds after the death the corpse still stands (it closes 15 s "
                                                                                               "after the death)");
    world_play(r, 14.0);
    CHECK(r.h.fx.world->registry().find(wolf) == nullptr && after.stats().corpses_closed == 1, "sixteen seconds after the death the corpse is gone");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}
static void test_e1_one_clock() {
    std::printf("E1 stage 3 fix 2: a kill through the session is on the world's "
                "clock (worldd older than the session)\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    ww.level = 1;
    g_world_time_offset = 100.0;
    Run r(0, true, &rec, [&](PlayerWorld &w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr && r.h.fx.world->aftermath() != nullptr, "a world with ground, a level-1 Wolf next to the player, its fights "
                                                                                                                  "and the aftermath");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat() || !r.h.fx.world->aftermath()) {
        g_world_time_offset = 0.0;
        return;
    }
    CHECK(r.h.s->bootstrap().setup().world_time >= 100.0, "the join's world time is the server's (100 s)");
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor *wa = r.h.fx.world->registry().find(ww.wolf);
    CHECK(wa != nullptr, "(the Wolf is in the registry)");
    const abilities::AbilityDef *axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!wa || !axe || axe->hits.empty()) {
        g_world_time_offset = 0.0;
        return;
    }
    int64_t targeting = 0;
    for (const content::TargetingRow &t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(axe->row.guid);
    r.h.fx.world->tick_world(r.now());
    verify_one_clock_kill_and_corpse(r, setup, ww.wolf, *axe, targeting);
    g_world_time_offset = 0.0;
}
struct BookSwingStep {
    const abilities::AbilityDef *book = nullptr;
    const abilities::HitDef *fx = nullptr;
    const abilities::HitDef *split = nullptr;
    const char *step = "";
    int anim_index = 0;
};
static void verify_book_swing_source(const world::WorldActor &wa, const BookSwingStep &s, const repl::HitInstance *fxc, const repl::HitInstance *spc) {
    const content::AbilityHitRow *carrier = nullptr;
    for (const content::AbilityHitRow &h : s.book->hits) {
        if (h.source == "projectile" && h.hit_guid == s.fx->row.guid) carrier = &h;
    }
    CHECK(carrier != nullptr, (std::string("step ") + s.step + ": (the FX hit is carried by a projectile row of the combo)").c_str());
    CHECK(fxc && carrier && fxc->source.ObjectId == 0 && fxc->source_record_type == content::kSourceTypeProjectile && fxc->source_record_guid == carrier->owner_guid,
          (std::string("step ") + s.step +
           ": the FX hit has no Source and the carrying projectile as its "
           "SourceRecordGuid (retail: Source 0, Weapon_Book_Projectile_n)")
              .c_str());
    CHECK(spc && carrier && spc->source == wa.actor.guid && spc->source_record_type == content::kSourceTypeProjectile && spc->source_record_guid == carrier->owner_guid,
          (std::string("step ") + s.step +
           ": the Split hit's Source is the Wolf ACTOR and its SourceRecordGuid "
           "the projectile that landed (retail 45/148: the origin of the split "
           "projectile)")
              .c_str());
}
static void verify_book_swing_confirmations(const world::WorldActor &wa, const BookSwingStep &s, const repl::HitInstance *fxc, const repl::HitInstance *spc, size_t others) {
    const bool fx_ok = fxc && fxc->targets.size() == 1 && fxc->targets[0].hit_comp == wa.actor.guid && fxc->targets[0].stat_mods.empty();
    CHECK(fx_ok && fxc->predicted_on_autonomous && fxc->predicted_on_simulated, (std::string("step ") + s.step +
                                                                                 ": the FX hit is confirmed on the Wolf actor with no StatMods, "
                                                                                 "predicted on both sides (retail: pa 1 ps 1)")
                                                                                    .c_str());
    const bool sp_ok = spc && spc->targets.size() == 1 && spc->targets[0].hit_comp == wa.actor.guid && !spc->targets[0].stat_mods.empty();
    CHECK(sp_ok && !spc->predicted_on_autonomous && !spc->predicted_on_simulated, (std::string("step ") + s.step +
                                                                                   ": the Split hit is confirmed on the Wolf actor with its StatMod, "
                                                                                   "predicted on neither side (retail: pa 0 ps 0)")
                                                                                      .c_str());
    CHECK(others == 0, (std::string("step ") + s.step +
                        ": no other hit of the combo is confirmed (one "
                        "projectile per swing, not ten)")
                           .c_str());
    verify_book_swing_source(wa, s, fxc, spc);
}
static void run_book_swing(Run &r, const bs::PlayerSetup &setup, const world::WorldActor &wa, const BookSwingStep &s) {
    combat::CombatWorld &cw = *r.h.fx.world->combat();
    const double max_hp = cw.max_health_of(wa.id), hp_before = cw.health_of(wa.id);
    const size_t hits_before = r.m.hits.size();
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_value(setup, s.book->row.guid, id, repl::Value(inline_cylinder_of(wa)), s.anim_index);
    r.send_trigger(s.book->row.guid, id, &trig);
    world_play(r, 1.5);
    const std::vector<int32_t> ends = r.m.end_ids();
    CHECK(r.m.has_ability(s.book->row.guid) && std::find(ends.begin(), ends.end(), id) == ends.end(), (std::string("step ") + s.step + ": the book's swing at the Wolf is accepted").c_str());
    const repl::HitInstance *fxc = nullptr, *spc = nullptr;
    size_t others = 0;
    std::set<int64_t> combo_hits;
    for (const content::AbilityHitRow &h : s.book->hits) combo_hits.insert(h.hit_guid);
    for (size_t i = hits_before; i < r.m.hits.size(); ++i) {
        const repl::HitInstance &h = r.m.hits[i];
        if (h.record_guid == s.fx->row.guid) fxc = &h;
        else if (h.record_guid == s.split->row.guid) spc = &h;
        else if (combo_hits.count(h.record_guid)) ++others;
    }
    verify_book_swing_confirmations(wa, s, fxc, spc, others);
    const double hp = cw.health_of(wa.id);
    const bool missed = spc && !spc->targets.empty() && (spc->targets[0].flags & stats::kHitMiss);
    std::printf("  (step %s: the Wolf %.1f -> %.1f of %.1f Health)\n", s.step, hp_before, hp, max_hp);
    CHECK(spc && (missed || hp < hp_before), (std::string("step ") + s.step + ": the Wolf lost Health to the Split hit (or it missed)").c_str());
}
static void run_book_combo_steps(Run &r, const bs::PlayerSetup &setup, const world::WorldActor &wa, const abilities::Catalog &cat) {
    const abilities::AbilityDef *book = cat.ability_named("WeaponCombo_Ranged_Book");
    const abilities::HitDef *fx1 = cat.hit_named("Weapon_Book_Hit_1"), *split1 = cat.hit_named("Book_Projectile_Split_Hit_1");
    const abilities::HitDef *fx2 = cat.hit_named("Weapon_Book_Hit_2"), *split2 = cat.hit_named("Book_Projectile_Split_Hit_2");
    CHECK(book != nullptr && fx1 && split1 && fx2 && split2, "the book combo and its FX and Split hits of steps 1 and 2 are in the "
                                                             "tables");
    if (!book || !fx1 || !split1 || !fx2 || !split2) return;
    CHECK(!split1->fires.empty() && fx1->fires.empty(), "the Split hit fires the split projectile when it lands "
                                                        "(ab_hit_projectile), the FX hit fires none");
    r.h.s->teach_ability(book->row.guid);
    r.h.fx.world->tick_world(r.now());
    run_book_swing(r, setup, wa, BookSwingStep{book, fx1, split1, "1", 0});
    CHECK(r.h.s->character_stats() && r.h.s->character_stats()->in_combat(), "the swing puts the player in combat");
    run_book_swing(r, setup, wa, BookSwingStep{book, fx2, split2, "2", 1});
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}
static void test_e1_ranged_book_at_wolf() {
    std::printf("E1 stage 3 fix 2: the book's auto attack at a Wolf (the step's FX hit "
                "and its Split hit, server-resolved, in one confirmation)\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld &w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr, "a world with ground, a Wolf next to the player and its fights");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return;
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor *wa = r.h.fx.world->registry().find(ww.wolf);
    CHECK(wa != nullptr, "(the Wolf is in the registry)");
    if (!wa) return;
    run_book_combo_steps(r, setup, *wa, *r.h.fx.world->ability_catalog());
}
static void verify_cylinder_block_exports(Run &r, const world::WorldActor &w1, size_t first, GuidCache &cache) {
    const emit::FIntrepidNetworkGUID cylinder = cylinder_guid_of(w1);
    uint64_t exported_cylinder = 0, exported_outer = 0;
    size_t cylinder_exports = 0;
    for (size_t i = first; i < r.h.got.size(); ++i) {
        for (const ExportRecord &e : export_records(r.h.got[i])) {
            if ((e.flags & 1) && e.path == "CollisionCylinder") {
                ++cylinder_exports;
                exported_cylinder = e.guid.ObjectId;
                exported_outer = e.outer.ObjectId;
            }
        }
        cache.take(r.h.got[i]);
    }
    CHECK(cylinder_exports >= 1 && exported_cylinder == cylinder.ObjectId && exported_outer == w1.id, "the item's and the confirmation's chains export the first Wolf's "
                                                                                                      "cylinder, its GUID the actor's + 4, the actor as the outer");
    for (const std::string &s : cache.described) std::printf("  (the client would refuse: %s)\n", s.c_str());
    CHECK(cache.mismatches == 0, "no GUID is exported under a second path or outer: nothing the client "
                                 "refuses (NetGuidMismatch)");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}
static void verify_cylinder_block_swing(Run &r, const bs::PlayerSetup &setup, const world::WorldActor &w1, GuidCache &cache) {
    const abilities::AbilityDef *axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    int64_t targeting = 0;
    for (const content::TargetingRow &t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(axe->row.guid);
    r.h.fx.world->tick_world(r.now());
    r.h.hold = true;
    const size_t first = r.h.got.size();
    const repl::InlineObject cyl = inline_cylinder_of(w1);
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_value(setup, axe->row.guid, id, repl::Value(cyl));
    r.send_trigger(axe->row.guid, id, &trig);
    r.play(0.6, false);
    repl::HitComponentRef comp;
    comp.inline_object = cyl;
    r.send_reliable(hit_request_with(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, comp));
    world_play(r, 0.5);
    r.h.release();
    r.absorb();
    bool confirmed = false;
    for (const repl::HitInstance &h : r.m.hits) {
        for (const repl::HitTarget &t : h.targets) confirmed = confirmed || t.hit_comp == w1.actor.guid;
    }
    CHECK(confirmed, "the hit is confirmed on the first Wolf");
    verify_cylinder_block_exports(r, w1, first, cache);
}
static void verify_cylinder_block_layout(Run &r, const world::WorldActor &w1, const world::WorldActor &w2, GuidCache &cache) {
    const uint64_t block = 2 * (3 + w1.actor.subobjects.size());
    CHECK(w2.id == w1.id + block, "the second Wolf's block follows the first's (the actor, its "
                                  "controller, its cylinder, its components)");
    const emit::FIntrepidNetworkGUID cylinder = cylinder_guid_of(w1);
    CHECK(cylinder.ObjectId == w1.id + 4 && cylinder.ObjectId > w1.id && cylinder.ObjectId < w2.id, "the first Wolf's cylinder GUID is inside its own block (the actor's + "
                                                                                                    "4)");
    CHECK(r.h.fx.world->combat()->actor_of_object(cylinder.ObjectId) == w1.id && r.h.fx.world->combat()->actor_of_object(w2.id + 6) != w1.id && r.h.fx.world->combat()->actor_of_object(w1.id + 24) != w1.id,
          "the combat world maps the cylinder's GUID to the first Wolf, and the "
          "second Wolf's AlignmentComponent (the first's + 24) never to the "
          "first (no arithmetic on ids)");
    for (const fake::Delivered &d : r.h.got) cache.take(d);
    const auto w2_alignment = cache.find_path("AlignmentComponent", w2.id);
    CHECK(w2_alignment != 0 && w2_alignment == w2.id + 6, "the second Wolf's open exported its AlignmentComponent (the actor's + "
                                                          "6) to the client");
    CHECK(w2_alignment == w1.id + 24, "(the draft's cylinder GUID, the first Wolf's + 24, IS that component: "
                                      "the user's collision)");
}
static void test_e1_cylinder_in_own_block() {
    std::printf("E1 stage 3 fix 3: the Wolf's cylinder GUID is inside its own block: a "
                "hit never exports a GUID another actor's open exported\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    ww.extra = 1;
    Run r(0, true, &rec, [&](PlayerWorld &w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && ww.wolves.size() == 1 && r.h.fx.world->combat() != nullptr, "a world with ground, two Wolves minted back to back next to the player");
    if (!ww.ground || ww.wolf == 0 || ww.wolves.size() != 1 || !r.h.fx.world->combat()) return;
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    r.play(1.0, true);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const world::WorldActor *w1 = r.h.fx.world->registry().find(ww.wolf), *w2 = r.h.fx.world->registry().find(ww.wolves[0]);
    CHECK(w1 && w2, "(both Wolves are in the registry)");
    if (!w1 || !w2) return;
    GuidCache cache;
    verify_cylinder_block_layout(r, *w1, *w2, cache);
    verify_cylinder_block_swing(r, setup, *w1, cache);
}
int main() {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log);
    auto logger = std::make_shared<spdlog::logger>("test", sink);
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    if (ability_rig::real_db_present()) {
        test_e1_inline_target_session();
        test_e1_npc_cylinder_export();
        test_e1_one_clock();
        test_e1_ranged_book_at_wolf();
        test_e1_cylinder_in_own_block();
    } else {
        std::printf("E1 ability session part5 tests: skipped (no content.db)\n");
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
