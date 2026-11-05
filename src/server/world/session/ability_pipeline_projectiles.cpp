// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.

#include "session/ability_pipeline_internal.h"

namespace aoc::worldd {

const ShotCast kShotCasts[37] = {
    {"Ranger_ScatterShot", -1, 0.300000, false,
     "Hit_Ranger_ScatterShot_Projectile", 0.088},
    {"Ranger_SniperShot", -1, 0.300000, false,
     "Hit_Ranger_SniperShot_Projectile", 0.082},
    {"Ranger_Powershot", -1, 0.300000, false, "Hit_Ranger_Powershot_Projectile",
     0.089},
    {"Ranger_SleepDart", -1, 0.166667, false, "Hit_Ranger_SleepDart_Projectile",
     0.128},
    {"Ranger_LightningReload", -1, 0.258709, false, "", 0},
    {"Mage_Fireball", -1, 0.200000, true, "Hit_Mage_Firebolt_Projectile",
     0.072},
    {"Mage_LightningStrike", -1, 0.200000, true,
     "Hit_Mage_LightningStrike_Projectile", 0.089},
    {"Bard_Mock", -1, 0.166667, true, "", 0},
    {"Bard_AnthemOfAlacrity_Melody_Pump", -1, 0.166667, true, "", 0},
    {"Bard_Shielding_Melody_Pump", -1, 0.166667, true, "", 0},
    {"Bard_Refreshing_Melody_Pump", -1, 0.166667, true, "", 0},
    {"Mage_Firebolt", 0, 0.200000, true, "", 0, 1},
    {"Mage_Firebolt", 1, 0.200000, true, "", 0, 2},
    {"Cleric_ResplendentBeam", -1, 0.562417, true, "", 0},
    {"Cleric_Mend", -1, 0.194506, true, "", 0},
    {"Rogue_Throw", -1, 0.100000, false, "", 0},
    {"WeaponCombo_Ranged_Book", 0, 0.160420, true, "", 0},
    {"WeaponCombo_Ranged_Book", 1, 0.199732, true, "", 0},
    {"WeaponCombo_Ranged_Book", 2, 0.201655, true, "", 0},
    {"WeaponCombo_Ranged_Book", 3, 0.198160, true, "", 0},
    {"WeaponCombo_Ranged_Book", 4, 0.332450, true, "", 0},
    {"WeaponCombo_Ranged_Book", 5, 0.333890, true, "", 0},
    {"WeaponCombo_Ranged_Book", 6, 0.335834, true, "", 0},
    {"WeaponCombo_Ranged_Longbow", 0, 0.506704, false, "", 0},
    {"WeaponCombo_Ranged_Longbow", 1, 0.500856, false, "", 0},
    {"WeaponCombo_Ranged_Longbow", 2, 0.436667, false, "", 0},
    {"WeaponCombo_Ranged_Longbow", 3, 0.566667, false, "", 0},
    {"WeaponCombo_Ranged_Longbow", 4, 0.625838, false, "", 0},
    {"WeaponCombo_Ranged_Shortbow", 0, 0.371158, false, "", 0},
    {"WeaponCombo_Ranged_Shortbow", 1, 0.369473, false, "", 0},
    {"WeaponCombo_Ranged_Shortbow", 2, 0.408469, false, "", 0},
    {"WeaponCombo_Ranged_Shortbow", 5, 0.500559, false, "", 0},
    {"WeaponCombo_Ranged_Wand", 0, 0.192013, true, "", 0},
    {"WeaponCombo_Ranged_Wand", 1, 0.199687, true, "", 0},
    {"WeaponCombo_Ranged_Wand", 2, 0.194391, true, "", 0},
    {"WeaponCombo_Ranged_Wand", 3, 0.351515, true, "", 0},
    {"WeaponCombo_Ranged_Wand", 4, 0.303399, true, "", 0},
};

const NoteCast kNoteCasts[23] = {
    {"Bard_Discordance", -1, 1, 0.100000, true},
    {"Bard_Discordance", -1, 2, 0.266667, true},
    {"Bard_Discordance", -1, 3, 0.433333, true},
    {"Bard_ResonantWeapon", -1, 1, 0.100000, true},
    {"Bard_ResonantWeapon", -1, 2, 0.266667, true},
    {"Bard_ResonantWeapon", -1, 3, 0.433333, true},
    {"WeaponCombo_Ranged_Shortbow", 3, 1, 0.391378, false},
    {"WeaponCombo_Ranged_Shortbow", 3, 2, 0.483226, false},
    {"WeaponCombo_Ranged_Shortbow", 3, 3, 0.573155, false},
    {"WeaponCombo_Ranged_Shortbow", 4, 1, 0.415800, false},
    {"WeaponCombo_Ranged_Shortbow", 4, 2, 0.525695, false},
    {"WeaponCombo_Ranged_Shortbow", 4, 3, 0.631772, false},
    {"WeaponCombo_Ranged_Shortbow", 6, 1, 0.168077, false},
    {"WeaponCombo_Ranged_Shortbow", 6, 2, 0.369325, false},
    {"WeaponCombo_Ranged_Shortbow", 6, 3, 0.576444, false},
    {"WeaponCombo_Ranged_Shortbow", 6, 4, 0.777815, false},
    {"WeaponCombo_Ranged_Shortbow", 6, 5, 1.073580, false},
    {"WeaponCombo_Ranged_Wand", 6, 1, 0.200000, true},
    {"WeaponCombo_Ranged_Wand", 6, 2, 0.333333, true},
    {"WeaponCombo_Ranged_Wand", 6, 3, 0.466667, true},
    {"WeaponCombo_Ranged_Wand", 7, 1, 0.233333, true},
    {"WeaponCombo_Ranged_Wand", 7, 2, 0.366667, true},
    {"WeaponCombo_Ranged_Wand", 7, 3, 0.500000, true},
};

const BounceRule kBounces[3] = {
    {"Hit_Bard_Discordance_Initial_Trigger", 2000.0, 2, true},
    {"Hit_Bard_ResonantWeapon_Bounce", 2000.0, 2, true},
    {"Hit_Rogue_Throw_Bounce", 1000.0, 2, false},
};

bool swing_flies(std::string_view ability, int animation) {
  for (const ShotCast &s : kShotCasts) {
    if (ability == s.ability && s.animation == animation)
      return true;
  }
  for (const NoteCast &n : kNoteCasts) {
    if (ability == n.ability && n.animation == animation)
      return true;
  }
  return false;
}

bool swing_flies(const std::string &ability, int animation) {
  return swing_flies(std::string_view(ability), animation);
}

namespace {

using abilities::AbilityDef;
using abilities::HitDef;
using bootstrap::AbilityChange;

int64_t find_keyed_projectile_owner(const AbilityDef &def, int key) {
  for (const content::AbilityHitRow &h : def.hits) {
    if (h.source == "projectile" && h.key == key)
      return h.owner_guid;
  }
  return 0;
}

bool hit_has_bounce_child(const abilities::Catalog &cat, const HitDef &hit) {
  for (const content::RecursiveHitRow &r : hit.recursive) {
    const HitDef *child = cat.hit(r.child_hit);
    if (!child)
      continue;
    for (const BounceRule &b : kBounces) {
      if (child->row.name == b.child_hit)
        return true;
    }
  }
  return false;
}

int64_t find_bounce_launched_projectile(const abilities::Catalog &cat,
                                        const AbilityDef &def) {
  bool bounces = false;
  int64_t launched = 0;
  for (const content::AbilityHitRow &h : def.hits) {
    if (h.source != "projectile")
      continue;
    const content::ProjectileRow *q = cat.projectile(h.owner_guid);
    if (launched == 0 && q && q->speed > 0)
      launched = h.owner_guid;
    const HitDef *hit = cat.hit(h.hit_guid);
    if (hit && hit_has_bounce_child(cat, *hit))
      bounces = true;
  }
  return bounces ? launched : 0;
}

const content::ProjectileRow *
find_shot_projectile_row(const abilities::Catalog &cat, const AbilityDef &def,
                         int64_t chain_owner) {
  for (const content::AbilityHitRow &h : def.hits) {
    if (h.source != "projectile")
      continue;
    if (chain_owner != 0 && h.owner_guid != chain_owner)
      continue;
    const content::ProjectileRow *q = cat.projectile(h.owner_guid);
    if (q && q->speed > 0)
      return q;
  }
  return nullptr;
}

const BounceRule *find_bounce_rule(const std::string &child_name) {
  for (const BounceRule &b : kBounces) {
    if (child_name == b.child_hit)
      return &b;
  }
  return nullptr;
}

const char *note_missile_label(bool looped, const content::AbilityRow &row,
                               int depth) {
  if (looped || is_weapon_combo(row))
    return "arrow";
  return depth == 0 ? "note" : "bounce";
}

double note_flight_distance_cm(const combat::TargetInfo &info, int depth,
                               double from_x, double from_y) {
  if (depth == 0)
    return info.surface_distance_cm + kMuzzleCm;
  return std::max(0.0,
                  std::hypot(info.position.x - from_x, info.position.y - from_y) -
                      info.radius_cm);
}

} // namespace

struct AbilityPipeline::Host::Projectiles {
  struct BounceStepCtx {
    AbilityChange &c;
    Cast &cast;
    int64_t projectile = 0;
    uint64_t target = 0;
    int depth = 0;
    double t = 0.0;
  };

