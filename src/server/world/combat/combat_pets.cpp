// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Summoner pets: contributed by MrJambix in #13.
#include "combat/combat_world.h"

#include "bootstrap/ability_replication.h"
#include "bootstrap/constants.h"
#include "combat/combat_world_npc.h"
#include "content/content_db.h"
#include "session/summoner_pets.h"

#include <spdlog/spdlog.h>

namespace aoc::worldd::combat {

namespace {

nav::Vec3 to_nav(const Vec3& v) { return nav::Vec3{v.x, v.y, v.z}; }

constexpr double kMeleeReachCm = 250.0, kRangedReachCm = 1500.0;
constexpr double kFirstHitSeconds = 0.4;
constexpr double kBoltFlightSeconds = 0.3;
constexpr double kOwnerLeftCm = 5000.0;
constexpr double kLungeCm = 600.0;

struct PetMontageShow {
    uint64_t pet_id = 0;
    int64_t  ability_guid = 0;
    uint64_t target = 0;
    Vec3     aim_pos{};
};

bool show_pet_montage_cast(world::ActorRegistry& registry, const PetMontageShow& req, const MontageData& montage, NpcCastState& cast_state) {
    const world::WorldActor* me = registry.find(req.pet_id);
    const auto play = bootstrap::montage_for(montage.path, montage.guid, montage.package_guid);
    if (!me || !npc_knows_ability(*me, req.ability_guid) || !play) return false;
    NpcCast shown;
    shown.ability = req.ability_guid;
    shown.target = req.target;
    shown.tx = req.aim_pos.x;
    shown.ty = req.aim_pos.y;
    shown.tz = req.aim_pos.z;
    shown.montage = montage.guid;
    start_npc_cast(registry, req.pet_id, cast_state, shown, play->chain);
    return true;
}

void collect_fallback_pet_cast_hits(const abilities::AbilityDef& def, const abilities::Catalog& catalog, double reach_cm, double now,
                                    std::vector<std::pair<double, int64_t>>& due) {
    const bool ranged = reach_cm >= 1000.0;
    std::set<int64_t> done;
    for (const content::AbilityHitRow& r : def.hits) {
        const abilities::HitDef* hit = catalog.hit(r.hit_guid);
        if (!hit || !done.insert(r.hit_guid).second) continue;
        const std::string& name = hit->row.name;
        if (ranged ? name.find("Melee") != std::string::npos : name.find("Ranged") != std::string::npos) continue;
        if (name.ends_with("_Redirect") || name.ends_with("_Remove")) continue;
        due.emplace_back(now + kFirstHitSeconds, r.hit_guid);
    }
}

void order_pet_lunge(npc::NpcWorld& npcs_world, const world::WorldActor& aim, uint64_t pet_id, const Vec3& from, double reach_cm) {
    const double dx = aim.x - from.x, dy = aim.y - from.y, length = std::max(1.0, std::hypot(dx, dy));
    const bool ranged = reach_cm >= 1000.0;
    const nav::Vec3 to = ranged ? nav::Vec3{from.x - dx / length * kLungeCm, from.y - dy / length * kLungeCm, from.z} : nav::Vec3{aim.x, aim.y, aim.z};
    npcs_world.order_move(pet_id, to, 2500.0, ranged ? 0.0 : 150.0);
}

std::vector<int64_t> pop_due_hits(std::vector<std::pair<double, int64_t>>& due, double now) {
    std::vector<int64_t> landing;
    for (auto d = due.begin(); d != due.end();) {
        if (d->first <= now) {
            landing.push_back(d->second);
            d = due.erase(d);
        } else {
            ++d;
        }
    }
    return landing;
}

bool order_pet_pursue(npc::NpcWorld* npcs_world, world::ActorRegistry& registry, uint64_t pet_id, uint64_t target_id, double reach_cm) {
    if (!npcs_world) return false;
    return npcs_world->order_pursue(
        pet_id,
        [&registry, target_id](nav::Vec3& out) {
            const world::WorldActor* a = registry.find(target_id);
            if (!a) return false;
            out = to_nav(Vec3{a->x, a->y, a->z});
            return true;
        },
        720.0, std::max(100.0, reach_cm * 0.8));
}

void regen_pet_health(world::ActorRegistry& registry, uint64_t id, stats::CharacterStats& cs, int64_t health_guid, double max_hp) {
    const stats::StatValue* regen = cs.find(content::kGuidHealthRegeneration);
    const stats::StatValue* cur = cs.find(health_guid);
    if (!regen || !(regen->value > 0) || !cur || !(cur->value < max_hp)) return;
    const float after = static_cast<float>(std::min(max_hp, static_cast<double>(cur->value) + static_cast<double>(regen->value)));
    cs.set_current(health_guid, after);
    write_stat(registry, id, health_guid, after);
}

bool resolve_pet_swing_hit(HitResolver& resolver, Fighter& owner, Fighter& target, const HitOrigin& origin, Resolved& r) {
    HitMeta meta{.source_type = origin.source_type, .source_guid = origin.source_guid, .ability_tags = origin.ability_tags};
    std::string why;
    if (resolver.resolve(owner, target, *origin.hit, meta, r, why)) return true;
    spdlog::debug("[pets] {}: {}", owner.id(), why);
    return false;
}

void fill_pet_dealt_hit(const CombatStatIds& ids, const Resolved& r, const Delivery& d, DealtHit& dealt) {
    for (size_t i = 0; i < r.mods.size(); ++i) {
        if (ids.is_status_stat(r.mods[i].stat_guid)) continue;
        dealt.mods.push_back(r.mods[i]);
        dealt.effective.push_back(d.effective[i]);
    }
}

}  // namespace

uint64_t CombatWorld::attacker_of(uint64_t player_actor, uint64_t pet, double radius_cm, bool owner_too) const {
    const auto port = std::find_if(players_.begin(), players_.end(), [&](const PlayerPort* p) { return p->actor_id() == player_actor; });
    if (port == players_.end()) return 0;
    const Vec3 at = (*port)->fighter().position();
    uint64_t best = 0;
    double nearest = radius_cm;
    for (const auto& [id, n] : npcs_) {
        if (n->dead || id == pet || !n->flags_in_combat || (!(owner_too && n->flags_target == player_actor) && n->flags_target != pet)) continue;
        const double d = distance_xy_cm(n->position(), at);
        if (d <= nearest) {
            nearest = d;
            best = id;
        }
    }
    return best;
}

uint64_t CombatWorld::owner_attacker(uint64_t player_actor, uint64_t pet, double radius_cm) const {
    const auto port = std::find_if(players_.begin(), players_.end(), [&](const PlayerPort* p) { return p->actor_id() == player_actor; });
    if (port == players_.end()) return 0;
    const Vec3 at = (*port)->fighter().position();
    uint64_t best = 0;
    double nearest = radius_cm;
    for (const auto& [id, n] : npcs_) {
        if (n->dead || id == pet || is_pet(id) || n->ai.target() != player_actor) continue;
        const double d = distance_xy_cm(n->position(), at);
        if (d <= nearest) {
            nearest = d;
            best = id;
        }
    }
    return best;
}

bool CombatWorld::pet_can_act(uint64_t pet) const {
    const auto it = npcs_.find(pet);
    return it != npcs_.end() && !it->second->dead;
}

uint64_t CombatWorld::take_pet_stand_down(uint64_t pet) {
    const auto it = pet_stand_downs_.find(pet);
    if (it == pet_stand_downs_.end()) return 0;
    const uint64_t target = it->second;
    pet_stand_downs_.erase(it);
    return target;
}

bool CombatWorld::fought_by(uint64_t npc_actor, uint64_t player_actor) const {
    const auto it = npcs_.find(npc_actor);
    return it != npcs_.end() && !it->second->dead && it->second->attackers.count(player_actor) != 0;
}

uint64_t CombatWorld::creature_target(uint64_t npc_actor) const {
    const auto found = npcs_.find(npc_actor);
    return found == npcs_.end() || found->second->dead ? 0 : found->second->ai.target();
}

bool CombatWorld::holds_dispellable(uint64_t npc_actor) const {
    const auto found = npcs_.find(npc_actor);
    if (found == npcs_.end()) return false;
    for (const effects::Instance& e : found->second->book.instances()) {
        if (e.def->row.dispellable && e.def->row.category.find("Debuff") != std::string::npos) return true;
    }
    return false;
}

uint64_t CombatWorld::pet_of(uint64_t owner) const { return pet_of_owner(owner); }

uint64_t CombatWorld::pet_of_owner(uint64_t owner) const {
    for (const auto& [pet, who] : pet_owners_) {
        if (who == owner) return pet;
    }
    return 0;
}

double CombatWorld::flight_seconds(const content::AbilityHitRow& row, const Vec3& from, const Vec3& to) const {
    const content::ProjectileRow* flying = catalog_.projectile(row.owner_guid);
    return flying && flying->speed > 0 ? distance_cm(from, to) / flying->speed : kBoltFlightSeconds;
}

void CombatWorld::pet_effects_changed(Npc& pet) {
    std::vector<stats::StatValue> changed;
    stats::CharacterStats::Next next;
    std::string err;
    if (pet.cs->prepare(pet.worn(), changed, next, err)) {
        pet.cs->commit(std::move(next));
        write_stats(registry_, pet.actor_id, *pet.cs);
    }
}

bool CombatWorld::pet_join(uint64_t pet, uint64_t owner) {
    PlayerPort* port = player(owner);
    Npc* n = port ? combatant(pet) : nullptr;
    if (!n) return false;
    n->aligns = port->fighter().alignments();
    pet_owners_[pet] = owner;
    if (pet_ports_.count(pet) == 0) {
        auto made = std::make_unique<PetPort>(*this, *n);
        add_player(made.get());
        pet_ports_[pet] = std::move(made);
    }
    return true;
}

bool CombatWorld::pet_use(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target, double reach_cm) {
    const abilities::AbilityDef* def = catalog_.ability(ability);
    if (!def || !pet_join(pet, owner)) return false;
    Npc* n = combatant(pet);
    if (!n || n->dead) return false;
    PetCast cast;
    cast.owner = owner;
    cast.target = target;
    cast.ability = def;
    cast.reach_cm = reach_cm;
    if (const auto fight = pet_fights_.find(pet); fight != pet_fights_.end()) {
        fight->second.swinging = false;
        fight->second.due.clear();
        fight->second.pursuing = false;
    }
    stop_cast(*n);
    pet_casts_[pet] = std::move(cast);
    return true;
}

bool CombatWorld::pet_can_use(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target, std::string* why) {
    const auto refuse = [&](const char* text) {
        if (why) *why = text;
        return false;
    };
    const abilities::AbilityDef* def = catalog_.ability(ability);
    PlayerPort* port = player(owner);
    if (!def || !port) return refuse("the ability or its owner is not there");
    if (!pet_can_act(pet)) return refuse("the pet cannot act");
    constexpr int kFriendly = 1 | 4 | 8 | 16;
    const int mask = def->row.applicable_targets;
    if (target == 0) return true;
    if (target == pet || target == owner) {
        if (mask != 0 && (mask & kFriendly) == 0) return refuse("it does not help allies");
        return true;
    }
    const world::WorldActor* t = registry_.find(target);
    if (!t || t->kind != world::ActorKind::Npc) return refuse("the target is not there");
    const auto fighting = npcs_.find(target);
    if (fighting != npcs_.end() && fighting->second->dead) return refuse("the target is dead");
    const npc::MotionStats bp = motion_.get(t->npc.archetype);
    const bool hostile = data_.relation(port->fighter().alignments(), std::vector<int64_t>(bp.alignments.begin(), bp.alignments.end())) == Relation::Hostile;
    if (hostile ? (mask & 2) == 0 : mask != 0 && (mask & kFriendly) == 0) return refuse(hostile ? "it does not harm enemies" : "it does not help allies");
    if (distance_xy_cm(port->fighter().position(), Vec3{t->x, t->y, t->z}) > kOwnerLeftCm) return refuse("the target is too far from its owner");
    return true;
}

void CombatWorld::pet_start_cast(Npc& pet, PetCast& cast, double now) {
    const abilities::AbilityDef& def = *cast.ability;
    PlayerPort* owner = player(cast.owner);
    const MontageData* montage = nullptr;
    if (anim_ && owner) {
        for (const content::MontageRow& m : def.montages) {
            if (m.soft_path.empty() || !resolver_.holds(owner->fighter(), m.select_expr, true)) continue;
            montage = anim_->montage(m.soft_path);
            if (montage && montage->length > 0 && montage->rate_scale > 0) break;
            montage = nullptr;
        }
    }
    double seconds = 1.0;
    bool timed = false, played = false;
    const world::WorldActor* aim = cast.target ? registry_.find(cast.target) : nullptr;
    if (montage) {
        seconds = std::max(seconds, montage->seconds(1.0));
        const Vec3 at = aim ? Vec3{aim->x, aim->y, aim->z} : pet.position();
        played = show_pet_montage_cast(registry_, PetMontageShow{pet.actor_id, def.row.guid, cast.target, at}, *montage, pet.cast);
        const auto add = [&](double delay, int key, const char* source) {
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source != source || r.key != key) continue;
                cast.due.emplace_back(now + delay, r.hit_guid);
                timed = true;
            }
        };
        for (const MontageData::Hit& h : montage->hits) add(h.time / montage->rate_scale, static_cast<int>(h.key), "hit");
        for (const MontageData::Fire& f : montage->fires) {
            double flying = kBoltFlightSeconds;
            if (aim) {
                for (const content::AbilityHitRow& r : def.hits) {
                    if (r.source == "projectile" && r.key == f.key) flying = flight_seconds(r, pet.position(), Vec3{aim->x, aim->y, aim->z});
                }
            }
            add(f.time / montage->rate_scale + flying, static_cast<int>(f.key), "projectile");
        }
    }
    if (!timed) collect_fallback_pet_cast_hits(def, catalog_, cast.reach_cm, now, cast.due);
    cast.end_at = now + seconds;
    cast.started = true;
    ++stats_.npc_attacks;
    spdlog::info("[pets] the pet (actor {}) starts {} at actor {}: {:.2f} s, {} hit(s) due, montage {}", pet.actor_id, def.row.name, cast.target, seconds, cast.due.size(),
                 played ? "shown" : montage ? "known to no list: not shown" : "none in the tables");
    if (def.row.guid == pets::kHunterLunge && aim && npcs_world_) order_pet_lunge(*npcs_world_, *aim, pet.actor_id, pet.position(), cast.reach_cm);
}

