#include "session/ability_pipeline_internal.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd {

using abilities::AbilityDef;
using abilities::EffectDef;
using abilities::HitDef;
using bootstrap::AbilityChange;

struct AbilityPipeline::Host::Core {
  struct ApplyEffectSpec {
    const EffectDef &def;
    int64_t owner = 0;
    double t = 0.0;
    double dur = kInf;
    int in_place_limit = 1;
    int same = 0;
    std::vector<stats::EffectMod> mods{};
    std::optional<float> shield{};
    bool never_refresh = false;
    bool extend = false;
  };

  static bool eval_effect_duration(AbilityPipeline &self,
                                   const content::EffectRow &row,
                                   std::optional<double> duration, double t,
                                   double &dur) {
    dur = kInf;
    if (duration) {
      dur = *duration;
      return true;
    }
    if (row.infinite_duration || row.duration_expr == 0)
      return true;
    std::string why;
    if (!self.eval(row.duration_expr, dur, why)) {
      AbilityEvent e;
      e.at = t;
      e.note = "the effect " + row.name +
               " is not applied: its duration does not run (" + why + ")";
      e.refusal = false;
      self.events_.push_back(std::move(e));
      return false;
    }
    if (!(dur >= 0))
      dur = 0;
    return true;
  }

  static void append_effect_mod(AbilityPipeline &self, const content::ModRow &m,
                                ApplyEffectSpec &spec) {
    if (m.shield_absorb || m.value_expr == 0)
      return;
    double v = 0;
    std::string why;
    if (!self.eval(m.value_expr, v, why))
      return;
    if (m.stat_guid == content::kGuidTempHealth) {
      if (std::isfinite(v) && v > 0)
        spec.shield = static_cast<float>(spec.shield.value_or(0.0f) + v);
      return;
    }
    spec.mods.push_back({m.stat_guid, v});
  }

  static void build_effect_spec(AbilityPipeline &self, ApplyEffectSpec &spec) {
    const content::EffectRow &row = spec.def.row;
    for (const content::ModRow &m : spec.def.mods) {
      append_effect_mod(self, m, spec);
    }
    spec.in_place_limit = std::max(1, row.global_stack_limit);
    if (row.in_place_stack_limit_expr != 0) {
      double v = 0;
      std::string why;
      if (self.eval(row.in_place_stack_limit_expr, v, why) && v >= 1) {
        spec.in_place_limit = static_cast<int>(std::min(v, 4096.0));
      }
    }
    spec.never_refresh =
        row.stack_duration_refresh.find("NeverRefresh") != std::string::npos;
    spec.extend =
        row.stack_duration_refresh.find("ExtendDuration") != std::string::npos;
  }

  static void refresh_effect_inst(EffectInst &e, const ApplyEffectSpec &spec) {
    if (spec.never_refresh)
      return;
    if (spec.shield) {
      e.has_shield = true;
      e.shield_base = *spec.shield;
      e.shield_value = *spec.shield;
    }
    double total = spec.dur;
    if (spec.extend && !std::isinf(e.end) && !std::isinf(spec.dur))
      total = (e.end - spec.t) + spec.dur;
    e.start = spec.t;
    e.end = std::isinf(total) ? kInf : spec.t + total;
  }

  static void create_effect_inst(AbilityPipeline &self, AbilityChange &c,
                                 const ApplyEffectSpec &spec, int stacks) {
    const content::EffectRow &row = spec.def.row;
    EffectInst e;
    e.key = self.a7_.allocate_effect_id();
    e.def = &spec.def;
    e.owner = spec.owner;
    e.zone = self.zone_owner_;
    e.stacks = stacks;
    e.start = spec.t;
    e.end = std::isinf(spec.dur) ? kInf : spec.t + spec.dur;
    e.mods = spec.mods;
    e.next_tick =
        row.tick_timer > 0
            ? (row.ignore_initial_tick ? spec.t + row.tick_timer : spec.t)
            : kInf;
    if (spec.shield) {
      e.has_shield = true;
      e.shield_base = *spec.shield;
      e.shield_value = *spec.shield;
    }
    for (uint32_t &w : e.shield_guid)
      w = static_cast<uint32_t>(self.guid_rng_.next() >> 16);
    self.effects_.push_back(e);
    c.effects_added.push_back(self.effect_item(self.effects_.back()));
    // the effect's Apply hits run now that it is on [R: Rogue_Stealth's effect
    // confirms its Apply hit with the effect's item]
    if (self.apply_depth_ < kMaxApplyDepth) {
      ++self.apply_depth_;
      const EffectInst created = self.effects_.back();
      self.sync_engine_effects();
      self.run_effect_hits(c, created, kApplyPhases, 1, spec.t);
      --self.apply_depth_;
    }
  }