  static bool proj_apply_missile_flights(const AbilityPipeline &self,
                                         Cast &cast, const FlightCalc &fc,
                                         double &first, double &last) {
    bool any = false;
    for (Cast::Missile &m : cast.missiles) {
      if (m.kind != Cast::Missile::Kind::Land)
        continue;
      const content::ProjectileRow *p = self.catalog_->projectile(m.projectile);
      if (!p || !(p->speed > 0))
        continue;
      const int64_t key = projectile_hit_key(*cast.def, m.projectile);
      if (resolve_flight_due(fc, key, *p, m.due)) {
        first = std::min(first, m.due);
        last = std::max(last, m.due);
        any = true;
      }
    }
    return any;
  }

  static bool proj_apply_cast_flight(const AbilityPipeline &self, Cast &cast,
                                     const FlightCalc &fc, double &first,
                                     double &last) {
    if (!cast.projectile_due)
      return false;
    for (const content::AbilityHitRow &h : cast.def->hits) {
      const content::ProjectileRow *p =
          h.source == "projectile" ? self.catalog_->projectile(h.owner_guid)
                                   : nullptr;
      double due = 0.0;
      if (p && p->speed > 0 && resolve_flight_due(fc, h.key, *p, due)) {
        cast.projectile_due = first = last = due;
        return true;
      }
    }
    return false;
  }

