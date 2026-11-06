#include "session/ability_pipeline_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd {

using abilities::AbilityDef;
using abilities::EffectDef;
using abilities::HitDef;
using bootstrap::AbilityChange;

struct AbilityPipeline::Host::Damage {
  class PortEffects final : public combat::TargetEffects {
  public:
    PortEffects(combat::HitPort &port, uint64_t actor, double t)
        : port_(port), actor_(actor), t_(t) {}
    double remaining(int64_t effect_guid) const override {
      return port_.effect_remaining(actor_, effect_guid, t_);
    }

  private:
    combat::HitPort &port_;
    uint64_t actor_;
    double t_;
  };

  struct TargetBatch {
    const HitDef &hit;
    const AbilityDef *ability = nullptr;
    const HitSource &src;
    double t = 0.0, total_targets = 0.0;
    int mask = 0;
    bool melee = false, active_in_hit = false;
    uint64_t active = 0, closest = 0;
  };

  static bool target_reached(const AbilityPipeline &self, const combat::TargetInfo &ti,
                             int mask, bool melee, bool reach_free = false) {
    if (!ti.exists || !ti.alive) return false;
    const bool hostile = ti.relation == combat::Relation::Hostile;
    if (hostile ? (mask & 2) == 0 : (mask & (1 | 4 | 8 | 16)) == 0) return false;
    return reach_free || ti.surface_distance_cm <=
           self.combat_->reach_cm(melee) + self.combat_->data().activation_leeway_cm();
  }

  static void scan_primary_target(const AbilityPipeline &self,
                                  const std::vector<uint64_t> &targets, TargetBatch &batch) {
    batch.active = self.active_target_ ? self.active_target_() : 0;
    double closest_cm = 0.0;
    for (const uint64_t id : targets) {
      const combat::TargetInfo ti = self.combat_->inspect(id);
      if (!target_reached(self, ti, batch.mask, batch.melee, batch.src.reach_free)) continue;
      if (id == batch.active) batch.active_in_hit = true;
      if (batch.closest == 0 || ti.surface_distance_cm < closest_cm) {
        batch.closest = id;
        closest_cm = ti.surface_distance_cm;
      }
    }
  }

  static bootstrap::HitTargetOut build_target_out(const AbilityPipeline &self, uint64_t id,
                                                  const combat::TargetInfo &ti,
                                                  const combat::Resolved &r,
                                                  const combat::Delivery &d) {
    bootstrap::HitTargetOut out;
    out.component = self.combat_->guid_of(id);
    out.cylinder = self.combat_->hit_component_of(id);
    out.has_impact = true;
    out.impact_location = {ti.position.x, ti.position.y, ti.position.z};
    const combat::Vec3 from = self.combat_->self().position();
    double nx = from.x - ti.position.x, ny = from.y - ti.position.y, nz = from.z - ti.position.z;
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    out.impact_normal = len > 1e-6 ? decltype(out.impact_normal){nx / len, ny / len, nz / len}
                                   : decltype(out.impact_normal){0.0, 0.0, 1.0};
    out.flags = d.flags;
    for (size_t i = 0; i < r.mods.size(); ++i) {
      if (r.mods[i].value == 0.0 && d.effective[i] == 0.0) continue;
      if (self.combat_->data().ids().is_status_stat(r.mods[i].stat_guid)) continue;
      out.mods.push_back({r.mods[i].stat_mod_guid, static_cast<float>(r.mods[i].value),
                          static_cast<float>(d.effective[i])});
    }
    return out;
  }

  static double eval_shield_duration(AbilityPipeline &self, const content::HitEffectRow &row,
                                     const EffectDef &def, std::string &why) {
    double seconds = 0.0;
    if (!def.row.infinite_duration && def.row.duration_expr != 0)
      self.eval(def.row.duration_expr, seconds, why);
    if (row.duration_expr != 0) self.eval(row.duration_expr, seconds, why);
    return std::max(0.0, seconds);
  }

  static void apply_target_shield_mods(AbilityPipeline &self, uint64_t id,
                                       const content::HitEffectRow &row, const EffectDef &def,
                                       double t) {
    std::string why;
    for (const content::ModRow &m : def.mods) {
      if (m.stat_guid != content::kGuidTempHealth || m.value_expr == 0) continue;
      double amount = 0.0;
      if (!self.eval(m.value_expr, amount, why) || !(amount > 0.0)) continue;
      self.combat_->shield(id, amount, eval_shield_duration(self, row, def, why), t);
    }
  }