  static void replace_effect_inst(AbilityPipeline &self, AbilityChange &c,
                                  int64_t old_key,
                                  const ApplyEffectSpec &spec) {
    if (spec.never_refresh)
      return;
    for (size_t i = 0; i < self.effects_.size(); ++i) {
      if (self.effects_[i].key == old_key) {
        self.remove_instance(c, i);
        break;
      }
    }
    create_effect_inst(self, c, spec, 1);
  }

  static void apply_existing_effect(AbilityPipeline &self, AbilityChange &c,
                                    EffectInst &existing,
                                    const ApplyEffectSpec &spec,
                                    int stacks_in) {
    const content::EffectRow &row = spec.def.row;
    if (row.stack_type.find("StackInPlace") != std::string::npos) {
      existing.stacks =
          std::min(spec.in_place_limit, existing.stacks + stacks_in);
      existing.mods = spec.mods;
      refresh_effect_inst(existing, spec);
      c.effects_changed.push_back(self.effect_item(existing));
      return;
    }
    if (row.stack_type.find("StackSeperate") != std::string::npos) {
      if (spec.same < std::max(1, row.global_stack_limit) &&
          self.effects_.size() < kMaxEffects) {
        create_effect_inst(self, c, spec, 1);
      }
      return;
    }
    if (row.stack_type.find("OverrideDuration") != std::string::npos) {
      existing.mods = spec.mods;
      refresh_effect_inst(existing, spec);
      c.effects_changed.push_back(self.effect_item(existing));
      return;
    }
    replace_effect_inst(self, c, existing.key, spec);
  }

  static int initial_stack_limit(const content::EffectRow &row,
                                 int in_place_limit, bool raw) {
    if (raw)
      return std::max(1, row.global_stack_limit);
    const bool in_place =
        row.stack_type.find("StackInPlace") != std::string::npos;
    return in_place ? in_place_limit : 1;
  }

  static void dispatch_effect_inst(AbilityPipeline &self, AbilityChange &c,
                                   ApplyEffectSpec &spec, int stacks_in,
                                   bool raw) {
    EffectInst *existing = nullptr;
    for (EffectInst &e : self.effects_) {
      if (e.def != &spec.def)
        continue;
      ++spec.same;
      if (!existing)
        existing = &e;
    }
    if (existing) {
      apply_existing_effect(self, c, *existing, spec, stacks_in);
      return;
    }
    if (self.effects_.size() >= kMaxEffects)
      return;
    const int limit =
        initial_stack_limit(spec.def.row, spec.in_place_limit, raw);
    create_effect_inst(self, c, spec, std::min(stacks_in, limit));
  }
};

AbilityPipeline::AbilityPipeline(const abilities::Catalog *catalog,
                                 stats::CharacterStats &stats,
                                 AbilityRuntime &a7, WornFn worn,
                                 double world_time_at_zero, uint64_t seed)
    : catalog_(catalog), stats_(stats), a7_(a7), worn_(std::move(worn)),
      world_time_at_zero_(world_time_at_zero) {
  rng_.state = seed;
  stats_.set_resources(std::set<int64_t>(std::begin(content::kResourceStats),
                                         std::end(content::kResourceStats)));
  resolve_resource();
}

void AbilityPipeline::resolve_resource() {
  resource_ = nullptr;
  resource_class_ = stats_.class_enum();
  if (!catalog_)
    return;
  // contentc version 35: with the game's reactions in the tables the class
  // resource is the passive's reaction (the Bard's Resonance); the modelled
  // rule below is for a content.db without them, or with the reactions switched
  // off
  if (catalog_->reactions() != 0 && reactions_enabled_)
    return;
  for (const ClassResource &r : kClassResources) {
    if (r.class_enum == stats_.class_enum())
      resource_ = catalog_->effect_named(r.effect);
  }
}

void AbilityPipeline::set_known(std::vector<int64_t> known) {
  known_ = std::set<int64_t>(known.begin(), known.end());
}

bool AbilityPipeline::handles(int64_t ability) const {
  return catalog_ && catalog_->ability(ability) != nullptr &&
         !a7_.enabled(ability);
}

double AbilityPipeline::sync(double t) const {
  return bootstrap::effect_clock(world_time_at_zero_ + t);
}

