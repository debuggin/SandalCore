#include "session/ability_pipeline_internal.h"
#include "session/consumable_timings.h"
#include "session/summoner_pets.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd {

using abilities::AbilityDef;
using abilities::EffectDef;
using abilities::HitDef;
using bootstrap::AbilityChange;

struct AbilityPipeline::Host::Casts {
  struct TriggerPlan {
    int64_t ability = 0, loop_effect = 0;
    int32_t client_id = 0;
    double t = 0.0, item_delay = kItemDelay, cost_delay = kCostDelay,
           hit_delay = -1.0;
    double zone_delay = -1.0, zone_end = -1.0, table_hit = -1.0,
           table_end = -1.0;
    const AbilityDef *def = nullptr;
    std::string name;
    bool loop = false, is_toggle = false, has_montage = false;
    std::shared_ptr<const ItemCast> item_cast;
    std::vector<const EffectDef *> own_effects;
    std::vector<uint64_t> target_actors;
  };

  static constexpr int kFriendlyMask = 1 | 4 | 8 | 16;

  static Refusal refuse_cast(AbilityPipeline &self, const TriggerPlan &plan,
                             Refusal why, const std::string &extra = {}) {
    self.refuse(plan.client_id, why, plan.t, plan.name + extra);
    return why;
  }

  static bool has_active_looped_cast(const AbilityPipeline &self,
                                     const AbilityDef *def) {
    for (const Cast &other : self.casts_) {
      if (other.looped && other.def == def &&
          (other.cost_due || other.branch_due))
        return true;
    }
    return false;
  }

  static void collect_trigger_effects(const AbilityPipeline &self,
                                      TriggerPlan &plan) {
    const content::AbilityRow &row = plan.def->row;
    const bool single_press =
        row.activation_input.find("Single") != std::string::npos;
    const std::set<int64_t> by_hits = self.effects_applied_by_hits(*plan.def);
    for (const auto &[key, effect_guid] : plan.def->effects) {
      const EffectDef *e = self.catalog_->effect(effect_guid);
      if (!e || !e->row.infinite_duration)
        continue;
      if (e->row.tick_timer > 0 && single_press) {
        plan.loop = true;
        plan.loop_effect = effect_guid;
      } else if (!by_hits.count(effect_guid)) {
        plan.own_effects.push_back(e);
      }
    }
    plan.is_toggle = row.held_behavior.find("Toggle") != std::string::npos;
  }

  static Refusal validate_trigger_shape(AbilityPipeline &self,
                                        const TriggerInfo &info,
                                        TriggerPlan &plan) {
    if (self.dead_)
      return refuse_cast(self, plan, Refusal::Dead);
    if (!plan.def || self.a7_.enabled(plan.ability))
      return refuse_cast(self, plan, Refusal::NotEnabled);
    if (!self.known(plan.ability) && !info.item_cast)
      return refuse_cast(self, plan, Refusal::NotKnown);
    if (info.item && !info.item_cast)
      return refuse_cast(self, plan, Refusal::Unsupported,
                         " (an item's ability is not run here)");
    if (self.casts_.size() >= kMaxCasts ||
        has_active_looped_cast(self, plan.def))
      return refuse_cast(self, plan, Refusal::Busy);
    const content::AbilityRow &row = plan.def->row;
    if (row.target_requirement.find("Always") != std::string::npos &&
        info.target_count == 0 &&
        (row.applicable_targets & kFriendlyMask) != 0) {
      return refuse_cast(self, plan, Refusal::NeedsTarget);
    }
    collect_trigger_effects(self, plan);
    return Refusal::None;
  }