  static void apply_target_shields(AbilityPipeline &self, uint64_t id, const HitDef &hit,
                                   double t) {
    std::string why;
    for (const content::HitEffectRow &row : hit.effects) {
      if (row.kind == "self") continue;
      const EffectDef *def = self.catalog_->effect(row.effect_guid);
      if (!def) continue;
      bool applies = true;
      if (!self.eval_bool(row.requirement_expr, true, applies, why) || !applies) continue;
      apply_target_shield_mods(self, id, row, *def, t);
    }
  }

  static combat::HitMeta build_hit_meta(const AbilityPipeline &self, const TargetBatch &batch,
                                        uint64_t id) {
    combat::HitMeta meta;
    meta.source_type = batch.src.type;
    meta.source_guid = batch.src.guid;
    if (batch.ability) meta.ability_tags = batch.ability->row.tags;
    if (batch.hit.use_item_tags && self.weapon_tags_) meta.item_tags = self.weapon_tags_();
    meta.targets = batch.total_targets;
    meta.target_is_active = id == batch.active;
    meta.active_in_hit = batch.active_in_hit;
    meta.target_is_closest = id == batch.closest;
    return meta;
  }

  static void record_delivered_hit(const combat::Resolved &r, const combat::Delivery &d,
                                   uint64_t id, HitEvent &event) {
    event.targets.push_back(id);
    event.flags = static_cast<uint8_t>(event.flags | d.flags);
    for (size_t i = 0; i < r.mods.size(); ++i) {
      event.mods[r.mods[i].stat_guid] += r.mods[i].value;
      if (i < d.effective.size()) event.effective[r.mods[i].stat_guid] += d.effective[i];
    }
  }

  static void apply_target_resolved_effects(AbilityPipeline &self, const TargetBatch &batch,
                                            uint64_t id, combat::Fighter &tf,
                                            const combat::TargetInfo &ti) {
    bool applies_effects = false;
    for (const content::HitEffectRow &row : batch.hit.effects)
      applies_effects = applies_effects || row.kind != "self";
    if (!applies_effects) return;
    std::string after_err, gate_why;
    auto after = tf.stats().evaluation(tf.worn(), after_err, true);
    if (!after) return;
    Host host(self);
    combat::HitMeta meta = build_hit_meta(self, batch, id);
    const PortEffects target_effects(*self.combat_, id, batch.t);
    meta.target_effects = &target_effects;
    combat::Resolved gate;
    if (!self.combat_->resolver().resolve_effects_with(
            *self.evaluation_, host, after.get(), ti.surface_distance_cm,
            ti.relation == combat::Relation::Hostile, ti.relation == combat::Relation::Neutral,
            batch.hit, meta, gate, gate_why))
      return;
    if (!gate_why.empty())
      spdlog::debug("[abilities] the effects of {} on actor {}: {}", batch.hit.row.name, id, gate_why);
    if (!gate.effects.empty()) self.combat_->apply_effects(id, gate.effects, batch.t);
  }

  static void resolve_single_target(AbilityPipeline &self, const TargetBatch &batch,
                                    uint64_t id, std::vector<bootstrap::HitTargetOut> &outs,
                                    HitEvent &event) {
    const combat::TargetInfo ti = self.combat_->inspect(id);
    if (!target_reached(self, ti, batch.mask, batch.melee, batch.src.reach_free)) return;
    combat::Fighter *tf = self.combat_->fighter(id);
    if (!tf) return;
    std::string err, why;
    auto target_eval = tf->stats().evaluation(tf->worn(), err, true);
    if (!target_eval) return;
    Host host(self);
    combat::HitMeta meta = build_hit_meta(self, batch, id);
    const PortEffects target_effects(*self.combat_, id, batch.t);
    meta.target_effects = &target_effects;
    combat::Resolved r;
    if (!self.combat_->resolver().resolve_with(
            *self.evaluation_, host, target_eval.get(), ti.surface_distance_cm,
            ti.relation == combat::Relation::Hostile, ti.relation == combat::Relation::Neutral,
            batch.hit, meta, r, why)) {
      if (self.no_formula_.insert(batch.hit.row.guid).second)
        spdlog::warn("[abilities] the hit {} does not run on another actor: {}", batch.hit.row.name, why);
      return;
    }
    combat::HitOrigin origin{&batch.hit, batch.src.type, batch.src.guid, meta.ability_tags};
    const combat::Delivery d = self.combat_->deliver_from(id, r, batch.t, origin);
    if (!d.applied) return;
    if (batch.hit.row.name == "Summoner_Transpose") self.transpose_with_ = id;
    record_delivered_hit(r, d, id, event);
    outs.push_back(build_target_out(self, id, ti, r, d));
    if (r.miss()) return;
    if (!batch.hit.remove_effects.empty() && batch.hit.row.name.rfind("Summoner_", 0) == 0)
      self.combat_->remove_effects(id, batch.hit, batch.t);
    apply_target_shields(self, id, batch.hit, batch.t);
    if (!d.lethal) apply_target_resolved_effects(self, batch, id, *tf, ti);
  }

