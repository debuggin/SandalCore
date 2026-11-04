#include "session/ability_pipeline_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd {

using abilities::AbilityDef;
using abilities::EffectDef;
using abilities::HitDef;
using bootstrap::AbilityChange;

struct AbilityPipeline::Host::Hits {
  static bool step_due_missile(AbilityPipeline &self, Cast &cast,
                               size_t cast_index, double t) {
    if (cast.missiles.empty())
      return false;
    size_t first = 0;
    for (size_t k = 1; k < cast.missiles.size(); ++k) {
      if (cast.missiles[k].due < cast.missiles[first].due)
        first = k;
    }
    if (cast.missiles[first].due > t)
      return false;
    if (cast.missiles[first].kind != Cast::Missile::Kind::Land) {
      const Cast::Missile step = cast.missiles[first];
      cast.missiles.erase(cast.missiles.begin() +
                          static_cast<std::ptrdiff_t>(first));
      self.step_timed(cast, step, t);
      self.finish_cast_if_done(cast_index);
      return true;
    }
    const int64_t projectile = cast.missiles[first].projectile;
    const uint64_t target = cast.missiles[first].target;
    cast.missiles.erase(cast.missiles.begin() +
                        static_cast<std::ptrdiff_t>(first));
    const int64_t kept = cast.chain_owner;
    cast.chain_owner = projectile;
    std::vector<uint64_t> kept_targets;
    if (target != 0) {
      kept_targets = cast.target_actors;
      cast.target_actors = {target};
    }
    self.step_hits(cast, t, "projectile");
    if (target != 0)
      cast.target_actors = std::move(kept_targets);
    cast.chain_owner = kept;
    self.finish_cast_if_done(cast_index);
    return true;
  }

  static bool pop_due(std::optional<double> &due, double t) {
    if (!due || *due > t)
      return false;
    due.reset();
    return true;
  }

  static bool finish_step(AbilityPipeline &self, size_t i) {
    self.finish_cast_if_done(i);
    return true;
  }

  static bool step_due_cast(AbilityPipeline &self, size_t i, double t) {
    Cast &cast = self.casts_[i];
    if (pop_due(cast.item_due, t)) {
      self.step_item(cast, t);
      return finish_step(self, i);
    }
    if (pop_due(cast.cost_due, t)) {
      self.step_cost(cast, t);
      return finish_step(self, i);
    }
    if (pop_due(cast.hit_due, t)) {
      self.step_hits(cast, t, "hit");
      return finish_step(self, i);
    }
    if (pop_due(cast.projectile_due, t)) {
      if (cast.flight_pending)
        self.launch_shot(cast, t);
      else
        self.step_hits(cast, t, "projectile");
      return finish_step(self, i);
    }
    return step_due_cast_secondary(self, cast, i, t);
  }

  static bool step_due_cast_secondary(AbilityPipeline &self, Cast &cast,
                                      size_t i, double t) {
    if (pop_due(cast.branch_due, t)) {
      self.step_branch(cast, t);
      return finish_step(self, i);
    }
    if (step_due_missile(self, cast, i, t))
      return true;
    if (pop_due(cast.end_due, t)) {
      self.step_end(cast, t);
      return finish_step(self, i);
    }
    if (pop_due(cast.loop_due, t)) {
      self.step_loop(cast, t);
      return finish_step(self, i);
    }
    if (pop_due(cast.own_due, t)) {
      self.step_own(cast, t);
      return finish_step(self, i);
    }
    return false;
  }

  static bool try_run_record_slot(AbilityPipeline &self, CastRecord &rec,
                                  double t) {
    for (HitSlot &s : rec.slots) {
      for (size_t i = 0; i < s.due.size(); ++i) {
        if (s.due[i].at > t)
          continue;
        const Pending p = s.due[i];
        s.due.erase(s.due.begin() + static_cast<std::ptrdiff_t>(i));
        self.run_slot(rec, s, t, true, p.found, p.targets);
        return true;
      }
    }
    return false;
  }

  static bool is_due(const std::optional<double> &due, double t) {
    return due && *due <= t;
  }