void AbilityPipeline::emit(AbilityEvent e) {
  // the hits this step made are confirmed with it (the client draws its
  // floating numbers from them)
  for (bootstrap::HitConfirm &h : confirmations_)
    e.confirmations.push_back(std::move(h));
  confirmations_.clear();
  if (e.change.empty() && !e.end_ability && e.confirmations.empty() &&
      e.zones.empty())
    return;
  events_.push_back(std::move(e));
}

std::vector<AbilityEvent> AbilityPipeline::take_events() {
  std::vector<AbilityEvent> out;
  out.swap(events_);
  return out;
}

void AbilityPipeline::refuse(int32_t client_id, Refusal why, double t,
                             const std::string &what, bool) {
  ++refused_;
  AbilityEvent e;
  e.end_ability = client_id;
  e.at = t;
  e.note = what + " refused: " + refusal_name(why);
  e.refusal = true;
  events_.push_back(std::move(e));
}

std::string AbilityPipeline::expr_problem(int64_t expr) const {
  const content::ExprRow *e = catalog_ ? catalog_->expr(expr) : nullptr;
  if (!e)
    return "no expression " + std::to_string(expr);
  return e->error.empty()
             ? "expression " + std::to_string(expr) + " has no program"
             : e->error;
}

bool AbilityPipeline::eval(int64_t expr, double &out, std::string &why) {
  if (!catalog_) {
    why = "no ability tables";
    return false;
  }
  const content::ExprRow *e = catalog_->expr(expr);
  if (!e || e->program < 0) {
    why = expr_problem(expr);
    return false;
  }
  if (!ensure_evaluation(why))
    return false;
  Host host(*this);
  std::string err;
  double v = 0;
  if (!evaluation_->eval(e->program, host, v, err)) {
    why = err;
    return false;
  }
  if (!std::isfinite(v)) {
    why = "expression " + std::to_string(expr) + " is not a finite number";
    return false;
  }
  out = v;
  return true;
}

bool AbilityPipeline::ensure_evaluation(std::string &why) {
  if (evaluation_)
    return true;
  std::string err;
  evaluation_ = stats_.evaluation(worn_(), err);
  if (!evaluation_) {
    why = "the stat engine cannot run: " + err;
    return false;
  }
  return true;
}

bool AbilityPipeline::eval_bool(int64_t expr, bool none, bool &out,
                                std::string &why) {
  if (expr == 0) {
    out = none;
    return true;
  }
  double v = 0;
  if (!eval(expr, v, why))
    return false;
  out = v != 0;
  return true;
}

bool AbilityPipeline::eval_requirement(int64_t expr,
                                       const std::vector<uint64_t> &targets,
                                       bool &out, std::string &why) {
  if (expr == 0 || targets.empty() || !combat_)
    return eval_bool(expr, true, out, why);
  const content::ExprRow *e = catalog_ ? catalog_->expr(expr) : nullptr;
  if (!e || e->program < 0) {
    why = expr_problem(expr);
    return false;
  }
  const combat::TargetInfo ti = combat_->inspect(targets[0]);
  combat::Fighter *tf = combat_->fighter(targets[0]);
  if (!ti.exists || !tf)
    return eval_bool(expr, true, out, why);
  if (!ensure_evaluation(why))
    return false;
  const auto res = Host::eval_target_program(*this, e->program, *tf, ti, why);
  out = res.value_or(false);
  return res ? why.empty() : eval_bool(expr, true, out, why);
}

bool AbilityPipeline::eval_in_event(int64_t expr, const ReactionContext &rc,
                                    double &out, std::string &why) {
  if (!catalog_) {
    why = "no ability tables";
    return false;
  }
  const content::ExprRow *e = catalog_->expr(expr);
  if (!e || e->program < 0) {
    why = expr_problem(expr);
    return false;
  }
  if (!ensure_evaluation(why))
    return false;
  combat::HitContext ctx;
  Host::populate_event_hit_context(*this, rc, ctx);
  Host host(*this);
  host.set_context(&ctx);
  host.set_reaction(&rc);
  std::string err;
  double v = 0;
  const bool ran = evaluation_->eval(e->program, host, v, err);
  drop_evaluation();
  if (!ran) {
    why = err;
    return false;
  }
  if (!std::isfinite(v)) {
    why = "expression " + std::to_string(expr) + " is not a finite number";
    return false;
  }
  out = v;
  return true;
}

