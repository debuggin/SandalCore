// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Reaction dispatch for Hit (OwnerHits), Ability (Consume), and hit-carried ReactionsIds.
#include "session/ability_pipeline_internal.h"
#include "session/summoner_pets.h"

#include "combat/combat_world.h"

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
    if (row.instigator != "Owner" && row.instigator != "Instigator") {
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

namespace {

constexpr const char* kMovesAtPress[] = {"Fighter_Leapstrike", "Fighter_LungingAssault", "Ranger_Dash"};

double refunded_stat_value(const stats::CharacterStats& stats, int64_t stat, double now_value, double amount,
                           std::pair<double, double> max_mana_and_health) {
    double cap = kInf;
    if (stat == content::kGuidMana) cap = max_mana_and_health.first;
    else if (stat == content::kGuidHealth) cap = max_mana_and_health.second;
    else if (const stats::StatValue* v = stats.find(stat); v && v->has_max) cap = static_cast<double>(v->max);
    return std::max(now_value, std::min(cap, now_value + amount));
}

bool cast_spends_mana(const ability_types::Cast& cast) {
    for (const auto& [stat, amount] : cast.stat_costs) {
        if (amount > 0 && stat == content::kGuidMana) return true;
    }
    return false;
}

const ability_types::Group* refund_group_charge(AbilityChange& c, const ability_types::Cast& cast,
                                                std::map<std::string, ability_types::Group>& groups) {
    if (!cast.triggers_cooldown || !(cast.cooldown > 0)) return nullptr;
    const auto it = groups.find(cast.group);
    if (it == groups.end()) return nullptr;
    ability_types::Group& g = it->second;
    g.charges = std::min(g.max_charges, g.charges + cast.charge_cost);
    if (g.charges < g.max_charges) return &g;
    for (const int64_t a : g.items) c.cooldowns_removed.push_back(a);
    groups.erase(it);
    return nullptr;
}

void erase_client_instances(std::deque<ability_types::Instance>& instances, int32_t client_id) {
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                                   [client_id](const ability_types::Instance& in) { return in.client_id == client_id; }),
                    instances.end());
}

bool record_has_launched_slot(const ability_types::CastRecord& r) {
    if (r.launched || !r.asked.empty()) return true;
    for (const ability_types::HitSlot& s : r.slots) {
        if (s.requests > 0 || s.applied || !s.due.empty()) return true;
    }
    return false;
}

