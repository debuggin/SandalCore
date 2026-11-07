// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "combat/combat_world.h"

#include "combat/combat_world_npc.h"
#include "combat/npc_ability_replicator.h"
#include "combat/npc_kits.h"
#include "combat/npc_replicator.h"
#include "spawns/generic_npc.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd::combat {

namespace {

nav::Vec3 to_nav(const Vec3& v) { return nav::Vec3{v.x, v.y, v.z}; }

constexpr double kBlueprintAggroDefaultCm = 1000.0;
constexpr double kBlueprintSightDefaultCm = 3000.0;
constexpr double kBlueprintSightConeDefaultDeg = 45.0;
constexpr double kBlueprintHearingDefaultCm = 2000.0;
constexpr double kBlueprintPlayerLookSeconds = 2.0;

bool runs_at_stat_speed(const std::string& archetype) {
    return archetype.find("/Population/NPC/Humanoid/") != std::string::npos || archetype.find("/Population/NPC/PCRaces/") != std::string::npos;
}

const npc_kits::Kit* kit_of(const std::string& leaf_in) {
    std::string leaf = leaf_in;
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    const npc_kits::Kit* best = nullptr;
    size_t best_len = 0;
    for (size_t i = 0; i < npc_kits::kKitCount; ++i) {
        const npc_kits::Kit& k = npc_kits::kKits[i];
        const std::string name = k.blueprint;
        if (name == leaf) return &k;
        const bool related = leaf.rfind(name + "_", 0) == 0 || name.rfind(leaf + "_", 0) == 0;
        if (related && name.size() > best_len) {
            best = &k;
            best_len = name.size();
        }
    }
    return best;
}

void configure_senses_params(const world::WorldActor& a, const npc::SensesTable* senses, const CombatWorldConfig& cfg, bool hostile, npc::ai::Params& p) {
    p.aggro_radius_cm = a.npc.aggro_radius_cm > 0 ? a.npc.aggro_radius_cm : cfg.default_aggro_cm;
    p.proximity_aggro = notices_players(hostile, a.npc);
    p.leash_radius_cm = a.npc.leash_radius_cm > 0 ? a.npc.leash_radius_cm : std::max(cfg.default_leash_cm, p.aggro_radius_cm * 3.0);
    const npc::SensesRow* row = senses ? senses->find(a.npc.archetype) : nullptr;
    if (!row) return;
    const double radius = a.npc.aggro_radius_cm > 0 ? a.npc.aggro_radius_cm : (row->aggro_radius >= 0 ? row->aggro_radius : kBlueprintAggroDefaultCm);
    p.aggro_radius_cm = radius * senses->curve_factor(row->aggro_curve, a.npc.level);
    p.proximity_aggro = p.proximity_aggro && p.aggro_radius_cm > 0 && row->detect_players != 0;
    npc::ai::Senses& e = p.senses;
    e.enabled = true;
    e.sight_radius_cm = row->sight == 1 ? (row->sight_radius >= 0 ? row->sight_radius : kBlueprintSightDefaultCm) : 0.0;
    e.sight_vertical_cm = std::max(0.0, row->sight_vertical);
    e.sight_cone_deg = row->sight_cone >= 0 ? row->sight_cone : kBlueprintSightConeDefaultDeg;
    e.hearing_radius_cm = row->hearing == 1 ? (row->hearing_radius >= 0 ? row->hearing_radius : kBlueprintHearingDefaultCm) : 0.0;
    e.hearing_vertical_cm = std::max(0.0, row->hearing_vertical);
    p.sense_interval = row->pc_interval >= 0 ? row->pc_interval : kBlueprintPlayerLookSeconds;
    p.anchor_at_fight_start = row->leash_at_fight_start;
    if (a.npc.leash_radius_cm <= 0 && row->leash_distance >= 0) p.leash_radius_cm = row->leash_distance;
}

void sort_and_cap_targets(std::vector<GatheredTarget>& out, size_t max) {
    std::sort(out.begin(), out.end(), [](const GatheredTarget& a, const GatheredTarget& b) {
        return a.distance_cm != b.distance_cm ? a.distance_cm < b.distance_cm : a.actor < b.actor;
    });
    if (out.size() > max) out.resize(max);
}

}  // namespace

CombatWorld::CombatWorld(world::ActorRegistry& registry, npc::NpcWorld* npcs, const nav::AreaNav* nav, const CombatData& data, const abilities::Catalog& catalog,
                         const NpcStatBook& book, const npc::MotionTable& motion, CombatWorldConfig config)
    : registry_(registry), npcs_world_(npcs), nav_(nav), data_(data), catalog_(catalog), book_(book), motion_(motion), cfg_(std::move(config)),
      resolver_(catalog, data, cfg_.seed ? cfg_.seed : 1) {
    rng_.state = cfg_.seed * 0x9E3779B97F4A7C15ull + 7;
}