  static bool run_due_lifecycles(AbilityPipeline &self, double t) {
    for (size_t i = 0; i < self.held_.size(); ++i) {
      if (is_due(self.held_[i].end_due, t)) {
        self.end_held(i, t);
        return true;
      }
    }
    for (size_t i = 0; i < self.instances_.size(); ++i) {
      if (is_due(self.instances_[i].end_due, t)) {
        self.end_instance(i, t);
        return true;
      }
    }
    for (CastRecord &rec : self.records_) {
      if (try_run_record_slot(self, rec, t))
        return true;
    }
    return false;
  }

  static bool run_due_effects_and_groups(AbilityPipeline &self, double t) {
    for (size_t i = 0; i < self.effects_.size(); ++i) {
      if (self.effects_[i].end <= t) {
        self.effect_expires(i, t);
        return true;
      }
    }
    for (size_t i = 0; i < self.effects_.size(); ++i) {
      if (self.effects_[i].next_tick <= t) {
        self.effect_ticks(i, t);
        return true;
      }
    }
    for (auto &[key, g] : self.groups_) {
      if (g.timer_end >= 0 && g.timer_end <= t) {
        const std::string k = key;
        self.group_timer_ends(k, t);
        return true;
      }
    }
    return false;
  }

  static HitVerdict unslotted_hit_verdict(CastRecord &rec,
                                          int64_t targeting_guid,
                                          int64_t hit_guid, double now) {
    if (targeting_guid == content::kTargetingDefaultSelf)
      return HitVerdict::Quiet;
    const auto [it, fresh] =
        rec.asked.try_emplace(hit_guid, std::make_pair(0, -1e9));
    (void)fresh;
    if (it->second.first >= rec.max_requests ||
        now - it->second.second < rec.min_gap)
      return HitVerdict::TooMany;
    ++it->second.first;
    it->second.second = now;
    return HitVerdict::Unsupported;
  }

  struct CostGroupCtx {
    AbilityPipeline &self;
    Cast &cast;
    const AbilityDef &def;
    std::string &why;
    int ord = 0;
    bool enabled = true;
  };

  static const char *resolve_stat_costs_in_group(CostGroupCtx &ctx, size_t &j) {
    for (; j < ctx.def.costs.size() && ctx.def.costs[j].cost_ord == ctx.ord;
         ++j) {
      const content::CostRow &cr = ctx.def.costs[j];
      if (!ctx.enabled || cr.stat_ord < 0 || cr.stat_guid == 0)
        continue;
      double amount = 0;
      if (cr.amount_expr != 0 &&
          !ctx.self.eval(cr.amount_expr, amount, ctx.why))
        return "a cost's amount";
      if (amount > 0)
        ctx.cast.stat_costs.emplace_back(cr.stat_guid, amount);
    }
    return nullptr;
  }

  static const char *resolve_effect_costs_in_group(CostGroupCtx &ctx) {
    if (!ctx.enabled)
      return nullptr;
    for (const content::EffectCostRow &ec : ctx.def.effect_costs) {
      if (ec.cost_ord != ctx.ord || ec.effect_guid == 0)
        continue;
      double n = 0;
      if (ec.num_expr != 0 && !ctx.self.eval(ec.num_expr, n, ctx.why))
        return "an effect cost's count";
      if (n >= 1)
        ctx.cast.effect_costs.emplace_back(
            ec.effect_guid, static_cast<int>(std::min(n, 4096.0)));
    }
    return nullptr;
  }

  static const char *resolve_cost_group(AbilityPipeline &self, Cast &cast,
                                        const AbilityDef &def, size_t &i,
                                        std::string &why) {
    const content::CostRow &first = def.costs[i];
    CostGroupCtx ctx{self, cast, def, why, first.cost_ord, true};
    if (!self.eval_bool(first.enabled_expr, true, ctx.enabled, why))
      return "a cost's enabled flag";
    int charge = 1;
    if (first.charge_cost_expr != 0) {
      double v = 1;
      if (!self.eval(first.charge_cost_expr, v, why))
        return "a cost's charge count";
      charge = static_cast<int>(std::clamp(std::round(v), 0.0, 99.0));
    }
    size_t j = i;
    if (const char *err = resolve_stat_costs_in_group(ctx, j))
      return err;
    if (ctx.enabled && first.triggers_cooldown) {
      cast.triggers_cooldown = true;
      cast.charge_cost = std::max(cast.charge_cost, charge);
    }
    if (const char *err = resolve_effect_costs_in_group(ctx))
      return err;
    i = j;
    return nullptr;
  }