  static void apply_single_caster_hit_effect(AbilityPipeline &self, AbilityChange &c,
                                             const content::HitEffectRow &row, double t,
                                             int mode) {
    const EffectDef *def = self.catalog_->effect(row.effect_guid);
    if (!def || (mode == 2 && def == self.resource_)) return;
    std::string why;
    double stacks = 1.0;
    if (row.stack_count_expr != 0 && !self.eval(row.stack_count_expr, stacks, why)) stacks = 1.0;
    std::optional<double> duration;
    if (row.duration_expr != 0) {
      double dsec = 0.0;
      if (self.eval(row.duration_expr, dsec, why)) duration = std::max(0.0, dsec);
    }
    self.apply_effect(c, *def, static_cast<int>(std::clamp(stacks, 1.0, 4096.0)), duration, t);
    self.sync_engine_effects();
  }

  static void apply_caster_hit_effects(AbilityPipeline &self, AbilityChange &c,
                                       const HitDef &hit, double t, int mode) {
    std::string why;
    for (const content::HitEffectRow &row : hit.effects) {
      if (mode == 0 && row.kind != "self") continue;
      bool applies = true;
      if (self.eval_bool(row.requirement_expr, true, applies, why) && applies)
        apply_single_caster_hit_effect(self, c, row, t, mode);
    }
  }

  static void forward_target_pet_hits(AbilityPipeline &self, AbilityChange &c,
                                      const HitEvent &event, double t) {
    HitSource fired = event.src;
    fired.confirm = false;
    for (const content::RecursiveHitRow &r : event.hit->recursive) {
      const HitDef *child = self.catalog_->hit(r.child_hit);
      if (!child || child->row.applicable_targets != 4 || self.carried_depth_ >= 2) continue;
      const uint64_t pet = self.combat_->own_pet();
      if (pet != 0 && std::find(event.targets.begin(), event.targets.end(), pet) != event.targets.end()) {
        ++self.carried_depth_;
        self.apply_hit_on_targets(c, *child, {pet}, t, fired, nullptr);
        --self.carried_depth_;
      }
    }
  }

  static void forward_recursive_pet_hits(AbilityPipeline &self, AbilityChange &c,
                                         const HitDef &hit, double t, const HitSource &src) {
    if (!self.combat_ || self.carried_depth_ >= 2) return;
    for (const content::RecursiveHitRow &r : hit.recursive) {
      const HitDef *child = self.catalog_->hit(r.child_hit);
      if (!child || child->row.applicable_targets != 4) continue;
      const uint64_t pet = self.combat_->own_pet();
      if (pet == 0) continue;
      HitSource fired = src;
      fired.confirm = false;
      ++self.carried_depth_;
      self.apply_hit_on_targets(c, *child, {pet}, t, fired, nullptr);
      --self.carried_depth_;
    }
  }

  static void push_target_confirm(AbilityPipeline &self, const HitDef &hit, const HitSource &src,
                                  std::vector<bootstrap::HitTargetOut> targets,
                                  std::optional<protocol::emit::FIntrepidNetworkGUID> source) {
    bootstrap::HitConfirm h;
    h.record_guid = hit.row.guid;
    h.source_type = src.type;
    h.source_guid = src.guid;
    h.source_is_actor = src.actor;
    h.source = source;
    h.targets = std::move(targets);
    h.predicted_autonomous = src.autonomous;
    h.predicted_simulated = src.simulated;
    h.response = src.response;
    self.confirmations_.push_back(std::move(h));
  }

