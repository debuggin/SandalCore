#include "session/ability_pipeline_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd {

using abilities::AbilityDef;
using abilities::EffectDef;
using abilities::HitDef;
using bootstrap::AbilityChange;

namespace {

constexpr double kPiRadians = 3.14159265358979323846;
// The capsule's centre is this high above the ground the character stands on: a
// Strike's arrows leave from the centre and end on the ground.
constexpr double kStandingCentreCm = 90.0;

} // namespace

struct AbilityPipeline::Host::Zones {
  struct StepHitCtx {
    AbilityChange &c;
    Cast &cast;
    const char *which = "";
    int depth = 0;
    bool combo_step = false;
    StepToken step{};
    double t = 0.0;
    std::vector<const HitDef *> fired{};
    std::vector<int64_t> fired_by{};
  };

  struct StrikeCtx {
    AbilityChange &c;
    Cast &cast;
    const Cast::Missile &m;
    double t = 0.0;
    int64_t projectile = 0;
    size_t hurt = 0;
  };

  static void abort_unpaid_cast(AbilityPipeline &self, Cast &cast, double t) {
    const AbilityDef &def = *cast.def;
    self.drop_record(cast.client_id);
    self.end_zones_of(cast, t);
    AbilityEvent e;
    if (self.item_instance_ && cast.instance >= 0 &&
        *self.item_instance_ == cast.instance) {
      e.change.abilities_removed.push_back(cast.instance);
      self.item_instance_.reset();
    }
    e.end_ability = cast.client_id;
    e.at = t;
    e.note = def.row.name + " cannot be paid for any more: the cast is over";
    e.refusal = true;
    ++self.refused_;
    cast.hit_due = cast.projectile_due = cast.loop_due = cast.own_due = std::nullopt;
    cast.branch_due.reset();
    cast.missiles.clear();
    self.events_.push_back(std::move(e));
  }

  static void end_looped_shots(Cast &cast, double now) {
    cast.branch_due = now + cast.looped->jump * cast.loop_scale;
    cast.end_due = *cast.branch_due + cast.looped->end * cast.loop_scale;
    const double jump_at = *cast.branch_due;
    cast.missiles.erase(
        std::remove_if(cast.missiles.begin(), cast.missiles.end(),
                       [jump_at](const Cast::Missile &m) {
                         return m.kind == Cast::Missile::Kind::Fly &&
                                m.depth == 0 && m.due >= jump_at;
                       }),
        cast.missiles.end());
  }

  static int64_t active_zone_id_for_hit(const AbilityPipeline &self,
                                        const Cast &cast, int64_t zone_guid) {
    int64_t id = 0;
    for (const Zone &z : self.zones_) {
      if (z.client_id == cast.client_id && z.ability == cast.def->row.guid &&
          z.zone == zone_guid && z.state != Zone::State::Ended)
        id = z.id;
    }
    return id;
  }

  static HitSource make_proj_source(int64_t guid, bool autonomous, bool simulated, bool source_is_target) {
    return {
        content::kSourceTypeProjectile, guid, false, true, false, simulated, autonomous, source_is_target,
    };
  }

  static void apply_initial_zone_hits(AbilityPipeline &self, AbilityChange &c,
                                      const Cast &cast, double t) {
    const AbilityDef &def = *cast.def;
    for (const content::AbilityHitRow &h : def.hits) {
      if (h.source != "zone" ||
          (h.role != "InitialHitsIds" && h.role != "OnBeginOverlapHitsIds"))
        continue;
      HitSource src;
      src.type = content::kSourceTypeZone;
      src.guid = h.owner_guid;
      src.confirm = true;
      self.zone_owner_ = active_zone_id_for_hit(self, cast, h.owner_guid);
      if (const HitDef *hit = self.catalog_->hit(h.hit_guid)) {
        self.mark_launched(cast.client_id);
        self.apply_hit(c, *hit, t, true, src);
      }
      self.zone_owner_ = 0;
    }
  }