  static CastRecord *find_active_cast_record(AbilityPipeline &self,
                                             int64_t ability, double now) {
    for (auto it = self.records_.rbegin(); it != self.records_.rend(); ++it) {
      if (it->ability == ability && now <= it->window_end)
        return &*it;
    }
    return nullptr;
  }

  static void maybe_anchor_cast_shot(AbilityPipeline &self,
                                     const CastRecord &rec, int64_t hit_guid) {
    Cast *c = self.cast_of(rec.client_id);
    if (!c || !c->shot || c->anchored || c->shot->ask[0] == '\0')
      return;
    const HitDef *h = self.catalog_->hit(hit_guid);
    if (h && h->row.name == c->shot->ask)
      self.anchor_shot(*c, self.now_);
  }

  static HitSlot *find_record_slot(CastRecord &rec, int64_t hit_guid) {
    for (HitSlot &s : rec.slots) {
      if (s.hit->row.guid == hit_guid)
        return &s;
    }
    return nullptr;
  }

  static std::vector<uint64_t>
  collect_unique_other_targets(const std::vector<uint64_t> &targets) {
    std::vector<uint64_t> others;
    for (const uint64_t id : targets) {
      if (others.size() >= 40)
        break;
      if (std::find(others.begin(), others.end(), id) == others.end())
        others.push_back(id);
    }
    return others;
  }

  static void push_no_target_slot_confirm(AbilityPipeline &self,
                                          const HitDef &hit,
                                          const HitSource &src, bool response) {
    if (hit.mods.empty() && !hit.row.has_fx &&
        (hit.row.applicable_targets & 8) == 0)
      return;
    bootstrap::HitConfirm h;
    h.record_guid = hit.row.guid;
    h.source_type = src.type;
    h.source_guid = src.guid;
    h.no_target = true;
    h.predicted_autonomous = true;
    h.response = response;
    self.confirmations_.push_back(std::move(h));
  }

  static void release_held_branched_casts(AbilityPipeline &self,
                                          int32_t client_id) {
    for (Cast &cast : self.casts_) {
      if (cast.client_id != client_id || !cast.held || cast.charged ||
          !cast.branched || !cast.branch_due)
        continue;
      const double at = self.now_ + cast.branched->release;
      if (at >= *cast.branch_due)
        continue;
      spdlog::info("[abilities] the key of {} went up {:.3f} s after the "
                   "trigger: the cast goes on",
                   cast.def->row.name, self.now_ - cast.trigger_at);
      self.reschedule_branch(cast, at, cast.branched->cost);
    }
  }

  static bool can_release_bar_cast(const Cast &cast, int32_t client_id) {
    return cast.client_id == client_id && cast.branched &&
           cast.branched->full > 0 && !cast.held && !cast.request_heard &&
           cast.branch_due.has_value();
  }

  static void release_bar_branched_cast(AbilityPipeline &self, Cast &cast,
                                        double held_seconds) {
    cast.request_heard = true;
    cast.heard_at = self.now_;
    const double fill = self.held_fill(cast, held_seconds);
    if (CastRecord *rec = self.record_of(cast.client_id))
      rec->fill = fill;
    const double made =
        std::max(self.now_, cast.trigger_at + cast.branched->item);
    const double earlier = (made + cast.branched->answer) - *cast.branch_due;
    spdlog::info("[abilities] the key of {} went up {:.3f} s after the "
                 "trigger: the bar ended at {:.0f}% of its charge{}",
                 cast.def->row.name, self.now_ - cast.trigger_at, fill * 100.0,
                 earlier < 0
                     ? ": the cast goes on " +
                           std::to_string(-earlier).substr(0, 5) + " s sooner"
                     : std::string(": the cast goes on as planned"));
    if (earlier < 0)
      self.bring_sooner(cast, earlier);
  }

