// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "combat/combat_world.h"

#include "combat/combat_world_npc.h"
#include "combat/npc_ability_replicator.h"
#include "combat/npc_kits.h"
#include "combat/npc_replicator.h"
#include "content/content_db.h"
#include "spawns/generic_npc.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd::combat {

namespace {

nav::Vec3 to_nav(const Vec3& v) { return nav::Vec3{v.x, v.y, v.z}; }

double angle_between_degrees(double a, double b) {
    double d = std::fmod(a - b, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d < -180.0) d += 360.0;
    return std::fabs(d);
}

void log_animated_cast_finished(const world::ActorRegistry& registry, uint64_t actor_id, const std::string& leaf, const std::string& ability_name,
                                PlayerPort* port) {
    if (!port) return;
    const world::WorldActor* w = registry.find(actor_id);
    if (!w) return;
    const Vec3 at = port->fighter().position();
    spdlog::info("[combat] {} ({}) {} is over: {:.0f} cm between its centre and the player's, the player {:.0f} deg off its facing", actor_id, leaf, ability_name,
                 std::hypot(at.x - w->x, at.y - w->y), angle_between_degrees(std::atan2(at.y - w->y, at.x - w->x) * 180.0 / 3.14159265358979323846, w->yaw_degrees));
}

bool swing_target_in_reach(const Fighter& npc, int next_range_cm, const Fighter& target) {
    const double reach = static_cast<double>(next_range_cm) * 1.5 + 300.0;
    const double d = distance_xy_cm(npc.position(), target.position()) - npc.radius_cm() - target.radius_cm();
    return d <= std::max(reach, 600.0);
}

void append_fallback_ability_hits(const abilities::AbilityDef& ability, const abilities::Catalog& catalog, std::vector<int64_t>& hits) {
    const auto hurts = [&](int64_t guid) {
        const abilities::HitDef* h = catalog.hit(guid);
        return h && (h->row.applicable_targets & 2) != 0 && !h->mods.empty();
    };
    if (std::any_of(hits.begin(), hits.end(), hurts)) return;
    for (const content::AbilityHitRow& r : ability.hits) {
        if (r.source == "hit" && hurts(r.hit_guid) && std::find(hits.begin(), hits.end(), r.hit_guid) == hits.end()) hits.push_back(r.hit_guid);
    }
}

int step_homing_flight(PlayerPort* port, double step, double radius, Vec3& at, double& travelled) {
    if (!port || !port->fighter().alive()) return -1;
    const Vec3 pos = port->fighter().position();
    const double dx = pos.x - at.x, dy = pos.y - at.y, dz = pos.z - at.z;
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist - (radius + port->fighter().radius_cm()) <= step) return 1;
    at.x += dx / dist * step;
    at.y += dy / dist * step;
    at.z += dz / dist * step;
    travelled += step;
    return 0;
}

bool segment_touches_player(const Vec3& from, const Vec3& dir, double length, double proj_radius, const Fighter& player) {
    const Vec3 p = player.position();
    const double along = std::clamp((p.x - from.x) * dir.x + (p.y - from.y) * dir.y + (p.z - from.z) * dir.z, 0.0, length);
    const Vec3 closest{from.x + dir.x * along, from.y + dir.y * along, from.z + dir.z * along};
    return distance_cm(closest, p) <= proj_radius + player.radius_cm();
}

void gather_nearby_players(const CombatWorld& world, const std::vector<PlayerPort*>& players, const Vec3& self, double max_dist,
                           std::vector<npc::ai::Candidate>& out) {
    for (PlayerPort* p : players) {
        const Fighter& f = p->fighter();
        if (distance_xy_cm(f.position(), self) > max_dist) continue;
        npc::ai::Candidate c;
        c.id = p->actor_id();
        c.pos = f.position();
        c.radius_cm = f.radius_cm();
        c.alive = f.alive();
        c.hostile = !world.is_pet(c.id);
        out.push_back(c);
    }
}