  static void advance_looped_shot(AbilityPipeline &self, Cast &cast, double t) {
    ++cast.shots;
    if (cast.shot_projectile != 0) {
      const uint64_t aim = cast.target_actors.empty()
                               ? (self.active_target_ ? self.active_target_() : 0)
                               : cast.target_actors.front();
      if (cast.looped->fire > 0 && aim != 0) {
        const double fire_at =
            std::max(t, cast.trigger_at +
                            (cast.looped->fire + (cast.shots - 1) * cast.looped->period) *
                                cast.loop_scale);
        Cast::Missile m{fire_at, cast.shot_projectile, aim, Cast::Missile::Kind::Fly, cast.shots};
        cast.missiles.push_back(m);
      } else {
        cast.missiles.push_back({t + kProjectileDelay, cast.shot_projectile});
      }
    }
    if (self.can_pay(cast))
      cast.cost_due = t + cast.looped->period * cast.loop_scale;
    else
      end_looped_shots(cast, t);
  }

  static StepToken resolve_combo_step_token(const AbilityPipeline &self,
                                            const Cast &cast) {
    const AbilityDef &def = *cast.def;
    const content::MontageRow *montage = nullptr;
    for (const content::MontageRow &m : def.montages) {
      if (m.anim_index == cast.animation_index) montage = &m;
    }
    if (!montage && !def.montages.empty()) montage = &def.montages.front();
    return montage ? step_of_montage(*self.catalog_, *montage) : StepToken{};
  }

  static bool is_paired_combo_variant(const AbilityPipeline &self,
                                      const AbilityDef &def,
                                      const StepToken &mine) {
    bool paired = false;
    for (const content::AbilityHitRow &d : def.hits) {
      if (d.source != "hit") continue;
      const HitDef *direct = self.catalog_->hit(d.hit_guid);
      if (!direct || direct->fires.empty()) continue;
      const StepToken theirs = step_token(direct->row.name);
      paired = paired || (same_step(mine, theirs) && theirs.variant == mine.variant);
    }
    return paired;
  }

  static bool accept_combo_projectile_step(const AbilityPipeline &self,
                                           StepHitCtx &ctx, const HitDef &hit,
                                           int64_t owner_guid) {
    const StepToken mine = step_token(hit.row.name);
    if (!mine.ok || (ctx.step.ok && !same_step(ctx.step, mine))) return false;
    if (std::find(ctx.fired.begin(), ctx.fired.end(), &hit) != ctx.fired.end())
      return false;
    if (mine.variant != 0 && !is_paired_combo_variant(self, *ctx.cast.def, mine))
      return false;
    ctx.fired.push_back(&hit);
    ctx.fired_by.push_back(owner_guid);
    return true;
  }

  static bool resolve_step_hit_source(const AbilityPipeline &self,
                                      StepHitCtx &ctx,
                                      const content::AbilityHitRow &h,
                                      const HitDef &hit, HitSource &src) {
    if (std::string_view(ctx.which) == "hit") {
      if (hit.row.net_type.find("ServerVerify") != std::string::npos) return false;
      src = HitSource{};
      src.guid = ctx.cast.def->row.guid;
      src.actor = true;
      src.confirm = true;
      src.simulated = true;
      return true;
    }
    if (ctx.cast.chain_owner != 0 && h.owner_guid != ctx.cast.chain_owner)
      return false;
    if (ctx.combo_step && !accept_combo_projectile_step(self, ctx, hit, h.owner_guid))
      return false;
    src = make_proj_source(h.owner_guid, true, true, false);
    src.reach_free = ctx.cast.ball != nullptr && h.owner_guid == ctx.cast.ball_flight.projectile;
    return true;
  }

