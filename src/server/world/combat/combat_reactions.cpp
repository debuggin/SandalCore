// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "combat/combat_world.h"

#include "combat/combat_world_npc.h"
#include "content/content_db.h"
#include "session/summoner_pets.h"

#include <spdlog/spdlog.h>

namespace aoc::worldd::combat {

namespace {

const char* const kRemovePhases[] = {"Remove", "End"};

class HeldView final : public TargetEffects {
public:
    HeldView(const CombatWorld& world, uint64_t holder, double at) : world_(world), holder_(holder), at_(at) {}
    double remaining(int64_t effect_guid) const override { return world_.effect_remaining(holder_, effect_guid, at_); }

private:
    const CombatWorld& world_;
    uint64_t           holder_;
    double             at_;
};

void scale_pet_hit_mods(const effects::Book& book, int64_t health_stat_guid, std::vector<ResolvedMod>& mods) {
    double damage = 1.0, healing = 1.0;
    for (const effects::Instance& e : book.instances()) {
        for (const stats::EffectMod& m : e.mods) {
            if (m.stat == pets::kStatDamageMult) damage *= m.value;
            if (m.stat == pets::kStatHealingMult) healing *= m.value;
        }
    }
    for (ResolvedMod& m : mods) {
        if (m.stat_guid == health_stat_guid) m.value *= m.value < 0 ? damage : healing;
    }
}

void fill_dealt_hit(const CombatStatIds& ids, const Resolved& r, const Delivery& d, DealtHit& dealt) {
    for (size_t i = 0; i < r.mods.size(); ++i) {
        if (ids.is_status_stat(r.mods[i].stat_guid)) continue;
        dealt.mods.push_back(r.mods[i]);
        dealt.effective.push_back(d.effective[i]);
    }
}

HitContext make_reaction_hit_ctx(const stats::DamageProfile& profile, const HitOrigin* origin, uint8_t flags) {
    HitContext ctx;
    ctx.profile = &profile;
    if (origin) {
        if (origin->hit) ctx.tags = origin->hit->tags;
        ctx.tags.insert(ctx.tags.end(), origin->ability_tags.begin(), origin->ability_tags.end());
        ctx.source_type = origin->source_type;
    }
    ctx.hit.flags = flags;
    return ctx;
}

}  // namespace

void CombatWorld::remove_effects_from_npc(uint64_t npc_actor, const abilities::HitDef& hit, double now) {
    const auto found = npcs_.find(npc_actor);
    if (found == npcs_.end() || found->second->book.empty()) return;
    Npc& n = *found->second;
    int left = hit.max_remove > 0 ? hit.max_remove : 1 << 20;
    const std::vector<effects::Instance> held = n.book.instances();
    for (const effects::Instance& e : held) {
        if (left <= 0 || n.dead) break;
        if (!hit.removes(*e.def)) continue;
        effects::Book::Changes changes;
        if (!n.book.remove(e.key, changes)) continue;
        send_effects(n, changes);
        run_effect_hits(n, e, kRemovePhases, 2, now);
        --left;
    }
}

void CombatWorld::remove_npc_effect(uint64_t npc_actor, int64_t effect_guid, double now) {
    const auto found = npcs_.find(npc_actor);
    if (found == npcs_.end()) return;
    Npc& n = *found->second;
    const std::vector<effects::Instance> held = n.book.instances();
    for (const effects::Instance& e : held) {
        if (e.def->row.guid != effect_guid || n.dead) continue;
        effects::Book::Changes changes;
        if (!n.book.remove(e.key, changes)) continue;
        send_effects(n, changes);
        run_effect_hits(n, e, kRemovePhases, 2, now);
    }
}

void CombatWorld::land_effects_of_hit(Npc& n, PlayerPort& port, const abilities::HitDef& hit, const HitMeta& meta, double now) {
    Resolved gate;
    std::string why;
    // Effect expressions read target effect counts via the stat engine updated in send_effects.
    const bool ran = resolver_.resolve_effects(port.fighter(), n, hit, meta, gate, why);
    if (!ran) {
        spdlog::debug("[combat] the effects of {} on {}: {}", hit.row.name, n.actor_id, why);
        return;
    }
    for (const effects::Applied& a : gate.effects) {
        if (n.dead) return;
        const double shield = resolver_.effect_shield(port.fighter(), n, *a.def);
        if (shield > 0) add_shield(n.actor_id, shield, std::isinf(a.duration) ? 0.0 : a.duration, now);
        land_effect(n, a, port.actor_id(), now);
    }
}

void CombatWorld::land_for_player(uint64_t by, uint64_t target, const abilities::HitDef& hit, const LandSource& src, double now, int depth) {
    PlayerPort* port = player(by);
    const auto found = npcs_.find(target);
    if (!port || depth > 4 || found == npcs_.end() || found->second->dead) return;
    Npc& n = *found->second;
    HitMeta meta{.source_type = src.type, .source_guid = src.guid};
    if (src.ability) meta.ability_tags = src.ability->row.tags;
    const HeldView held_view(*this, target, now);
    meta.target_effects = &held_view;
    if (!src.skip_requirement && !resolver_.requirement_holds(port->fighter(), n, hit, meta)) return;
    Resolved r;
    std::string why;
    bool ok = false;
    if (src.event) {
        std::string err;
        auto a = port->fighter().stats().evaluation(port->fighter().worn(), err, true);
        auto t = n.stats().evaluation(n.worn(), err, true);
        if (a && t) {
            EventHost host(rng_, *src.event, by);
            const Relation rel = data_.relation(port->fighter().alignments(), n.alignments());
            ok = resolver_.resolve_with(*a, host, t.get(), distance_cm(port->fighter().position(), n.position()), rel == Relation::Hostile, rel == Relation::Neutral, hit, meta, r, why);
        }
    } else {
        ok = resolver_.resolve(port->fighter(), n, hit, meta, r, why);
    }
    if (!ok) {
        spdlog::debug("[combat] the hit {} does not run on {}: {}", hit.row.name, target, why);
        return;
    }
    if (src.pet != 0) {
        if (const auto pet = npcs_.find(src.pet); pet != npcs_.end()) scale_pet_hit_mods(pet->second->book, data_.ids().health, r.mods);
    }
    HitOrigin origin{.hit = &hit, .source_type = src.type, .source_guid = src.guid, .ability_tags = meta.ability_tags, .landed_here = true};
    const Delivery d = land_hit(n, by, r, now, true, target == src.pet ? 0 : src.pet, &origin);
    if (d.applied) {
        DealtHit dealt{hit.row.guid, meta.source_type, meta.source_guid, src.pet, target, d.flags, {}, {}};
        fill_dealt_hit(data_.ids(), r, d, dealt);
        if (!dealt.mods.empty() || hit.row.has_fx || r.miss()) port->hit_dealt(dealt, now);
    }
    if (d.applied && !r.miss() && !d.lethal) {
        if (hit.max_remove > 0 || !hit.remove_effects.empty()) remove_effects_from_npc(target, hit, now);
        land_effects_of_hit(n, *port, hit, meta, now);
    }
    if (src.children) land_children(by, target, hit, src, now, depth);
}

void CombatWorld::land_children(uint64_t by, uint64_t target, const abilities::HitDef& hit, const LandSource& src, double now, int depth) {
    PlayerPort* port = player(by);
    if (!port) return;
    const world::WorldActor* at = registry_.find(target);
    if (!at) return;
    const Vec3 centre{at->x, at->y, at->z};
    for (const content::RecursiveHitRow& row : hit.recursive) {
        const abilities::HitDef* child = catalog_.hit(row.child_hit);
        if (!child) continue;
        const int mask = child->row.applicable_targets;
        if ((mask & 2) != 0) {
            if (row.radius_cm > 0) {
                for (const GatheredTarget& g : enemies_around(port->fighter(), centre, row.radius_cm, 40)) {
                    if (!is_pet(g.actor)) land_for_player(by, g.actor, *child, src, now, depth + 1);
                }
            } else if (!is_pet(target) && target != by) {
                land_for_player(by, target, *child, src, now, depth + 1);
            }
        } else if ((mask & 4) != 0) {
            if (const uint64_t pet = pet_of_owner(by)) land_for_player(by, pet, *child, src, now, depth + 1);
        } else if ((mask & 1) != 0) {
            port->hit_from_pet(child->row.guid, src.guid, now);
        }
    }
}

void CombatWorld::effect_hit_fires(Npc& n, uint64_t by, const abilities::HitDef& hit, int64_t effect_guid, double now) {
    if (hit.recursive.empty() || hit.row.name.rfind("Summoner_", 0) != 0) return;
    LandSource src;
    src.type = content::kSourceTypeEffect;
    src.guid = effect_guid;
    land_children(by, n.actor_id, hit, src, now, 0);
}

CombatWorld::HolderEvent CombatWorld::event_of_hit(const Npc& n, uint64_t attacker, const Resolved& r, const Delivery& d, bool lethal, const HitOrigin* origin) const {
    HolderEvent event;
    event.attacker = attacker;
    event.origin = origin;
    event.flags = lethal ? static_cast<uint8_t>(r.flags | stats::kHitLethal) : r.flags;
    event.lethal = lethal;
    for (size_t i = 0; i < r.mods.size(); ++i) {
        event.mods[r.mods[i].stat_guid] += r.mods[i].value;
        event.effective[r.mods[i].stat_guid] += d.effective[i];
    }
    for (const effects::Instance& e : n.book.instances()) event.held.emplace_back(e.def, e.instigator);
    return event;
}

void CombatWorld::hit_reacts(uint64_t npc_actor, const HolderEvent& event, uint64_t threat_to, const HitOrigin* origin, double now) {
    holder_reacts(npc_actor, event, now);
    if (threat_to != 0 && is_pet(threat_to)) {
        pet_hit_reacts(threat_to, event, now);
    } else if (origin && origin->landed_here && origin->hit && !origin->hit->reactions.empty()) {
        // Non-session hits (such as effect ticks) carry their own reactions here.
        HolderEvent own = event;
        own.held.clear();
        own.lethal = false;
        holder_reacts(npc_actor, own, now, 2 | 4, &origin->hit->reactions);
    }
}

void CombatWorld::pet_hit_reacts(uint64_t pet, const HolderEvent& event, double now) {
    const auto it = npcs_.find(pet);
    if (it == npcs_.end() || !event.origin || it->second->dead) return;
    HolderEvent own = event;
    own.lethal = false;
    own.held.clear();
    for (const effects::Instance& e : it->second->book.instances()) own.held.emplace_back(e.def, e.instigator);
    holder_reacts(pet, own, now, 4, event.origin->hit ? &event.origin->hit->reactions : nullptr);
}

void CombatWorld::holder_reacts(uint64_t holder, const HolderEvent& event, double now, int hit_bit, const std::vector<int64_t>* carried) {
    // Hits produced by reactions do not recursively trigger reactions.
    if ((event.origin && event.origin->source_type == content::kSourceTypeReaction) || (!event.origin && !event.lethal)) return;
    std::vector<std::pair<const std::vector<int64_t>*, uint64_t>> lists;
    for (const auto& [def, left_by] : event.held) lists.emplace_back(&def->reactions, left_by);
    if (carried) lists.emplace_back(carried, event.attacker);
    for (const auto& [list, by] : lists) {
        for (const int64_t guid : *list) {
            const abilities::ReactionDef* reaction = catalog_.reaction(guid);
            if (!reaction) continue;
            const content::ReactionRow& row = reaction->row;
            const bool on_hit = event.origin && (row.bind_hit & hit_bit) != 0;
            const bool on_death = hit_bit == 2 && event.lethal && (row.bind_stat & 2) != 0;
            if (!on_hit && !on_death) continue;
            const uint64_t caster = row.instigator == "HitInstigator" ? event.attacker : by;
            PlayerPort* port = player(caster);
            if (!port || is_pet(caster)) continue;
            const auto holder_it = npcs_.find(holder);
            if (holder_it == npcs_.end()) return;
            if (row.trigger_expr != 0) {
                const content::ExprRow* e = catalog_.expr(row.trigger_expr);
                if (!e || e->program < 0) {
                    if (reaction_noted_.insert(guid).second) spdlog::info("[abilities] the reaction {} on a creature stays inert: its trigger does not compile", row.name);
                    continue;
                }
                if (e->text.find("GetEffectInstigator") != std::string::npos && caster != by) continue;
                std::string err;
                auto a = port->fighter().stats().evaluation(port->fighter().worn(), err, true);
                auto t = holder_it->second->stats().evaluation(holder_it->second->worn(), err, true);
                if (!a || !t || !a->set_actor(stats::kRoleTarget, *t, err)) continue;
                HitContext ctx = make_reaction_hit_ctx(data_.profile(), event.origin, event.flags);
                EventHost host(rng_, event, caster);
                host.set_context(&ctx);
                double v = 0;
                const bool ran = a->eval(e->program, host, v, err);
                host.set_context(nullptr);
                a->clear_actors();
                if (!ran) {
                    if (reaction_noted_.insert(guid).second) spdlog::info("[abilities] the reaction {} on a creature stays inert: {}", row.name, err);
                    continue;
                }
                if (v == 0.0) continue;
            }
            LandSource src{.type = content::kSourceTypeReaction, .guid = guid, .event = &event, .skip_requirement = true};
            for (const int64_t hit_guid : row.hits) {
                const abilities::HitDef* hit = catalog_.hit(hit_guid);
                if (!hit) continue;
                if ((row.targets_mask & (1 | 8 | 16)) != 0) land_for_player(caster, holder, *hit, src, now);
                if ((row.targets_mask & 2) != 0) port->hit_from_pet(hit_guid, guid, now);
                if ((row.targets_mask & 4) != 0 && event.attacker != caster) land_for_player(caster, event.attacker, *hit, src, now);
                if ((row.targets_mask & 8192) != 0) {
                    if (const uint64_t pet = pet_of_owner(caster)) land_for_player(caster, pet, *hit, src, now);
                }
            }
        }
    }
}

void CombatWorld::pet_was_hit(Npc& pet, const IncomingHit& hit, double taken, double now) {
    if (pet.book.empty()) return;
    HitOrigin origin;
    origin.hit = catalog_.hit(hit.hit_guid);
    origin.source_type = hit.source_type;
    origin.source_guid = hit.source_guid;
    HolderEvent event;
    event.attacker = hit.instigator;
    event.origin = &origin;
    event.flags = hit.flags;
    for (const ResolvedMod& m : hit.mods) event.mods[m.stat_guid] += m.value;
    event.effective[data_.ids().health] = -taken;
    for (const effects::Instance& e : pet.book.instances()) event.held.emplace_back(e.def, e.instigator);
    holder_reacts(pet.actor_id, event, now);
}

}  // namespace aoc::worldd::combat