npc::ai::Sense build_ai_sense(const CombatWorld& world, const world::WorldActor& a, double radius_cm, double attack_range_cm, double max_dist,
                              const std::vector<PlayerPort*>& players) {
    npc::ai::Sense s;
    s.self = {a.x, a.y, a.z};
    s.self_radius_cm = radius_cm;
    s.home = {a.npc.home_x, a.npc.home_y, a.npc.home_z};
    s.self_yaw_degrees = a.yaw_degrees;
    s.attack_range_cm = attack_range_cm;
    gather_nearby_players(world, players, s.self, max_dist, s.known);
    return s;
}

void log_fight_began(const world::WorldActor& a, const npc::ai::Params& pr, uint64_t target, PlayerPort* who) {
    if (!pr.senses.enabled) return;
    const Vec3 self{a.x, a.y, a.z};
    spdlog::info("[combat] {} ({}, level {}) began a fight with player {}{}: it notices within {:.1f} m (hears {:.1f} m, sees {:.1f} m in front), leashes {:.0f} m from {}",
                 spawns::blueprint_leaf(a.npc.archetype), a.id, a.npc.level, target,
                 who ? fmt::format(", {:.1f} m away", distance_xy_cm(who->fighter().position(), self) / 100.0) : std::string(),
                 pr.aggro_radius_cm / 100.0, pr.senses.hearing_radius_cm / 100.0, pr.senses.sight_radius_cm / 100.0, pr.leash_radius_cm / 100.0,
                 pr.anchor_at_fight_start ? "where the fight began" : "home");
}

void log_fight_leashed(const world::WorldActor& a, const npc::ai::NpcAi& ai, const std::string& reason) {
    if (!ai.params().senses.enabled) return;
    const Vec3 self{a.x, a.y, a.z}, home{a.npc.home_x, a.npc.home_y, a.npc.home_z};
    spdlog::info("[combat] {} ({}, level {}) leashed: {}; {:.1f} m from {}, {:.1f} m from home", spawns::blueprint_leaf(a.npc.archetype), a.id, a.npc.level, reason,
                 distance_xy_cm(self, ai.leash_anchor(home)) / 100.0, ai.params().anchor_at_fight_start ? "where the fight began" : "home",
                 distance_xy_cm(self, home) / 100.0);
}

void expire_player_combat_timers(double now, const std::vector<PlayerPort*>& players, std::map<uint64_t, double>& combat_until) {
    for (auto it = combat_until.begin(); it != combat_until.end();) {
        if (now < it->second) {
            ++it;
            continue;
        }
        for (PlayerPort* p : players) {
            if (p->actor_id() == it->first) {
                p->set_in_combat(false, now);
                break;
            }
        }
        it = combat_until.erase(it);
    }
}

void collect_nearby_stats_candidates(world::ActorRegistry& registry, const Vec3& pos, double radius_cm, const std::set<uint64_t>& stats_filled,
                                     std::vector<std::pair<double, uint64_t>>& out) {
    std::vector<uint64_t> visible;
    registry.query(pos.x, pos.y, radius_cm, visible);
    for (const uint64_t id : visible) {
        if (stats_filled.count(id) != 0) continue;
        const world::WorldActor* a = registry.find(id);
        if (!a || a->kind != world::ActorKind::Npc) continue;
        out.emplace_back(std::hypot(a->x - pos.x, a->y - pos.y), id);
    }
    std::sort(out.begin(), out.end());
}

struct ModApplyResult {
    std::vector<double> effective;
    double              burn_before = 0;
    double              burn_dur_before = 0;
    bool                burn_moved = false;
};