  static void apply_step_hit_row(AbilityPipeline &self, StepHitCtx &ctx,
                                 const content::AbilityHitRow &h) {
    if (h.source != ctx.which) return;
    const HitDef *hit = self.catalog_->hit(h.hit_guid);
    HitSource src;
    if (!hit || !resolve_step_hit_source(self, ctx, h, *hit, src)) return;
    const AbilityDef &def = *ctx.cast.def;
    bool landed = false;
    if (!ctx.cast.target_actors.empty() &&
        (hit->row.applicable_targets & (2 | 4 | 8 | 16)) != 0) {
      self.apply_hit_on_targets(ctx.c, *hit, ctx.cast.target_actors, ctx.t, src,
                                &def, &landed);
      if (landed && std::string_view(ctx.which) == "projectile" &&
          !hit->recursive.empty() && ctx.cast.target_actors.size() == 1) {
        self.bounce_from(ctx.c, ctx.cast, *hit, h.owner_guid,
                         ctx.cast.target_actors.front(), ctx.depth, ctx.t);
      }
    }
    const bool at_others = std::string_view(ctx.which) == "projectile" &&
                           !ctx.cast.target_actors.empty();
    if (self.hits_caster(hit->row) && !at_others)
      self.apply_hit(ctx.c, *hit, ctx.t, true, src);
  }

  static int64_t find_landed_combo_projectile(const StepHitCtx &ctx,
                                              const StepToken &mine) {
    for (size_t i = 0; i < ctx.fired.size(); ++i) {
      const StepToken theirs = step_token(ctx.fired[i]->row.name);
      if (same_step(mine, theirs) && mine.variant == theirs.variant)
        return ctx.fired_by[i];
    }
    return 0;
  }

  static const HitDef *resolve_combo_split_hit_def(
      const AbilityPipeline &self, const content::AbilityHitRow &h,
      const std::set<int64_t> &carried, std::set<int64_t> &done) {
    if (h.source != "hit" || carried.count(h.hit_guid) ||
        !done.insert(h.hit_guid).second)
      return nullptr;
    const HitDef *hit = self.catalog_->hit(h.hit_guid);
    if (!hit || hit->fires.empty() ||
        (hit->row.applicable_targets & (2 | 4 | 8 | 16)) == 0)
      return nullptr;
    return hit;
  }

  static void apply_combo_split_hits(AbilityPipeline &self, StepHitCtx &ctx) {
    if (!ctx.combo_step || ctx.fired.empty() || ctx.cast.target_actors.empty())
      return;
    const AbilityDef &def = *ctx.cast.def;
    std::set<int64_t> carried, done;
    for (const content::AbilityHitRow &h : def.hits) {
      if (h.source == "projectile")
        carried.insert(h.hit_guid);
    }
    for (const content::AbilityHitRow &h : def.hits) {
      const HitDef *hit = resolve_combo_split_hit_def(self, h, carried, done);
      const int64_t landed =
          hit ? find_landed_combo_projectile(ctx, step_token(hit->row.name))
              : 0;
      if (landed != 0) {
        self.apply_hit_on_targets(ctx.c, *hit, ctx.cast.target_actors, ctx.t,
                                  make_proj_source(landed, false, false, true),
                                  &def);
      }
    }
  }

  static void launch_timed_volley(AbilityPipeline &self, Cast &cast,
                                  const Cast::Missile &launch, double t) {
    const TimedCast &tc = *cast.timed;
    const int64_t projectile = projectile_of(*cast.def, tc.projectile);
    if (projectile == 0)
      return;
    const std::vector<combat::GatheredTarget> found =
        self.combat_->enemies_in_cone(tc.angle, tc.length, tc.behind,
                                      kMaxGathered);
    for (const combat::GatheredTarget &e : found)
      cast.missiles.push_back(
          {t + e.distance_cm / tc.speed, projectile, e.actor});
    spdlog::info("[abilities] {} (volley {} of {}): {} enem{} in the cone{}",
                 cast.def->row.name, launch.index + 1, tc.count, found.size(),
                 found.size() == 1 ? "y" : "ies",
                 gathered_log_suffix(found.size()));
  }

  static void launch_timed_barrage(AbilityPipeline &self, Cast &cast,
                                   const Cast::Missile &launch, double t) {
    const TimedCast &tc = *cast.timed;
    const int64_t projectile = projectile_of(*cast.def, tc.projectile);
    if (projectile == 0 || cast.target_actors.empty())
      return;
    const uint64_t target = cast.target_actors.front();
    const combat::TargetInfo ti = self.combat_->inspect(target);
    if (!ti.exists || !ti.alive)
      return;
    cast.missiles.push_back(
        {t + std::max(0.0, ti.surface_distance_cm) / tc.speed, projectile,
         target});
    if (launch.index == 0) {
      spdlog::info("[abilities] {}: {} arrows, one every {:.0f} ms, fly at the "
                   "target {:.1f} m away",
                   cast.def->row.name, tc.count, tc.gap * 1000.0,
                   ti.surface_distance_cm / 100.0);
    }
  }

