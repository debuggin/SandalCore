// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "combat_rig.h"
#include "combat/hit_resolver.h"
#include "combat/npc_ability_data.h"
#include "combat/npc_ability_replicator.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/move_packed.h"
#include "spawns/generic_npc.h"

#ifndef NPC_MOTION_PATH
#define NPC_MOTION_PATH "dist/content/npc_motion.tsv"
#endif

namespace sandal::worldd {
namespace content = ::aoc::worldd::content;
}  // namespace sandal::worldd

namespace sandal::worldd::tests {

using namespace ability_rig;
using namespace aoc::worldd;
namespace repl = ::aoc::protocol::repl;

struct Fixture {
    std::string                        path;
    std::optional<content::ContentDb>  db;
    const bootstrap::StatContext*      ctx = nullptr;
    npc::MotionTable                   motion;
    std::optional<combat::CombatData>  data;
    std::optional<abilities::Catalog>  catalog;
    std::string                        problem;

    Fixture() {
        path = real_db_path();
        std::string error;
        db = content::ContentDb::open(path, error);
        if (!db) {
            problem = "content.db: " + error;
            return;
        }
        ctx = &world_stats::of(*db);
        if (!motion.load(NPC_MOTION_PATH, error)) {
            problem = std::string("npc_motion.tsv: ") + error;
            return;
        }
        const auto rows = db->combat_rows(error);
        if (!rows) {
            problem = "the combat tables: " + error;
            return;
        }
        data = combat::CombatData::build(*rows, *ctx->bundle, error);
        if (!data) {
            problem = "the combat data: " + error;
            return;
        }
        catalog = abilities::Catalog::load(path, *ctx->bundle, error);
        if (!catalog) problem = "the ability tables: " + error;
    }

    bool ok() const { return problem.empty(); }

    std::optional<npc::MotionStats> blueprint(const std::string& leaf) const {
        const std::string suffix = "." + std::string("Default__") + leaf;
        for (const auto& [row_path, row] : motion.rows()) {
            if (row_path.size() > suffix.size() && row_path.compare(row_path.size() - suffix.size(), suffix.size(), suffix) == 0) return row;
        }
        return std::nullopt;
    }
};

struct TestHarness {
    Fixture& f;
    int      pass = 0;
    int      fail = 0;
};

#define CHECK(cond, msg) do { \
    if (cond) { h.pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); h.fail++; } \
} while (0)

#define CHECKF(cond, ...) do { \
    if (cond) { h.pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); h.fail++; } \
} while (0)

inline bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

inline double wrap180(double a) {
    while (a > 180.0) a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}

inline std::string creature_path(Fixture& f, const char* leaf) {
    std::string found;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find(leaf) != std::string::npos) found = path;
    }
    return found;
}

inline bool load_creature_tables(combat::NpcAbilityData& tables) {
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string err;
    if (tables.load(dir + "npc_abilities.tsv", dir + "npc_montages.tsv", dir + "npc_targeting.tsv", err)) return true;
    std::printf("  [skip] the creature tables (tools/npcabilities, the npcabilities extractor step): %s\n", err.c_str());
    return false;
}

struct GremCastView {
    int32_t                                   element = 0;
    int64_t                                   ability = 0;
    uint64_t                                  montage = 0;
    aoc::protocol::emit::FIntrepidNetworkGUID target;
    aoc::protocol::emit::FIntrepidNetworkGUID instigator;
};

inline GremCastView grem_cast_view(world::ActorRegistry& registry, uint64_t actor) {
    GremCastView v;
    world::WorldActor* a = registry.find(actor);
    repl::FastArrayState* arr = a ? combat::npc_ability_detail::active_abilities(*a) : nullptr;
    if (!arr) return v;
    for (const repl::FastArrayItem& it : arr->items()) {
        v.element = it.id;
        const auto g = it.values.values.find(1);
        if (g != it.values.values.end() && g->second.get_if<int64_t>()) v.ability = *g->second.get_if<int64_t>();
        const auto m = it.values.values.find(11);
        if (m != it.values.values.end() && m->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()) {
            v.montage = m->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()->ObjectId;
        }
        const auto t = it.values.values.find(3);
        const repl::ArrayValue* targets = t != it.values.values.end() ? t->second.get_if<repl::ArrayValue>() : nullptr;
        v.target = {};
        if (targets && !targets->elements.empty()) {
            const auto first = targets->elements[0].values.find(1);
            if (first != targets->elements[0].values.end() && first->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()) {
                v.target = *first->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>();
            }
        }
        const auto inst = it.values.values.find(17);
        v.instigator = inst != it.values.values.end() && inst->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()
                           ? *inst->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()
                           : aoc::protocol::emit::FIntrepidNetworkGUID{};
    }
    return v;
}