bool pop_pending_swing_cast(std::vector<ability_types::Cast>& casts, double t, const char* why, AbilityEvent& out) {
    for (size_t i = casts.size(); i-- > 0;) {
        const ability_types::Cast& p = casts[i];
        if (!is_weapon_combo(p.def->row) || !p.item_due) continue;
        out = {};
        out.at = t;
        out.end_ability = p.client_id;
        out.note = "the swing of " + p.def->row.name + " is ended: " + why;
        casts.erase(casts.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

void clear_unlaunched_swing_casts(std::vector<ability_types::Cast>& casts, int64_t server_id) {
    for (ability_types::Cast& cast : casts) {
        if (cast.instance != server_id) continue;
        cast.hit_due.reset();
        if (!cast.shot && !cast.flight_pending) cast.projectile_due.reset();
    }
}

}  // namespace

bool gone_out_at_the_press(const content::AbilityRow& row) {
    for (const pets::PetBarCommand& c : pets::kPetBarCommands) {
        if (c.ability == row.guid) return true;
    }
    if (row.networking_max_speed > 0) return true;
    for (const Dash& d : kDashes) {
        if (row.name == d.ability) return true;
    }
    for (const char* name : kMovesAtPress) {
        if (row.name == name) return true;
    }
    return false;
}

void AbilityPipeline::restore_stacks(AbilityChange& c, const EffectDef& def, int stacks, double t) {
    for (EffectInst& e : effects_) {
        if (e.def != &def) continue;
        e.stacks += stacks;
        c.effects_changed.push_back(effect_item(e));
        return;
    }
    apply_effect(c, def, stacks, std::nullopt, t, true);
}

bool AbilityPipeline::in_cast_bar(const Cast& cast) const {
    if (pets::for_ability(cast.def->row.guid)) return cast.cost_due.has_value();
    return cast.branched != nullptr && cast.branch_due && !cast.request_heard && !cast.charged && !cast.released;
}

void AbilityPipeline::refund(AbilityChange& c, Cast& cast) {
    if (!cast.paid) return;
    cast.paid = false;
    give_back(c, cast);
}

void AbilityPipeline::give_back(AbilityChange& c, const Cast& cast) {
    const std::pair<double, double> max_vitals{stat_now(content::kGuidMaxMana), stat_now(content::kGuidMaxHealth)};
    for (const auto& [stat, amount] : cast.stat_costs) {
        if (!(amount > 0)) continue;
        if (stat == content::kGuidStamina) {
            a7_.add_stamina(c, amount);
        } else {
            set_held(c, stat, refunded_stat_value(stats_, stat, stat_now(stat), amount, max_vitals));
        }
    }
    if (resource_ && cast.resource_added > 0 && cast_spends_mana(cast)) {
        remove_stacks(c, resource_->row.guid, cast.resource_added);
    }
    if (catalog_) {
        for (const auto& [effect, n] : cast.effect_costs) {
            if (const EffectDef* def = catalog_->effect(effect)) restore_stacks(c, *def, n, now_);
        }
    }
    if (const Group* g = refund_group_charge(c, cast, groups_)) {
        for (const int64_t a : g->items) c.cooldowns_set.push_back(cooldown_item(a, *g));
    }
    recompute(c);
}

size_t AbilityPipeline::end_cast_bars(double now, const char* why) {
    advance(now);
    size_t ended = 0;
    for (size_t i = casts_.size(); i-- > 0;) {
        if (!in_cast_bar(casts_[i])) continue;
        const int32_t client_id = casts_[i].client_id;
        const std::string name = casts_[i].def->row.name;
        const bool paid = casts_[i].paid;
        AbilityEvent e;
        e.at = now_;
        e.end_ability = client_id;
        abort_cast(i, now_, why, true);
        erase_client_instances(instances_, client_id);
        e.note = name + " is over: " + why + (paid ? ", what it had paid is given back" : "");
        events_.push_back(std::move(e));
        ++ended;
    }
    return ended;
}

size_t AbilityPipeline::end_by_dodge(double now) {
    size_t ended = end_cast_bars(now, "a Dodge ended the cast bar");
    ended += end_unlaunched(now_, "a Dodge cancelled it before it went out");
    ended += end_swings(now_, "a Dodge ended the swing");
    return ended;
}

bool AbilityPipeline::gone_out(int32_t client_id, const Cast* cast) const {
    if (cast && (cast->request_heard || cast->charged || cast->released)) return true;
    for (const CastRecord& r : records_) {
        if (r.client_id == client_id) return record_has_launched_slot(r);
    }
    return true;
}

void AbilityPipeline::mark_launched(int32_t client_id) {
    if (CastRecord* r = record_of(client_id)) r->launched = true;
}

bool AbilityPipeline::dodge_cancels(const Cast& cast) const {
    if (cast.loop || cast.toggle_off || cast.item_cast || is_weapon_combo(cast.def->row) || gone_out_at_the_press(cast.def->row)) return false;
    return !gone_out(cast.client_id, &cast);
}

size_t AbilityPipeline::end_unlaunched(double t, const char* why) {
    size_t ended = 0;
    for (size_t i = casts_.size(); i-- > 0;) {
        if (!dodge_cancels(casts_[i])) continue;
        Cast& cast = casts_[i];
        const int32_t client_id = cast.client_id;
        const std::string name = cast.def->row.name;
        const bool paid = cast.paid;
        const double since = t - cast.trigger_at;
        AbilityEvent e;
        e.at = t;
        e.end_ability = client_id;
        for (int shot = 1; shot < cast.shots; ++shot) give_back(e.change, cast);
        refund(e.change, cast);
        step_end(cast, t);
        drop_record(client_id);
        end_zones_of(cast, t);
        casts_.erase(casts_.begin() + static_cast<std::ptrdiff_t>(i));
        erase_client_instances(instances_, client_id);
        e.note = name + " is over: " + why + (paid ? ", what it had paid is given back" : "");
        spdlog::info("[abilities] {} {:.3f} s after the trigger: {}", name, since, e.note);
        events_.push_back(std::move(e));
        ++ended;
    }
    return ended;
}

size_t AbilityPipeline::end_swings(double t, const char* why) {
    size_t ended = 0;
    AbilityEvent pending;
    while (pop_pending_swing_cast(casts_, t, why, pending)) {
        drop_record(*pending.end_ability);
        events_.push_back(std::move(pending));
        ++ended;
    }
    if (!item_instance_) return ended;
    for (size_t i = 0; i < instances_.size(); ++i) {
        const Instance inst = instances_[i];
        if (inst.server_id != *item_instance_ || inst.end_due) continue;
        const AbilityDef* a = catalog_ ? catalog_->ability(inst.ability) : nullptr;
        if (!a || !is_weapon_combo(a->row)) continue;
        if (gone_out(inst.client_id)) return ended;
        drop_record(inst.client_id);
        clear_unlaunched_swing_casts(casts_, inst.server_id);
        stop_swing(inst.server_id);
        end_instance(i, t, a->row.name + " is over: " + why);
        ++ended;
        break;
    }
    return ended;
}

}  // namespace aoc::worldd
