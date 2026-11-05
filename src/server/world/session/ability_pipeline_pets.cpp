// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.

#include "session/ability_pipeline_internal.h"
#include "session/summoner_pets.h"

namespace aoc::worldd {

using abilities::AbilityDef;
using abilities::EffectDef;
using abilities::HitDef;
using bootstrap::AbilityChange;

struct AbilityPipeline::Host::Pets {
  static void collect_owner_cost_mods(
      AbilityPipeline &self, const HitDef &hit,
      std::vector<std::pair<int64_t, double>> &each) {
    std::string why;
    for (const content::ModRow &m : hit.mods) {
      double amount = 0;
      if (m.value_expr == 0 || !self.eval(m.value_expr, amount, why) ||
          !std::isfinite(amount))
        continue;
      each.emplace_back(m.stat_guid, amount);
    }
  }

  static std::vector<std::pair<int64_t, double>>
  eval_owner_costs(AbilityPipeline &self, const AbilityDef &def) {
    std::vector<std::pair<int64_t, double>> each;
    std::vector<int64_t> counted;
    for (const content::AbilityHitRow &h : def.hits) {
      const HitDef *hit = self.catalog_->hit(h.hit_guid);
      if (!hit || !hit->row.name.ends_with("_OwnerCost") ||
          std::find(counted.begin(), counted.end(), h.hit_guid) !=
              counted.end())
        continue;
      counted.push_back(h.hit_guid);
      collect_owner_cost_mods(self, *hit, each);
    }
    return pets::sum_by_stat(each);
  }

  static bool resolve_cooldown_delta(
      AbilityPipeline &self, const abilities::Catalog::ModifyCooldownDef &m,
      const std::string &key, double &delta) {
    const auto g = self.groups_.find(key);
    const content::ExprRow *e = self.catalog_->expr(m.delta_expr);
    if (e && e->text.find("GetInitialAbilityCooldown") != std::string::npos) {
      delta = g != self.groups_.end() ? -g->second.cooldown : 0.0;
      return true;
    }
    std::string why;
    if (!self.eval(m.delta_expr, delta, why)) {
      spdlog::debug("[abilities] the cooldown change {}: {}", m.name, why);
      return false;
    }
    return true;
  }

  static void apply_cooldown_delta(AbilityPipeline &self, AbilityChange &c,
                                   const AbilityDef &def,
                                   const std::string &key, double delta,
                                   double t) {
    const auto g = self.groups_.find(key);
    if (g == self.groups_.end()) {
      if (delta <= 0)
        return;
      Cast cast;
      cast.def = &def;
      cast.group = key;
      cast.cooldown = delta;
      cast.max_charges = 1;
      cast.charge_cost = 0;
      cast.triggers_cooldown = true;
      self.use_charge(c, cast, t);
      return;
    }
    if (delta == 0 || g->second.timer_end < 0)
      return;
    g->second.timer_end += delta;
    g->second.chain_start += delta;
    if (g->second.timer_end <= t) {
      self.group_timer_ends(key, t);
    } else {
      for (const int64_t a : g->second.items)
        c.cooldowns_set.push_back(self.cooldown_item(a, g->second));
    }
  }