CombatWorld::~CombatWorld() = default;

void CombatWorld::add_player(PlayerPort* port) {
    if (port && std::find(players_.begin(), players_.end(), port) == players_.end()) players_.push_back(port);
}

void CombatWorld::remove_player(PlayerPort* port) {
    players_.erase(std::remove(players_.begin(), players_.end(), port), players_.end());
    if (port) {
        player_combat_until_.erase(port->actor_id());
        drop_effects_of(port->actor_id());
    }
}

void CombatWorld::player_died(uint64_t actor_id) {
    player_combat_until_.erase(actor_id);
    drop_effects_of(actor_id);
}

PlayerPort* CombatWorld::player(uint64_t actor_id) {
    for (PlayerPort* p : players_) {
        if (p->actor_id() == actor_id) return p;
    }
    return nullptr;
}

void CombatWorld::player_exchange(uint64_t player_id, double now) {
    const double until = now + data_.in_combat_timer();
    const auto it = player_combat_until_.find(player_id);
    const bool was = it != player_combat_until_.end() && it->second > now;
    player_combat_until_[player_id] = until;
    if (!was) {
        if (PlayerPort* p = player(player_id)) p->set_in_combat(true, now);
    }
}

CombatWorld::Npc* CombatWorld::make_combatant(const world::WorldActor& a) {
    if (a.kind != world::ActorKind::Npc) return nullptr;
    const std::string leaf = spawns::blueprint_leaf(a.npc.archetype);
    const npc::MotionStats bp = motion_.get(a.npc.archetype);
    auto n = std::make_unique<Npc>(*this, a.id, bp, leaf, a.npc.level);
    std::string err;
    n->cs = book_.make(bp, a.npc.level, err);
    if (!n->cs) {
        spdlog::warn("[combat] {} ({}): its stats do not run: {}", a.id, leaf, err);
        return nullptr;
    }
    {
        std::set<int64_t> held;
        for (const int64_t stat : {data_.ids().status_burning, data_.ids().status_burning_dur}) {
            if (stat != 0) held.insert(stat);
        }
        n->cs->set_resources(std::move(held));
    }
    n->aligns.assign(bp.alignments.begin(), bp.alignments.end());
    n->hostile = data_.relation(n->aligns, CombatData::player_alignments()) == Relation::Hostile;
    n->kit = kit_of(leaf);
    npc::ai::Params p;
    configure_senses_params(a, senses_, cfg_, n->hostile, p);
    const double run = std::max({bp.max_run_speed, bp.max_walk_speed, 300.0});
    double chase = run;
    if (runs_at_stat_speed(a.npc.archetype)) {
        if (const stats::StatValue* mult = n->cs->find(data_.ids().move_speed_mult); mult && mult->value > 1.0) chase = run * mult->value;
    }
    p.chase_speed_cm_s = chase;
    p.run_home_speed_cm_s = run;
    if (npcs_world_ && runs_at_stat_speed(a.npc.archetype)) npcs_world_->set_yaw_rate(a.id, bp.rotation_rate_yaw * kHumanoidTurnGain);
    n->archetype = a.npc.archetype;
    n->anim = anim_ ? anim_->abilities_of(a.npc.archetype) : nullptr;
    if (n->anim && !can_animate(*n)) n->anim = nullptr;
    stand_off_of(*n, n->stand_off_cm, n->shot_reach_cm);
    if (n->anim) {
        p.attack_hysteresis = 1.0;
        p.chase_stop_fraction = 0.96;
    }
    n->ai.set_params(p);
    n->next = choose_attack(*n);
    write_stats(registry_, a.id, *n->cs);
    write_elite_rank(registry_, a.id, static_cast<int>(n->cs->npc().elite_rank));
    Npc* raw = n.get();
    npcs_[a.id] = std::move(n);
    return raw;
}

CombatWorld::Npc* CombatWorld::combatant(uint64_t actor_id) {
    if (const auto it = npcs_.find(actor_id); it != npcs_.end()) return it->second.get();
    const world::WorldActor* a = registry_.find(actor_id);
    if (!a || a->kind != world::ActorKind::Npc) return nullptr;
    return make_combatant(*a);
}

Fighter* CombatWorld::fighter_of(uint64_t actor_id) { return combatant(actor_id); }

uint64_t CombatWorld::actor_of_object(uint64_t object_id) const {
    if (registry_.find(object_id)) return object_id;
    if (const world::WorldActor* a = registry_.find_by_component(object_id)) return a->id;
    return 0;
}

uint64_t CombatWorld::hit_component_of(uint64_t actor_id) const {
    const world::WorldActor* a = registry_.find(actor_id);
    return a ? a->npc.collision_cylinder : 0;
}