  static Refusal validate_single_target(AbilityPipeline &self,
                                        const TriggerPlan &plan, bool melee,
                                        uint64_t id) {
    const content::AbilityRow &row = plan.def->row;
    const combat::TargetInfo ti = self.combat_->inspect(id);
    if (!ti.exists || !ti.alive)
      return refuse_cast(self, plan, Refusal::BadTarget,
                         " (the target is not there or is dead)");
    const bool hostile = ti.relation == combat::Relation::Hostile;
    const bool bad_mask =
        hostile ? (row.applicable_targets & 2) == 0
                : (row.applicable_targets != 0 &&
                   (row.applicable_targets & kFriendlyMask) == 0);
    if (bad_mask) {
      return refuse_cast(self, plan, Refusal::BadTarget,
                         hostile ? " (it does not harm enemies)"
                                 : " (it does not help allies)");
    }
    if (ti.surface_distance_cm >
        self.combat_->reach_cm(melee) +
            self.combat_->data().activation_leeway_cm()) {
      return refuse_cast(self, plan, Refusal::OutOfRange);
    }
    return Refusal::None;
  }

  static bool has_pet_out_or_pending_bolt(const AbilityPipeline &self,
                                          const AbilityDef *def,
                                          bool &pet_out) {
    pet_out = false;
    for (const pets::Definition &d : pets::kDefinitions)
      pet_out = pet_out || self.stacks_of(d.effect) > 0;
    for (const Cast &other : self.casts_) {
      if (other.def == def && other.cost_due)
        return true;
    }
    return false;
  }

  static std::optional<Refusal>
  try_handle_dimensional_bolt(AbilityPipeline &self, const TriggerPlan &plan) {
    if (plan.ability != pets::kBolt)
      return std::nullopt;
    const uint64_t aim =
        plan.target_actors.empty() ? 0 : plan.target_actors.front();
    const bool marked =
        aim != 0 && self.combat_ &&
        self.combat_->effect_remaining(aim, pets::kBoltMark, plan.t) > 0;
    bool pet_out = false;
    const bool pending = has_pet_out_or_pending_bolt(self, plan.def, pet_out);
    if (const auto g = self.groups_.find(group_key(plan.def->row));
        g != self.groups_.end() && g->second.timer_end >= 0)
      return refuse_cast(self, plan, Refusal::Charges);
    if (marked && pet_out) {
      self.land_bolt_recall(plan.client_id, aim, plan.t);
      return Refusal::None;
    }
    if (pending)
      return refuse_cast(self, plan, Refusal::Busy);
    if (marked || self.stacks_of(pets::kBoltLockout) > 0 ||
        self.stacks_of(pets::kBoltLockout2) > 0)
      return refuse_cast(self, plan, Refusal::Busy, " (its lockout)");
    return std::nullopt;
  }

  static Refusal validate_trigger_targets(AbilityPipeline &self,
                                          const TriggerInfo &info,
                                          TriggerPlan &plan) {
    if (!self.combat_)
      return Refusal::None;
    const bool melee = is_melee_ability(plan.def->row);
    for (const uint64_t id : info.target_actors) {
      if (plan.target_actors.size() >= 16)
        break;
      if (const Refusal r = validate_single_target(self, plan, melee, id);
          r != Refusal::None)
        return r;
      plan.target_actors.push_back(id);
    }
    return Refusal::None;
  }

  static void resolve_chain_owner(const AbilityPipeline &self, Cast &cast,
                                  const AbilityDef &def) {
    std::vector<const content::AbilityHitRow *> rows;
    std::set<int64_t> distinct;
    for (const content::AbilityHitRow &h : def.hits) {
      if (h.source != "projectile")
        continue;
      rows.push_back(&h);
      distinct.insert(h.hit_guid);
    }
    if (rows.size() > 1 && distinct.size() == 1 && cast.max_charges > 1) {
      const auto g = self.groups_.find(cast.group);
      const int left =
          g != self.groups_.end() ? g->second.charges : cast.max_charges;
      const int used = std::clamp(cast.max_charges - left, 0,
                                  static_cast<int>(rows.size()) - 1);
      cast.chain_owner = rows[static_cast<size_t>(used)]->owner_guid;
    }
  }