  static void queue_target_confirmations(AbilityPipeline &self, const HitDef &hit,
                                         const HitSource &src,
                                         std::vector<bootstrap::HitTargetOut> outs) {
    bool shows = hit.row.has_fx;
    for (const bootstrap::HitTargetOut &o : outs) shows = shows || !o.mods.empty();
    if (src.type == content::kSourceTypeReaction && !shows) return;
    if (!src.source_is_target) {
      const auto source =
          src.source_actor != 0 ? std::optional(self.combat_->guid_of(src.source_actor)) : std::nullopt;
      push_target_confirm(self, hit, src, std::move(outs), source);
      return;
    }
    for (bootstrap::HitTargetOut &o : outs) {
      std::vector<bootstrap::HitTargetOut> one;
      const protocol::emit::FIntrepidNetworkGUID actor = o.component;
      one.push_back(std::move(o));
      push_target_confirm(self, hit, src, std::move(one), actor);
    }
  }

  static double clamp_stat_bounds(const stats::StatValue &v, double next) {
    if (v.has_max) next = std::min(next, static_cast<double>(v.max));
    if (v.has_min) next = std::max(next, static_cast<double>(v.min));
    return next;
  }

  static double apply_custom_resource_delta(AbilityPipeline &self, AbilityChange &c,
                                            int64_t stat, double amount) {
    const stats::StatValue *v = self.stats_.find(stat);
    if (!v || !v->resource()) return 0;
    const double cur = v->value, next = clamp_stat_bounds(*v, cur + amount);
    if (static_cast<float>(next) != static_cast<float>(cur)) self.set_held(c, stat, next);
    return next - cur;
  }

  static void record_health_loss(AbilityPipeline &self, double loss) {
    self.losses_.emplace_back(self.clock_, loss);
    while (!self.losses_.empty() && self.clock_ - self.losses_.front().first > 60.0)
      self.losses_.pop_front();
    if (self.losses_.size() > 256) self.losses_.pop_front();
  }

  static double eval_max_targets(AbilityPipeline &self, const HitDef &hit, std::string &why) {
    double max_targets = 40.0, v = 0.0;
    if (hit.row.max_targets_expr != 0 && self.eval(hit.row.max_targets_expr, v, why) && v >= 1.0)
      max_targets = std::min(max_targets, v);
    return max_targets;
  }

  static void populate_mount_hit_mods(const combat::IncomingHit &in, double taken,
                                      bootstrap::HitTargetOut &t) {
    bool health_done = false;
    for (const combat::ResolvedMod &m : in.mods) {
      if (!std::isfinite(m.value)) continue;
      double effective = 0;
      if (m.stat_guid == content::kGuidHealth && m.value < 0 && !health_done) {
        effective = -taken;
        health_done = true;
      }
      t.mods.push_back({m.stat_mod_guid, static_cast<float>(m.value), static_cast<float>(effective)});
    }
  }

  static void apply_self_hit_mods(AbilityPipeline &self, AbilityChange &c, const HitDef &hit,
                                  std::vector<bootstrap::HitStatModOut> &outs, HitEvent &event) {
    std::string why;
    for (const content::ModRow &m : hit.mods) {
      if (m.shield_absorb || m.value_expr == 0) continue;
      double amount = 0;
      if (!self.eval(m.value_expr, amount, why) || !std::isfinite(amount)) continue;
      const double effective = self.apply_delta(c, m.stat_guid, amount);
      outs.push_back({m.stat_mod_guid, static_cast<float>(amount), static_cast<float>(effective)});
      event.mods[m.stat_guid] += amount;
      event.effective[m.stat_guid] += effective;
    }
  }

  static bool should_confirm_self_hit(const HitDef &hit, const HitSource &src,
                                      const std::vector<bootstrap::HitStatModOut> &outs) {
    return src.confirm && (!outs.empty() || hit.row.has_fx ||
                           (hit.row.applicable_targets & 8) != 0 || !hit.fires.empty());
  }

  static void apply_infinite_owner_siblings(AbilityPipeline &self, AbilityChange &c,
                                            const EffectInst &copy, double t) {
    if (copy.owner == 0 || !copy.def->row.infinite_duration) return;
    const AbilityDef *owner_def = self.catalog_->ability(copy.owner);
    if (!owner_def) return;
    for (const auto &[key, guid] : owner_def->effects) {
      if (guid == copy.def->row.guid) continue;
      if (const EffectDef *sibling = self.catalog_->effect(guid))
        self.apply_effect(c, *sibling, 1, std::nullopt, t, false, copy.owner);
    }
    self.sync_engine_effects();
  }