void CombatWorld::pet_land_on_self(Npc& pet, uint64_t owner, const abilities::HitDef& hit, double now) {
    PlayerPort* port = player(owner);
    if (!port || pet.dead) return;
    if (hit.max_remove > 0 || !hit.remove_effects.empty()) remove_effects_from_npc(pet.actor_id, hit, now);
    Resolved r;
    std::string why;
    HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    if (!hit.mods.empty() && resolver_.resolve(port->fighter(), pet, hit, meta, r, why)) {
        for (const ResolvedMod& m : r.mods) {
            if (m.stat_guid != data_.ids().health || !std::isfinite(m.value)) continue;
            const double after = std::clamp(pet.health() + m.value, 0.0, pet.max_health());
            pet.cs->set_current(m.stat_guid, static_cast<float>(after));
            write_stat(registry_, pet.actor_id, m.stat_guid, static_cast<float>(after));
        }
    }
    land_effects_of_hit(pet, *port, hit, meta, now);
}

void CombatWorld::pet_land(Npc& pet, const PetCast& cast, const abilities::HitDef& hit, double now, int depth) {
    if (depth > 3 || hit.row.name.ends_with("_OwnerCost")) return;
    const int mask = hit.row.applicable_targets;
    PlayerPort* owner = player(cast.owner);
    if (!owner) return;
    if ((mask & 2) != 0) {
        const pets::HitArea area = pets::area_of_hit(hit.row.guid);
        const world::WorldActor* aim = cast.target ? registry_.find(cast.target) : nullptr;
        LandSource src{.type = content::kSourceTypeAbility, .guid = cast.ability->row.guid, .ability = cast.ability, .pet = pet.actor_id, .children = false, .skip_requirement = area.others_only};
        if (area.radius_cm > 0) {
            const Vec3 centre = area.around_pet || !aim ? pet.position() : Vec3{aim->x, aim->y, aim->z};
            for (const GatheredTarget& g : enemies_around(owner->fighter(), centre, area.radius_cm, 40)) {
                if (area.others_only && g.actor == cast.target) continue;
                if (!is_pet(g.actor)) land_for_player(cast.owner, g.actor, hit, src, now);
            }
        } else if (aim && !is_pet(cast.target) && cast.target != cast.owner) {
            land_for_player(cast.owner, cast.target, hit, src, now);
        }
    }
    if ((mask & 5) == 5) {
        if (cast.target == pet.actor_id) pet_land_on_self(pet, cast.owner, hit, now);
        else owner->hit_from_pet(hit.row.guid, cast.ability->row.guid, now);
    } else if ((mask & 1) != 0) {
        pet_land_on_self(pet, cast.owner, hit, now);
    } else if ((mask & (4 | 8)) != 0) {
        owner->hit_from_pet(hit.row.guid, cast.ability->row.guid, now);
    }
    if (cast.ability->row.guid == pets::kGuardianTaunt && cast.target != 0) {
        const auto taunted = npcs_.find(cast.target);
        if (taunted != npcs_.end() && !taunted->second->dead) {
            double top = 0;
            for (const npc::ai::ThreatTable::Entry& e : taunted->second->ai.threat().entries()) top = std::max(top, e.threat);
            taunted->second->ai.notice(pet.actor_id, top * npc::ai::ThreatTable::kSwitchRatio + taunted->second->max_health(), now);
        }
    }
    if (hit.row.guid == pets::kHunterMeleeIncapHit && cast.target != 0) pet_stand_downs_[pet.actor_id] = cast.target;
    for (const content::RecursiveHitRow& r : hit.recursive) {
        if (const abilities::HitDef* child = catalog_.hit(r.child_hit)) pet_land(pet, cast, *child, now, depth + 1);
    }
}