  static void init_cast_from_plan(const AbilityPipeline &self,
                                  const TriggerInfo &info,
                                  const TriggerPlan &plan, Cast &cast) {
    const content::AbilityRow &row = plan.def->row;
    cast.client_id = plan.client_id;
    cast.def = plan.def;
    cast.trigger_at = plan.t;
    cast.animation_index = info.animation_index;
    cast.item_cast = info.item_cast;
    cast.target = info.target_count > 0 && plan.target_actors.empty();
    cast.self_reachable = (row.applicable_targets & kFriendlyMask) != 0;
    cast.target_actors = plan.target_actors;
    cast.loop = plan.loop;
    const bool loop_on = plan.loop && self.stacks_of(plan.loop_effect) > 0;
    const bool toggle_on = plan.is_toggle && !plan.own_effects.empty() &&
                           self.stacks_of(plan.own_effects[0]->row.guid) > 0;
    cast.toggle_off = loop_on || toggle_on;
    if (!cast.toggle_off)
      cast.own_effects = plan.own_effects;
    cast.group = group_key(row);
  }

  static bool has_group_charges(const AbilityPipeline &self,
                                const content::AbilityRow &row,
                                const Cast &cast) {
    const auto g = self.groups_.find(cast.group);
    if (g == self.groups_.end())
      return true;
    return g->second.charges >= cast.charge_cost &&
           !(row.disable_during_cooldown && g->second.timer_end >= 0);
  }

  static Refusal validate_cast_costs(AbilityPipeline &self,
                                     const TriggerPlan &plan,
                                     const Cast &cast) {
    if (!has_group_charges(self, plan.def->row, cast))
      return refuse_cast(self, plan, Refusal::Charges);
    for (const auto &[stat, amount] : cast.stat_costs) {
      if (self.stat_now(stat) < amount) {
        return refuse_cast(self, plan, Refusal::Cost,
                           " (" + std::to_string(amount) + " of stat " +
                               std::to_string(stat) + " needed)");
      }
    }
    for (const auto &[effect, n] : cast.effect_costs) {
      if (self.stacks_of(effect) < n) {
        return refuse_cast(self, plan, Refusal::Cost,
                           " (" + std::to_string(n) + " stacks of effect " +
                               std::to_string(effect) + " needed)");
      }
    }
    return Refusal::None;
  }

  static Refusal build_and_validate_cast(AbilityPipeline &self,
                                         const TriggerInfo &info,
                                         TriggerPlan &plan, Cast &cast) {
    init_cast_from_plan(self, info, plan, cast);
    self.drop_evaluation();
    std::string why;
    auto broken = [&](const char *what) {
      self.no_formula_.insert(plan.ability);
      return refuse_cast(self, plan, Refusal::NoFormula,
                         " (" + std::string(what) + ": " + why + ")");
    };
    bool ok = true;
    if (!self.eval_requirement(plan.def->row.requirement_expr,
                               plan.target_actors, ok, why))
      return broken("its activation requirement");
    if (!ok)
      return refuse_cast(self, plan, Refusal::Requirement);
    if (const char *what = self.resolve_formulas(cast, *plan.def, why))
      return broken(what);
    if (cast.toggle_off) {
      cast.stat_costs.clear();
      cast.effect_costs.clear();
      cast.triggers_cooldown = false;
    }
    resolve_chain_owner(self, cast, *plan.def);
    return validate_cast_costs(self, plan, cast);
  }

  static void apply_timing_row(const AbilityTiming &it, TriggerPlan &plan) {
    if (it.item_ms >= 0)
      plan.item_delay = it.item_ms / 1000.0;
    if (it.cost_ms >= 0)
      plan.cost_delay = it.cost_ms / 1000.0;
    else if (it.item_ms >= 0)
      plan.cost_delay = plan.item_delay;
    if (it.zone_ms >= 0)
      plan.zone_delay = it.zone_ms / 1000.0;
    if (it.zone_close_ms >= 0)
      plan.zone_end = it.zone_close_ms / 1000.0 - kZoneCloseDelay;
    if (it.hit_ms >= 0)
      plan.table_hit = it.hit_ms / 1000.0;
    if (it.end_ms >= 0)
      plan.table_end = it.end_ms / 1000.0;
  }

