// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_ability_session_common.h"
#include "capture_abilities.h"
#include "capture_blocks.h"
#include "combat_rig.h"
#include "session/player_session.h"
#include "spawns/area.h"
#include "tooltip_key.h"
#include "world/relevancy.h"
using namespace aoc::worldd;
static void send_ready_to_phase_in(Run &r) {
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
}
static void test_e1_sprint_with_world() {
    std::printf("E1: a Sprint after the phase-in, with the living area around "
                "the player\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    std::string error;
    CHECK(r.h.fx.world->load_area(spawns::AreaConfig{}, error), ("the living area loads (" + error + ")").c_str());
    send_ready_to_phase_in(r);
    r.play(3.0, true);
    CHECK(r.h.s->actor_view() && r.h.s->actor_view()->count(world::ClientState::Awake) > 20, "(the world's actors are open on the client)");
    CHECK(r.m.stats.count(kStaminaRegeneration) != 0 && near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6), "joined: the client's StaminaRegeneration is 5");
    const float full = r.m.stamina_now();
    const size_t before = r.m.stamina.size();
    const int32_t id = r.trigger(kSprint);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "the Sprint's first packet sets StaminaRegeneration to 0 on the client");
    r.play(5.0, true);
    CHECK(near(full - r.m.stamina_now(), 26.25, 2.6), "5 s of Sprint while moving: the client's Stamina fell by about 25 (5 "
                                                      "a second)");
    bool only_down = r.m.stamina.size() > before + 15;
    for (size_t i = before + 1; i < r.m.stamina.size(); ++i) { only_down = only_down && near(r.m.stamina[i - 1].second - r.m.stamina[i].second, 1.25, 1e-6); }
    CHECK(only_down, "...in steps of 1.25, each one an item the client received");
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "StaminaRegeneration stayed 0 the whole time");
    r.cancel(id);
    r.play(0.5, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "the Sprint is released: the pause keeps StaminaRegeneration at 0");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}