TargetInfo CombatWorld::inspect(uint64_t target_actor, const Fighter& from) {
    TargetInfo t;
    const world::WorldActor* a = registry_.find(target_actor);
    if (!a || a->kind != world::ActorKind::Npc) return t;
    t.exists = true;
    t.position = {a->x, a->y, a->z};
    const npc::MotionStats bp = motion_.get(a->npc.archetype);
    t.radius_cm = bp.radius;
    const auto it = npcs_.find(target_actor);
    t.alive = it == npcs_.end() ? true : !it->second->dead;
    std::vector<int64_t> aligns(bp.alignments.begin(), bp.alignments.end());
    t.relation = data_.relation(from.alignments(), aligns);
    const Vec3 me = from.position();
    t.surface_distance_cm = std::max(0.0, distance_xy_cm(me, t.position) - from.radius_cm() - t.radius_cm);
    t.in_line_of_sight = true;
    if (nav_) {
        const nav::Vec3 p = to_nav(me), q = to_nav(t.position);
        if (nav_->on_navmesh(p, 150, 300) && nav_->on_navmesh(q, 150, 300)) t.in_line_of_sight = nav_->line_of_sight(p, q);
    }
    return t;
}

std::vector<GatheredTarget> CombatWorld::enemies_in_cone(const Fighter& from, double angle_degrees, double length_cm, double behind_cm, size_t max) {
    std::vector<GatheredTarget> out;
    if (max == 0 || length_cm <= 0) return out;
    const Vec3 me = from.position();
    BiteArea cone;
    cone.distance = length_cm;
    cone.angle = angle_degrees;
    cone.distance_center = -behind_cm;
    HitSubject subject;
    subject.cx = me.x;
    subject.cy = me.y;
    subject.yaw = from.facing_degrees();
    std::vector<uint64_t> ids;
    registry_.query(me.x, me.y, length_cm + std::max(0.0, behind_cm), ids);
    for (const uint64_t id : ids) {
        const TargetInfo ti = inspect(id, from);
        if (!ti.exists || !ti.alive || ti.relation != Relation::Hostile || !ti.in_line_of_sight) continue;
        subject.tx = ti.position.x;
        subject.ty = ti.position.y;
        subject.tradius = ti.radius_cm;
        if (!validity_holds(cone, subject)) continue;
        out.push_back({id, distance_xy_cm(me, ti.position)});
    }
    sort_and_cap_targets(out, max);
    return out;
}

std::vector<GatheredTarget> CombatWorld::enemies_in_line(const Fighter& from, double length_cm, double radius_cm, size_t max) {
    std::vector<GatheredTarget> out;
    if (max == 0 || length_cm <= 0) return out;
    const Vec3 me = from.position();
    const double yaw = from.facing_degrees() * bite_area_detail::kPi / 180.0;
    const double fx = std::cos(yaw), fy = std::sin(yaw);
    std::vector<uint64_t> ids;
    registry_.query(me.x, me.y, length_cm + radius_cm, ids);
    for (const uint64_t id : ids) {
        const TargetInfo ti = inspect(id, from);
        if (!ti.exists || !ti.alive || ti.relation != Relation::Hostile || !ti.in_line_of_sight) continue;
        const double dx = ti.position.x - me.x, dy = ti.position.y - me.y;
        const double ahead = dx * fx + dy * fy, aside = std::fabs(dy * fx - dx * fy);
        if (ahead + ti.radius_cm < 0 || ahead - ti.radius_cm > length_cm || aside > radius_cm + ti.radius_cm) continue;
        out.push_back({id, std::max(0.0, ahead)});
    }
    sort_and_cap_targets(out, max);
    return out;
}

std::vector<GatheredTarget> CombatWorld::enemies_around(const Fighter& from, const Vec3& centre, double radius_cm, size_t max) {
    std::vector<GatheredTarget> out;
    if (max == 0 || radius_cm <= 0) return out;
    constexpr double kBodyMargin = 600.0;
    std::vector<uint64_t> ids;
    registry_.query(centre.x, centre.y, radius_cm + kBodyMargin, ids);
    for (const uint64_t id : ids) {
        const TargetInfo ti = inspect(id, from);
        if (!ti.exists || !ti.alive || ti.relation != Relation::Hostile) continue;
        const double d = distance_xy_cm(centre, ti.position);
        if (d - ti.radius_cm > radius_cm) continue;
        if (std::fabs(ti.position.z - centre.z) - ti.radius_cm > radius_cm + 200.0) continue;
        if (!line_clear(centre, ti.position)) continue;
        out.push_back({id, d});
    }
    sort_and_cap_targets(out, max);
    return out;
}