  static void regenerate_single_vital(AbilityPipeline &self, AbilityChange &c, int64_t current,
                                      int64_t regeneration, int64_t maximum) {
    if (!self.stats_.find(current) || !self.stats_.find(regeneration) || !self.stats_.find(maximum))
      return;
    const double cur = self.stat_now(current), reg = self.stat_now(regeneration),
                 max = self.stat_now(maximum);
    if (!(reg > 0) || !(cur < max)) return;
    const float next =
        std::min(static_cast<float>(max), static_cast<float>(cur) + static_cast<float>(reg));
    if (next > static_cast<float>(cur)) self.set_held(c, current, next);
  }

  static bool regenerate_custom_resources(AbilityPipeline &self, AbilityChange &c) {
    bool resource_moved = false;
    for (const stats::StatValue &v : self.stats_.values()) {
      if (!v.resource() || !v.has_regen || v.regen == 0.0f || !Host::is_replicated_stat(v))
        continue;
      const double raw =
          static_cast<double>(v.value) + static_cast<double>(v.regen) * kRegenInterval;
      const double next = clamp_stat_bounds(v, raw);
      if (static_cast<float>(next) == v.value) continue;
      self.set_held(c, v.guid, next);
      resource_moved = true;
    }
    return resource_moved;
  }

  static double cast_next_due(const Cast &c) {
    double due = kInf;
    for (const std::optional<double> *d :
         {&c.item_due, &c.cost_due, &c.hit_due, &c.projectile_due, &c.loop_due, &c.own_due,
          &c.branch_due, &c.end_due}) {
      if (*d) due = std::min(due, **d);
    }
    for (const Cast::Missile &m : c.missiles) due = std::min(due, m.due);
    return due;
  }
};

bool AbilityPipeline::absorb(AbilityChange &, double &amount) {
  bool any = false;
  for (EffectInst &e : effects_) {
    if (!e.has_shield || !(e.shield_value > 0) || !(amount > 0)) continue;
    const float take =
        static_cast<float>(std::min(static_cast<double>(e.shield_value), amount));
    e.shield_value -= take;
    amount -= take;
    any = true;
  }
  return any;
}

void AbilityPipeline::sync_shields(AbilityChange &c) {
  std::vector<bootstrap::ShieldElement> now;
  for (const EffectInst &e : effects_) {
    if (!e.has_shield) continue;
    bootstrap::ShieldElement el;
    for (int i = 0; i < 4; ++i) el.guid[i] = e.shield_guid[i];
    el.base = e.shield_base;
    el.value = e.shield_value;
    now.push_back(el);
  }
  if (now == shields_sent_) return;
  shields_sent_ = now;
  c.shields = std::move(now);
}

std::vector<std::pair<double, double>> AbilityPipeline::shields() const {
  std::vector<std::pair<double, double>> out;
  for (const EffectInst &e : effects_) {
    if (e.has_shield) out.emplace_back(e.shield_base, e.shield_value);
  }
  return out;
}

double AbilityPipeline::apply_delta(AbilityChange &c, int64_t stat, double amount) {
  if (!std::isfinite(amount)) return 0;
  if (stat == content::kGuidStamina) return a7_.add_stamina(c, amount);
  if (stat != content::kGuidHealth && stat != content::kGuidMana)
    return Host::Damage::apply_custom_resource_delta(*this, c, stat, amount);
  const int64_t max_stat =
      stat == content::kGuidHealth ? content::kGuidMaxHealth : content::kGuidMaxMana;
  const double max = std::max(0.0, stat_now(max_stat));
  const double cur = stat_now(stat);
  double through = amount;
  if (stat == content::kGuidHealth && amount < 0) {
    double loss = -amount;
    absorb(c, loss);
    through = -loss;
  }
  const double next = std::clamp(cur + through, 0.0, max);
  if (static_cast<float>(next) != static_cast<float>(cur)) set_held(c, stat, next);
  if (stat == content::kGuidHealth && next < cur)
    Host::Damage::record_health_loss(*this, cur - next);
  return next - cur;
}

double AbilityPipeline::take_damage(double amount, double now) {
  advance(now);
  AbilityChange c;
  const double before = stat_now(content::kGuidHealth);
  apply_delta(c, content::kGuidHealth, -std::max(0.0, amount));
  recompute(c);
  const double through = before - stat_now(content::kGuidHealth);
  emit({std::move(c), std::nullopt, now_, "damage reaches the character"});
  return through;
}