static void test_e1_clock_resync() {
    std::printf("E1 stage 3 fix: ClientSetTime again after the phase-in and "
                "every 120 s\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    const size_t joined = r.m.set_times.size();
    r.play(2.0, false);
    CHECK(r.m.set_times.size() == joined, "nothing more before the phase-in");
    send_ready_to_phase_in(r);
    r.play(0.5, false);
    CHECK(r.m.set_times.size() == joined, "not with the phase-in itself (the client's frames still hitch)");
    r.play(1.0, false);
    CHECK(r.m.set_times.size() == joined + 1 && near(r.m.set_times.back(), bs::effect_clock(r.now()), 1.6), "1 s after the phase-in: ClientSetTime again, with the world time of "
                                                                                                            "its send on the synchronized clock");
    r.play(4.0, false);
    CHECK(r.m.set_times.size() == joined + 2 && near(r.m.set_times.back(), bs::effect_clock(r.now()), 1.6), "...and again 5 s after it");
    r.play(60.0, false);
    CHECK(r.m.set_times.size() == joined + 2, "...then not for a while");
    r.play(61.0, false);
    std::printf("  (%zu sent since the join, the last %.2f s behind the clock)\n", r.m.set_times.size() - joined, r.m.set_times.empty() ? 0.0 : bs::effect_clock(r.now()) - r.m.set_times.back());
    CHECK(r.m.set_times.size() == joined + 3 && near(r.m.set_times.back(), bs::effect_clock(r.now()), 2.0), "...then every 120 s, as retail (pkt#23 and every 120.0 s after)");
    CHECK(r.m.unreadable == 0, "(every chain reads)");
}
static void test_e1_sprint_under_load() {
    std::printf("E1: a Sprint while the client is busy with the world's actors\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    std::string error;
    CHECK(r.h.fx.world->load_area(spawns::AreaConfig{}, error), "the living area loads");
    send_ready_to_phase_in(r);
    r.play(0.3, true);
    r.h.hold = true;
    size_t peak_flight = 0;
    const int32_t id = r.trigger(kSprint);
    for (int i = 0; i < 30; ++i) {
        r.tick(true);
        peak_flight = std::max(peak_flight, r.h.conn.packets_in_flight());
    }
    CHECK(peak_flight <= 140, "the connection never has more than about a hundred packets in flight "
                              "while the client acknowledges nothing (the window is 256)");
    CHECK(r.h.conn.unreliable_dropped() == 0, "no unreliable chain of the pawn was dropped from the queue");
    CHECK(r.h.s->actor_view() && r.h.s->actor_view()->stats().throttled > 0, "the actor system held back (throttled passes)");
    r.h.release();
    r.h.settle();
    r.absorb();
    r.play(1.0, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "once the client is back: StaminaRegeneration is 0 on it");
    CHECK(near(r.m.stamina_now(), r.rt().stamina(), 1.3), "...and its Stamina is the runtime's (every drop item arrived)");
    r.cancel(id);
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}
static void run_loss_session_steps(Run &r, size_t &wraps, size_t &cycles, size_t &bad_cycles) {
    uint16_t last_seq = r.h.conn.notify().out_seq();
    for (int i = 0; i < 24000; ++i) {
        r.ts += 0.016f;
        r.h.client_sends({r.h.client.actor(r.h.pawn_ch, client_move(move_at(r.ts, r.x, false)), false)});
        r.h.step(milliseconds(16));
        r.absorb();
        const uint16_t seq = r.h.conn.notify().out_seq();
        if (seq < last_seq) { ++wraps; }
        last_seq = seq;
        if (i % 1000 == 100) { r.trigger(kDodge); }
        if (i % 1000 == 999) {
            ++cycles;
            if (!near(r.m.stamina_now(), r.rt().stamina(), 1.3)) {
                ++bad_cycles;
                std::printf("    cycle %zu at step %d: client Stamina %.2f, runtime "
                            "%.2f (out seq %u)\n",
                            cycles, i, r.m.stamina_now(), r.rt().stamina(), seq);
            }
        }
    }
}
static void test_e1_long_session_with_loss(unsigned loss_percent, unsigned burst) {
    std::printf("E1: a long session across the packet sequence wrap, %u%% loss, "
                "bursts of %u\n",
                loss_percent, burst);
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    std::string error;
    r.h.fx.world->load_area(spawns::AreaConfig{}, error);
    send_ready_to_phase_in(r);
    std::mt19937 rng(7);
    auto link = [&, left = std::make_shared<unsigned>(0)](const std::vector<uint8_t> &) {
        if (*left > 0) {
            --*left;
            return true;
        }
        if (rng() % 100 < loss_percent) {
            *left = burst - 1;
            return true;
        }
        return false;
    };
    if (loss_percent > 0) {
        r.h.lose_to_client = link;
        r.h.lose_to_server = link;
    }
    size_t wraps = 0;
    size_t bad_cycles = 0;
    size_t cycles = 0;
    run_loss_session_steps(r, wraps, cycles, bad_cycles);
    std::printf("    wraps %zu, cycles %zu, bad %zu, packets in flight %zu, "
                "unreliable dropped %zu, lost to client %zu, to server %zu\n",
                wraps, cycles, bad_cycles, r.h.conn.packets_in_flight(), r.h.conn.unreliable_dropped(), r.h.lost_to_client, r.h.lost_to_server);
    CHECK(wraps >= 1 && bad_cycles == 0, "the client's Stamina follows the runtime's across the packet sequence "
                                         "wrap");
}
static void test_e1_clock_reset() {
    std::printf("E1: the client's movement clock resets at 240 s\n");
    Run r;
    r.ts = 238.0f;
    r.play(1.0, true);
    const size_t acked_before = r.h.s->moves_acknowledged();
    CHECK(acked_before >= 9, "(the moves of the first 239 s are acknowledged)");
    r.ts = 239.9f;
    r.play(0.1, true);
    r.ts = 0.0f;
    const size_t before_reset = r.h.s->moves_acknowledged();
    r.play(1.0, true);
    CHECK(r.h.s->moves_acknowledged() >= before_reset + 9, "the moves after the reset (TimeStamp 0.1 ...) are acknowledged, one "
                                                           "by one");
    CHECK(r.h.s->moves_stale() == 0 && r.h.s->moves_refused() == 0, "none is dropped as a duplicate, none refused");
    CHECK(r.h.s->movement().good().time_stamp < 5.0f, "the last good TimeStamp follows the client's new clock");
    const int32_t id = r.trigger(kSprint);
    r.play(3.0, true);
    CHECK(r.m.stamina_now() < 96.0f && r.rt().sprinting(), "a Sprint run after the reset drains the client's Stamina (the server "
                                                           "hears the player move)");
    CHECK(r.m.stamina.size() >= 10, "...in steps the HUD sees");
    r.cancel(id);
    r.ts = 239.95f;
    r.play(0.2, true);
    r.ts = 0.0f;
    const size_t before_second = r.h.s->moves_acknowledged();
    r.play(1.0, true);
    CHECK(r.h.s->moves_acknowledged() >= before_second + 9, "and again at the next 240 s");
    const size_t acked = r.h.s->moves_acknowledged();
    r.ts -= 2.0f;
    r.play(0.1, true);
    CHECK(r.h.s->moves_acknowledged() == acked && r.h.s->moves_stale() >= 1, "a TimeStamp 2 s back is no reset: dropped without a reply");
}
static bool verify_maddening_dance_open(Run &r, double t_trigger, uint32_t &out_channel) {
    r.trigger(kMaddening);
    r.play(0.2, false);
    CHECK(r.m.other.empty(), "nothing at the trigger and 200 ms on: the "
                             "montage's first notify comes at 300 ms");
    r.play(0.4, false);
    CHECK(r.m.other.size() == 1 && r.m.other[0].open && !r.m.other[0].close, "Maddening Dance: one actor channel opens");
    if (r.m.other.empty()) { return false; }
    out_channel = r.m.other[0].channel;
    const double opened_at = r.m.other[0].at - t_trigger;
    CHECK(opened_at >= 0.3 && opened_at <= 0.55, "within 300 to 500 ms of the trigger, as retail's (+362 ms)");
    CHECK(r.m.other[0].exports >= 1 && r.m.other[0].bits > 1500 && r.m.other[0].bits < 2600, "it is the retail actor's size: an export bunch (the archetype, the "
                                                                                             "pawn's mesh, its three components) and about 2,000 bits");
    CHECK(out_channel != r.h.pawn_ch && out_channel != r.h.pc_ch && r.h.s->zones_opened() == 1 && r.h.s->zone_actors() == 1, "(on a channel of its own; the session holds it)");
    r.play(3.0, false);
    CHECK(r.m.other.size() == 1 && r.h.s->zone_actors() == 1 && !r.h.s->abilities_failed(), "3.6 s on: no close, no update");
    return true;
}
static void verify_maddening_dance_close(Run &r, double t_trigger, uint32_t channel) {
    r.h.s->teach_ability(kFlourish);
    r.trigger(kFlourish);
    r.play(0.8, false);
    CHECK(r.m.other.size() == 1, "(a Flourish meanwhile does not end it)");
    r.play(1.8, false);
    bool faded = false;
    bool closed = false;
    double closed_at = 0;
    for (size_t i = 1; i < r.m.other.size(); ++i) {
        const Mirror::ChannelEvent &e = r.m.other[i];
        if (e.channel != channel) { continue; }
        faded = faded || (!e.open && !e.close && e.bits > 0);
        if (e.close) {
            closed = true;
            closed_at = e.at - t_trigger;
        }
    }
    CHECK(faded && closed, "6.2 s after the trigger: the actor got its fade-out "
                           "update (bFadingOut) and its channel is closed");
    CHECK(closed && closed_at > 5.1 && closed_at < 5.8, "the close came about 5.4 s after the trigger [R: 5,383 ms]");
    CHECK(r.h.s->zone_actors() == 0 && r.h.s->zones_closed() == 1 && r.h.s->zone_ops_pending() == 0, "(the session holds none: one opened, one closed)");
}
static void verify_maddening_dance_actor(Run &r) {
    const double t_trigger = r.now();
    uint32_t channel = 0;
    if (verify_maddening_dance_open(r, t_trigger, channel)) { verify_maddening_dance_close(r, t_trigger, channel); }
}
static void verify_shielding_dance_actor(Run &r) {
    r.play(20.0, false);
    const double t_shield = r.now();
    r.trigger(kShielding);
    r.play(1.0, false);
    CHECK(r.m.has_effect(6064631046974416627ll) && r.m.other.size() > 2, "Shielding Dance: its Aura is on the Bard and a second actor is open");
    const size_t events = r.m.other.size();
    r.play(3.0, false);
    CHECK(r.m.has_effect(6064631046974416627ll), "4 s on: the Aura is still there");
    r.play(2.0, false);
    CHECK(!r.m.has_effect(6064631046974416627ll), "6 s on: the Aura is gone (it stayed for good before)");
    bool second_closed = false;
    double second_at = 0;
    for (size_t i = events; i < r.m.other.size(); ++i) {
        if (r.m.other[i].close) {
            second_closed = true;
            second_at = r.m.other[i].at - t_shield;
        }
    }
    CHECK(second_closed && second_at > 5.0 && second_at < 5.6 && r.h.s->zone_actors() == 0 && !r.h.s->abilities_failed(), "and the second actor is closed about 5.2 s after the trigger [R: "
                                                                                                                          "5,123 and 5,371 ms]");
}
static void test_d2_dance_actor() {
    std::printf("D2: a dance opens the zone's actor on a channel of its own and "
                "destroys it when the dance ends\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    send_ready_to_phase_in(r);
    r.play(0.5, true);
    r.h.s->teach_ability(kShielding);
    r.h.s->teach_ability(kMaddening);
    CHECK(r.m.other.empty() && r.h.s->zone_actors() == 0, "(no actor before a dance)");
    verify_maddening_dance_actor(r);
    verify_shielding_dance_actor(r);
}
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
static BunchWriter trigger_at_object(const bs::PlayerSetup &setup, int64_t guid, int32_t id, const emit::FIntrepidNetworkGUID &object) { return trigger_at_value(setup, guid, id, repl::Value(object)); }
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
static repl::InlineObject inline_cylinder_of(const world::WorldActor &wa) {
    repl::InlineObject cyl;
    cyl.flags = 5;
    cyl.outer = wa.actor.guid;
    cyl.path = "CollisionCylinder";
    cyl.checksum = 0xb876932f;
    return cyl;
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
struct ZoneExportStats {
    size_t total = 0;
    size_t zones_opened = 0;
    size_t refused = 0;
    size_t bard_dances = 0;
    size_t bard_zones = 0;
};
struct ClassSpec {
    int id;
    const char *name;
    int64_t archetype;
};
static std::vector<const abilities::AbilityDef *> collect_zone_ability_defs(const abilities::Catalog *cat, const char *class_name, size_t limit) {
    std::vector<const abilities::AbilityDef *> defs;
    if (!cat) return defs;
    for (const tooltip_key::Key &k : tooltip_key::kKeys) {
        if (std::strcmp(k.group, class_name) != 0) continue;
        const abilities::AbilityDef *def = cat->ability(k.guid);
        if (!def) continue;
        bool zone = !def->zones.empty();
        for (const content::AbilityHitRow &h : def->hits) {
            const abilities::HitDef *hit = cat->hit(h.hit_guid);
            zone = zone || (hit && !hit->zones.empty());
        }
        if (zone && defs.size() < limit) defs.push_back(def);
    }
    return defs;
}
static void cast_zone_ability_and_hit_wolf(Run &r, const ClassSpec &cls, const abilities::AbilityDef &def, uint64_t target_wolf, ZoneExportStats &stats) {
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const abilities::AbilityDef *axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    int64_t targeting = 0;
    for (const content::TargetingRow &t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    ++stats.total;
    const bool dance = def.row.name.find("_Dance_") != std::string::npos && cls.id == 2;
    if (dance) ++stats.bard_dances;
    r.h.s->teach_ability(def.row.guid);
    r.h.fx.world->tick_world(r.now());
    const size_t others_before = r.m.other.size();
    const int32_t id = r.trigger(def.row.guid);
    r.play(0.8, false);
    r.h.fx.world->tick_world(r.now());
    size_t opened = 0;
    for (size_t k = others_before; k < r.m.other.size(); ++k) opened += r.m.other[k].open ? 1 : 0;
    const std::vector<int32_t> ends = r.m.end_ids();
    const bool ended = std::find(ends.begin(), ends.end(), id) != ends.end();
    if (ended) ++stats.refused;
    if (opened > 0) ++stats.zones_opened;
    if (dance) {
        stats.bard_zones += opened > 0 ? 1 : 0;
        CHECKF(opened > 0 && !ended, "Bard: %s opens its zone's actor (%zu opened, %s)", def.row.name.c_str(), opened, ended ? "ended" : "accepted");
    }
    const world::WorldActor *wa = r.h.fx.world->registry().find(target_wolf);
    if (!wa) return;
    const repl::InlineObject cyl = inline_cylinder_of(*wa);
    const int32_t swing = r.next_id++;
    const BunchWriter trig = trigger_at_value(setup, axe->row.guid, swing, repl::Value(cyl));
    r.send_trigger(axe->row.guid, swing, &trig);
    r.play(0.5, false);
    repl::HitComponentRef comp;
    comp.inline_object = cyl;
    r.send_reliable(hit_request_with(setup, axe->row.guid, targeting, axe->hits[0].hit_guid, comp));
    for (int k = 0; k < 6; ++k) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
}
static void run_class_zone_abilities(const ClassSpec &cls, ZoneExportStats &stats) {
    constexpr int kSpareWolves = 8;
    const svc::Character rec = class25(cls.id, cls.name, cls.archetype);
    WolfWorld ww;
    ww.extra = kSpareWolves;
    Run r(0, true, &rec, [&](PlayerWorld &w) { ww.prepare(w); });
    CHECKF(ww.ground && ww.wolf != 0 && ww.wolves.size() == kSpareWolves && r.h.fx.world->combat() != nullptr, "%s: a world with a Wolf for each zone ability", cls.name);
    if (!ww.ground || ww.wolf == 0 || ww.wolves.size() != kSpareWolves || !r.h.fx.world->combat()) return;
    const auto defs = collect_zone_ability_defs(r.h.fx.world->ability_catalog(), cls.name, kSpareWolves);
    if (defs.empty()) return;
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        BunchWriter phase;
        phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        r.send_reliable(phase);
    }
    r.play(1.0, true);
    const abilities::AbilityDef *axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    if (!axe || axe->hits.empty()) {
        CHECK(false, "the 1H axe combo is in the tables");
        return;
    }
    r.h.s->teach_ability(axe->row.guid);
    for (size_t i = 0; i < defs.size(); ++i) { cast_zone_ability_and_hit_wolf(r, cls, *defs[i], ww.wolves[i], stats); }
    r.play(6.0, false);
    r.absorb();
    GuidCache cache;
    for (const fake::Delivered &d : r.h.got) cache.take(d);
    for (const std::string &s : cache.described) std::printf("  (%s: the client would refuse: %s)\n", cls.name, s.c_str());
    CHECKF(cache.mismatches == 0,
           "%s: no GUID of any chain (the zones' actors, the Wolves' opens, the "
           "items, the confirmations) is exported under a second path or outer",
           cls.name);
    CHECKF(!r.h.s->abilities_failed() && r.m.unreadable == 0, "%s: the session is fine, every chain reads", cls.name);
    std::printf("  (%s: %zu zone abilities cast)\n", cls.name, defs.size());
}
static void test_e1_zone_abilities_export_rule() {
    std::printf("E1 stage 3 fix 3: every class's dances and lingering-zone "
                "abilities, each with a Wolf hit while its zone stands: nothing "
                "the client refuses\n");
    const ClassSpec classes[] = {{1, "Ranger", 17751}, {2, "Bard", 17747}, {3, "Cleric", 17748}, {4, "Fighter", 17749}, {5, "Rogue", 17752}, {6, "Summoner", 17753}, {7, "Tank", 17754}, {8, "Mage", 17750}};
    ZoneExportStats stats;
    for (const ClassSpec &cls : classes) { run_class_zone_abilities(cls, stats); }
    std::printf("  (%zu zone abilities of every class: %zu opened a zone actor, %zu were "
                "ended by the server; the Bard's dances: %zu of %zu opened)\n",
                stats.total, stats.zones_opened, stats.refused, stats.bard_zones, stats.bard_dances);
    CHECK(stats.total >= 8 && stats.bard_dances >= 4 && stats.bard_zones == stats.bard_dances, "every Bard dance opened its zone; the other classes' zone abilities "
                                                                                               "were cast");
}
static void verify_combat_session_swing(Run &r, const bs::PlayerSetup &setup, uint64_t wolf, const abilities::AbilityDef &axe) {
    combat::CombatWorld &cw = *r.h.fx.world->combat();
    const int64_t ability = axe.row.guid;
    const int64_t hit = axe.hits[0].hit_guid;
    int64_t targeting = 0;
    for (const content::TargetingRow &t : axe.targetings) {
        if (t.key == axe.hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(ability);
    emit::FIntrepidNetworkGUID cylinder;
    cylinder.ObjectId = wolf + 4;
    cylinder.ServerId = kServerId;
    cylinder.Randomizer = kRandomizer;
    r.h.fx.world->tick_world(r.now());
    const double max_hp = cw.max_health_of(wolf);
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_object(setup, ability, id, cylinder);
    r.send_trigger(ability, id, &trig);
    r.play(0.6, false);
    const std::vector<int32_t> ends = r.m.end_ids();
    CHECK(r.m.has_ability(ability) && std::find(ends.begin(), ends.end(), id) == ends.end(), "the cast at the Wolf is accepted: its ability item is there, no "
                                                                                             "ClientEndAbility");
    const BunchWriter req = hit_request_at_object(setup, ability, targeting, hit, cylinder);
    r.send_reliable(req);
    for (int i = 0; i < 5; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    const repl::HitInstance *conf = nullptr;
    for (const repl::HitInstance &h : r.m.hits) {
        if (!h.targets.empty()) conf = &h;
    }
    CHECK(conf != nullptr, "the hit is confirmed to the client with a target");
    const double hp = cw.health_of(wolf);
    const bool missed = conf && (conf->targets[0].flags & stats::kHitMiss);
    std::printf("  (the Wolf: %.1f of %.1f Health)\n", hp, max_hp);
    CHECK(missed || hp < max_hp, "the Wolf lost Health (or the hit missed)");
}
static void verify_combat_session_retaliation_and_exit(Run &r, uint64_t wolf) {
    combat::CombatWorld &cw = *r.h.fx.world->combat();
    double bitten_at = -1;
    const float full = r.m.stats[content::kGuidMaxHealth];
    for (int i = 0; i < 80 && bitten_at < 0; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
        if (r.m.stats[content::kGuidHealth] < full) bitten_at = r.now();
    }
    CHECK(bitten_at >= 0, "the Wolf, hurt by the player, goes for it and its "
                          "bite takes the player's Health");
    CHECK(r.h.s->character_stats() && r.h.s->character_stats()->in_combat(), "the player is in combat");
    const auto ai = cw.ai_state(wolf);
    CHECK(ai.has_value() && *ai != npc::ai::State::Idle, "the Wolf's AI is fighting (Chase or Attack)");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    cw.apply_hit_to_npc(wolf, 1, kill, r.now());
    for (int i = 0; i < 90; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
    CHECK(r.h.s->character_stats() && !r.h.s->character_stats()->in_combat(), "with the Wolf dead and no exchange for six seconds the player is out "
                                                                              "of combat");
}
static void test_e1_combat_session() {
    std::printf("E1 stage 3: a player's fight with a Wolf through the session\n");
    const svc::Character rec = bard25();
    WolfWorld ww;
    Run r(0, true, &rec, [&](PlayerWorld &w) { ww.prepare(w); });
    CHECK(ww.ground && ww.wolf != 0 && r.h.fx.world->combat() != nullptr, "a world with ground, a Wolf next to the player (off the ground "
                                                                          "fixture: it stands) and its fights");
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return;
    r.play(0.5, false);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    CHECK(r.h.s->character_stats() && !r.h.s->character_stats()->in_combat(), "the player is not in combat to begin with");
    const abilities::AbilityDef *axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    verify_combat_session_swing(r, setup, ww.wolf, *axe);
    verify_combat_session_retaliation_and_exit(r, ww.wolf);
}
int main() {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log);
    auto logger = std::make_shared<spdlog::logger>("test", sink);
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    if (ability_rig::real_db_present()) {
        test_e1_sprint_with_world();
        test_e1_clock_resync();
        test_e1_sprint_under_load();
        test_e1_long_session_with_loss(0, 1);
        test_e1_long_session_with_loss(5, 4);
        test_e1_long_session_with_loss(10, 20);
        test_e1_clock_reset();
        test_d2_dance_actor();
        test_e1_zone_abilities_export_rule();
        test_e1_combat_session();
    } else {
        std::printf("E1/D2 ability session combat tests: skipped (no content.db)\n");
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