double AbilityPipeline::stat_now(int64_t guid) const {
  if (a7_.tracks(guid))
    return a7_.stat(guid);
  const stats::StatValue *v = stats_.find(guid);
  return v ? static_cast<double>(v->value) : 0.0;
}

void AbilityPipeline::set_held(AbilityChange &c, int64_t stat, double value) {
  const float f = static_cast<float>(value);
  stats_.set_current(stat, f);
  for (bootstrap::StatChange &s : c.stats) {
    if (s.record == stat) {
      s.value = f;
      return;
    }
  }
  bootstrap::StatChange sc;
  sc.record = stat;
  sc.value = f;
  if (const stats::StatValue *v = stats_.find(stat)) {
    sc.buckets = v->buckets();
    sc.base = v->base;
    sc.equipment = v->equip;
  }
  c.stats.push_back(sc);
}

bool AbilityPipeline::take_stat(AbilityChange &c, int64_t stat, double amount) {
  if (stat == content::kGuidStamina)
    return a7_.spend_stamina(c, amount);
  const double cur = stat_now(stat);
  if (cur < amount)
    return false;
  set_held(c, stat, std::max(0.0, cur - amount));
  return true;
}

std::vector<stats::ActiveEffect> AbilityPipeline::engine_effects() const {
  std::vector<stats::ActiveEffect> out;
  out.reserve(effects_.size());
  for (const EffectInst &e : effects_) {
    stats::ActiveEffect a;
    a.effect = {stats::kRefTypeEffect, e.def->row.guid};
    a.stacks = e.stacks;
    a.tags = e.def->row.tags;
    a.tags.insert(a.tags.end(), e.def->row.effect_tags.begin(),
                  e.def->row.effect_tags.end());
    a.mods = e.mods;
    out.push_back(std::move(a));
  }
  return out;
}

void AbilityPipeline::recompute(AbilityChange &c) {
  drop_evaluation();
  sync_shields(c);
  if (!stats_.started())
    return;
  stats_.set_effects(engine_effects());
  std::vector<stats::StatValue> changed;
  stats::CharacterStats::Next next;
  std::string err;
  if (!stats_.prepare(worn_(), changed, next, err))
    return;
  stats_.commit(std::move(next));
  for (const stats::StatValue &v : changed) {
    Host::record_recomputed_stat(*this, c, v);
  }
}

bool AbilityPipeline::grant_effect(int64_t effect_guid, int stacks,
                                   double now) {
  const EffectDef *d = catalog_ ? catalog_->effect(effect_guid) : nullptr;
  if (!d)
    return false;
  advance(now);
  AbilityChange c;
  apply_effect(c, *d, stacks, std::nullopt, now_, true);
  recompute(c);
  emit({std::move(c), std::nullopt, now_,
        "the effect " + d->row.name + " is given"});
  return true;
}

bool AbilityPipeline::remove_effect(int64_t effect_guid, double now) {
  advance(now);
  AbilityChange c;
  bool any = false;
  for (size_t i = 0; i < effects_.size();) {
    if (effects_[i].def->row.guid == effect_guid) {
      remove_instance(c, i);
      any = true;
    } else {
      ++i;
    }
  }
  if (!any)
    return false;
  recompute(c);
  emit({std::move(c), std::nullopt, now_, "an effect is taken off"});
  return true;
}

void AbilityPipeline::set_moving(bool moving) {
  moving_ = moving;
  stats_.set_moving(moving);
}

double AbilityPipeline::progress_of(const std::string &group) const {
  if (group.empty())
    return fill_;
  if (group == "DefaultGroup") {
    return (last_item_at_ >= 0 && item_instance_)
               ? std::clamp((clock_ - last_item_at_) / kCastSeconds, 0.0, 0.99)
               : 0.0;
  }
  for (const EffectInst &e : effects_) {
    if (group == "SongGroup" && e.owner != 0 && std::isinf(e.end))
      return std::clamp((clock_ - e.start) / kSongProgressSeconds, 0.0, 1.0);
  }
  return 0;
}

double AbilityPipeline::remaining_charges(int64_t ability) const {
  const AbilityDef *d = catalog_ ? catalog_->ability(ability) : nullptr;
  if (!d)
    return 0;
  const auto g = groups_.find(group_key(d->row));
  if (g != groups_.end())
    return g->second.charges;
  const auto m = last_max_charges_.find(ability);
  return m == last_max_charges_.end() ? 1.0 : m->second;
}

double AbilityPipeline::stat_delta_over_time(int64_t stat,
                                             double seconds) const {
  if (stat != content::kGuidHealth)
    return 0;
  double sum = 0;
  for (const auto &[at, lost] : losses_) {
    if (clock_ - at <= seconds)
      sum -= lost;
  }
  return sum;
}