  static void apply_special_trigger_timings(const Cast &cast,
                                            TriggerPlan &plan) {
    if (plan.item_cast) {
      plan.item_delay = plan.cost_delay = kItemCastDelay;
      for (const content::MontageRow &m : plan.def->montages) {
        if (m.anim_index == cast.animation_index)
          plan.table_end = consumable_montage_end(m.soft_path);
      }
    }
    if (pets::for_ability(plan.ability)) {
      plan.cost_delay = pets::kSummonSeconds;
      plan.hit_delay = 0;
      if (plan.table_end < 0)
        plan.table_end = pets::kSummonSeconds + 0.38;
    }
  }

  static void load_trigger_timings(const AbilityPipeline &self,
                                   TriggerPlan &plan, Cast &cast) {
    const int aimed = static_cast<int>(!plan.target_actors.empty());
    if (self.data_timings_) {
      if (const AbilityTiming *it = find_timing(plan.ability, aimed))
        apply_timing_row(*it, plan);
    }
    for (const Timing &tm : kTimings) {
      if (plan.def->row.name != tm.ability)
        continue;
      plan.item_delay = tm.item;
      plan.cost_delay = tm.cost;
      plan.hit_delay = tm.hit;
    }
    apply_special_trigger_timings(cast, plan);
    for (const content::MontageRow &m : plan.def->montages) {
      if (!m.soft_path.empty())
        plan.has_montage = true;
    }
    if (plan.has_montage)
      cast.item_due = plan.t + plan.item_delay;
    cast.cost_due = plan.t + plan.cost_delay;
  }

  static void apply_branched_spec(const AbilityPipeline &self,
                                  const TriggerPlan &plan,
                                  const BranchedCast &b, Cast &cast) {
    cast.branched = &b;
    cast.montage_branch = true;
    cast.branch_position = b.position;
    cast.item_due = plan.t + b.item;
    cast.held = b.held_skill != 0 && self.holds_skill(b.held_skill);
    if (cast.held)
      spdlog::info("[abilities] {}: the character's talent makes it a held "
                   "cast: it waits for the key's release or the full charge",
                   plan.def->row.name);
    cast.branch_due = cast.held ? plan.t + b.hold_limit + b.release
                                : plan.t + b.request + b.answer;
    if (!b.cost_first)
      cast.cost_due = *cast.branch_due + b.cost;
    cast.end_due = (b.cost_first ? *cast.branch_due : *cast.cost_due) + b.end;
  }

  static void bind_gathered_cast(const AbilityDef &def, Cast &cast) {
    for (const GatheredCast &g : kGatheredCasts) {
      if (def.row.name != g.ability)
        continue;
      cast.gathered_projectile = projectile_of(def, g.projectile);
      if (g.charged_projectile != 0)
        cast.charged_projectile = projectile_of(def, g.charged_projectile);
      if (cast.gathered_projectile != 0)
        cast.gathered = &g;
    }
  }

  static void schedule_branched_and_gathered(const AbilityPipeline &self,
                                             const TriggerPlan &plan,
                                             Cast &cast) {
    if (!plan.has_montage || plan.loop)
      return;
    const content::AbilityRow &row = plan.def->row;
    for (const BranchedCast &b : kBranchedCasts) {
      if (row.name != b.ability)
        continue;
      if (b.animation >= 0 && b.animation != cast.animation_index)
        continue;
      apply_branched_spec(self, plan, b, cast);
      break;
    }
    if (cast.branched)
      bind_gathered_cast(*plan.def, cast);
  }

  static void apply_table_end(const TriggerPlan &plan, Cast &cast) {
    if (plan.table_end < 0 || cast.branched || cast.looped || cast.end_due)
      return;
    if (!cast.own_effects.empty() || is_weapon_combo(plan.def->row))
      return;
    cast.end_due = std::max(plan.t + plan.table_end, *cast.cost_due + 0.05);
    cast.clears_montage = true;
  }