  static void proj_append_fired_straight(AbilityPipeline &self,
                                         const HitDef &hit, int64_t guid,
                                         double t, Cast &wave) {
    const content::ProjectileRow *p = self.catalog_->projectile(guid);
    if (!p || p->speed <= 0)
      return;
    if (p->projectile_type.find("Straight") == std::string::npos) {
      if (self.no_formula_.insert(guid).second) {
        spdlog::info("[abilities] the hit {} fires {}, a {} projectile: only "
                     "straight ones are made",
                     hit.row.name, p->name, p->projectile_type);
      }
      return;
    }
    const std::vector<combat::GatheredTarget> found =
        self.combat_->enemies_in_line(p->range, p->sphere_radius, kMaxGathered);
    for (const combat::GatheredTarget &e : found)
      wave.missiles.push_back({t + e.distance_cm / p->speed, guid, e.actor});
    spdlog::info("[abilities] {} ({}): a straight projectile of {:.0f} cm/s "
                 "flies {:.0f} m ahead: {} enem{} in its way",
                 wave.def->row.name, hit.row.name, p->speed, p->range / 100.0,
                 found.size(), found.size() == 1 ? "y" : "ies");
  }

  static bool proj_can_bounce_to(AbilityPipeline &self,
                                 const BounceStepCtx &ctx,
                                 const BounceRule &rule, uint64_t actor,
                                 combat::TargetInfo &out_ti) {
    if (actor == ctx.target || (!rule.random && ctx.cast.bounced.count(actor)))
      return false;
    out_ti = self.combat_->inspect(actor);
    const double reach = self.combat_->reach_cm(false) +
                         self.combat_->data().activation_leeway_cm();
    return out_ti.exists && out_ti.alive && out_ti.surface_distance_cm <= reach;
  }

  static std::vector<uint64_t>
  proj_gather_bounce_pool(AbilityPipeline &self, const BounceStepCtx &ctx,
                          const BounceRule &rule, const combat::Vec3 &from_pos,
                          uint64_t &out_nearest) {
    const combat::Vec3 me = self.combat_->self().position();
    std::vector<uint64_t> pool;
    double best = 0;
    for (const combat::GatheredTarget &e :
         self.combat_->enemies_around(from_pos, rule.radius, 32)) {
      combat::TargetInfo ti;
      if (!proj_can_bounce_to(self, ctx, rule, e.actor, ti))
        continue;
      pool.push_back(e.actor);
      const double score =
          e.distance_cm +
          std::hypot(ti.position.x - me.x, ti.position.y - me.y);
      if (out_nearest == 0 || score < best) {
        out_nearest = e.actor;
        best = score;
      }
    }
    return pool;
  }

