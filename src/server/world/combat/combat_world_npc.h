// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "combat/combat_world.h"
#include "combat/npc_ability_replicator.h"
#include "combat/npc_replicator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd::combat {

namespace npc_kits {
struct Kit;
}

class CombatWorld::Npc final : public Fighter {
public:
    Npc(CombatWorld& w, uint64_t actor, npc::MotionStats bp_in, std::string leaf_in, int level_in)
        : world(w), actor_id(actor), bp(std::move(bp_in)), leaf(std::move(leaf_in)), lvl(level_in) {}

    uint64_t id() const override { return actor_id; }
    bool     is_player() const override { return false; }
    int      level() const override { return lvl; }
    Vec3     position() const override {
        const world::WorldActor* a = world.registry_.find(actor_id);
        return a ? Vec3{a->x, a->y, a->z} : Vec3{};
    }
    double radius_cm() const override { return bp.radius; }
    bool   alive() const override { return !dead; }
    const std::vector<int64_t>& alignments() const override { return aligns; }
    stats::CharacterStats&      stats() override { return *cs; }

    double health() const {
        const stats::StatValue* v = cs ? cs->find(content::kGuidHealth) : nullptr;
        return v ? v->value : 0.0;
    }
    double max_health() const {
        const stats::StatValue* v = cs ? cs->find(content::kGuidMaxHealth) : nullptr;
        return v ? v->value : 0.0;
    }

    CombatWorld&                           world;
    uint64_t                               actor_id;
    npc::MotionStats                       bp;
    std::string                            leaf;
    int                                    lvl;
    std::vector<int64_t>                   aligns;
    std::unique_ptr<stats::CharacterStats> cs;
    npc::ai::NpcAi                         ai;
    bool                                   hostile = false;
    bool                                   dead = false;
    bool                                   flags_in_combat = false;
    uint64_t                               flags_target = 0;
    bool                                   returning = false;
    const npc_kits::Kit*                   kit = nullptr;
    CombatWorld::Attack                    next;
    double                                 next_attack_at = 0;
    std::string                            archetype;
    const std::vector<CreatureAbility>*    anim = nullptr;
    NpcCastState                           cast;
    std::map<std::string, double>          cooldown_until;
    double                                 hold_until = 0;
    double                                 attack_walk_share = 1.0;
    double                                 stand_off_cm = -1;
    double                                 shot_reach_cm = -1;
    double                                 fight_began = 0;
    std::map<std::string, double>          fight_rolls;
    uint64_t                               killer = 0;
    std::set<uint64_t>                     attackers;
    struct Shield {
        double amount = 0;
        double until = 0;
    };
    std::vector<Shield>                    shields;
    effects::Book                          book;
    EffectItemIds                          item_ids;
    effects::Book::Crowd                   cc;
    effects::Book::Crowd                   cc_logged;

    double absorb(double loss, double now) {
        shields.erase(std::remove_if(shields.begin(), shields.end(), [&](const Shield& s) { return s.amount <= 0 || (s.until > 0 && s.until <= now); }), shields.end());
        for (Shield& s : shields) {
            const double take = std::min(s.amount, loss);
            s.amount -= take;
            loss -= take;
        }
        return loss;
    }
};

struct CombatWorld::HolderEvent {
    uint64_t                  attacker = 0;
    const HitOrigin*          origin = nullptr;
    uint8_t                   flags = 0;
    bool                      lethal = false;
    std::map<int64_t, double> mods, effective;
    std::vector<std::pair<const abilities::EffectDef*, uint64_t>> held;
};