  static void launch_timed_strike(AbilityPipeline &self, Cast &cast,
                                  const Cast::Missile &launch, double t) {
    const TimedCast &tc = *cast.timed;
    const combat::Fighter &me = self.combat_->self();
    const combat::Vec3 from = me.position();
    const double yaw = me.facing_degrees() * kPiRadians / 180.0;
    const double pitch =
        tc.pitch[std::clamp(launch.index, 0, 2)] * kPiRadians / 180.0;
    const double along = std::cos(pitch), down = -std::sin(pitch);
    if (down <= 1e-6)
      return;
    double x = from.x, y = from.y, ground = launch.z - kStandingCentreCm;
    for (int pass = 0; pass < 4; ++pass) {
      double g = 0;
      if (self.combat_->ground_height(x, y, ground, &g))
        ground = g;
      const double way = std::clamp((from.z - ground) / down, 0.0, 10000.0);
      x = from.x + std::cos(yaw) * along * way;
      y = from.y + std::sin(yaw) * along * way;
    }
    double g = 0;
    if (self.combat_->ground_height(x, y, ground, &g))
      ground = g;
    const double way = std::clamp((from.z - ground) / down, 0.0, 10000.0);
    Cast::Missile land{t + way / tc.speed, 0, 0, Cast::Missile::Kind::Strike,
                       launch.index,       0, x, y, ground};
    cast.missiles.push_back(land);
    spdlog::info(
        "[abilities] {} (arrow {} of {}): it leaves {:.1f} m up and lands "
        "{:.1f} m {} the caster after {:.0f} ms",
        cast.def->row.name, launch.index + 1, tc.count,
        (from.z - ground) / 100.0, std::hypot(x - from.x, y - from.y) / 100.0,
        along < -1e-6 ? "behind" : "ahead of", way / tc.speed * 1000.0);
  }

  static void push_empty_strike_confirm(AbilityPipeline &self,
                                        const HitDef &area,
                                        const HitSource &around) {
    if (area.mods.empty() && !area.row.has_fx)
      return;
    bootstrap::HitConfirm none;
    none.record_guid = area.row.guid;
    none.source_type = around.type;
    none.source_guid = around.guid;
    none.source_is_actor = false;
    none.no_target = true;
    none.predicted_autonomous = false;
    none.predicted_simulated = false;
    self.confirmations_.push_back(std::move(none));
  }

  static void apply_strike_recursive(AbilityPipeline &self, StrikeCtx &ctx,
                                     const HitDef &hit) {
    const TimedCast &tc = *ctx.cast.timed;
    const AbilityDef &def = *ctx.cast.def;
    for (const content::RecursiveHitRow &r : hit.recursive) {
      const HitDef *area = self.catalog_->hit(r.child_hit);
      if (!area)
        continue;
      const HitSource around =
          make_proj_source(ctx.projectile, false, false, false);
      std::vector<uint64_t> ids;
      for (const combat::GatheredTarget &e :
           self.combat_->enemies_around(combat::Vec3{ctx.m.x, ctx.m.y, ctx.m.z},
                                        tc.radius, kMaxGathered)) {
        if (ctx.cast.struck.insert(e.actor).second)
          ids.push_back(e.actor);
      }
      if (!ids.empty()) {
        ctx.hurt += ids.size();
        self.apply_hit_on_targets(ctx.c, *area, ids, ctx.t, around, &def);
        continue;
      }
      push_empty_strike_confirm(self, *area, around);
    }
  }

  static bool is_cast_cost_affordable(const AbilityPipeline &self,
                                      const Cast &cast) {
    for (const auto &[stat, amount] : cast.stat_costs) {
      if (self.stat_now(stat) < amount)
        return false;
    }
    for (const auto &[effect, n] : cast.effect_costs) {
      if (self.stacks_of(effect) < n)
        return false;
    }
    const auto g = self.groups_.find(cast.group);
    return !(g != self.groups_.end() && cast.triggers_cooldown &&
             g->second.charges < cast.charge_cost);
  }