  static void schedule_timed_and_looped(AbilityPipeline &self,
                                        const TriggerPlan &plan, Cast &cast) {
    if (!plan.has_montage || plan.loop)
      return;
    const content::AbilityRow &row = plan.def->row;
    for (const TimedCast &tc : kTimedCasts) {
      if (row.name == tc.ability && !cast.toggle_off)
        self.start_timed(cast, tc, plan.t);
    }
    for (const LoopedCast &l : kLoopedCasts) {
      if (row.name != l.ability)
        continue;
      cast.looped = &l;
      cast.montage_branch = true;
      cast.branch_position = l.position;
      cast.branch_predicted = false;
      cast.loop_scale = 1.0 / self.client_rate(false);
      cast.item_due = plan.t + l.item;
      cast.cost_due = plan.t + l.first_shot * cast.loop_scale;
    }
    apply_table_end(plan, cast);
  }

  static void add_branched_missiles(const AbilityDef &def, Cast &cast) {
    for (const content::AbilityHitRow &h : def.hits) {
      if (h.source != "projectile")
        continue;
      for (const MissileLanding &m : kMissileLandings) {
        if (def.row.name == m.ability && h.key == m.key)
          cast.missiles.push_back(
              {*cast.cost_due + m.after_cost, h.owner_guid});
      }
    }
    if (!cast.missiles.empty())
      cast.projectile_due.reset();
  }

  static bool matches_flight_row(std::string_view ability_name, int anim_index,
                                 int64_t key) {
    for (const Flight &f : kFlights) {
      if (ability_name == f.ability &&
          (f.animation < 0 || f.animation == anim_index) && f.key == key)
        return true;
    }
    return false;
  }

  static void resolve_branched_flight_owner(const AbilityPipeline &self,
                                            const AbilityDef &def, Cast &cast) {
    if (!cast.branched || !cast.projectile_due || cast.chain_owner != 0)
      return;
    for (const content::AbilityHitRow &h : def.hits) {
      if (h.source != "projectile")
        continue;
      const content::ProjectileRow *p =
          self.catalog_->projectile(h.owner_guid);
      if (!p || !(p->speed > 0))
        continue;
      if (matches_flight_row(def.row.name, cast.animation_index, h.key)) {
        cast.chain_owner = h.owner_guid;
        return;
      }
    }
  }

  static void try_plan_shot(AbilityPipeline &self, const TriggerPlan &plan,
                            Cast &cast, bool swing) {
    const content::AbilityRow &row = plan.def->row;
    const ShotCast *shot = nullptr;
    for (const ShotCast &sc : kShotCasts) {
      if (row.name != sc.ability)
        continue;
      if (!shot && !swing)
        shot = &sc;
      if (sc.animation < 0 || sc.animation == cast.animation_index) {
        shot = &sc;
        break;
      }
    }
    if (shot)
      self.plan_shot(cast, *shot, plan.t);
  }

  static bool is_plain_projectile_cast(const Cast &cast,
                                       const content::AbilityRow &row,
                                       bool aimed, bool swing) {
    if (!cast.projectile_due || cast.branched || cast.looped || cast.gathered ||
        cast.timed || cast.toggle_off)
      return false;
    return !swing || (aimed && swing_flies(row.name, cast.animation_index));
  }

  static bool has_custom_projectile_timing(const Cast &cast) {
    return cast.branched || cast.looped || cast.gathered ||
           is_weapon_combo(cast.def->row);
  }