ModApplyResult apply_resolved_mods_to_npc(world::ActorRegistry& registry, const CombatStatIds& id, CombatWorld::Npc& n, const Resolved& r,
                                          double now) {
    ModApplyResult out;
    out.effective.assign(r.mods.size(), 0.0);
    if (r.miss()) return out;
    if (const stats::StatValue* v = id.status_burning ? n.cs->find(id.status_burning) : nullptr) out.burn_before = v->value;
    if (const stats::StatValue* v = id.status_burning_dur ? n.cs->find(id.status_burning_dur) : nullptr) out.burn_dur_before = v->value;
    for (size_t i = 0; i < r.mods.size(); ++i) {
        const ResolvedMod& m = r.mods[i];
        if (!std::isfinite(m.value)) continue;
        if (id.is_status_stat(m.stat_guid)) {
            const stats::StatValue* cur = n.cs->find(m.stat_guid);
            if (!cur || !cur->held()) continue;
            const double before = cur->value;
            double next = before + m.value;
            if (cur->has_max) next = std::min(next, static_cast<double>(cur->max));
            next = std::max(next, cur->has_min ? static_cast<double>(cur->min) : 0.0);
            out.effective[i] = static_cast<float>(next) - before;
            n.cs->set_current(m.stat_guid, static_cast<float>(next));
            out.burn_moved = true;
            continue;
        }
        if (m.stat_guid != id.health && m.stat_guid != id.mana) continue;
        const stats::StatValue* cur = n.cs->find(m.stat_guid);
        const stats::StatValue* max = n.cs->find(m.stat_guid == id.health ? id.max_health : id.max_mana);
        if (!cur || !max) continue;
        const double before = cur->value;
        double value = m.value;
        if (m.stat_guid == id.health && value < 0) value = -n.absorb(-value, now);
        const double after = std::clamp(before + value, 0.0, static_cast<double>(max->value));
        out.effective[i] = after - before;
        n.cs->set_current(m.stat_guid, static_cast<float>(after));
        write_stat(registry, n.actor_id, m.stat_guid, static_cast<float>(after));
    }
    return out;
}

}  // namespace

void CombatWorld::deliver_hit(Npc& n, PlayerPort& port, const abilities::AbilityDef& ability, int64_t guid, double now, int64_t projectile) {
    const abilities::HitDef* hit = catalog_.hit(guid);
    if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) return;
    HitMeta meta;
    meta.source_type = projectile != 0 ? content::kSourceTypeProjectile : content::kSourceTypeAbility;
    meta.source_guid = projectile != 0 ? projectile : ability.row.guid;
    meta.ability_tags = ability.row.tags;
    Resolved r;
    std::string why;
    if (!resolver_.resolve(n, port.fighter(), *hit, meta, r, why)) {
        spdlog::debug("[combat] {} ({}): {}", n.actor_id, n.leaf, why);
        return;
    }
    double raw_health = 0, health = 0;
    for (ResolvedMod& m : r.mods) {
        if (m.stat_guid != data_.ids().health || !(m.value < 0) || !std::isfinite(m.value)) continue;
        raw_health += m.value;
        m.value *= cfg_.npc_damage_mult;
        health += m.value;
    }
    if (!r.miss() && npc_hits_logged_ < kLoggedNpcHits) {
        ++npc_hits_logged_;
        const stats::CharacterStats& target_stats = port.fighter().stats();
        const stats::StatValue* physical = target_stats.find(data_.profile().physical.mitigation);
        const stats::StatValue* magical = target_stats.find(data_.profile().magical.mitigation);
        spdlog::info("[combat] {} (level {}) hit the player (level {}): Health {:.2f}, {:.2f} before the damage setting; the player's PhysicalMitigation {:.4f}, MagicalMitigation {:.4f}{}", n.leaf,
                     n.lvl, port.fighter().level(), health, raw_health, physical ? physical->value : 0.0, magical ? magical->value : 0.0,
                     (r.flags & stats::kHitCrit) ? ", a critical hit" : "");
    }
    IncomingHit in;
    in.instigator = n.actor_id;
    in.hit_guid = guid;
    in.source_type = meta.source_type;
    in.source_guid = meta.source_guid;
    in.flags = r.flags;
    in.mods = r.mods;
    in.at = now;
    r.miss() ? ++stats_.npc_misses : ++stats_.npc_hits;
    port.receive_hit(in, now);
    player_exchange(port.actor_id(), now);
}