void CombatWorld::tick_pet_casts(double now) {
    for (auto it = pet_casts_.begin(); it != pet_casts_.end();) {
        const uint64_t id = it->first;
        PetCast& cast = it->second;
        Npc* pet = combatant(id);
        PlayerPort* owner = player(cast.owner);
        bool over = !pet || pet->dead || !owner || !owner->fighter().alive();
        const bool at_enemy = cast.target != 0 && cast.target != id && cast.target != cast.owner && (cast.ability->row.applicable_targets & 2) != 0;
        const auto aimed = at_enemy ? npcs_.find(cast.target) : npcs_.end();
        if (!over && at_enemy && (aimed == npcs_.end() || aimed->second->dead)) over = true;
        if (over) {
            spdlog::info("[pets] the pet (actor {}): {} ends {} its hits: {}", id, cast.ability->row.name, cast.started ? "before all of" : "before any of",
                         !pet || pet->dead ? "the pet is gone" : !owner || !owner->fighter().alive() ? "its owner is gone" : "its target is gone");
            if (pet) stop_cast(*pet);
            it = pet_casts_.erase(it);
            continue;
        }
        if (!cast.started) {
            const double distance = at_enemy ? distance_xy_cm(pet->position(), aimed->second->position()) - pet->radius_cm() - aimed->second->radius_cm() : 0.0;
            if (distance > cast.reach_cm) {
                if (!cast.pursuing) cast.pursuing = order_pet_pursue(npcs_world_, registry_, id, cast.target, cast.reach_cm);
                ++it;
                continue;
            }
            if (npcs_world_) npcs_world_->halt(id);
            pet_start_cast(*pet, cast, now);
        }
        const std::vector<int64_t> landing = pop_due_hits(cast.due, now);
        const PetCast held = cast;
        for (const int64_t guid : landing) {
            if (const abilities::HitDef* hit = catalog_.hit(guid)) {
                spdlog::info("[pets] the pet (actor {}): the hit {} of {} lands", id, hit->row.name, held.ability->row.name);
                pet_land(*pet, held, *hit, now);
            }
        }
        const bool channel = cast.ability->row.guid == pets::kHunterIncapacitate && cast.reach_cm >= 1000.0 && cast.target != 0 && effect_stacks(cast.target, pets::kHunterIncapRanged) > 0;
        if (cast.due.empty() && now >= cast.end_at && !channel) {
            stop_cast(*pet);
            it = pet_casts_.erase(it);
            continue;
        }
        ++it;
    }
}

