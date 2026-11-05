// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Reaction dispatch for Hit (OwnerHits), Ability (Consume), and hit-carried ReactionsIds.
#include "session/ability_pipeline.h"

#include "combat/combat_world.h"
#include "content/content_db.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace aoc::worldd {

using abilities::AbilityDef;
using abilities::EffectDef;
using abilities::HitDef;
using abilities::ReactionDef;
using bootstrap::AbilityChange;

namespace {

constexpr int kTargetOwner = 1;
constexpr int kTargetTargets = 8;
constexpr int kTargetInstigator = 2;
constexpr int kTargetInstigatorsPet = 8192;

constexpr int kKindHit = 0;
constexpr int kKindAbility = 2;
constexpr int kBitOwnerHits = 4;
constexpr int kBitConsume = 4;

int binding_of(const content::ReactionRow& r, int kind) {
    switch (kind) {
        case kKindHit: return r.bind_hit;
        case kKindAbility: return r.bind_ability;
        default: return 0;
    }
}

}  // namespace

void AbilityPipeline::set_reaction_carriers(std::vector<int64_t> effect_guids) {
    reaction_carriers_ = std::move(effect_guids);
}

void AbilityPipeline::fire_carried_reactions(AbilityChange& c, const HitEvent& ev, double t) {
    if (!reactions_enabled_ || carried_depth_ >= 2 || !catalog_ || !ev.hit || ev.hit->reactions.empty() || (ev.targets.empty() && !ev.on_self)) return;
    ReactionContext rc;
    rc.hit = &ev;
    for (const int64_t guid : ev.hit->reactions) {
        const ReactionDef* r = catalog_->reaction(guid);
        if (!r) continue;
        const content::ReactionRow& row = r->row;
        if (row.trigger_expr != 0) {
            double v = 0;
            std::string why;
            if (!eval_in_event(row.trigger_expr, rc, v, why)) {
                if (reaction_noted_.insert(guid).second) spdlog::info("[abilities] the reaction {} stays inert: {}", row.name, why);
                continue;
            }
            if (v == 0) continue;
        }
        HitSource src;
        src.type = content::kSourceTypeReaction;
        src.guid = guid;
        src.actor = false;
        src.confirm = true;
        ++carried_depth_;
        const ReactionContext* const outer = active_reaction_;
        active_reaction_ = &rc;
        for (const int64_t hit_guid : row.hits) {
            const HitDef* hit = catalog_->hit(hit_guid);
            if (!hit) continue;
            if ((row.targets_mask & (kTargetOwner | kTargetInstigator)) != 0) apply_hit_here(c, *hit, t, true, src);
            if ((row.targets_mask & kTargetTargets) != 0 && !ev.targets.empty()) apply_hit_on_targets(c, *hit, ev.targets, t, src, nullptr);
            if ((row.targets_mask & kTargetInstigatorsPet) != 0 && combat_) {
                if (const uint64_t pet = combat_->own_pet()) apply_hit_on_targets(c, *hit, {pet}, t, src, nullptr);
            }
        }
        active_reaction_ = outer;
        drop_evaluation();
        --carried_depth_;
        ++reactions_fired_;
        spdlog::log(reactions_fired_ <= 40 ? spdlog::level::info : spdlog::level::debug, "[abilities] the reaction {} of the hit {} fired", row.name, ev.hit->row.name);
    }
}

void AbilityPipeline::raise_hit_event(AbilityChange& c, const HitEvent& ev, double t) {
    fire_carried_reactions(c, ev, t);
    if (!reactions_enabled_ || !combat::can_trigger_proc_at_depth(reaction_depth_) || !catalog_ || catalog_->reactions() == 0 || !ev.hit) return;
    if (ev.hit->tags.empty() && ev.hit->mods.empty()) return;
    if (ev.src.type == content::kSourceTypeReaction) return;
    ReactionContext rc;
    rc.hit = &ev;
    run_reactions(c, kKindHit, kBitOwnerHits, rc, t);
}

void AbilityPipeline::raise_ability_event(AbilityChange& c, const AbilityDef& ability, double t) {
    if (!reactions_enabled_ || !combat::can_trigger_proc_at_depth(reaction_depth_) || !catalog_ || catalog_->reactions() == 0) return;
    ReactionContext rc;
    rc.consumed = &ability;
    run_reactions(c, kKindAbility, kBitConsume, rc, t);
}