  static void schedule_cast_projectiles(AbilityPipeline &self,
                                        const TriggerPlan &plan, Cast &cast,
                                        bool aimed) {
    cast.projectile_due = *cast.cost_due + kProjectileDelay;
    if (plan.table_hit >= 0 && !has_custom_projectile_timing(cast))
      cast.projectile_due = std::max(*cast.cost_due, plan.t + plan.table_hit);
    if (cast.branched)
      add_branched_missiles(*plan.def, cast);
    if (cast.looped) {
      cast.shot_projectile = first_projectile_guid(*plan.def);
      cast.projectile_due.reset();
    }
    if (cast.gathered || cast.timed || cast.toggle_off)
      cast.projectile_due.reset();
    if (aimed)
      resolve_branched_flight_owner(self, *plan.def, cast);
    const content::AbilityRow &row = plan.def->row;
    const bool at_caster = !aimed && cast.target && cast.self_reachable;
    const bool swing = is_weapon_combo(row);
    const bool plain = is_plain_projectile_cast(cast, row, aimed, swing);
    if ((aimed || at_caster) && plain)
      try_plan_shot(self, plan, cast, swing);
    if (aimed && plain && !cast.shot)
      self.plan_notes(cast, plan.t);
  }

  static void classify_cast_hit_row(const AbilityPipeline &self,
                                    const Cast &cast,
                                    const content::AbilityHitRow &h,
                                    bool &direct, bool &projectile) {
    const HitDef *hit = self.catalog_->hit(h.hit_guid);
    if (!hit)
      return;
    const bool aimed = !cast.target_actors.empty();
    const bool others =
        aimed && (hit->row.applicable_targets & (2 | 4 | 8 | 16)) != 0;
    if (!self.hits_caster(hit->row) && !others)
      return;
    if (h.source == "hit" &&
        hit->row.net_type.find("ServerVerify") == std::string::npos)
      direct = true;
    if (h.source == "projectile" &&
        (aimed || (cast.target && cast.self_reachable)))
      projectile = true;
  }

  static double effective_hit_delay(const TriggerPlan &plan) {
    if (plan.hit_delay >= 0)
      return plan.hit_delay;
    return plan.has_montage ? kHitDelay : 0.0;
  }

  static void schedule_cast_hits(AbilityPipeline &self, const TriggerPlan &plan,
                                 Cast &cast) {
    if (plan.loop)
      return;
    bool direct = false;
    bool projectile = false;
    for (const content::AbilityHitRow &h : plan.def->hits) {
      classify_cast_hit_row(self, cast, h, direct, projectile);
    }
    if (direct)
      cast.hit_due = *cast.cost_due + effective_hit_delay(plan);
    if (projectile)
      schedule_cast_projectiles(self, plan, cast, !cast.target_actors.empty());
  }

  static void append_verified_hit_slot(AbilityPipeline &self,
                                       const AbilityDef &def,
                                       const content::AbilityHitRow &h,
                                       std::set<int64_t> &seen,
                                       CastRecord &rec) {
    if (h.source != "hit" || !seen.insert(h.hit_guid).second)
      return;
    const HitDef *hit = self.catalog_->hit(h.hit_guid);
    if (!hit || hit->row.net_type.find("ServerVerify") == std::string::npos)
      return;
    HitSlot slot;
    slot.hit = hit;
    slot.targeting = self.targeting_of(def, static_cast<int>(h.key));
    slot.reaches = self.hits_caster(hit->row);
    slot.aims_others = self.combat_ != nullptr &&
                       (hit->row.applicable_targets & (2 | 4 | 8 | 16)) != 0;
    if (pets::for_ability(def.row.guid) && hit->row.applicable_targets == 1 &&
        hit->row.fallback_target.find("SelfOnly") != std::string::npos) {
      slot.server_self_use = true;
    }
    rec.slots.push_back(std::move(slot));
  }

  static void record_cast_slots(AbilityPipeline &self, const TriggerPlan &plan,
                                const Cast &cast) {
    const content::AbilityRow &row = plan.def->row;
    CastRecord rec;
    rec.client_id = plan.client_id;
    rec.ability = row.guid;
    rec.trigger_at = plan.t;
    rec.cost_due = *cast.cost_due;
    rec.window_end =
        rec.cost_due + (plan.item_cast ? kItemRequestWindow : kRequestWindow);
    rec.item_cast = plan.item_cast;
    rec.hit_delay = effective_hit_delay(plan);
    rec.reach = self.reach_of(*plan.def);
    for (const content::MontageRow &m : plan.def->montages) {
      if (is_weapon_combo(row) && m.anim_index == cast.animation_index &&
          step_token(leaf_of_path(m.soft_path)).beam) {
        rec.max_requests = kBeamRequestsPerHit;
        rec.min_gap = kBeamRequestGap;
      }
    }
    std::set<int64_t> seen;
    for (const content::AbilityHitRow &h : plan.def->hits) {
      append_verified_hit_slot(self, *plan.def, h, seen, rec);
    }
    self.records_.push_back(std::move(rec));
    while (self.records_.size() > kMaxRecords)
      self.records_.pop_front();
  }