struct LungeWire {
    double t = 0;
    int    active = -1;
    int    obstructed = -1;
    double position = -1;
    bool   has_mv_yaw = false, has_rrm_yaw = false;
    double mv_yaw = 0, rrm_yaw = 0;
    bool   has_rrm_vz = false, has_rrm_z = false;
    double rrm_vz = 0, rrm_z = 0;
};

inline LungeWire decode_lunge_wire_state(const aoc::protocol::schema::ClassSchema& sc, const repl::ObjectState& st, double now) {
    LungeWire w;
    w.t = now;
    if (const auto* v = st.state.get(sc, "RepRootMotion.bIsActive")) {
        if (v->get_if<bool>()) w.active = *v->get_if<bool>() ? 1 : 0;
    }
    if (const auto* v = st.state.get(sc, "bRootMotionObstructed")) {
        if (v->get_if<bool>()) w.obstructed = *v->get_if<bool>() ? 1 : 0;
    }
    if (const auto* v = st.state.get(sc, "RepRootMotion.Position")) {
        if (v->get_if<float>()) w.position = *v->get_if<float>();
    }
    if (const auto* v = st.state.get(sc, "ReplicatedMovement")) {
        if (const auto* m = v->get_if<repl::RepMovement>()) {
            w.has_mv_yaw = true;
            w.mv_yaw = m->rotation.yaw * 360.0 / (st.opts.rep_movement_short_rotation ? 65536.0 : 256.0);
        }
    }
    if (const auto* v = st.state.get(sc, "RepRootMotion.Rotation")) {
        if (const auto* r = v->get_if<repl::Rot>()) {
            w.has_rrm_yaw = true;
            w.rrm_yaw = r->yaw * 360.0 / 65536.0;
        }
    }
    if (const auto* v = st.state.get(sc, "RepRootMotion.LinearVelocity")) {
        if (const auto* q = v->get_if<repl::QuantVec>()) {
            w.has_rrm_vz = true;
            w.rrm_vz = repl::dequantize_vector(*q, 10).z;
        }
    }
    if (const auto* v = st.state.get(sc, "RepRootMotion.Location")) {
        if (const auto* q = v->get_if<repl::QuantVec>()) {
            w.has_rrm_z = true;
            w.rrm_z = repl::dequantize_vector(*q, 100).z;
        }
    }
    return w;
}

inline void read_lunge_wire(world::WorldActor& actor, repl::ActorShadow& shadow, double now, std::vector<LungeWire>& out) {
    std::vector<repl::ContentBlock> blocks;
    if (!repl::collect_update_blocks(actor.actor, shadow, blocks)) return;
    for (const repl::ContentBlock& b : blocks) {
        if (b.sub || !b.has_rep_layout) continue;
        const auto st = repl::load_object_state(*actor.actor.obj.schema, b.payload.data(), b.payload.bit_pos(), true);
        if (!st) continue;
        out.push_back(decode_lunge_wire_state(*actor.actor.obj.schema, *st, now));
    }
}

struct LungeSample {
    double                                    t = 0, wx = 0, wy = 0, px = 0, py = 0, position = -1;
    int                                       active = 0, obstructed = 0;
    int64_t                                   ability = 0;
    uint64_t                                  montage = 0;
    bool                                      cast_began = false;
    double                                    yaw = 0;
    double                                    mv_yaw = 0, rrm_yaw = 0;
    bool                                      has_mv_yaw = false, has_rrm_yaw = false;
    double                                    wz = 0;
    bool                                      has_rrm_vz = false;
    double                                    rrm_vz = 0;
    aoc::protocol::emit::FIntrepidNetworkGUID item_target, item_instigator, active_target, creature_guid;
};

using LungeSteer = std::function<bool(double now, const LungeSample* prev, double& px, double& py, const world::WorldActor& creature)>;

struct LungeFightSpec {
    std::string                      path;
    double                           seconds = 0;
    double                           player_x = 0;
    double                           player_y = 0;
    LungeSteer                       steer = nullptr;
    std::vector<combat::IncomingHit>* hits = nullptr;
    bool                             timid = false;
    double                           provoke_at = -1;
    combat::CombatWorld::Stats*      stats = nullptr;
    double                           leash_cm = 0;
    const spawns::OpenTemplate*      made = nullptr;
    double                           creature_x = 0;
    double                           creature_y = 0;
};