void AbilityPipeline::run_reactions(AbilityChange& c, int kind, int bit, const ReactionContext& rc, double t) {
    if (!combat::can_trigger_proc_at_depth(reaction_depth_)) return;
    // the carriers in the order the character got them: the ranks' passives first, then what abilities and hits applied (a snapshot: a reaction's hits change
    // the effects)
    std::vector<int64_t> carriers = reaction_carriers_;
    for (const EffectInst& e : effects_) carriers.push_back(e.def->row.guid);
    std::vector<int64_t> seen;
    for (const int64_t effect_guid : carriers) {
        if (std::find(seen.begin(), seen.end(), effect_guid) != seen.end()) continue;
        seen.push_back(effect_guid);
        const EffectDef* carrier = catalog_->effect(effect_guid);
        if (!carrier || carrier->reactions.empty()) continue;
        for (const int64_t reaction_guid : carrier->reactions) {
            const ReactionDef* r = catalog_->reaction(reaction_guid);
            if (!r || (binding_of(r->row, kind) & bit) == 0) continue;
            const int limit = r->row.global_limit > 0 ? r->row.global_limit : r->row.instigator_limit;
            if (limit > 0 && reaction_procs_[reaction_guid] >= limit) continue;
            if (r->row.trigger_expr != 0) {
                double v = 0;
                std::string why;
                if (!eval_in_event(r->row.trigger_expr, rc, v, why)) {
                    if (reaction_noted_.insert(reaction_guid).second) spdlog::info("[abilities] the reaction {} stays inert: {}", r->row.name, why);
                    continue;
                }
                if (v == 0) continue;
            }
            fire_reaction(c, *r, rc, t);
            if (limit > 0 && reaction_procs_[reaction_guid] >= limit) {
                // a reaction that reached its limit takes its carrier off [the game's code: the effect goes with all its stacks]
                for (const EffectInst& e : effects_) {
                    if (e.def->row.guid == effect_guid) {
                        remove_stacks(c, effect_guid, 1 << 20);
                        break;
                    }
                }
            }
        }
    }
}

void AbilityPipeline::fire_reaction(AbilityChange& c, const ReactionDef& r, const ReactionContext& rc, double t) {
    const content::ReactionRow& row = r.row;
    // who casts the hits: only the holder (Owner) is served; a reaction cast by the hit's instigator or its source needs actors the world does not hold yet
    if (row.instigator != "Owner") {
        if (reaction_noted_.insert(row.guid).second) spdlog::info("[abilities] the reaction {} is cast by {}, which is not served yet: it does not fire", row.name, row.instigator);
        return;
    }
    const int served = kTargetOwner | kTargetTargets;
    if ((row.targets_mask & ~served) != 0 && reaction_noted_.insert(-row.guid).second) {
        spdlog::info("[abilities] the reaction {} lands on targets (mask {}) that are not all served yet: the holder and the hit's targets get its hits", row.name, row.targets_mask);
    }
    if ((row.targets_mask & served) == 0) return;
    combat::ProcDepthScope depth_scope(reaction_depth_);
    if (!depth_scope.allowed()) return;
    ++reaction_procs_[row.guid];
    ++reactions_fired_;
    // the reaction's hits are the game's own records, applied under the reaction as their source: the client takes them as the server's (predicted on neither
    // side) and draws their numbers from the confirmations
    HitSource src;
    src.type = content::kSourceTypeReaction;
    src.guid = row.guid;
    src.actor = false;
    src.confirm = true;
    src.autonomous = false;
    src.simulated = false;
    size_t applied = 0;
    for (const int64_t hit_guid : row.hits) {
        const HitDef* hit = catalog_->hit(hit_guid);
        if (!hit) continue;
        // a hit that fires further hits through a targeting record (the melodies that heal or cleanse the allies around) lands as itself only: the hits it fires
        // are not followed yet
        if (!hit->recursive.empty() && reaction_recursive_noted_.insert(row.guid).second) {
            spdlog::info("[abilities] the reaction {}: its hit {} fires {} further hit(s) through a targeting record, which are not followed yet: they do not land", row.name,
                         hit->row.name, hit->recursive.size());
        }
        if ((row.targets_mask & kTargetTargets) != 0 && rc.hit && !rc.hit->targets.empty()) {
            apply_hit_on_targets(c, *hit, rc.hit->targets, t, src, nullptr);
            ++applied;
        }
        if ((row.targets_mask & kTargetOwner) != 0) {
            apply_hit(c, *hit, t, true, src);
            ++applied;
        }
    }
    spdlog::log(reactions_fired_ <= 40 ? spdlog::level::info : spdlog::level::debug, "[abilities] the reaction {} fired ({} of its {} hit(s) applied; event: {})", row.name, applied, row.hits.size(),
                rc.hit && rc.hit->hit ? "a hit, " + rc.hit->hit->row.name : rc.consumed ? "the cost of " + rc.consumed->row.name + " paid" : std::string("?"));
}

}  // namespace aoc::worldd