  static void proj_run_bounce_child(AbilityPipeline &self, BounceStepCtx &ctx,
                                    const HitDef &child,
                                    const BounceRule &rule) {
    const content::ProjectileRow *flying =
        self.catalog_->projectile(child.fires.front());
    const combat::TargetInfo from = self.combat_->inspect(ctx.target);
    if (!flying || !(flying->speed > 0) || !from.exists)
      return;
    ctx.cast.bounced.insert(ctx.target);
    uint64_t nearest = 0;
    const std::vector<uint64_t> pool =
        proj_gather_bounce_pool(self, ctx, rule, from.position, nearest);
    if (pool.empty()) {
      spdlog::info("[abilities] {}: no enemy within {:.0f} m of the one just "
                   "hit: no bounce",
                   ctx.cast.def->row.name, rule.radius / 100.0);
      return;
    }
    const uint64_t pick =
        rule.random
            ? pool[static_cast<size_t>(self.bounce_rng_() % pool.size())]
            : nearest;
    HitSource src;
    src.type = content::kSourceTypeProjectile;
    src.guid = ctx.projectile;
    src.actor = false;
    src.confirm = true;
    src.simulated = false;
    src.autonomous = false;
    src.source_actor = ctx.target;
    self.apply_hit_on_targets(ctx.c, child, {pick}, ctx.t, src, ctx.cast.def);
    if (!rule.random)
      ctx.cast.bounced.insert(pick);
    Cast::Missile fly;
    fly.kind = Cast::Missile::Kind::Fly;
    fly.due = ctx.t;
    fly.projectile = child.fires.front();
    fly.target = pick;
    fly.index = ctx.depth + 1;
    fly.depth = ctx.depth + 1;
    fly.x = from.position.x;
    fly.y = from.position.y;
    fly.z = from.position.z;
    ctx.cast.missiles.push_back(fly);
  }
};

double AbilityPipeline::client_rate(bool magical) const {
  const double r = stat_now(magical ? kStatMagicalSpeed : kStatPhysicalSpeed);
  return (r > 0.2 && r < 3.0) ? r : 1.0;
}

bool AbilityPipeline::plays_at_magical(const AbilityDef &def,
                                       bool row_magical) const {
  if (!is_weapon_combo(def.row))
    return row_magical;
  const int cls = stats_.class_enum();
  return cls == 2 || cls == 3 || cls == 6 || cls == 8;
}

double AbilityPipeline::item_rate(const AbilityDef &def) const {
  const std::string &name = def.row.name;
  for (const ShotCast &s : kShotCasts) {
    if (name == s.ability)
      return client_rate(plays_at_magical(def, s.magical));
  }
  for (const NoteCast &n : kNoteCasts) {
    if (name == n.ability)
      return client_rate(plays_at_magical(def, n.magical));
  }
  for (const LoopedCast &l : kLoopedCasts) {
    if (name == l.ability)
      return client_rate(false);
  }
  return 1.0;
}

void AbilityPipeline::plan_shot(Cast &cast, const ShotCast &shot, double t) {
  if (!combat_ || !cast.cost_due)
    return;
  if (cast.target_actors.empty() && !(cast.target && cast.self_reachable))
    return;
  cast.shot = &shot;
  cast.flight_pending = true;
  cast.launch_at =
      t + shot.fire / client_rate(plays_at_magical(*cast.def, shot.magical));
  cast.projectile_due = cast.launch_at;
  if (shot.key != 0 && cast.chain_owner == 0)
    cast.chain_owner = find_keyed_projectile_owner(*cast.def, shot.key);
  if (cast.chain_owner == 0)
    cast.chain_owner = find_bounce_launched_projectile(*catalog_, *cast.def);
}

void AbilityPipeline::land_shot(Cast &cast, double t) {
  if (!cast.cost_due)
    return;
  cast.projectile_due =
      std::max({cast.launch_at + cast.flight, *cast.cost_due, t});
}

void AbilityPipeline::launch_shot(Cast &cast, double t) {
  cast.flight_pending = false;
  const combat::TargetInfo info =
      (cast.target_actors.empty() || !combat_)
          ? combat::TargetInfo{}
          : combat_->inspect(cast.target_actors.front());
  const content::ProjectileRow *p =
      find_shot_projectile_row(*catalog_, *cast.def, cast.chain_owner);
  const double rate =
      client_rate(plays_at_magical(*cast.def, cast.shot->magical));
  if (!p || !info.exists || !info.alive) {
    cast.flight = 0;
    land_shot(cast, t);
    spdlog::info(
        "[abilities] {}: the projectile leaves {:.3f} s after the trigger (the "
        "client plays at {:.3f}); {}: it lands as it leaves",
        cast.def->row.name, cast.launch_at - cast.trigger_at, rate,
        cast.target_actors.empty() ? "the cast is at the caster"
                                   : "its target is not there");
    return;
  }
  const double distance = info.surface_distance_cm + kMuzzleCm;
  cast.flight = std::min(distance / p->speed, kMaxShotFlight);
  land_shot(cast, t);
  spdlog::info(
      "[abilities] {}: the projectile leaves {:.3f} s after the trigger (the "
      "client plays at {:.3f}{}); the target is {:.1f} m away: it lands "
      "{:.3f} s after the launch",
      cast.def->row.name, cast.launch_at - cast.trigger_at, rate,
      cast.anchored ? ", the launch is the client's request" : "",
      distance / 100.0, *cast.projectile_due - cast.launch_at);
}

void AbilityPipeline::anchor_shot(Cast &cast, double now) {
  cast.anchored = true;
  const double was = cast.launch_at;
  const double rate =
      client_rate(plays_at_magical(*cast.def, cast.shot->magical));
  cast.launch_at = std::max(
      {now - cast.shot->ask_gap / rate, cast.trigger_at, was - kLaunchAdvance});
  if (cast.flight_pending)
    cast.projectile_due = std::max(now, cast.launch_at);
  else if (cast.projectile_due)
    land_shot(cast, now);
  spdlog::info("[abilities] {}: the client asked for the hit that goes with "
               "the launch {:.3f} s after the trigger: the projectile {} "
               "{:.3f} s from the plan",
               cast.def->row.name, now - cast.trigger_at,
               cast.launch_at < was ? "left" : "leaves",
               std::fabs(cast.launch_at - was));
}

void AbilityPipeline::plan_notes(Cast &cast, double t) {
  if (!combat_ || cast.target_actors.empty() || !cast.cost_due)
    return;
  std::vector<Cast::Missile> notes;
  double rate = 1.0;
  for (const NoteCast &n : kNoteCasts) {
    if (cast.def->row.name != n.ability)
      continue;
    if (n.animation >= 0 && n.animation != cast.animation_index)
      continue;
    const int64_t projectile = find_keyed_projectile_owner(*cast.def, n.key);
    if (projectile == 0)
      continue;
    Cast::Missile fly;
    fly.kind = Cast::Missile::Kind::Fly;
    rate = client_rate(plays_at_magical(*cast.def, n.magical));
    fly.due = t + n.fire / rate;
    fly.projectile = projectile;
    fly.target = cast.target_actors.front();
    fly.index = static_cast<int>(notes.size()) + 1;
    notes.push_back(fly);
  }
  if (notes.empty())
    return;
  for (const Cast::Missile &m : notes)
    cast.missiles.push_back(m);
  cast.projectile_due.reset();
  spdlog::info("[abilities] {}: {} projectiles leave {:.3f} to {:.3f} s after "
               "the trigger (the client plays at {:.3f}); each lands when it "
               "gets to the target",
               cast.def->row.name, notes.size(), notes.front().due - t,
               notes.back().due - t, rate);
}

void AbilityPipeline::launch_note(Cast &cast, const Cast::Missile &fly,
                                  double t) {
  if (!combat_)
    return;
  if (fly.depth == 0 && cast.instance >= 0 &&
      (!item_instance_ || *item_instance_ != cast.instance))
    return;
  const char *what =
      note_missile_label(cast.looped != nullptr, cast.def->row, fly.depth);
  const content::ProjectileRow *p = catalog_->projectile(fly.projectile);
  const combat::TargetInfo info = combat_->inspect(fly.target);
  if (!p || !(p->speed > 0) || !info.exists || !info.alive) {
    spdlog::info("[abilities] {}: {} {} finds nobody at its target",
                 cast.def->row.name, what, fly.index);
    return;
  }
  const double distance =
      note_flight_distance_cm(info, fly.depth, fly.x, fly.y);
  Cast::Missile land;
  land.kind = Cast::Missile::Kind::Land;
  land.projectile = fly.projectile;
  land.target = fly.target;
  land.index = fly.index;
  land.depth = fly.depth;
  land.due = t + distance / p->speed;
  if (cast.cost_due && !cast.looped)
    land.due = std::max(land.due, *cast.cost_due);
  cast.missiles.push_back(land);
  spdlog::info("[abilities] {}: {} {} leaves {:.3f} s after the trigger; the "
               "{} is {:.1f} m away: it lands {:.3f} s after it leaves",
               cast.def->row.name, what, fly.index, t - cast.trigger_at,
               fly.depth == 0 ? "target" : "next enemy", distance / 100.0,
               land.due - t);
}

void AbilityPipeline::bounce_from(AbilityChange &c, Cast &cast,
                                  const HitDef &hit, int64_t projectile,
                                  uint64_t target, int depth, double t) {
  if (!combat_)
    return;
  Host::Projectiles::BounceStepCtx ctx{c, cast, projectile, target, depth, t};
  for (const content::RecursiveHitRow &r : hit.recursive) {
    const HitDef *child = catalog_->hit(r.child_hit);
    if (!child || child->fires.empty())
      continue;
    const BounceRule *rule = find_bounce_rule(child->row.name);
    if (!rule || depth >= rule->max_bounces)
      continue;
    Host::Projectiles::proj_run_bounce_child(*this, ctx, *child, *rule);
  }
}

void AbilityPipeline::land_by_flight(Cast &cast, double t) {
  if (!combat_ || !cast.branched || cast.target_actors.empty() ||
      cast.heard_at < 0)
    return;
  const Flight *plan = Host::find_cast_flight(cast);
  if (!plan)
    return;
  const combat::TargetInfo info = combat_->inspect(cast.target_actors.front());
  if (!info.exists || !info.alive)
    return;
  const double distance = info.surface_distance_cm + kMuzzleCm;
  const double rate =
      plan->bar / std::clamp(cast.heard_at - cast.trigger_at, 0.3, 8.0);
  const Host::FlightCalc fc{cast, distance, rate, t};
  double first = 1e18;
  double last = 0.0;
  bool any =
      Host::Projectiles::proj_apply_missile_flights(*this, cast, fc, first, last);
  if (!any)
    any = Host::Projectiles::proj_apply_cast_flight(*this, cast, fc, first, last);
  if (any) {
    spdlog::info("[abilities] {}: the target is {:.1f} m away: the projectiles "
                 "land {:.3f} to {:.3f} s after the cast bar's end",
                 cast.def->row.name, distance / 100.0, first - cast.heard_at,
                 last - cast.heard_at);
  }
}

void AbilityPipeline::launch_gathered(Cast &cast, double t) {
  if (!combat_ || !cast.gathered)
    return;
  const GatheredCast &g = *cast.gathered;
  const bool charged = cast.charged && cast.charged_projectile != 0;
  const std::vector<combat::GatheredTarget> found =
      charged
          ? combat_->enemies_in_line(g.charged_length, g.charged_radius,
                                     kMaxGathered)
          : combat_->enemies_in_cone(g.angle, g.length, g.behind, kMaxGathered);
  const int64_t projectile =
      charged ? cast.charged_projectile : cast.gathered_projectile;
  const double speed = charged ? g.charged_speed : g.speed;
  for (const combat::GatheredTarget &e : found)
    cast.missiles.push_back({t + e.distance_cm / speed, projectile, e.actor});
  spdlog::info("[abilities] {} ({}): {} enem{} ahead of the caster{}",
               cast.def->row.name, charged ? "the charged shot" : "the scatter",
               found.size(), found.size() == 1 ? "y" : "ies",
               gathered_log_suffix(found.size()));
}

void AbilityPipeline::fire_from_hit(const CastRecord &rec, const HitDef &hit,
                                    double t) {
  if (!combat_ || hit.fires.empty() || dead_)
    return;
  std::string why;
  bool ok = true;
  const bool allowed = eval_bool(hit.row.requirement_expr, true, ok, why) && ok;
  drop_evaluation();
  if (!allowed)
    return;
  const AbilityDef *def = catalog_->ability(rec.ability);
  if (!def)
    return;
  Cast wave;
  wave.client_id = rec.client_id;
  wave.def = def;
  wave.trigger_at = rec.trigger_at;
  for (const int64_t guid : hit.fires)
    Host::Projectiles::proj_append_fired_straight(*this, hit, guid, t, wave);
  if (!wave.missiles.empty())
    casts_.push_back(std::move(wave));
}

} // namespace aoc::worldd