  static bool try_charge_held_branch(AbilityPipeline &self, Cast &cast,
                                     int64_t branch_guid) {
    const BranchedCast &b = *cast.branched;
    if (!cast.held || cast.charged || b.charged_branch == 0 ||
        branch_guid != b.charged_branch)
      return false;
    cast.charged = true;
    spdlog::info("[abilities] the charge of {} was full {:.3f} s after the "
                 "trigger: the cast fires its charged shot",
                 cast.def->row.name, self.now_ - cast.trigger_at);
    self.reschedule_branch(cast, self.now_ + b.charged_answer, b.charged_cost);
    return true;
  }

  static bool complete_branch_request(AbilityPipeline &self, Cast &cast,
                                      int64_t branch_guid) {
    const BranchedCast &b = *cast.branched;
    if (b.branch != branch_guid) {
      if (!b.cancels)
        return false;
      spdlog::info(
          "[abilities] the client asked for the branch {} of {}, which the "
          "table does not list (it has {}): taken as the end of the "
          "cast bar",
          branch_guid, cast.def->row.name, b.branch);
    }
    if (cast.held)
      return true;
    cast.request_heard = true;
    cast.heard_at = self.now_;
    const double made =
        std::max(self.now_, cast.trigger_at + kEarliestBranchRequest);
    const double earlier = (made + cast.branched->answer) - *cast.branch_due;
    spdlog::info("[abilities] the client asked for the branch of {} {:.3f} s "
                 "after the trigger{}",
                 cast.def->row.name, self.now_ - cast.trigger_at,
                 earlier < 0
                     ? ": the cast goes on " +
                           std::to_string(-earlier).substr(0, 5) + " s sooner"
                     : std::string(": the cast goes on as planned"));
    if (b.full > 0 && self.now_ - cast.trigger_at >= 0.5 * b.full) {
      self.full_bars_[cast.def->row.guid] = std::max(
          self.full_bars_[cast.def->row.guid], self.now_ - cast.trigger_at);
    }
    if (earlier < 0)
      self.bring_sooner(cast, earlier);
    return true;
  }

  static bool is_cancellable_bar_cast(const Cast &c, int32_t server_id) {
    return c.instance == server_id && c.branched && c.branched->cancels &&
           !c.held && !c.request_heard && c.branch_due.has_value();
  }

  static void populate_inspected_stat_costs(const AbilityDef &def,
                                            const Cast &cast, Inspection &out) {
    for (const auto &[stat, amount] : cast.stat_costs) {
      std::string name;
      for (const content::CostRow &cr : def.costs) {
        if (cr.stat_guid == stat)
          name = cr.stat_name;
      }
      out.stat_costs.push_back({stat, name, amount});
    }
  }
};

void AbilityPipeline::run_due(double t) {
  clock_ = t;
  for (size_t i = 0; i < casts_.size(); ++i) {
    if (Host::Hits::step_due_cast(*this, i, t))
      return;
  }
  if (Host::Hits::run_due_lifecycles(*this, t) ||
      Host::Hits::run_due_effects_and_groups(*this, t))
    return;
  for (size_t i = 0; i < zones_.size(); ++i) {
    if (zone_due(zones_[i]) <= t)
      return run_zone(i, t);
  }
  if (next_regen_ <= t)
    regenerate(t);
}

void AbilityPipeline::finish_cast_if_done(size_t index) {
  const Cast &c = casts_[index];
  if (!c.item_due && !c.cost_due && !c.hit_due && !c.projectile_due &&
      !c.loop_due && !c.own_due && !c.branch_due && !c.end_due &&
      c.missiles.empty()) {
    casts_.erase(casts_.begin() + static_cast<std::ptrdiff_t>(index));
  }
}

std::string AbilityPipeline::montage_group_of(const AbilityDef &def) const {
  return std::string(group_of_name(def.row.name));
}

const char *AbilityPipeline::verdict_name(HitVerdict v) {
  static constexpr const char *kNames[] = {
      "scheduled",    "accepted quietly",         "found nobody",
      "no such cast", "not a hit of the ability", "too many requests",
      "not served"};
  const size_t idx = static_cast<size_t>(v);
  return idx < std::size(kNames) ? kNames[idx] : "?";
}