void AbilityPipeline::apply_hit_on_targets(AbilityChange &c, const HitDef &hit,
                                           const std::vector<uint64_t> &targets, double t,
                                           const HitSource &src, const AbilityDef *ability,
                                           bool *landed) {
  if (landed) *landed = false;
  if (!combat_ || targets.empty()) return;
  std::string why;
  bool ok = true;
  if (!eval_bool(hit.row.requirement_expr, true, ok, why) || !ok) return;
  if (!ensure_evaluation(why)) return;
  const double max_targets = Host::Damage::eval_max_targets(*this, hit, why);
  Host::Damage::TargetBatch batch{hit, ability, src, t, static_cast<double>(targets.size()),
                                  hit.row.applicable_targets, is_melee_ability(ability)};
  Host::Damage::scan_primary_target(*this, targets, batch);
  if (landed) *landed = batch.closest != 0;
  std::vector<bootstrap::HitTargetOut> outs;
  HitEvent event;
  event.hit = &hit;
  event.ability = ability;
  event.src = src;
  for (const uint64_t id : targets) {
    if (static_cast<double>(outs.size()) >= max_targets) break;
    Host::Damage::resolve_single_target(*this, batch, id, outs, event);
  }
  if (!outs.empty()) {
    Host::Damage::apply_caster_hit_effects(*this, c, hit, t, 0);
    Host::Damage::forward_target_pet_hits(*this, c, event, t);
  }
  drop_evaluation();
  if (outs.empty()) return;
  Host::Damage::queue_target_confirmations(*this, hit, src, std::move(outs));
  raise_hit_event(c, event, t);
}

void AbilityPipeline::receive_hit(const combat::IncomingHit &in, double now) {
  advance(now);
  AbilityChange c;
  bootstrap::HitConfirm h;
  h.record_guid = in.hit_guid;
  h.source_type = in.source_type;
  h.source_guid = in.source_guid;
  const bool from_projectile = in.source_type == content::kSourceTypeProjectile;
  h.source_is_actor = !from_projectile;
  h.predicted_autonomous = true;
  h.predicted_simulated = from_projectile;
  if (combat_) h.instigator = combat_->guid_of(in.instigator);
  uint8_t flags = in.flags;
  const double before = stat_now(content::kGuidHealth);
  if ((flags & stats::kHitMiss) == 0) {
    for (const combat::ResolvedMod &m : in.mods) {
      if (!std::isfinite(m.value)) continue;
      const double effective = apply_delta(c, m.stat_guid, m.value);
      h.mods.push_back({m.stat_mod_guid, static_cast<float>(m.value), static_cast<float>(effective)});
    }
  }
  if (before > 0 && stat_now(content::kGuidHealth) <= 0) flags |= stats::kHitLethal;
  h.flags = flags;
  confirmations_.push_back(std::move(h));
  drop_evaluation();
  recompute(c);
  emit({std::move(c), std::nullopt, now_,
        "a hit of actor " + std::to_string(in.instigator) + " on the character"});
}

void AbilityPipeline::confirm_effect_hit(const combat::EffectHitNote &note, double now) {
  if (!combat_) return;
  advance(now);
  bootstrap::HitConfirm h;
  h.record_guid = note.hit_guid;
  h.source_type = content::kSourceTypeEffect;
  h.source_guid = note.effect_guid;
  h.source_is_actor = false;
  h.source = combat_->guid_of(note.holder);
  h.predicted_autonomous = note.predicted;
  h.predicted_simulated = note.predicted;
  bootstrap::HitTargetOut out;
  out.component = combat_->guid_of(note.holder);
  out.cylinder = combat_->hit_component_of(note.holder);
  const combat::TargetInfo ti = combat_->inspect(note.holder);
  out.has_impact = true;
  out.impact_location = {ti.position.x, ti.position.y, ti.position.z};
  const combat::Vec3 from = combat_->self().position();
  double nx = from.x - ti.position.x, ny = from.y - ti.position.y, nz = from.z - ti.position.z;
  const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
  out.impact_normal = len > 1e-6 ? decltype(out.impact_normal){nx / len, ny / len, nz / len}
                                 : decltype(out.impact_normal){0.0, 0.0, 1.0};
  out.flags = note.flags;
  for (size_t i = 0; i < note.mods.size(); ++i) {
    out.mods.push_back(
        {note.mods[i].stat_mod_guid, static_cast<float>(note.mods[i].value),
         static_cast<float>(i < note.effective.size() ? note.effective[i] : 0.0)});
  }
  h.targets.push_back(std::move(out));
  confirmations_.push_back(std::move(h));
  emit({AbilityChange{}, std::nullopt, now_,
        "an effect's hit on actor " + std::to_string(note.holder)});
}