  static void apply_modify_cooldown_def(
      AbilityPipeline &self, AbilityChange &c,
      const abilities::Catalog::ModifyCooldownDef &m, double t) {
    for (const int64_t ability : m.abilities) {
      const AbilityDef *def = self.catalog_->ability(ability);
      if (!def)
        continue;
      const std::string key = cooldown_group_of(def->row);
      double delta = 0;
      if (resolve_cooldown_delta(self, m, key, delta))
        apply_cooldown_delta(self, c, *def, key, delta, t);
    }
  }
};

bool AbilityPipeline::can_pay_pet_ability(int64_t ability, double now,
                                          PetPayment &out) {
  advance(now);
  out = {};
  const AbilityDef *def = catalog_ ? catalog_->ability(ability) : nullptr;
  if (!def)
    return false;
  std::string why;
  double cooldown = 0;
  if (def->row.cooldown_expr != 0 &&
      eval(def->row.cooldown_expr, cooldown, why))
    out.cooldown = std::max(0.0, cooldown);
  out.costs = Host::Pets::eval_owner_costs(*this, *def);
  for (const auto &[stat, amount] : out.costs) {
    if (stat_now(stat) + amount < 0)
      return false;
  }
  return true;
}

void AbilityPipeline::commit_pet_payment(int64_t ability,
                                         const PetPayment &pay, double now) {
  advance(now);
  const AbilityDef *def = catalog_ ? catalog_->ability(ability) : nullptr;
  if (!def)
    return;
  AbilityChange c;
  for (const auto &[stat, amount] : pay.costs)
    apply_delta(c, stat, amount);
  drop_evaluation();
  recompute(c);
  emit({std::move(c), std::nullopt, now, "the Essence of " + def->row.name});
  start_pet_cooldown(ability, pay.cooldown, now);
}

bool AbilityPipeline::pay_pet_ability(int64_t ability, double now,
                                      double &cooldown) {
  PetPayment pay;
  cooldown = 0;
  if (!can_pay_pet_ability(ability, now, pay))
    return false;
  commit_pet_payment(ability, pay, now);
  cooldown = pay.cooldown;
  return true;
}

void AbilityPipeline::start_pet_cooldown(int64_t ability, double seconds,
                                         double now) {
  const AbilityDef *def = catalog_ ? catalog_->ability(ability) : nullptr;
  if (!def || seconds <= 0)
    return;
  advance(now);
  Cast cast;
  cast.def = def;
  cast.group = cooldown_group_of(def->row);
  cast.cooldown = seconds;
  cast.max_charges = 1;
  cast.charge_cost = 1;
  cast.triggers_cooldown = true;
  AbilityChange c;
  use_charge(c, cast, now);
  emit({std::move(c), std::nullopt, now,
        "the cooldown of " + def->row.name + " on the owner"});
}

void AbilityPipeline::land_pet_hit(int64_t hit_guid, int64_t ability,
                                   double now) {
  advance(now);
  const HitDef *hit = catalog_ ? catalog_->hit(hit_guid) : nullptr;
  if (!hit)
    return;
  AbilityChange c;
  if (hit->max_remove > 0 && hit->remove_effects.empty() &&
      hit->removal == HitDef::Removal::Unknown) {
    for (size_t i = 0; i < effects_.size(); ++i) {
      if (effects_[i].def->row.category.find("Debuff") == std::string::npos)
        continue;
      remove_instance(c, i);
      break;
    }
    sync_engine_effects();
  }
  HitSource src;
  src.type = content::kSourceTypeAbility;
  src.guid = ability;
  src.confirm = true;
  apply_hit_here(c, *hit, now, true, src);
  drop_evaluation();
  recompute(c);
  emit({std::move(c), std::nullopt, now,
        "a hit of the pet on the character: " + hit->row.name});
}

void AbilityPipeline::confirm_dealt(const combat::DealtHit &dealt, double now) {
  if (!combat_)
    return;
  advance(now);
  bootstrap::HitConfirm h;
  h.record_guid = dealt.hit_guid;
  h.source_type = dealt.source_type;
  h.source_guid = dealt.source_guid;
  h.source_is_actor = dealt.source_type != content::kSourceTypeProjectile &&
                      dealt.source_type != content::kSourceTypeEffect;
  h.predicted_autonomous = false;
  h.predicted_simulated = false;
  if (dealt.instigator != 0)
    h.instigator = combat_->guid_of(dealt.instigator);
  if (dealt.source_type == content::kSourceTypeEffect)
    h.source = combat_->guid_of(dealt.target);
  bootstrap::HitTargetOut out;
  out.component = combat_->guid_of(dealt.target);
  out.cylinder = combat_->hit_component_of(dealt.target);
  out.flags = dealt.flags;
  for (size_t i = 0; i < dealt.mods.size(); ++i) {
    const double effective =
        i < dealt.effective.size() ? dealt.effective[i] : 0.0;
    if (dealt.mods[i].value == 0.0 && effective == 0.0)
      continue;
    out.mods.push_back({dealt.mods[i].stat_mod_guid,
                        static_cast<float>(dealt.mods[i].value),
                        static_cast<float>(effective)});
  }
  h.targets.push_back(std::move(out));
  confirmations_.push_back(std::move(h));
  emit({AbilityChange{}, std::nullopt, now_,
        "a hit of the character's pet or reaction on actor " +
            std::to_string(dealt.target)});
}

bool AbilityPipeline::holds_dispellable() const {
  for (const EffectInst &e : effects_) {
    if (e.def->row.dispellable &&
        e.def->row.category.find("Debuff") != std::string::npos)
      return true;
  }
  return false;
}

bool AbilityPipeline::grant_effect(int64_t effect_guid, double now) {
  advance(now);
  const EffectDef *def = catalog_ ? catalog_->effect(effect_guid) : nullptr;
  if (!def)
    return false;
  AbilityChange c;
  apply_effect(c, *def, 1, std::nullopt, now);
  sync_engine_effects();
  drop_evaluation();
  recompute(c);
  emit({std::move(c), std::nullopt, now, "the effect " + def->row.name});
  return true;
}

void AbilityPipeline::modify_cooldowns(AbilityChange &c, const HitDef &hit,
                                       double t) {
  for (const int64_t id : hit.modify_cooldowns) {
    if (const abilities::Catalog::ModifyCooldownDef *m =
            catalog_->modify_cooldown(id)) {
      Host::Pets::apply_modify_cooldown_def(*this, c, *m, t);
    }
  }
}

void AbilityPipeline::land_bolt_recall(int32_t client_id, uint64_t target,
                                       double t) {
  if (combat_)
    combat_->remove_effect(target, pets::kBoltMark, t);
  AbilityChange c;
  HitSource src;
  src.type = content::kSourceTypeAbility;
  src.guid = pets::kBolt;
  if (const HitDef *remove =
          catalog_ ? catalog_->hit(pets::kBoltRemoveHit) : nullptr)
    apply_hit_here(c, *remove, t, true, src);
  if (const HitDef *cooldown =
          catalog_ ? catalog_->hit(pets::kBoltCooldownHit) : nullptr)
    apply_hit_here(c, *cooldown, t, true, src);
  drop_evaluation();
  recompute(c);
  bolt_recall_ = {target, stacks_of(pets::kHunterEffect) > 0};
  emit({std::move(c), std::nullopt, t,
        "the second press of Dimensional Bolt sends the pet"});
  AbilityEvent e;
  e.at = t;
  e.end_ability = client_id;
  e.note = "the second press of Dimensional Bolt is over";
  events_.push_back(std::move(e));
}

} // namespace aoc::worldd