std::vector<GatheredTarget> CombatWorld::enemies_touching(const Fighter& from, const Vec3& centre, double radius_cm, size_t max) {
    std::vector<GatheredTarget> out;
    if (max == 0 || radius_cm <= 0) return out;
    constexpr double kBodyMargin = 600.0;
    std::vector<uint64_t> ids;
    registry_.query(centre.x, centre.y, radius_cm + kBodyMargin, ids);
    for (const uint64_t id : ids) {
        const TargetInfo ti = inspect(id, from);
        if (!ti.exists || !ti.alive || ti.relation != Relation::Hostile) continue;
        const double d = distance_xy_cm(centre, ti.position);
        if (d - ti.radius_cm > radius_cm) continue;
        if (std::fabs(ti.position.z - centre.z) - ti.radius_cm > radius_cm + 200.0) continue;
        out.push_back({id, d});
    }
    sort_and_cap_targets(out, max);
    return out;
}

bool CombatWorld::ground_height(double x, double y, double z_hint, double* z) const {
    return nav_ != nullptr && nav_->ground_height(x, y, z_hint, z, 1500.0, 1500.0);
}

std::optional<npc::ai::State> CombatWorld::ai_state(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return std::nullopt;
    return it->second->ai.state();
}

const npc::ai::NpcAi* CombatWorld::ai_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? nullptr : &it->second->ai;
}

double CombatWorld::health_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? -1.0 : it->second->health();
}

double CombatWorld::max_health_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? -1.0 : it->second->max_health();
}

bool CombatWorld::dead(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it != npcs_.end() && it->second->dead;
}

void CombatWorld::summaries(std::vector<Summary>& out) const {
    out.reserve(out.size() + npcs_.size());
    for (const auto& entry : npcs_) {
        const Npc& n = *entry.second;
        Summary s;
        s.id = entry.first;
        const double max = n.max_health(), now = n.health();
        const double pct = max > 0 ? std::clamp(now / max * 100.0, 0.0, 100.0) : 100.0;
        s.health_pct = static_cast<uint8_t>(std::lround(pct));
        if (!n.dead && now > 0 && s.health_pct == 0) s.health_pct = 1;
        s.in_combat = n.flags_in_combat;
        s.dead = n.dead;
        out.push_back(s);
    }
}

std::string CombatWorld::next_attack_name(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end() || !it->second->next.ability) return {};
    return it->second->next.ability->row.name;
}

void CombatWorld::stop_cast(Npc& n) {
    if (npcs_world_) {
        npcs_world_->end_root_motion(n.actor_id);
        npcs_world_->stop_tracking(n.actor_id);
    }
    end_npc_cast(registry_, n.actor_id, n.cast);
}

void CombatWorld::drop_attack(Npc& n) {
    stop_cast(n);
    n.hold_until = 0;
    n.attack_walk_share = 1.0;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
}

void CombatWorld::forget(uint64_t actor_id) {
    if (is_pet(actor_id)) pet_cleanup(actor_id);
    if (const auto it = npcs_.find(actor_id); it != npcs_.end()) stop_cast(*it->second);
    flights_.erase(std::remove_if(flights_.begin(), flights_.end(), [&](const Flight& f) { return f.npc == actor_id; }), flights_.end());
    npcs_.erase(actor_id);
    stats_filled_.erase(actor_id);
    effect_holders_.erase(actor_id);
}

bool CombatWorld::fill_stats(const world::WorldActor& a) {
    if (!stats_filled_.insert(a.id).second) return false;
    if (!has_stats(a)) return false;
    const npc::MotionStats bp = motion_.get(a.npc.archetype);
    std::string err;
    const auto cs = book_.make(bp, a.npc.level, err);
    if (!cs) {
        spdlog::warn("[combat] {} ({}): an NPC whose stats do not run keeps its template's: {}", a.id, spawns::blueprint_leaf(a.npc.archetype), err);
        return true;
    }
    write_stats(registry_, a.id, *cs);
    return true;
}

void CombatWorld::add_shield(uint64_t npc_actor, double amount, double seconds, double now) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead || !std::isfinite(amount) || amount <= 0) return;
    if (n->shields.size() >= 16) n->shields.erase(n->shields.begin());
    n->shields.push_back({amount, seconds > 0 ? now + seconds : 0.0});
}

double CombatWorld::shield_of(uint64_t npc_actor, double now) const {
    const auto it = npcs_.find(npc_actor);
    if (it == npcs_.end()) return 0;
    double total = 0;
    for (const Npc::Shield& s : it->second->shields) {
        if (s.amount > 0 && (s.until <= 0 || s.until > now)) total += s.amount;
    }
    return total;
}

void CombatWorld::add_threat(uint64_t npc_actor, uint64_t attacker, double amount, double now) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead) return;
    n->ai.notice(attacker, amount, now);
    n->attackers.insert(attacker);
}

}  // namespace aoc::worldd::combat