struct LungeLoopState {
    GremCastView last;
    LungeSample  prev;
    bool         have_prev = false;
    int          active = 0, obstructed = 0;
    double       mv_yaw = 0, rrm_yaw = 0;
    bool         has_mv_yaw = false, has_rrm_yaw = false;
    double       px = 0, py = 0;
    double       now = 0, next_hit = -1;
};

inline LungeSample record_lunge_sample(combat_rig::World& w, uint64_t creature, repl::ActorShadow& shadow, LungeLoopState& st) {
    world::WorldActor* actor = w.registry.find(creature);
    std::vector<LungeWire> wire;
    read_lunge_wire(*actor, shadow, st.now, wire);
    double position = -1, tick_vz = 0;
    bool tick_has_vz = false;
    for (const LungeWire& u : wire) {
        if (u.active >= 0) st.active = u.active;
        if (u.obstructed >= 0) st.obstructed = u.obstructed;
        if (u.position >= 0) position = u.position;
        if (u.has_mv_yaw) st.mv_yaw = u.mv_yaw, st.has_mv_yaw = true;
        if (u.has_rrm_yaw) st.rrm_yaw = u.rrm_yaw, st.has_rrm_yaw = true;
        if (u.has_rrm_vz) tick_vz = u.rrm_vz, tick_has_vz = true;
    }
    const GremCastView view = grem_cast_view(w.registry, creature);
    const world::WorldActor* g = w.registry.find(creature);
    LungeSample s;
    s.t = st.now;
    s.wx = g->x;
    s.wy = g->y;
    s.px = st.px;
    s.py = st.py;
    s.position = position;
    s.active = st.active;
    s.obstructed = st.obstructed;
    s.ability = view.ability;
    s.montage = view.montage;
    s.cast_began = view.montage != 0 && (view.element != st.last.element || st.last.montage == 0);
    s.yaw = g->yaw_degrees;
    s.mv_yaw = st.mv_yaw;
    s.rrm_yaw = st.rrm_yaw;
    s.has_mv_yaw = st.has_mv_yaw;
    s.has_rrm_yaw = st.has_rrm_yaw;
    s.wz = g->z;
    s.has_rrm_vz = tick_has_vz;
    s.rrm_vz = tick_vz;
    s.item_target = view.target;
    s.item_instigator = view.instigator;
    s.active_target = combat_rig::active_target_guid(*g);
    s.creature_guid = g->actor.guid;
    st.last = view;
    st.prev = s;
    st.have_prev = true;
    return s;
}

inline bool run_lunge_fight(Fixture& f, const combat::NpcAbilityData& tables, const LungeFightSpec& spec, std::vector<LungeSample>& out) {
    Rig rig(4, 20, {});
    if (!rig.ok()) return false;
    combat_rig::World w;
    if (!w.problem.empty()) return false;
    const auto bp = f.motion.find(spec.path);
    if (!bp) return false;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t creature = w.spawn(spec.path, spec.creature_x, spec.creature_y, 7, 900, bp->half_height, spec.made);
    if (creature == 0) return false;
    world::WorldActor* actor = w.registry.find(creature);
    if (spec.timid) actor->npc.aggro_off = true;
    if (spec.leash_cm > 0) actor->npc.leash_radius_cm = spec.leash_cm;
    repl::ActorShadow shadow;
    repl::ExportAcks acks;
    bool failed = false;
    const auto open = repl::open_actor(actor->actor, shadow, acks, &failed);
    if (!open.has_value() || failed) return false;
    LungeLoopState st;
    st.px = spec.player_x;
    st.py = spec.player_y;
    st.next_hit = spec.provoke_at;
    combat_rig::FakePlayer player(rig, 77, {st.px, st.py, 1098});
    world.add_player(&player);
    while (st.now < spec.seconds) {
        st.now += 1.0 / 15.0;
        if (spec.steer && spec.steer(st.now, st.have_prev ? &st.prev : nullptr, st.px, st.py, *w.registry.find(creature))) {
            player.rig_fighter().move_to({st.px, st.py, 1098});
        }
        if (st.next_hit >= 0 && st.now >= st.next_hit) {
            st.next_hit = st.now + 1.0;
            world.add_threat(creature, 77, 100.0, st.now);
        }
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
        w.npcs->note_player(77, st.px, st.py, st.now);
        w.npcs->tick(st.now);
        world.tick(st.now);
        out.push_back(record_lunge_sample(w, creature, shadow, st));
    }
    if (spec.hits) *spec.hits = player.hits;
    if (spec.stats) *spec.stats = world.stats();
    return true;
}

struct LungeRun {
    size_t first = 0, last = 0;
};