  static void spend_cast_costs(AbilityPipeline &self, AbilityChange &c,
                               Cast &cast, double t) {
    bool spent_mana = false;
    for (const auto &[stat, amount] : cast.stat_costs) {
      self.take_stat(c, stat, amount);
      spent_mana = spent_mana || stat == content::kGuidMana;
    }
    for (const auto &[effect, n] : cast.effect_costs)
      self.remove_stacks(c, effect, n);
    self.drop_evaluation();
    cast.resource_added = 0;
    if (self.resource_ && spent_mana) {
      const int held = self.stacks_of(self.resource_->row.guid);
      self.apply_effect(c, *self.resource_, 1, std::nullopt, t);
      cast.resource_added =
          std::max(0, self.stacks_of(self.resource_->row.guid) - held);
    }
  }

  static bool is_cost_launched_cast(const Cast &cast) {
    return cast.def->hits.empty() && cast.def->zones.empty() &&
           cast.own_effects.empty() && !cast.toggle_off;
  }

  static std::set<int64_t>
  collect_shared_tag_owners(const AbilityPipeline &self,
                            const AbilityDef &def) {
    std::set<int64_t> others;
    if (is_none_tag(def.row.shared_cooldown_tag))
      return others;
    for (const EffectInst &e : self.effects_) {
      if (e.owner == 0 || e.owner == def.row.guid)
        continue;
      const AbilityDef *a = self.catalog_->ability(e.owner);
      if (a && a->row.shared_cooldown_tag == def.row.shared_cooldown_tag)
        others.insert(e.owner);
    }
    return others;
  }

  static void extend_record_for_zone(AbilityPipeline &self, int32_t client_id,
                                     const Zone &z, double life,
                                     double tick_rate) {
    CastRecord *rec = self.record_of(client_id);
    if (!rec)
      return;
    rec->window_end = std::max(rec->window_end, z.end_at + kZoneRequestSlack);
    rec->max_requests = std::max(
        rec->max_requests, static_cast<int>(life / std::max(0.25, tick_rate)) +
                               kZoneExtraRequests);
  }

  static size_t drop_pending_launches(std::vector<Cast::Missile> &missiles) {
    size_t kept = 0;
    for (const Cast::Missile &m : missiles) {
      if (m.kind != Cast::Missile::Kind::Launch)
        missiles[kept++] = m;
    }
    const size_t dropped = missiles.size() - kept;
    missiles.resize(kept);
    return dropped;
  }

  static void schedule_server_self_use_slots(AbilityPipeline &self,
                                             const Cast &cast, double t) {
    CastRecord *rec = self.record_of(cast.client_id);
    if (!rec) return;
    for (HitSlot &s : rec->slots) {
      if (s.server_self_use && s.due.empty() && s.requests == 0) {
        ++s.requests;
        s.last_request = t;
        s.due.push_back({t + kConfirmDelay, true, {}});
      }
    }
  }

  static void finish_toggle_off_cast(AbilityPipeline &self, AbilityChange &c,
                                     int64_t ability, double t) {
    self.end_loop(c, ability, t);
    self.held_.erase(
        std::remove_if(self.held_.begin(), self.held_.end(),
                       [&](const Held &h) { return h.ability == ability; }),
        self.held_.end());
  }
};

bool AbilityPipeline::can_pay(const Cast &cast) const {
  return Host::Zones::is_cast_cost_affordable(*this, cast);
}