int64_t AbilityPipeline::targeting_of(const AbilityDef &def, int key) const {
  for (const content::TargetingRow &t : def.targetings) {
    if (t.key == key)
      return t.guid;
  }
  return content::kTargetingDefaultSelf;
}

std::set<int64_t> AbilityPipeline::reach_of(const AbilityDef &def) const {
  return Host::collect_ability_hit_graph(catalog_, def, nullptr);
}

AbilityPipeline::CastRecord *AbilityPipeline::record_of(int32_t client_id) {
  for (CastRecord &r : records_) {
    if (r.client_id == client_id)
      return &r;
  }
  return nullptr;
}

void AbilityPipeline::drop_record(int32_t client_id) {
  for (auto it = records_.begin(); it != records_.end(); ++it) {
    if (it->client_id == client_id) {
      records_.erase(it);
      return;
    }
  }
}

void AbilityPipeline::prune_records(double t) {
  for (auto it = records_.begin(); it != records_.end();) {
    if (!Host::has_pending_slot_requests(*it) && it->window_end < t) {
      it = records_.erase(it);
    } else {
      ++it;
    }
  }
}

double AbilityPipeline::slot_due(const CastRecord &rec,
                                 const HitSlot &s) const {
  double due = kInf;
  (void)rec;
  for (const Pending &p : s.due)
    due = std::min(due, p.at);
  return due;
}

AbilityPipeline::HitVerdict
AbilityPipeline::hit_request(int64_t ability, int64_t targeting_guid,
                             int64_t hit_guid, size_t components, size_t own,
                             double now, const std::vector<uint64_t> &targets) {
  advance(now);
  CastRecord *rec = Host::Hits::find_active_cast_record(*this, ability, now);
  if (!rec)
    return HitVerdict::NoCast;
  Host::Hits::maybe_anchor_cast_shot(*this, *rec, hit_guid);
  if (!rec->reach.count(hit_guid))
    return HitVerdict::NotItsHit;
  HitSlot *slot = Host::Hits::find_record_slot(*rec, hit_guid);
  if (!slot)
    return Host::Hits::unslotted_hit_verdict(*rec, targeting_guid, hit_guid,
                                             now);
  if (slot->requests >= rec->max_requests ||
      now - slot->last_request < rec->min_gap)
    return HitVerdict::TooMany;
  ++slot->requests;
  slot->last_request = now;
  const bool found = own > 0 && slot->reaches;
  (void)components;
  std::vector<uint64_t> others =
      slot->aims_others ? Host::Hits::collect_unique_other_targets(targets)
                        : std::vector<uint64_t>{};
  const double at =
      std::max(now + kConfirmDelay, record_reference(*rec) + rec->hit_delay);
  slot->due.push_back({at, found, others});
  return (found || !others.empty()) ? HitVerdict::Scheduled : HitVerdict::Empty;
}

void AbilityPipeline::run_slot(CastRecord &rec, HitSlot &slot, double t,
                               bool response, bool found,
                               const std::vector<uint64_t> &targets) {
  const FillScope fill(*this, rec.fill);
  AbilityChange c;
  const AbilityDef *def = catalog_->ability(rec.ability);
  const std::string ab_name = def ? def->row.name : std::string("?");
  HitSource src{
      content::kSourceTypeAbility, rec.ability, true, true, false, false};
  src.response = response;
  if (!targets.empty()) {
    apply_hit_on_targets(c, *slot.hit, targets, t, src, def);
    if (!found) {
      recompute(c);
      emit({std::move(c), std::nullopt, t,
            "the hit " + slot.hit->row.name + " of " + ab_name + " on " +
                std::to_string(targets.size()) +
                " other actor(s) (the client asked)"});
      return;
    }
  }
  if (!found) {
    Host::Hits::push_no_target_slot_confirm(*this, *slot.hit, src, response);
  } else {
    apply_hit(c, *slot.hit, t, true, src);
    fire_from_hit(rec, *slot.hit, t);
  }
  recompute(c);
  const char *who =
      response ? " (the client asked)" : " (the server's own: no request came)";
  emit({std::move(c), std::nullopt, t,
        "the hit " + slot.hit->row.name + " of " + ab_name + who +
            (found ? "" : ": it found nobody")});
}