void AbilityPipeline::confirm_hit_on(const combat::IncomingHit &in, double now,
                                     const protocol::emit::FIntrepidNetworkGUID &target,
                                     const protocol::emit::FIntrepidNetworkGUID &cylinder,
                                     double taken, bool lethal) {
  advance(now);
  bootstrap::HitConfirm h;
  h.record_guid = in.hit_guid;
  h.source_type = in.source_type;
  h.source_guid = in.source_guid;
  h.source_is_actor = true;
  h.predicted_autonomous = true;
  if (combat_) h.instigator = combat_->guid_of(in.instigator);
  bootstrap::HitTargetOut t;
  t.component = target;
  t.cylinder = cylinder;
  uint8_t flags = in.flags;
  if ((flags & stats::kHitMiss) == 0) Host::Damage::populate_mount_hit_mods(in, taken, t);
  if (lethal) flags |= stats::kHitLethal;
  t.flags = flags;
  h.targets.push_back(std::move(t));
  h.flags = flags;
  confirmations_.push_back(std::move(h));
  emit({AbilityChange{}, std::nullopt, now_,
        "a hit of actor " + std::to_string(in.instigator) + " on the character's mount"});
}

void AbilityPipeline::apply_hit(AbilityChange &c, const HitDef &hit, double t,
                                bool skip_resource, const HitSource &src) {
  if (!hits_caster(hit.row)) return;
  apply_hit_here(c, hit, t, skip_resource, src);
}

void AbilityPipeline::apply_hit_here(AbilityChange &c, const HitDef &hit, double t,
                                     bool skip_resource, const HitSource &src) {
  bool ok = true;
  std::string why;
  if (!eval_bool(hit.row.requirement_expr, true, ok, why) || !ok) return;
  if (!hit.remove_effects.empty() ||
      (hit.max_remove > 0 && hit.removal != HitDef::Removal::Unknown))
    remove_by_hit(c, hit);
  modify_cooldowns(c, hit, t);
  Host::Damage::apply_caster_hit_effects(*this, c, hit, t, skip_resource ? 2 : 1);
  std::vector<bootstrap::HitStatModOut> outs;
  HitEvent event;
  Host::Damage::apply_self_hit_mods(*this, c, hit, outs, event);
  drop_evaluation();
  if (Host::Damage::should_confirm_self_hit(hit, src, outs)) {
    bootstrap::HitConfirm h;
    h.record_guid = hit.row.guid;
    h.source_type = src.type;
    h.source_guid = src.guid;
    h.source_is_actor = src.actor;
    h.mods = std::move(outs);
    h.predicted_autonomous = src.autonomous;
    h.predicted_simulated = src.simulated;
    h.response = src.response;
    confirmations_.push_back(std::move(h));
  }
  Host::Damage::forward_recursive_pet_hits(*this, c, hit, t, src);
  event.hit = &hit;
  event.src = src;
  event.on_self = true;
  raise_hit_event(c, event, t);
}

void AbilityPipeline::effect_expires(size_t index, double t) {
  AbilityChange c;
  const EffectInst gone = effects_[index];
  remove_instance(c, index);
  run_effect_hits(c, gone, kExpirePhases, 2, t);
  recompute(c);
  emit({std::move(c), std::nullopt, t, "the effect " + gone.def->row.name + " ends"});
}

void AbilityPipeline::effect_ticks(size_t index, double t) {
  EffectInst &e = effects_[index];
  const double period = e.def->row.tick_timer;
  e.next_tick = period > 0 ? t + period : kInf;
  const EffectInst copy = e;
  AbilityChange c;
  Host::Damage::apply_infinite_owner_siblings(*this, c, copy, t);
  run_effect_hits(c, copy, kTickPhases, 1, t);
  recompute(c);
  emit({std::move(c), std::nullopt, t, "the effect " + copy.def->row.name + " ticks"});
}

bootstrap::CooldownItem AbilityPipeline::cooldown_item(int64_t ability,
                                                       const Group &g) const {
  bootstrap::CooldownItem cd;
  cd.ability_guid = ability;
  cd.triggered = true;
  cd.initial_cooldown = static_cast<float>(g.cooldown);
  cd.start_time = sync(g.chain_start);
  cd.end_time = cd.start_time + static_cast<double>(std::max(1, g.timers)) *
                                    static_cast<double>(cd.initial_cooldown);
  cd.charges = g.charges;
  return cd;
}