void AbilityPipeline::end_zones_of(const Cast &cast, double t) {
  const int64_t ability = cast.def ? cast.def->row.guid : 0;
  for (size_t i = 0; i < zones_.size();) {
    Zone &z = zones_[i];
    if (z.client_id != cast.client_id || z.ability != ability ||
        z.state == Zone::State::Ended) {
      ++i;
    } else if (z.state == Zone::State::Pending) {
      zones_.erase(zones_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      z.end_at = std::min(z.end_at, t);
      ++i;
    }
  }
}

void AbilityPipeline::release_held(int32_t client_id, double t) {
  const double at = t + kHeldEndDelay;
  for (Held &h : held_) {
    if (h.client_id == client_id && !h.end_due) h.end_due = at;
  }
  for (Cast &c : casts_) {
    if (c.client_id == client_id && c.def && c.def->row.hold_animation &&
        !c.held_release_due)
      c.held_release_due = at;
  }
}

void AbilityPipeline::step_cost(Cast &cast, double t) {
  if (cast.looped && cast.shots > 0 && cast.instance >= 0 &&
      (!item_instance_ || *item_instance_ != cast.instance)) {
    Host::Zones::end_looped_shots(cast, t);
    return;
  }
  const AbilityDef &def = *cast.def;
  AbilityChange c;
  bool affordable = can_pay(cast);
  if (affordable && cast.item_cast && cast.item_cast->consumes && item_taker_)
    affordable = item_taker_(cast.client_id);
  if (CastRecord *rec = record_of(cast.client_id)) rec->cost_at = t;
  if (!affordable) {
    if (cast.looped && cast.shots > 0)
      Host::Zones::end_looped_shots(cast, t);
    else
      Host::Zones::abort_unpaid_cast(*this, cast, t);
    return;
  }
  cast.paid = true;
  Host::Zones::schedule_server_self_use_slots(*this, cast, t);
  Host::Zones::spend_cast_costs(*this, c, cast, t);
  if (!cast.toggle_off)
    raise_ability_event(c, def, t);
  else
    Host::Zones::finish_toggle_off_cast(*this, c, def.row.guid, t);
  use_charge(c, cast, t);
  if (Host::Zones::is_cost_launched_cast(cast)) mark_launched(cast.client_id);
  if (!cast.loop) Host::Zones::apply_initial_zone_hits(*this, c, cast, t);
  if (cast.looped) Host::Zones::advance_looped_shot(*this, cast, t);
  recompute(c);
  emit({std::move(c), std::nullopt, t,
        "the cost, cooldown and class resource of " + def.row.name});
}

void AbilityPipeline::step_own(Cast &cast, double t) {
  if (cast.held_release_due && *cast.held_release_due <= t) return;
  const ConsumedScope consumed(*this, cast.item_cast);
  const AbilityDef &def = *cast.def;
  AbilityChange c;
  bool started = false;
  for (const EffectDef *e : cast.own_effects) {
    bool applies = true;
    std::string why;
    if (!eval_bool(e->row.requirement_expr, true, applies, why) || !applies)
      continue;
    apply_effect(c, *e, 1, std::nullopt, t, false, def.row.guid);
    started = true;
  }
  if (started) mark_launched(cast.client_id);
  sync_engine_effects();
  if (def.row.hold_animation &&
      !(cast.held_release_due && *cast.held_release_due <= t)) {
    held_.erase(std::remove_if(
                    held_.begin(), held_.end(),
                    [&](const Held &h) { return h.ability == def.row.guid; }),
                held_.end());
    held_.push_back({cast.client_id, def.row.guid, cast.held_release_due});
    if (held_.size() > 16) held_.erase(held_.begin());
  }
  recompute(c);
  emit({std::move(c), std::nullopt, t, "the own effects of " + def.row.name + " start"});
}

void AbilityPipeline::step_loop(Cast &cast, double t) {
  const AbilityDef &def = *cast.def;
  AbilityChange c;
  // a song replaces the one that plays (the songs share a tag): its effects end
  for (const int64_t o : Host::Zones::collect_shared_tag_owners(*this, def))
    end_loop(c, o, t);
  for (const auto &[key, guid] : def.effects) {
    if (const EffectDef *e = catalog_->effect(guid))
      apply_effect(c, *e, 1, std::nullopt, t, false, def.row.guid);
  }
  recompute(c);
  emit({std::move(c), std::nullopt, t, "the effects of " + def.row.name + " start"});
}

void AbilityPipeline::spawn_zones(const Cast &cast, double open_at,
                                  double end_at) {
  if (cast.loop) return;
  for (const int64_t guid : cast.def->zones) {
    const content::ZoneRow *row = catalog_->zone(guid);
    if (!row) continue;
    Zone z;
    z.id = next_zone_id_++;
    z.ability = cast.def->row.guid;
    z.zone = guid;
    z.client_id = cast.client_id;
    z.open_at = open_at;
    const double life = row->life_time > 0 ? row->life_time : 30.0;
    z.end_at = end_at >= 0 ? std::max(end_at, open_at) : open_at + life;
    zones_.push_back(z);
    if (zones_.size() > kMaxZones) zones_.erase(zones_.begin());
    Host::Zones::extend_record_for_zone(*this, cast.client_id, z, life, row->tick_rate);
  }
}

void AbilityPipeline::run_zone(size_t index, double t) {
  Zone &z = zones_[index];
  AbilityEvent e;
  e.at = t;
  bootstrap::ZoneOp op;
  op.id = z.id;
  op.ability = z.ability;
  op.zone = z.zone;
  const AbilityDef *a = catalog_->ability(z.ability);
  const std::string who = a ? a->row.name : std::string("?");
  switch (z.state) {
  case Zone::State::Pending:
    z.state = Zone::State::Open;
    op.kind = bootstrap::ZoneOp::Kind::Open;
    e.zones.push_back(op);
    e.note = "the zone of " + who + " opens";
    emit(std::move(e));
    return;
  case Zone::State::Open: {
    AbilityChange c;
    end_zone_effects(c, z.id, t);
    recompute(c);
    z.state = Zone::State::Ended;
    z.fade_at = t + kZoneFadeDelay;
    z.close_at = t + kZoneCloseDelay;
    if (CastRecord *rec = record_of(z.client_id))
      rec->window_end =
          std::max(rec->cost_due + kRequestWindow, t + kZoneRequestSlack);
    e.change = std::move(c);
    e.note = "the dance of " + who + " ends";
    emit(std::move(e));
    return;
  }
  case Zone::State::Ended:
    if (z.fade_at >= 0 && z.fade_at <= t) {
      z.fade_at = -1;
      op.kind = bootstrap::ZoneOp::Kind::Fade;
      e.zones.push_back(op);
      e.note = "the zone of " + who + " fades out";
      emit(std::move(e));
      return;
    }
    op.kind = bootstrap::ZoneOp::Kind::Close;
    e.zones.push_back(op);
    e.note = "the zone of " + who + " closes";
    zones_.erase(zones_.begin() + static_cast<std::ptrdiff_t>(index));
    emit(std::move(e));
    return;
  }
}

void AbilityPipeline::step_hits(Cast &cast, double t, const char *which, int depth) {
  const CastRecord *fill_record = record_of(cast.client_id);
  const FillScope fill(*this, fill_record ? fill_record->fill : 1.0);
  const ConsumedScope consumed(*this, cast.item_cast);
  const AbilityDef &def = *cast.def;
  AbilityChange c;
  const bool combo_step =
      std::string_view(which) == "projectile" && is_weapon_combo(def.row);
  StepToken step = combo_step
                       ? Host::Zones::resolve_combo_step_token(*this, cast)
                       : StepToken{};
  if (combo_step && step.beam) {
    emit({std::move(c), std::nullopt, t,
          "the projectile step of " + def.row.name + " (a beam: nothing flies)"});
    return;
  }
  Host::Zones::StepHitCtx ctx{c, cast, which, depth, combo_step, step, t};
  for (const content::AbilityHitRow &h : def.hits)
    Host::Zones::apply_step_hit_row(*this, ctx, h);
  Host::Zones::apply_combo_split_hits(*this, ctx);
  recompute(c);
  emit({std::move(c), std::nullopt, t,
        std::string("the ") +
            (std::string_view(which) == "hit" ? "hits" : "projectile hits") +
            " of " + def.row.name});
}

void AbilityPipeline::start_timed(Cast &cast, const TimedCast &timed,
                                  double t) {
  cast.timed = &timed;
  if (projectile_of(*cast.def, timed.projectile) == 0 &&
      no_formula_.insert(cast.def->row.guid).second) {
    spdlog::warn("[abilities] {} lists no projectile with key {}: its launches do nothing",
                 cast.def->row.name, timed.projectile);
  }
  const combat::Vec3 at = combat_ ? combat_->self().position() : combat::Vec3{};
  for (int i = 0; i < timed.count; ++i) {
    cast.missiles.push_back({t + timed.first + timed.gap * i, 0, 0,
                             Cast::Missile::Kind::Launch, i, 0, at.x, at.y, at.z});
  }
  if (timed.jump_at > 0 && timed.end_at > timed.jump_at) {
    cast.montage_branch = true;
    cast.branch_position = timed.jump_position;
    cast.branch_predicted = true;
    cast.branch_due = t + timed.jump_at;
    cast.end_due = t + timed.end_at;
  }
}

void AbilityPipeline::step_timed(Cast &cast, const Cast::Missile &m, double t) {
  if (!combat_ || !cast.timed) return;
  if (m.kind == Cast::Missile::Kind::Launch)
    launch_timed(cast, m, t);
  else if (m.kind == Cast::Missile::Kind::Strike)
    land_strike(cast, m, t);
}

void AbilityPipeline::launch_timed(Cast &cast, const Cast::Missile &launch,
                                   double t) {
  if (cast.instance >= 0 && (!item_instance_ || *item_instance_ != cast.instance))
    return;
  switch (cast.timed->kind) {
  case TimedCast::Kind::Volley:
    Host::Zones::launch_timed_volley(*this, cast, launch, t);
    break;
  case TimedCast::Kind::Barrage:
    Host::Zones::launch_timed_barrage(*this, cast, launch, t);
    break;
  case TimedCast::Kind::Strike:
    Host::Zones::launch_timed_strike(*this, cast, launch, t);
    break;
  }
}

void AbilityPipeline::land_strike(Cast &cast, const Cast::Missile &m,
                                  double t) {
  const TimedCast &tc = *cast.timed;
  const AbilityDef &def = *cast.def;
  const int64_t projectile = projectile_of(def, tc.projectile + m.index);
  if (projectile == 0) return;
  AbilityChange c;
  Host::Zones::StrikeCtx ctx{c, cast, m, t, projectile, 0};
  for (const content::AbilityHitRow &h : def.hits) {
    if (h.source != "projectile" || h.owner_guid != projectile) continue;
    if (const HitDef *hit = catalog_->hit(h.hit_guid)) {
      apply_hit(c, *hit, t, true,
                Host::Zones::make_proj_source(projectile, false, true, false));
      Host::Zones::apply_strike_recursive(*this, ctx, *hit);
    }
  }
  spdlog::info("[abilities] {} (arrow {} of {}) lands at {:.1f} m from the caster: {} enem{} hurt",
               def.row.name, m.index + 1, tc.count,
               std::hypot(m.x - combat_->self().position().x,
                          m.y - combat_->self().position().y) /
                   100.0,
               ctx.hurt, ctx.hurt == 1 ? "y" : "ies");
  recompute(c);
  emit({std::move(c), std::nullopt, t,
        "arrow " + std::to_string(m.index + 1) + " of " + def.row.name + " lands"});
}

void AbilityPipeline::cancel_timed(int32_t client_instance_id, double now) {
  for (Cast &cast : casts_) {
    if (cast.client_id != client_instance_id || !cast.timed ||
        cast.timed->kind != TimedCast::Kind::Barrage)
      continue;
    const size_t dropped = Host::Zones::drop_pending_launches(cast.missiles);
    const TimedCast &tc = *cast.timed;
    if (cast.branch_due && now_ + kHeldEndDelay < *cast.branch_due) {
      cast.branch_due = now_ + kHeldEndDelay;
      cast.end_due = *cast.branch_due + (tc.end_at - tc.jump_at);
    }
    if (dropped > 0) {
      spdlog::info("[abilities] the key of {} went up {:.3f} s after the trigger: {} arrows stay in the quiver",
                   cast.def->row.name, now - cast.trigger_at, dropped);
    }
  }
}

} // namespace aoc::worldd