void AbilityPipeline::sync_a7_values(double now) {
  drop_evaluation();
  const content::AbilityRules &rules = a7_.rules();
  if (catalog_) {
    if (const EffectDef *sprint = catalog_->effect(rules.sprint.effect_guid)) {
      Host::sync_sprint_mods(*this, rules.sprint.effect_guid, *sprint, now);
    }
  }
  if (const stats::StatValue *regen =
          stats_.find(content::kGuidStaminaRegeneration)) {
    if (regen->value >= 0 && std::isfinite(regen->value))
      a7_.set_regen_rate(regen->value, now);
  }
}

void AbilityPipeline::refresh(double now, const char *why, bool with_stats) {
  advance(now);
  AbilityChange c;
  if (stats_.class_enum() != resource_class_) {
    while (!effects_.empty())
      remove_instance(c, effects_.size() - 1);
    resolve_resource();
  }
  if (with_stats) {
    recompute(c);
  } else {
    drop_evaluation();
    if (stats_.started())
      stats_.set_effects(engine_effects());
  }
  emit({std::move(c), std::nullopt, now_, why});
}

int AbilityPipeline::stacks_of(int64_t effect_guid) const {
  int n = 0;
  for (const EffectInst &e : effects_) {
    if (e.def->row.guid == effect_guid)
      n += e.stacks;
  }
  return n;
}

std::optional<double> AbilityPipeline::effect_end(int64_t effect_guid) const {
  for (const EffectInst &e : effects_) {
    if (e.def->row.guid == effect_guid)
      return e.end;
  }
  return std::nullopt;
}

bootstrap::EffectItem AbilityPipeline::effect_item(const EffectInst &e) const {
  bootstrap::EffectItem item;
  item.effect_id = e.key;
  item.effect_guid = e.def->row.guid;
  item.start_time = sync(e.start);
  item.duration = std::isinf(e.end) ? AbilityRuntime::kInfiniteDuration
                                    : static_cast<float>(e.end - e.start);
  item.tick_time = static_cast<float>(e.def->row.tick_timer);
  item.stacks = e.stacks;
  return item;
}

void AbilityPipeline::apply_effect(AbilityChange &c, const EffectDef &def,
                                   int stacks_in,
                                   std::optional<double> duration, double t,
                                   bool raw, int64_t owner) {
  const int stacks = std::max(1, stacks_in);
  Host::Core::ApplyEffectSpec spec{def, owner, t};
  if (!Host::Core::eval_effect_duration(*this, def.row, duration, t, spec.dur))
    return;
  Host::Core::build_effect_spec(*this, spec);
  Host::Core::dispatch_effect_inst(*this, c, spec, stacks, raw);
}

void AbilityPipeline::remove_instance(AbilityChange &c, size_t index) {
  const int64_t key = effects_[index].key;
  bool unsent = false;
  for (auto it = c.effects_added.begin(); it != c.effects_added.end(); ++it) {
    if (it->effect_id == key) {
      c.effects_added.erase(it);
      unsent = true;
      break;
    }
  }
  for (auto it = c.effects_changed.begin(); it != c.effects_changed.end();) {
    it = it->effect_id == key ? c.effects_changed.erase(it) : it + 1;
  }
  if (!unsent)
    c.effects_removed.push_back(key);
  effects_.erase(effects_.begin() + static_cast<std::ptrdiff_t>(index));
}

void AbilityPipeline::remove_stacks(AbilityChange &c, int64_t effect_guid,
                                    int stacks) {
  for (size_t i = 0; i < effects_.size() && stacks > 0;) {
    EffectInst &e = effects_[i];
    if (e.def->row.guid != effect_guid) {
      ++i;
      continue;
    }
    const int take = std::min(stacks, e.stacks);
    e.stacks -= take;
    stacks -= take;
    if (e.stacks <= 0) {
      remove_instance(c, i);
    } else {
      c.effects_changed.push_back(effect_item(e));
      ++i;
    }
  }
}

void AbilityPipeline::sync_engine_effects() {
  drop_evaluation();
  if (stats_.started())
    stats_.set_effects(engine_effects());
}

void AbilityPipeline::remove_by_hit(AbilityChange &c, const HitDef &hit) {
  int left = hit.max_remove > 0 ? hit.max_remove : 1 << 20;
  for (const int64_t guid : hit.remove_effects) {
    Host::remove_guid_by_hit(*this, c, guid, left);
  }
  sync_engine_effects();
}