bool AbilityPipeline::cancel(int32_t client_instance_id, double now,
                             double held_seconds) {
  advance(now);
  cancel_timed(client_instance_id, now);
  for (Held &h : held_) {
    if (h.client_id == client_instance_id && !h.end_due)
      h.end_due = now_ + kHeldEndDelay;
  }
  Host::Hits::release_held_branched_casts(*this, client_instance_id);
  for (Cast &cast : casts_) {
    if (!Host::Hits::can_release_bar_cast(cast, client_instance_id))
      continue;
    Host::Hits::release_bar_branched_cast(*this, cast, held_seconds);
    break;
  }
  return std::find(recent_ids_.begin(), recent_ids_.end(),
                   client_instance_id) != recent_ids_.end();
}

double AbilityPipeline::held_fill(const Cast &cast, double held_seconds) const {
  const double held = held_seconds > 0
                          ? held_seconds
                          : std::max(0.0, now_ - cast.trigger_at - kHoldStart);
  const auto shown = full_bars_.find(cast.def->row.guid);
  const double bar =
      shown != full_bars_.end() ? shown->second : cast.branched->full;
  return std::clamp(held / std::max(0.1, bar - kHoldStart), 0.0, 1.0);
}

void AbilityPipeline::bring_sooner(Cast &cast, double earlier) {
  const bool cost_first = cast.branched && cast.branched->cost_first;
  const auto move = [earlier](std::optional<double> &d) {
    if (d)
      *d += earlier;
  };
  move(cast.branch_due);
  if (!cost_first) {
    move(cast.cost_due);
    move(cast.hit_due);
    move(cast.own_due);
  }
  move(cast.end_due);
  move(cast.projectile_due);
  for (Cast::Missile &m : cast.missiles)
    m.due += earlier;
  if (CastRecord *rec = record_of(cast.client_id); rec && !cost_first) {
    rec->cost_due += earlier;
    rec->window_end += earlier;
  }
}

void AbilityPipeline::abort_cast(size_t index, double t, const char *why) {
  Cast &cast = casts_[index];
  const bool paid = cast.branched && cast.branched->cost_first;
  spdlog::info(
      "[abilities] {} {:.3f} s after the trigger: {}: {}", cast.def->row.name,
      t - cast.trigger_at, why,
      paid ? "its cost was paid before the bar's end, nothing more happens"
           : "nothing is paid and nothing fires");
  AbilityChange c;
  if (cast.montage_branch && !cast.item_due && item_instance_ &&
      *item_instance_ == cast.instance) {
    cast.item.montage_path.clear();
    cast.item.montage_guid = 0;
    cast.item.branch_predicted = cast.branch_predicted;
    cast.item.animation_counter = ++animation_counter_;
    c.abilities_changed.push_back(cast.item);
  }
  const std::string text =
      "the montage of " + cast.def->row.name + " is cleared: " + why;
  drop_record(cast.client_id);
  casts_.erase(casts_.begin() + static_cast<std::ptrdiff_t>(index));
  if (!c.empty())
    emit({std::move(c), std::nullopt, t, text});
}

void AbilityPipeline::reschedule_branch(Cast &cast, double branch_at,
                                        double cost_after) {
  cast.branch_due = branch_at;
  cast.cost_due = branch_at + cost_after;
  cast.end_due = *cast.cost_due + cast.branched->end;
  if (CastRecord *rec = record_of(cast.client_id)) {
    rec->cost_due = *cast.cost_due;
    rec->window_end = *cast.cost_due + kRequestWindow;
  }
}

bool AbilityPipeline::holds_skill(int64_t skill) const {
  for (const stats::SkillRank &r : stats_.ranks()) {
    if (r.skill.type_id == stats::kRefTypeSkill && r.skill.guid == skill)
      return true;
  }
  return false;
}