bool CombatWorld::pet_attack(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target) {
    if (pet == target || !player(owner)) return false;
    const abilities::AbilityDef* def = catalog_.ability(ability);
    if (!def) return false;
    const world::WorldActor* t = registry_.find(target);
    if (!t || t->kind != world::ActorKind::Npc) return false;
    Fighter& side = player(owner)->fighter();
    const npc::MotionStats bp = motion_.get(t->npc.archetype);
    if (data_.relation(side.alignments(), std::vector<int64_t>(bp.alignments.begin(), bp.alignments.end())) != Relation::Hostile) return false;
    Npc* other = combatant(target);
    if (!other || other->dead || !pet_join(pet, owner)) return false;
    PetFight fight;
    fight.owner = owner;
    fight.target = target;
    fight.ability = def;
    fight.reach_cm = launches_projectiles(*def) ? kRangedReachCm : kMeleeReachCm;
    pet_fights_[pet] = std::move(fight);
    return true;
}

void CombatWorld::pet_stop(uint64_t pet) {
    if (pet_fights_.erase(pet) == 0) return;
    const auto it = npcs_.find(pet);
    if (it != npcs_.end()) stop_cast(*it->second);
    if (npcs_world_) npcs_world_->halt(pet);
}

void CombatWorld::pet_cleanup(uint64_t pet) {
    pet_stop(pet);
    pet_stand_downs_.erase(pet);
    pet_casts_.erase(pet);
    pet_owners_.erase(pet);
    if (const auto it = pet_ports_.find(pet); it != pet_ports_.end()) {
        remove_player(it->second.get());
        pet_ports_.erase(it);
    }
}