void CombatWorld::resolve_pending(double now) {
    std::vector<Pending> keep;
    for (size_t i = 0; i < pending_.size();) {
        if (pending_[i].due > now) {
            bool gone = false;
            if (pending_[i].animated) {
                const auto owner = npcs_.find(pending_[i].npc);
                gone = owner == npcs_.end() || owner->second->dead;
            }
            if (!gone) {
                ++i;
                continue;
            }
        }
        Pending p = pending_[i];
        const auto it = npcs_.find(p.npc);
        if (p.animated && (it == npcs_.end() || it->second->dead)) {
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        PlayerPort* port = player(p.target);
        if (!p.animated) {
            if (it == npcs_.end() || it->second->dead || !port || !port->fighter().alive()) continue;
            if (!swing_target_in_reach(*it->second, it->second->next.range_cm, port->fighter())) continue;
            for (const int64_t guid : p.hits) deliver_hit(*it->second, *port, *p.ability, guid, now);
            continue;
        }
        Npc& n = *it->second;
        while (p.next_hit < p.timed_hits.size() && p.timed_hits[p.next_hit].first <= now) {
            const size_t at_hit = p.next_hit++;
            const BiteArea& area = p.timed_areas[at_hit];
            const uint64_t aimed = area.has_target_part() ? aimed_at(n, area, p.target) : 0;
            const std::vector<PlayerPort*> candidates = players_;
            for (PlayerPort* q : candidates) {
                if (q->fighter().alive() && hit_reaches(n, q->fighter(), area, aimed != 0 && q->actor_id() == aimed)) deliver_hit(n, *q, *p.ability, p.timed_hits[at_hit].second, now);
            }
        }
        while (p.next_fire < p.timed_fires.size() && p.timed_fires[p.next_fire].first <= now) {
            launch(n, p.target, *p.ability, p.timed_fires[p.next_fire++].second, now);
        }
        if (p.next_hit >= p.timed_hits.size() && p.next_fire >= p.timed_fires.size() && now >= p.end_at) {
            if (p.logged) log_animated_cast_finished(registry_, n.actor_id, n.leaf, p.ability->row.name, port);
            stop_cast(n);
            continue;
        }
        p.due = p.end_at;
        if (p.next_hit < p.timed_hits.size()) p.due = std::min(p.due, p.timed_hits[p.next_hit].first);
        if (p.next_fire < p.timed_fires.size()) p.due = std::min(p.due, p.timed_fires[p.next_fire].first);
        keep.push_back(std::move(p));
    }
    for (Pending& p : keep) pending_.push_back(std::move(p));
}

void CombatWorld::launch(Npc& n, uint64_t target, const abilities::AbilityDef& ability, const PlannedFire& fire, double now) {
    const content::ProjectileRow* record = catalog_.projectile(fire.projectile);
    const world::WorldActor* w = registry_.find(n.actor_id);
    PlayerPort* port = player(target);
    if (!record || !w || !port || !port->fighter().alive() || !(record->speed > 0)) return;
    Flight f;
    f.npc = n.actor_id;
    f.target = target;
    f.ability = &ability;
    f.projectile = fire.projectile;
    f.hits = fire.hits;
    append_fallback_ability_hits(ability, catalog_, f.hits);
    f.speed = record->speed;
    f.radius = std::max(0.0, record->sphere_radius);
    f.range = record->range > 0 ? record->range : 1e9;
    f.life = std::max(0.0, record->life_time);
    f.homing = record->projectile_type.find("Homing") != std::string::npos;
    f.stops_at_aim = record->projectile_type.find("ToTarget") != std::string::npos;
    f.hits_left = std::max(1, record->max_hits);
    f.at = {w->x, w->y, w->z};
    const Vec3 aim = port->fighter().position();
    f.aim_distance = distance_cm(f.at, aim);
    if (f.aim_distance > 1e-6) {
        f.dir = {(aim.x - f.at.x) / f.aim_distance, (aim.y - f.at.y) / f.aim_distance, (aim.z - f.at.z) / f.aim_distance};
    } else {
        const double yaw = w->yaw_degrees * 3.14159265358979323846 / 180.0;
        f.dir = {std::cos(yaw), std::sin(yaw), 0.0};
    }
    f.launched = f.last = now;
    flights_.push_back(std::move(f));
}

void CombatWorld::advance_flights(double now) {
    struct Landing {
        Flight   flight;
        uint64_t player = 0;
    };
    std::vector<Landing> landings;
    for (size_t i = 0; i < flights_.size();) {
        Flight& f = flights_[i];
        const double dt = now - f.last;
        if (dt <= 0) {
            ++i;
            continue;
        }
        f.last = now;
        bool gone = npcs_.find(f.npc) == npcs_.end();
        const double step = f.speed * dt;
        if (!gone && f.homing) {
            const int status = step_homing_flight(player(f.target), step, f.radius, f.at, f.travelled);
            if (status != 0) gone = true;
            if (status > 0) landings.push_back({f, f.target});
        } else if (!gone) {
            const Vec3 from = f.at;
            const double length = f.stops_at_aim ? std::min(step, std::max(0.0, f.aim_distance + 200.0 - f.travelled)) : step;
            f.at = {from.x + f.dir.x * length, from.y + f.dir.y * length, from.z + f.dir.z * length};
            f.travelled += length;
            for (PlayerPort* q : players_) {
                if (!q->fighter().alive() || f.struck.count(q->actor_id()) || f.hits_left <= 0) continue;
                if (!segment_touches_player(from, f.dir, length, f.radius, q->fighter())) continue;
                landings.push_back({f, q->actor_id()});
                f.struck.insert(q->actor_id());
                --f.hits_left;
            }
            if (f.hits_left <= 0 || (f.stops_at_aim && f.travelled >= f.aim_distance + 200.0)) gone = true;
        }
        if (!gone && ((f.life > 0 && now - f.launched > f.life) || f.travelled > f.range)) gone = true;
        if (gone) flights_.erase(flights_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    for (const Landing& l : landings) {
        const auto it = npcs_.find(l.flight.npc);
        PlayerPort* port = player(l.player);
        if (it == npcs_.end() || !port || !port->fighter().alive()) continue;
        if (projectiles_logged_ < kLoggedProjectiles) {
            ++projectiles_logged_;
            spdlog::info("[combat] {} ({}) projectile of {} reached player {} after {:.2f} s and {:.1f} m", it->second->actor_id, it->second->leaf, l.flight.ability->row.name, l.player, now - l.flight.launched,
                         l.flight.travelled / 100.0);
        }
        for (const int64_t hit : l.flight.hits) deliver_hit(*it->second, *port, *l.flight.ability, hit, now, l.flight.projectile);
    }
}

Delivery CombatWorld::apply_hit_to_npc(uint64_t npc_actor, uint64_t attacker, const Resolved& r, double now, uint64_t threat_to,
                                       const HitOrigin* origin) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead) {
        Delivery d;
        d.target_object = hit_component_of(npc_actor);
        return d;
    }
    return land_hit(*n, attacker, r, now, true, threat_to, origin);
}

Delivery CombatWorld::land_hit(Npc& nn, uint64_t attacker, const Resolved& r, double now, bool log, uint64_t threat_to,
                               const HitOrigin* origin) {
    Npc* n = &nn;
    const uint64_t npc_actor = n->actor_id;
    Delivery d;
    d.target_object = hit_component_of(npc_actor);
    d.applied = true;
    d.flags = r.flags;
    n->attackers.insert(attacker);
    ++stats_.player_hits;
    const double health_before = n->health();
    const CombatStatIds& id = data_.ids();
    ModApplyResult mod_res = apply_resolved_mods_to_npc(registry_, id, *n, r, now);
    d.effective = std::move(mod_res.effective);
    const bool lethal = n->health() <= 0.0;
    const HolderEvent event = event_of_hit(*n, attacker, r, d, lethal, origin);
    if (is_pet(npc_actor)) {
        n->attackers.erase(attacker);
        if (lethal) {
            n->dead = true;
            d.lethal = true;
            d.flags |= stats::kHitLethal;
            pet_stop(npc_actor);
        }
        return d;
    }
    double threat = r.threat;
    if (threat <= 0 && !r.miss()) {
        for (size_t i = 0; i < d.effective.size(); ++i) {
            if (d.effective[i] < 0 && !id.is_status_stat(r.mods[i].stat_guid)) threat += -d.effective[i];
        }
    }
    if (log) {
        spdlog::info("[combat] player {} hit {} ({}, level {}): {}Health {:.1f} -> {:.1f}, threat {:.1f}", attacker, n->leaf, npc_actor, n->lvl,
                     r.miss() ? "a miss, " : "", health_before, n->health(), std::max(threat, 1.0));
    }
    if (mod_res.burn_moved && log) {
        const stats::StatValue* pool = id.status_burning ? n->cs->find(id.status_burning) : nullptr;
        const stats::StatValue* dur = id.status_burning_dur ? n->cs->find(id.status_burning_dur) : nullptr;
        if (pool && dur && (pool->value != mod_res.burn_before || dur->value != mod_res.burn_dur_before)) {
            spdlog::info("[combat] {} ({}) is burning: {:.1f} fire damage to burn over {:.1f} s (it was {:.1f} over {:.1f} s)", n->leaf, npc_actor,
                         pool->value, dur->value, mod_res.burn_before, mod_res.burn_dur_before);
        }
    }
    if (!lethal) {
        n->ai.notice(threat_to != 0 ? threat_to : attacker, std::max(threat, 1.0), now);
        if (threat_to != 0) n->ai.notice(attacker, 0.01, now);
    }
    player_exchange(attacker, now);
    if (lethal) {
        d.lethal = true;
        d.flags |= stats::kHitLethal;
        kill(*n, attacker, now);
    } else {
        think(*n, now);
    }
    hit_reacts(npc_actor, event, threat_to, origin, now);
    return d;
}

void CombatWorld::kill(Npc& n, uint64_t killer, double now, bool gm) {
    if (n.dead) return;
    n.dead = true;
    n.killer = killer;
    clear_effects(n);
    n.ai.kill();
    n.cs->set_in_combat(false);
    n.cs->set_current(content::kGuidHealth, 0.0f);
    write_stat(registry_, n.actor_id, content::kGuidHealth, 0.0f);
    write_combat(registry_, n.actor_id, false, true, 0);
    n.flags_in_combat = false;
    n.flags_target = 0;
    if (npcs_world_) npcs_world_->halt(n.actor_id);
    stop_cast(n);
    n.hold_until = 0;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
    ++stats_.kills;
    if (death_hook_ && !is_pet(n.actor_id)) {
        const world::WorldActor* a = registry_.find(n.actor_id);
        DeathEvent e;
        e.npc = n.actor_id;
        e.killer = killer;
        e.at = now;
        e.level = n.lvl;
        e.position = n.position();
        e.blueprint = n.leaf;
        e.info = a ? &a->npc : nullptr;
        e.motion = &n.bp;
        e.attackers = n.attackers;
        e.gm = gm;
        death_hook_(e);
    }
}

bool CombatWorld::gm_kill(uint64_t actor_id, double now, std::string& why) {
    Npc* n = combatant(actor_id);
    if (!n) {
        why = "world has no combat stats for this creature";
        return false;
    }
    if (n->dead) {
        why = "it is dead already";
        return false;
    }
    kill(*n, 0, now, true);
    return true;
}

bool CombatWorld::gm_heal(uint64_t actor_id) {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return true;
    Npc& n = *it->second;
    if (n.dead) return false;
    if (const stats::StatValue* max = n.cs->find(data_.ids().max_health)) {
        n.cs->set_current(content::kGuidHealth, max->value);
        write_stat(registry_, actor_id, content::kGuidHealth, max->value);
    }
    return true;
}

CombatWorld::GmReset CombatWorld::gm_reset(uint64_t actor_id, double now) {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return GmReset::Standing;
    Npc& n = *it->second;
    if (n.dead) return GmReset::Dead;
    n.ai.reset_now();
    begin_reset(n, now);
    const npc::Mover* m = npcs_world_ ? npcs_world_->mover(actor_id) : nullptr;
    if (m && m->moving()) return GmReset::Walking;
    n.ai.revive();
    n.returning = false;
    if (npcs_world_) npcs_world_->release(actor_id);
    return GmReset::Standing;
}

void CombatWorld::set_flags(Npc& n, bool in_combat, uint64_t target_actor) {
    const uint64_t object = target_actor;
    if (n.flags_in_combat == in_combat && n.flags_target == object) return;
    n.flags_in_combat = in_combat;
    n.flags_target = object;
    n.cs->set_in_combat(in_combat);
    const PlayerPort* who = object != 0 ? player(object) : nullptr;
    write_combat(registry_, n.actor_id, in_combat, false, who ? who->pawn_guid() : ::aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(object));
}

void CombatWorld::begin_reset(Npc& n, double now) {
    (void)now;
    ++stats_.resets;
    n.returning = true;
    n.attackers.clear();
    if (const stats::StatValue* max = n.cs->find(data_.ids().max_health)) {
        n.cs->set_current(content::kGuidHealth, max->value);
        write_stat(registry_, n.actor_id, content::kGuidHealth, max->value);
    }
    stop_cast(n);
    n.hold_until = 0;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
    set_flags(n, false, 0);
    if (npcs_world_) npcs_world_->order_return_home(n.actor_id);
}

void CombatWorld::pursue(Npc& n, uint64_t target, double speed_cm_s, double stop_distance_cm) {
    if (!npcs_world_) return;
    npcs_world_->order_pursue(
        n.actor_id,
        [this, target](nav::Vec3& out) {
            PlayerPort* p = player(target);
            if (!p || !p->fighter().alive()) return false;
            out = to_nav(p->fighter().position());
            return true;
        },
        speed_cm_s, stop_distance_cm);
}

bool CombatWorld::faces(const Npc& n, const Vec3& at, double tolerance_degrees) const {
    const world::WorldActor* w = registry_.find(n.actor_id);
    if (!w) return true;
    return angle_between_degrees(std::atan2(at.y - w->y, at.x - w->x) * 180.0 / 3.14159265358979323846, w->yaw_degrees) <= tolerance_degrees;
}

void CombatWorld::think(Npc& n, double now) {
    if (n.dead) return;
    const world::WorldActor* a = registry_.find(n.actor_id);
    if (!a) return;
    const double attack_range = n.shot_reach_cm > 0 ? n.shot_reach_cm : static_cast<double>(n.next.range_cm);
    const double max_dist = n.ai.params().leash_radius_cm * 2.0 + n.ai.params().aggro_radius_cm;
    npc::ai::Sense s = build_ai_sense(*this, *a, n.bp.radius, attack_range, max_dist, players_);
    s.now = now;
    if (nav_) s.line_of_sight = [this](const Vec3& from, const Vec3& to) { return line_clear(from, to); };
    const npc::ai::State before = n.ai.state();
    const npc::ai::Orders o = n.ai.think(s);
    if (o.began_fight) { ++stats_.aggros; n.fight_began = now; n.fight_rolls.clear(); log_fight_began(*a, n.ai.params(), o.target, player(o.target)); }
    if (o.began_reset) { log_fight_leashed(*a, n.ai, o.reset_reason); begin_reset(n, now); }
    if (o.reset_done) { n.returning = false; if (npcs_world_) npcs_world_->release(n.actor_id); }
    if (o.in_combat) set_flags(n, true, o.target);
    else if (n.ai.state() == npc::ai::State::Idle || n.ai.state() == npc::ai::State::Reset) set_flags(n, false, 0);
    const bool carried = npcs_world_ && npcs_world_->root_motion_active(n.actor_id);
    if (npcs_world_ && !o.began_reset && now >= n.hold_until && !carried) {
        if (o.move == npc::ai::Orders::Move::ChaseTo) pursue(n, o.target, o.speed_cm_s, o.stop_distance_cm);
        else if (o.move == npc::ai::Orders::Move::Stand && before != npc::ai::State::Attack) npcs_world_->order_move(n.actor_id, {a->x, a->y, a->z}, 0.0, 0.0);
    }
    if (!o.attack || now < n.next_attack_at) return;
    bool allowed = true, have_pick = false;
    Attack picked;
    PlayerPort* port = (n.anim && npcs_world_ && now >= n.hold_until) ? player(o.target) : nullptr;
    TreeWhy why;
    if (port) {
        picked = choose_animated(n, &port->fighter(), &why);
        have_pick = picked.ability != nullptr;
    }
    if (port && !have_pick && why.waits_on_timers) {
        picked = choose_attack(n, true);
        const double gap = distance_xy_cm(Vec3{a->x, a->y, a->z}, port->fighter().position()) - n.radius_cm() - port->fighter().radius_cm();
        if (!picked.ability) allowed = false;
        else if (gap > picked.range_cm) {
            allowed = false;
            pursue(n, o.target, n.ai.params().chase_speed_cm_s, std::max(0.0, picked.range_cm * n.ai.params().chase_stop_fraction) + n.radius_cm() + port->fighter().radius_cm());
        } else have_pick = true;
    } else if (port && !have_pick) {
        allowed = false;
        const Vec3 at = port->fighter().position();
        const double pad = n.radius_cm() + port->fighter().radius_cm();
        if (why.off_angle) npcs_world_->turn_toward(n.actor_id, at.x, at.y);
        else if (why.too_far) pursue(n, o.target, n.ai.params().chase_speed_cm_s, std::max(0.0, (why.reach_cm >= 0 ? why.reach_cm : static_cast<double>(n.next.range_cm)) * n.ai.params().chase_stop_fraction) + pad);
        else if (n.stand_off_cm > 0 && distance_xy_cm(s.self, at) - pad > n.stand_off_cm + 5.0) pursue(n, o.target, n.ai.params().chase_speed_cm_s, n.stand_off_cm + pad);
    } else if (port && have_pick && picked.task && picked.task->stop_movement && !picked.task->rotates && !faces(n, port->fighter().position(), 2.0)) {
        allowed = false;
        const Vec3 at = port->fighter().position();
        npcs_world_->turn_toward(n.actor_id, at.x, at.y);
    }
    if (allowed) start_attack(n, o.target, now, have_pick ? &picked : nullptr);
}

void CombatWorld::tick(double now) {
    now_ = now;
    if (!cfg_.enabled) return;
    log_crowd_control();
    resolve_pending(now);
    advance_flights(now);
    advance_effects(now);
    if (now < next_think_) return;
    next_think_ = now + cfg_.think_seconds;
    expire_player_combat_timers(now, players_, player_combat_until_);
    std::set<uint64_t> todo;
    for (PlayerPort* p : players_) {
        const Vec3 pos = p->fighter().position();
        std::vector<uint64_t> ids;
        registry_.query(pos.x, pos.y, cfg_.scan_radius_cm, ids);
        for (const uint64_t id : ids) {
            const world::WorldActor* a = registry_.find(id);
            if (!a || a->kind != world::ActorKind::Npc) continue;
            if (npcs_.find(id) != npcs_.end()) {
                todo.insert(id);
                continue;
            }
            if (huntable_ && huntable_(a->npc)) {
                fill_stats(*a);
                continue;
            }
            auto h = hostile_by_archetype_.find(a->npc.archetype);
            if (h == hostile_by_archetype_.end()) {
                const npc::MotionStats bp = motion_.get(a->npc.archetype);
                const std::vector<int64_t> al(bp.alignments.begin(), bp.alignments.end());
                h = hostile_by_archetype_.emplace(a->npc.archetype, data_.relation(al, CombatData::player_alignments()) == Relation::Hostile).first;
            }
            if (h->second) todo.insert(id);
        }
    }
    for (const auto& [id, n] : npcs_) {
        if (!n->dead && n->ai.state() != npc::ai::State::Idle) todo.insert(id);
    }
    if (now >= next_stats_) {
        next_stats_ = now + cfg_.stats_seconds;
        size_t budget = 64;
        for (PlayerPort* p : players_) {
            std::vector<std::pair<double, uint64_t>> todo_stats;
            collect_nearby_stats_candidates(registry_, p->fighter().position(), cfg_.stats_radius_cm, stats_filled_, todo_stats);
            for (const auto& [distance, id] : todo_stats) {
                if (budget == 0) break;
                if (npcs_.count(id) == 0 && registry_.find(id) && fill_stats(*registry_.find(id))) --budget;
            }
        }
    }
    for (const uint64_t id : todo) {
        if (is_pet(id)) continue;
        if (Npc* n = combatant(id)) think(*n, now);
    }
    tick_pets(now);
}

}  // namespace aoc::worldd::combat