bool AbilityPipeline::branch_request(int32_t client_instance_id,
                                     int64_t branch_guid, double now) {
  advance(now);
  for (Cast &cast : casts_) {
    if (cast.client_id != client_instance_id || !cast.branched ||
        !cast.branch_due)
      continue;
    if (Host::Hits::try_charge_held_branch(*this, cast, branch_guid))
      return true;
    return Host::Hits::complete_branch_request(*this, cast, branch_guid);
  }
  return false;
}

bool AbilityPipeline::end_request(int32_t server_instance_id, double now) {
  advance(now);
  for (size_t i = 0; i < casts_.size(); ++i) {
    if (Host::Hits::is_cancellable_bar_cast(casts_[i], server_instance_id)) {
      abort_cast(i, now_, "the client ended the cast bar");
      break;
    }
  }
  for (Instance &i : instances_) {
    if (i.server_id == server_instance_id && !i.end_due) {
      i.end_due = now_ + kEndDelay;
      return true;
    }
  }
  return false;
}

void AbilityPipeline::end_instance(size_t index, double t) {
  const Instance inst = instances_[index];
  instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(index));
  AbilityEvent e;
  e.at = t;
  const AbilityDef *a =
      (item_instance_ && *item_instance_ == inst.server_id && catalog_)
          ? catalog_->ability(inst.ability)
          : nullptr;
  if (a) {
    bootstrap::ActiveAbilityItem item;
    item.ability_guid = inst.ability;
    item.server_instance_id = inst.server_id;
    item.montage_group = montage_group_of(*a);
    item.animation_counter = animation_counter_;
    e.change.abilities_changed.push_back(std::move(item));
  }
  e.end_ability = inst.client_id;
  e.note =
      "the client ended the ability instance " + std::to_string(inst.server_id);
  events_.push_back(std::move(e));
}

std::set<int64_t>
AbilityPipeline::effects_applied_by_hits(const AbilityDef &def) const {
  std::set<int64_t> out;
  Host::collect_ability_hit_graph(catalog_, def, &out);
  return out;
}

const char *AbilityPipeline::resolve_formulas(Cast &cast, const AbilityDef &def,
                                              std::string &why) {
  const content::AbilityRow &row = def.row;
  if (row.cooldown_expr != 0 && !eval(row.cooldown_expr, cast.cooldown, why))
    return "its cooldown";
  cast.cooldown = std::max(0.0, cast.cooldown);
  if (row.charges_expr != 0) {
    double v = 1;
    if (!eval(row.charges_expr, v, why))
      return "its charges";
    cast.max_charges = static_cast<int>(std::clamp(std::round(v), 1.0, 99.0));
  }
  for (size_t i = 0; i < def.costs.size();) {
    if (const char *err =
            Host::Hits::resolve_cost_group(*this, cast, def, i, why))
      return err;
  }
  return nullptr;
}

std::optional<AbilityPipeline::Inspection>
AbilityPipeline::inspect(int64_t ability) {
  const AbilityDef *def = catalog_ ? catalog_->ability(ability) : nullptr;
  if (!def)
    return std::nullopt;
  Inspection out;
  Cast cast;
  cast.def = def;
  drop_evaluation();
  std::string why;
  if (const char *what = resolve_formulas(cast, *def, why))
    out.problem = std::string(what) + ": " + why;
  out.cooldown = cast.cooldown;
  out.max_charges = cast.max_charges;
  out.charge_cost = cast.charge_cost;
  out.triggers_cooldown = cast.triggers_cooldown;
  out.group = group_key(def->row);
  Host::Hits::populate_inspected_stat_costs(*def, cast, out);
  out.effect_costs = cast.effect_costs;
  if (def->row.cast_expr != 0) {
    double v = 0;
    if (eval(def->row.cast_expr, v, why)) {
      out.cast_seconds = v;
    } else if (out.problem.empty()) {
      out.problem = "its cast time: " + why;
    }
  }
  if (!eval_bool(def->row.requirement_expr, true, out.requirement, why) &&
      out.problem.empty()) {
    out.problem = "its activation requirement: " + why;
  }
  return out;
}

} // namespace aoc::worldd