class CombatWorld::PetPort final : public PlayerPort {
public:
    PetPort(CombatWorld& w, Npc& pet) : world_(w), pet_(pet) {}
    uint64_t actor_id() const override { return pet_.actor_id; }
    Fighter& fighter() override { return pet_; }
    void receive_hit(const IncomingHit& hit, double now) override {
        if (pet_.dead || (hit.flags & stats::kHitMiss) != 0) return;
        const CombatStatIds& id = world_.data_.ids();
        double taken = 0;
        for (const ResolvedMod& m : hit.mods) {
            if (m.stat_guid != id.health || !std::isfinite(m.value)) continue;
            const stats::StatValue* cur = pet_.cs->find(id.health);
            const stats::StatValue* max = pet_.cs->find(id.max_health);
            if (!cur || !max) continue;
            double value = m.value;
            if (value < 0) value = -pet_.absorb(-value, now);
            const double after = std::clamp(static_cast<double>(cur->value) + value, 0.0, static_cast<double>(max->value));
            taken += std::max(0.0, static_cast<double>(cur->value) - after);
            pet_.cs->set_current(id.health, static_cast<float>(after));
            write_stat(world_.registry_, pet_.actor_id, id.health, static_cast<float>(after));
        }
        world_.pet_was_hit(pet_, hit, taken, now);
        if (pet_.health() <= 0.0) {
            pet_.dead = true;
            world_.pet_stop(pet_.actor_id);
            world_.drop_effects_of_pet(pet_.actor_id);
        }
    }
    void set_in_combat(bool, double) override {}
    protocol::emit::FIntrepidNetworkGUID pawn_guid() const override {
        const world::WorldActor* a = world_.registry_.find(pet_.actor_id);
        return a ? a->actor.guid : protocol::emit::FIntrepidNetworkGUID{};
    }
    protocol::emit::FIntrepidNetworkGUID cylinder_guid() const override {
        protocol::emit::FIntrepidNetworkGUID g = pawn_guid();
        if (const uint64_t cylinder = world_.hit_component_of(pet_.actor_id)) g.ObjectId = cylinder;
        return g;
    }

private:
    CombatWorld& world_;
    Npc&         pet_;
};

struct CombatWorld::LandSource {
    int64_t                      type = 0, guid = 0;
    const abilities::AbilityDef* ability = nullptr;
    uint64_t                     pet = 0;
    const HolderEvent*           event = nullptr;
    bool                         children = true;
    bool                         skip_requirement = false;
};

class CombatWorld::EventHost final : public ContextHost {
public:
    EventHost(stats::SplitMix& rng, const HolderEvent& event, uint64_t instigator) : rng_(rng), event_(event), instigator_(instigator) {}
    double rand_double() override { return rng_.rand_double(); }
    double effect_count_by_instigator(const stats::RecordRef& r) override {
        double count = 0;
        for (const auto& [def, by] : event_.held) count += def->row.guid == r.guid && by == instigator_ ? 1.0 : 0.0;
        return count;
    }

protected:
    double other_fn(int fn, const std::string& name, const double* args, int) override {
        switch (fn) {
            case stats::kHostHitStatMod:
            case stats::kHostHitStatEffective: {
                const std::map<int64_t, double>& m = fn == stats::kHostHitStatMod ? event_.mods : event_.effective;
                const auto it = m.find(std::strtoll(name.c_str(), nullptr, 10));
                return it == m.end() ? 0.0 : it->second;
            }
            case stats::kHostHitRecordIs: return event_.origin && event_.origin->hit && event_.origin->hit->row.guid == std::strtoll(name.c_str(), nullptr, 10) ? 1.0 : 0.0;
            case stats::kHostHitRecordNameIs: return event_.origin && event_.origin->hit && event_.origin->hit->row.name == name ? 1.0 : 0.0;
            case stats::kHostRandIntRange: {
                const double lo = std::floor(args[0]), hi = std::floor(args[1]);
                return hi < lo ? lo : lo + std::floor(rng_.rand_double() * (hi - lo + 1.0));
            }
            default: return 0.0;
        }
    }

private:
    stats::SplitMix&   rng_;
    const HolderEvent& event_;
    uint64_t           instigator_;
};

}  // namespace aoc::worldd::combat