void CombatWorld::pet_gone(uint64_t pet) {
    pet_cleanup(pet);
    forget(pet);
}

void CombatWorld::pet_swing(Npc& pet, PetFight& fight, const Npc& target, double now) {
    const abilities::AbilityDef& def = *fight.ability;
    double seconds = 1.5;
    bool timed = false;
    const MontageData* montage = nullptr;
    if (anim_) {
        for (const content::MontageRow& m : def.montages) {
            if (m.soft_path.empty()) continue;
            montage = anim_->montage(m.soft_path);
            if (montage && montage->length > 0 && montage->rate_scale > 0) break;
            montage = nullptr;
        }
    }
    if (montage) {
        seconds = std::max(seconds, montage->seconds(1.0));
        show_pet_montage_cast(registry_, PetMontageShow{pet.actor_id, def.row.guid, target.actor_id, target.position()}, *montage, pet.cast);
        const auto add = [&](double delay, int key, const char* source) {
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source != source || r.key != key) continue;
                const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
                if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
                fight.due.emplace_back(now + delay, r.hit_guid);
                timed = true;
            }
        };
        for (const MontageData::Hit& h : montage->hits) add(h.time / montage->rate_scale, static_cast<int>(h.key), "hit");
        for (const MontageData::Fire& f : montage->fires) {
            double flying = kBoltFlightSeconds;
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source == "projectile" && r.key == f.key) flying = flight_seconds(r, pet.position(), target.position());
            }
            add(f.time / montage->rate_scale + flying, static_cast<int>(f.key), "projectile");
        }
    }
    if (!timed) {
        for (const content::AbilityHitRow& r : def.hits) {
            const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
            if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
            fight.due.emplace_back(now + kFirstHitSeconds, r.hit_guid);
            break;
        }
    }
    fight.end_at = now + seconds;
    fight.next_at = fight.end_at;
    fight.swinging = true;
    ++stats_.npc_attacks;
}