inline std::vector<LungeRun> lunge_runs(const std::vector<LungeSample>& s, int64_t ability = 0) {
    std::vector<LungeRun> runs;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].active != 1 || (i > 0 && s[i - 1].active == 1)) continue;
        size_t j = i;
        while (j + 1 < s.size() && s[j + 1].active == 1) ++j;
        if ((ability == 0 || s[i].ability == ability) && j + 1 < s.size()) runs.push_back({i, j});
    }
    return runs;
}

inline const combat::MontageData* montage_of_cast(Fixture& f, const combat::NpcAbilityData& tables, int64_t ability, uint64_t montage) {
    const abilities::AbilityDef* def = f.catalog->ability(ability);
    if (!def) return nullptr;
    for (const content::MontageRow& m : def->montages) {
        const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
        if (md && md->guid == montage) return md;
    }
    return nullptr;
}

inline std::vector<double> cast_bites(Fixture& f, const combat::NpcAbilityData& tables, int64_t ability, uint64_t montage) {
    std::vector<double> out;
    const abilities::AbilityDef* def = f.catalog->ability(ability);
    const combat::MontageData* md = montage_of_cast(f, tables, ability, montage);
    if (!def || !md) return out;
    for (const combat::MontageData::Hit& h : md->hits) {
        for (const content::AbilityHitRow& r : def->hits) {
            if (r.source != "hit" || r.key != h.key) continue;
            const abilities::HitDef* hit = f.catalog->hit(r.hit_guid);
            if (hit && (hit->row.applicable_targets & 2) && !hit->mods.empty()) out.push_back(std::max(0.1, h.time / md->rate_scale));
            break;
        }
    }
    return out;
}

inline void check_targets_named_in_full(TestHarness& h, const std::vector<LungeSample>& samples, const char* name) {
    constexpr uint64_t kPlayer = 77;
    const aoc::protocol::emit::FIntrepidNetworkGUID pawn = combat_rig::rig_pawn_guid(kPlayer), cylinder = combat_rig::rig_cylinder_guid(kPlayer);
    size_t element_ticks = 0, wrong_target = 0, wrong_instigator = 0, combat_ticks = 0, wrong_active = 0;
    for (const LungeSample& s : samples) {
        if (s.ability != 0) {
            ++element_ticks;
            wrong_target += s.item_target == cylinder ? 0 : 1;
            wrong_instigator += s.item_instigator == s.creature_guid && s.creature_guid.ServerId != 0 && s.creature_guid.Randomizer != 0 ? 0 : 1;
        }
        if (s.active_target.ObjectId != 0) {
            ++combat_ticks;
            wrong_active += s.active_target == pawn ? 0 : 1;
        }
    }
    CHECKF(element_ticks > 0 && wrong_target == 0,
           "the Targets of the %s's cast name the player's collision cylinder in full (ServerId %u, Randomizer %#x): %zu of %zu ticks wrong",
           name, cylinder.ServerId, cylinder.Randomizer, wrong_target, element_ticks);
    CHECKF(wrong_instigator == 0, "the Instigator of the %s's cast is its own NetGUID in full (%zu of %zu ticks wrong)", name, wrong_instigator, element_ticks);
    CHECKF(combat_ticks > 0 && wrong_active == 0, "the %s's CombatInfo.ActiveTargets names the player's pawn in full (%zu of %zu ticks wrong)", name, wrong_active, combat_ticks);
}

struct BanditOpen {
    std::optional<spawns::CapturedData> captured;
    std::optional<spawns::GenericNpcs>  generic;
    const spawns::OpenTemplate*         open = nullptr;
    spawns::Verdict                     why = spawns::Verdict::Generic;
};

inline bool make_bandit_open(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, BanditOpen& out, bool with_tree_abilities = true) {
    std::string err;
    out.captured = spawns::CapturedData::load(err);
    if (!out.captured) return false;
    spawns::GenericConfig config;
    if (with_tree_abilities) {
        config.abilities_of = [&tables](const std::string& archetype) {
            std::vector<int64_t> kit;
            if (const std::vector<combat::CreatureAbility>* tasks = tables.abilities_of(archetype)) {
                for (const combat::CreatureAbility& t : *tasks) {
                    if (std::find(kit.begin(), kit.end(), t.ability) == kit.end()) kit.push_back(t.ability);
                }
            }
            return kit;
        };
    }
    out.generic.emplace(*out.captured, &f.motion, config);
    content::NpcAssetRow asset;
    asset.archetype_path = path;
    out.open = out.generic->template_for(asset, &out.why);
    return out.open != nullptr;
}

void run_combat_cast_grem_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db);
void run_combat_gates_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db);
void run_combat_lunge_chase_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db);
void run_combat_projectile_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db);
void run_combat_generic_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db);

}  // namespace sandal::worldd::tests