void AbilityPipeline::use_charge(AbilityChange &c, const Cast &cast, double t) {
  if (!(cast.triggers_cooldown && cast.cooldown > 0)) return;
  Group &g = groups_[cast.group];
  if (g.items.empty() && g.timer_end < 0) {
    g.charges = cast.max_charges;
    g.chain_start = t;
    g.timers = 0;
  }
  g.max_charges = cast.max_charges;
  g.charges = std::min(g.charges, g.max_charges);
  g.cooldown = cast.cooldown;
  g.charges = std::max(0, g.charges - cast.charge_cost);
  if (g.timer_end < 0) {
    g.timer_start = t;
    g.timer_end = t + g.cooldown;
    g.chain_start = t;
    g.timers = 1;
  }
  g.items.insert(cast.def->row.guid);
  g.last_ability = cast.def->row.guid;
  for (const int64_t a : g.items) c.cooldowns_set.push_back(cooldown_item(a, g));
}

void AbilityPipeline::group_timer_ends(const std::string &key, double t) {
  Group &g = groups_.at(key);
  AbilityChange c;
  g.charges = std::min(g.max_charges, g.charges + 1);
  if (g.charges < g.max_charges) {
    g.timer_start = g.timer_end;
    g.timer_end = g.timer_start + g.cooldown;
    ++g.timers;
    for (const int64_t a : g.items) c.cooldowns_set.push_back(cooldown_item(a, g));
    emit({std::move(c), std::nullopt, t, "a charge is back"});
    return;
  }
  for (const int64_t a : g.items) c.cooldowns_removed.push_back(a);
  groups_.erase(key);
  emit({std::move(c), std::nullopt, t, "the cooldown ends"});
}

int AbilityPipeline::charges_left(int64_t ability) const {
  const AbilityDef *def = catalog_ ? catalog_->ability(ability) : nullptr;
  if (!def) return -1;
  const auto it = groups_.find(group_key(def->row));
  return it == groups_.end() ? -1 : it->second.charges;
}

double AbilityPipeline::movement_allowance(double now) const {
  double speed = 0;
  for (const auto &[until, s] : movement_) {
    if (now <= until) speed = std::max(speed, s);
  }
  return speed;
}

void AbilityPipeline::regenerate(double t) {
  next_regen_ = t + kRegenInterval;
  if (dead_) return;
  AbilityChange c;
  Host::Damage::regenerate_single_vital(*this, c, content::kGuidHealth,
                                        content::kGuidHealthRegeneration,
                                        content::kGuidMaxHealth);
  Host::Damage::regenerate_single_vital(*this, c, content::kGuidMana,
                                        content::kGuidManaRegeneration,
                                        content::kGuidMaxMana);
  if (Host::Damage::regenerate_custom_resources(*this, c)) recompute(c);
  if (c.empty()) return;
  ++regen_ticks_;
  emit({std::move(c), std::nullopt, t, "Health and Mana regenerate"});
}

double AbilityPipeline::next_due() const {
  double due = next_regen_;
  for (const Cast &c : casts_) due = std::min(due, Host::Damage::cast_next_due(c));
  for (const CastRecord &r : records_) {
    for (const HitSlot &s : r.slots) due = std::min(due, slot_due(r, s));
  }
  for (const Held &h : held_) due = std::min(due, h.end_due.value_or(kInf));
  for (const Instance &i : instances_) due = std::min(due, i.end_due.value_or(kInf));
  for (const EffectInst &e : effects_) due = std::min({due, e.end, e.next_tick});
  for (const auto &[key, g] : groups_)
    due = std::min(due, g.timer_end >= 0 ? g.timer_end : kInf);
  for (const Zone &z : zones_) due = std::min(due, zone_due(z));
  return due;
}

void AbilityPipeline::advance(double now) {
  if (now < now_) now = now_;
  if (now - next_regen_ > kMaxRegenSteps * kRegenInterval)
    next_regen_ = now - (kMaxRegenSteps - 1) * kRegenInterval;
  for (size_t guard = 0; guard < kMaxStepsPerAdvance; ++guard) {
    const double t = next_due();
    if (t > now) break;
    run_due(t);
  }
  now_ = now;
  prune_records(now);
}

void AbilityPipeline::tick(double now) { advance(now); }

} // namespace aoc::worldd