  static void record_trigger_movement(AbilityPipeline &self,
                                      const TriggerPlan &plan,
                                      int max_charges) {
    const content::AbilityRow &row = plan.def->row;
    double speed = row.networking_max_speed;
    for (const Dash &d : kDashes) {
      if (row.name == d.ability)
        speed = std::max(speed, d.speed);
    }
    self.last_max_charges_[plan.ability] = max_charges;
    if (speed > 0)
      self.movement_.emplace_back(plan.t + kMovementWindow,
                                  speed * kMovementMargin);
    self.movement_.erase(
        std::remove_if(self.movement_.begin(), self.movement_.end(),
                       [&](const auto &m) { return m.first < plan.t; }),
        self.movement_.end());
  }

  static void finalize_triggered_cast(AbilityPipeline &self,
                                      const TriggerPlan &plan, Cast &cast) {
    record_trigger_movement(self, plan, cast.max_charges);
    ++self.casts_started_;
    if (plan.loop && !cast.toggle_off)
      cast.loop_due = plan.t + kLoopDelay;
    if (!cast.own_effects.empty())
      cast.own_due = *cast.cost_due + kHitDelay;
    record_cast_slots(self, plan, cast);
    const double base_delay =
        plan.has_montage ? plan.item_delay : plan.cost_delay;
    const double zone_open =
        plan.t +
        (plan.zone_delay >= 0 ? plan.zone_delay : base_delay + kZoneOpenDelay);
    self.spawn_zones(cast, zone_open,
                     plan.zone_end >= 0 ? plan.t + plan.zone_end : -1.0);
    if (!is_weapon_combo(plan.def->row))
      self.cancel_swings(plan.t, plan.name);
    self.casts_.push_back(std::move(cast));
  }
};

Refusal AbilityPipeline::trigger(int64_t ability, int32_t client_id, double now,
                                 const TriggerInfo &info) {
  advance(now);
  if (std::find(recent_ids_.begin(), recent_ids_.end(), client_id) !=
      recent_ids_.end())
    return Refusal::Duplicate;
  recent_ids_.push_back(client_id);
  if (recent_ids_.size() > kRecentIds)
    recent_ids_.pop_front();

  Host::Casts::TriggerPlan plan;
  plan.ability = ability;
  plan.client_id = client_id;
  plan.t = now_;
  plan.def = catalog_ ? catalog_->ability(ability) : nullptr;
  plan.name = plan.def ? plan.def->row.name : std::to_string(ability);
  plan.item_cast = info.item_cast;

  if (const Refusal r = Host::Casts::validate_trigger_shape(*this, info, plan);
      r != Refusal::None)
    return r;
  if (const Refusal r =
          Host::Casts::validate_trigger_targets(*this, info, plan);
      r != Refusal::None)
    return r;
  if (const std::optional<Refusal> r =
          Host::Casts::try_handle_dimensional_bolt(*this, plan))
    return *r;

  Cast cast;
  if (const Refusal r =
          Host::Casts::build_and_validate_cast(*this, info, plan, cast);
      r != Refusal::None)
    return r;

  Host::Casts::load_trigger_timings(*this, plan, cast);
  Host::Casts::schedule_branched_and_gathered(*this, plan, cast);
  Host::Casts::schedule_timed_and_looped(*this, plan, cast);
  Host::Casts::schedule_cast_hits(*this, plan, cast);
  Host::Casts::finalize_triggered_cast(*this, plan, cast);
  return Refusal::None;
}