void CombatWorld::tick_pets(double now) {
    tick_pet_casts(now);
    if (now >= pet_regen_at_) {
        pet_regen_at_ = now + 1.0;
        for (const auto& [id, owner] : pet_owners_) {
            if (Npc* pet = combatant(id); pet && !pet->dead && pet->cs) {
                regen_pet_health(registry_, id, *pet->cs, data_.ids().health, pet->max_health());
            }
        }
    }
    for (auto it = pet_fights_.begin(); it != pet_fights_.end();) {
        const uint64_t id = it->first;
        PetFight& fight = it->second;
        Npc* pet = combatant(id);
        if (pet_casts_.count(id) != 0) {
            ++it;
            continue;
        }
        PlayerPort* owner = player(fight.owner);
        const auto found = npcs_.find(fight.target);
        const bool over = !pet || pet->dead || !owner || !owner->fighter().alive() || found == npcs_.end() || found->second->dead;
        if (over || distance_xy_cm(owner->fighter().position(), found->second->position()) > kOwnerLeftCm) {
            if (pet) stop_cast(*pet);
            if (npcs_world_) npcs_world_->halt(id);
            it = pet_fights_.erase(it);
            continue;
        }
        Npc& target = *found->second;
        const uint64_t owner_id = fight.owner, target_id = fight.target;
        const abilities::AbilityDef* ability = fight.ability;
        for (const int64_t guid : pop_due_hits(fight.due, now)) {
            const abilities::HitDef* hit = catalog_.hit(guid);
            const auto again = npcs_.find(target_id);
            if (!hit || again == npcs_.end() || again->second->dead) continue;
            HitOrigin origin{.hit = hit, .source_type = content::kSourceTypeAbility, .source_guid = ability->row.guid, .ability_tags = ability->row.tags};
            Resolved r;
            if (!resolve_pet_swing_hit(resolver_, owner->fighter(), *again->second, origin, r)) continue;
            const Delivery d = apply_hit_to_npc(target_id, owner_id, r, now, id, &origin);
            if (d.applied) {
                DealtHit dealt{guid, origin.source_type, origin.source_guid, id, target_id, d.flags, {}, {}};
                fill_pet_dealt_hit(data_.ids(), r, d, dealt);
                if (!dealt.mods.empty() || hit->row.has_fx || r.miss()) owner->hit_dealt(dealt, now);
            }
        }
        if (fight.swinging && now >= fight.end_at) {
            fight.swinging = false;
            stop_cast(*pet);
        }
        if (!fight.swinging) {
            const double distance = distance_xy_cm(pet->position(), target.position()) - pet->radius_cm() - target.radius_cm();
            if (distance > fight.reach_cm) {
                if (!fight.pursuing) fight.pursuing = order_pet_pursue(npcs_world_, registry_, id, target_id, fight.reach_cm);
            } else if (now >= fight.next_at) {
                if (npcs_world_) npcs_world_->halt(id);
                fight.pursuing = false;
                pet_swing(*pet, fight, target, now);
            }
        }
        ++it;
    }
}

}  // namespace aoc::worldd::combat