void AbilityPipeline::run_effect_hits(AbilityChange &c, const EffectInst &e,
                                      const char *const *phases, size_t n,
                                      double t) {
  HitSource src; // an effect's hit: confirmed by the server when it lands,
                 // Source is the pawn [R: 1,525 effect hits]
  src.type = content::kSourceTypeEffect;
  src.guid = e.def->row.guid;
  src.actor = true;
  src.confirm = true;
  const bool tick = n == 1 && std::string_view(phases[0]) == "Tick";
  src.simulated = tick;
  src.autonomous = tick;
  for (const auto &[phase, hit_guid] : e.def->hits) {
    bool wanted = false;
    for (size_t i = 0; i < n; ++i)
      wanted = wanted || phase == phases[i];
    if (!wanted)
      continue;
    if (const HitDef *h = catalog_->hit(hit_guid))
      apply_hit(c, *h, t, false, src);
  }
}

double AbilityPipeline::client_rate(bool magical) const {
  const double r = stat_now(magical ? kStatMagicalSpeed : kStatPhysicalSpeed);
  return (r > 0.2 && r < 3.0) ? r : 1.0;
}

AbilityPipeline::Cast *AbilityPipeline::cast_of(int32_t client_id) {
  for (Cast &c : casts_) {
    if (c.client_id == client_id)
      return &c;
  }
  return nullptr;
}

void AbilityPipeline::plan_shot(Cast &cast, const ShotCast &shot, double t) {
  if (!combat_ || cast.target_actors.empty() || !cast.cost_due)
    return;
  cast.shot = &shot;
  cast.flight_pending = true;
  cast.launch_at = t + shot.fire / client_rate(shot.magical);
  cast.projectile_due = cast.launch_at;
}

void AbilityPipeline::land_shot(Cast &cast, double t) {
  double land = std::max(t, cast.launch_at + cast.flight);
  if (cast.cost_due)
    land = std::max(land, *cast.cost_due);
  cast.projectile_due = land;
}

void AbilityPipeline::anchor_shot(Cast &cast, double now) {
  cast.anchored = true;
  const double was = cast.launch_at;
  cast.launch_at =
      std::max(now - cast.shot->ask_gap / client_rate(cast.shot->magical),
               cast.trigger_at);
  if (cast.flight_pending) {
    cast.projectile_due = std::max(now, cast.launch_at);
  } else if (cast.projectile_due) {
    land_shot(cast, now);
  }
  spdlog::info(
      "[abilities] {}: the client asked for the hit that goes with the launch "
      "{:.3f} s after the trigger: the projectile {} {:.3f} s from the plan",
      cast.def->row.name, now - cast.trigger_at,
      cast.launch_at < was ? "left" : "leaves",
      std::fabs(cast.launch_at - was));
}

void AbilityPipeline::end_held(size_t index, double t) {
  const int64_t ability = held_[index].ability;
  held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(index));
  AbilityChange c;
  end_loop(c, ability, t);
  recompute(c);
  const AbilityDef *a = catalog_->ability(ability);
  emit({std::move(c), std::nullopt, t,
        "the held ability " + (a ? a->row.name : std::string("?")) +
            " is released"});
}

void AbilityPipeline::end_loop(AbilityChange &c, int64_t ability, double t) {
  for (size_t i = 0; i < effects_.size();) {
    if (effects_[i].owner != ability) {
      ++i;
      continue;
    }
    const EffectInst gone = effects_[i];
    remove_instance(c, i);
    run_effect_hits(c, gone, kExpirePhases, 2, t);
  }
  sync_engine_effects();
}

double AbilityPipeline::zone_due(const Zone &z) const {
  switch (z.state) {
  case Zone::State::Pending:
    return z.open_at;
  case Zone::State::Open:
    return z.end_at;
  case Zone::State::Ended:
    return std::min(z.fade_at >= 0 ? z.fade_at : kInf, z.close_at);
  }
  return kInf;
}

void AbilityPipeline::end_zone_effects(AbilityChange &c, int64_t zone_id,
                                       double t) {
  for (size_t i = 0; i < effects_.size();) {
    if (effects_[i].zone != zone_id || !std::isinf(effects_[i].end)) {
      ++i;
      continue;
    }
    const EffectInst gone = effects_[i];
    remove_instance(c, i);
    run_effect_hits(c, gone, kExpirePhases, 2, t);
  }
  sync_engine_effects();
}

} // namespace aoc::worldd