void AbilityPipeline::step_item(Cast &cast, double t) {
  const AbilityDef &def = *cast.def;
  AbilityChange c;
  if (item_instance_) {
    c.abilities_removed.push_back(*item_instance_);
    item_instance_.reset();
  }
  bootstrap::ActiveAbilityItem item;
  item.ability_guid = def.row.guid;
  item.server_instance_id = cast.instance = a7_.allocate_instance_id();
  instances_.push_back(
      {cast.instance, cast.client_id, std::nullopt, def.row.guid});
  while (instances_.size() > 32)
    instances_.pop_front();
  item.targets_self = cast.target;
  if (combat_) {
    for (const uint64_t id : cast.target_actors) {
      item.target_objects.push_back(combat_->hit_component_of(id));
      item.target_actors.push_back(combat_->guid_of(id));
    }
  }
  for (const content::MontageRow &m : def.montages) {
    if (m.anim_index != cast.animation_index)
      continue;
    item.montage_path = m.soft_path;
    item.montage_guid = m.net_guid ? *m.net_guid : 0;
    break;
  }
  item.montage_group = montage_group_of(def);
  item.play_rate = static_cast<float>(item_rate(def));
  item.animation_counter = ++animation_counter_;
  cast.item = item;
  c.abilities_added.push_back(std::move(item));
  item_instance_ = cast.instance;
  last_item_at_ = t;
  emit({std::move(c), std::nullopt, t, "the item of " + def.row.name});
}

void AbilityPipeline::cancel_swings(double t, const std::string &by) {
  for (size_t i = 0; i < casts_.size();) {
    const Cast &p = casts_[i];
    if (!is_weapon_combo(p.def->row)) {
      ++i;
      continue;
    }
    if (!Host::has_pending_slot_requests(record_of(p.client_id)))
      drop_record(p.client_id);
    if (p.item_due) {
      AbilityEvent e;
      e.at = t;
      e.end_ability = p.client_id;
      e.note = "the swing of " + p.def->row.name + " is ended: " + by +
               " came before its item";
      events_.push_back(std::move(e));
    }
    casts_.erase(casts_.begin() + static_cast<std::ptrdiff_t>(i));
  }
}

void AbilityPipeline::step_branch(Cast &cast, double t) {
  if (cast.gathered && (!cast.cost_due || can_pay(cast)))
    launch_gathered(cast, t);
  land_by_flight(cast, t);
  if (!cast.montage_branch || !item_instance_ ||
      *item_instance_ != cast.instance)
    return;
  if (cast.charged && cast.gathered &&
      cast.gathered->charged_montage[0] != '\0') {
    cast.item.montage_path = cast.gathered->charged_montage;
    cast.item.montage_guid = cast.gathered->charged_montage_guid;
  }
  cast.item.montage_position = cast.branch_position;
  cast.item.may_jump_to_position = true;
  cast.item.branch_predicted = cast.branch_predicted;
  cast.item.animation_counter = ++animation_counter_;
  AbilityChange c;
  c.abilities_changed.push_back(cast.item);
  emit({std::move(c), std::nullopt, t,
        "the montage of " + cast.def->row.name +
            " moves to the end of its cast"});
}

void AbilityPipeline::step_end(Cast &cast, double t) {
  AbilityChange c;
  if (!cast.own_effects.empty()) {
    end_loop(c, cast.def->row.guid, t);
    recompute(c);
  }
  if ((cast.montage_branch || cast.clears_montage) && item_instance_ &&
      *item_instance_ == cast.instance) {
    cast.item.montage_path.clear();
    cast.item.montage_guid = 0;
    cast.item.branch_predicted = cast.branch_predicted;
    cast.item.animation_counter = ++animation_counter_;
    c.abilities_changed.push_back(cast.item);
  }
  if (c.empty())
    return;
  emit({std::move(c), std::nullopt, t,
        "the montage of " + cast.def->row.name + " ends"});
}

} // namespace aoc::worldd
