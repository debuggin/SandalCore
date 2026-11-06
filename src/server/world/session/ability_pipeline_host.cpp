// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "session/ability_pipeline_internal.h"
#include "session/summoner_pets.h"

#include <cstdlib>

namespace aoc::worldd {

// Fallback class resource rule when content.db reactions are absent or disabled.
const ClassResource kClassResources[1] = {{2, "Bard_Resonance"}};

// Abilities whose cost step comes later than the montage's first notify in retail captures.
const Timing kTimings[9] = {
    {"Bard_Dance_Mesmerizing", 0.323, 1.011, -1},
    {"Bard_Saga", 0.315, 0.906, -1},
    {"Bard_Story_Joy", 0.312, 1.573, 0.0},
    {"Bard_Song_Anthem", 0.303, 1.904, -1},
    {"Bard_Song_Serenade", 0.303, 0.303, 0.307},
    {"WeaponCombo_Ranged_Book", 0.084, 0.084, -1},
    {"WeaponCombo_Ranged_Longbow", 0.088, 0.088, -1},
    {"WeaponCombo_Ranged_Shortbow", 0.258, 0.258, -1},
    {"WeaponCombo_Ranged_Wand", 0.107, 0.107, -1},
};

// Casts that end with a montage branch on client request or release deadline.
const BranchedCast kBranchedCasts[27] = {
    {"Mage_ArcaneVolley", 6064628901575655424ll, 2.0003722f, 0.099, 3.25, 0.082,
     0.069, 1.170},
    {"Ranger_ScatterShot", 6064630600000733338ll, 1.1363468f, 0.087, 0.100,
     0.120, 0.036, 1.275, -1, false, false, 0.0, 6064630644737576924ll, 3.0,
     0.100, 6064629886734172383ll, 0.082, 0.0},
    {"Cleric_BlessWeapon", 6064631002320929220ll, 1.8000f, 0.059, 3.05, 0.084,
     0.070, 1.765},
    {"Cleric_ChainsOfRestraint", 109341746222712ll, 1.6668f, 0.067, 5.20, 0.069,
     0.100, 1.285},
    {"Cleric_CommunalRestoration", 109382755440251ll, 1.8588f, 0.258, 4.45,
     0.232, 0.036, 1.062},
    {"Cleric_Condemn", 6064629326169899648ll, 1.4024f, 0.087, 4.30, 0.107,
     0.101, 0.903},
    {"Cleric_Judgment", 109313609651832ll, 2.9978f, 0.065, 9.15, 0.094, 0.068,
     1.237, 0, false},
    {"Cleric_Judgment", 109315044890234ll, 2.9981f, 0.070, 8.85, 0.089, 0.034,
     1.237, 1, false},
    {"Cleric_Purify", 6064630996510049918ll, 0.9975f, 0.302, 2.40, 0.315, 0.070,
     0.965},
    {"Cleric_ResplendentBeam", 6064629659855553923ll, 1.3333f, 0.071, 3.15,
     0.066, 0.168, 1.171, -1, false, true, 1.241},
    {"Cleric_Smite", 6064630407207993048ll, 0.8672f, 0.072, 2.75, 0.096, 0.000,
     1.236, 0, true},
    {"Mage_ArcaneCircle", 6064629093978406920ll, 1.7333f, 0.075, 2.10, 0.099,
     0.000, 0.768, -1, true},
    {"Mage_BallLightning", 6064629048751489028ll, 2.0000f, 0.090, 3.20, 0.061,
     0.035, 1.709},
    {"Mage_ChainLightning", 6064628934316589056ll, 2.3351f, 0.071, 4.10, 0.072,
     0.318, 0.705},
    {"Mage_Fireball", 6064631985904026106ll, 3.0000f, 0.070, 6.05, 0.066, 0.068,
     1.234},
    {"Mage_Frostbolt", 6064628890262700041ll, 2.3334f, 0.083, 4.15, 0.087,
     0.204, 0.701},
    {"Mage_LightningStrike", 6064628894971461633ll, 3.0000f, 0.089, 6.05, 0.080,
     0.102, 2.147, -1, false, true, 2.416},
    {"Mage_MagmaField", 6064629060044980230ll, 2.9327f, 0.076, 3.20, 0.065,
     0.036, 1.404},
    {"Mage_Slumber", 6064629064360525827ll, 2.7670f, 0.096, 5.40, 0.074, 0.333,
     2.175},
    {"Summoner_ExtractVessel", 6064633645892571971ll, 2.0000f, 0.051, 4.00,
     0.059, 0.070, 1.302},
    {"Summoner_MassAffliction", 6064633572821731033ll, 2.9998f, 0.074, 5.35,
     0.068, 0.101, 2.319},
    {"Summoner_MortalBlast", 6064633391727706791ll, 2.0000f, 0.064, 4.00, 0.058,
     0.199, 1.268},
    {"Summoner_Shear", 6064633578622266111ll, 1.2667f, 0.072, 2.65, 0.073,
     0.103, 0.867},
    {"Summoner_SiphonVessel", 6064633662084370415ll, 2.0000f, 0.059, 4.00,
     0.064, 0.070, 1.133},
    {"Summoner_SoulFlare", 6064633748263029144ll, 1.5003f, 0.053, 2.70, 0.052,
     0.400, 0.803},
    {"Summoner_SpectralBindings", 6064633702393973495ll, 2.4962f, 0.068, 3.85,
     0.067, 0.350, 1.235},
    {"Summoner_Void", 6064633583948214747ll, 3.0000f, 0.060, 5.35, 0.072, 0.167,
     2.139},
};

const GatheredCast kGatheredCasts[1] = {
    {"Ranger_ScatterShot", 10, 50.0, 2500.0, 250.0, 6000.0, 9, 3500.0, 150.0,
     7000.0,
     "/Game/Characters/Animations/00_ABLT/Ranger/"
     "ABLT_Scattershot_FullCharge_Montage.ABLT_Scattershot_FullCharge_Montage",
     10550296575633473351ull},
};

const TimedCast kTimedCasts[3] = {
    {"Ranger_AirStrike",
     TimedCast::Kind::Strike,
     1,
     1.0296,
     0.0882,
     3,
     4000.0,
     0.0,
     0.0,
     0.0,
     400.0,
     {-130.0, -90.0, -50.0}},
    {"Ranger_RainingDeath", TimedCast::Kind::Volley, 3, 0.6522, 0.2756, 2,
     6000.0, 70.0, 3000.0, 500.0},
    {"Ranger_Barrage",
     TimedCast::Kind::Barrage,
     1,
     0.2335,
     0.067,
     22,
     7000.0,
     0.0,
     0.0,
     0.0,
     0.0,
     {0.0, 0.0, 0.0},
     0.43836784f,
     1.70,
     2.55},
};

const MissileLanding kMissileLandings[7] = {
    {"Mage_ArcaneVolley", 1, 1.272}, {"Mage_ArcaneVolley", 2, 1.138},
    {"Mage_ArcaneVolley", 3, 1.338}, {"Mage_ArcaneVolley", 4, 1.405},
    {"Mage_ArcaneVolley", 5, 1.138}, {"Mage_ArcaneVolley", 6, 1.272},
    {"Mage_ArcaneVolley", 7, 0.940},
};

const Flight kFlights[12] = {
    {"Mage_ArcaneVolley", 1.000001, 5, 0.0083},
    {"Mage_ArcaneVolley", 1.000001, 7, 0.0716},
    {"Mage_ArcaneVolley", 1.000001, 2, 0.1192},
    {"Mage_ArcaneVolley", 1.000001, 3, 0.1683},
    {"Mage_ArcaneVolley", 1.000001, 6, 0.2145},
    {"Mage_ArcaneVolley", 1.000001, 4, 0.2512},
    {"Mage_ArcaneVolley", 1.000001, 1, 0.2821},
    {"Mage_Fireball", 2.0, 1, 0.0354},
    {"Mage_Frostbolt", 1.3337822, 1, 0.1650},
    {"Summoner_SoulFlare", 1.000645, 1, 0.1957},
    {"Cleric_Judgment", 2.9972165, 2, 0.0390, 0, 1.0e9},
    {"Cleric_Judgment", 2.9962265, 1, 0.0376, 1},
};

const LoopedCast kLoopedCasts[1] = {
    {"Ranger_Salvo", 0.43836784f, 0.083, 0.184, 0.2373, 0.150, 0.503,
     0.29427722},
};

const Dash kDashes[6] = {
    {"Bard_Misc_Flourish", 3659.0}, {"Ranger_Disengage", 2500.0},
    {"Tank_Charge", 5050.0},        {"Tank_Grapple", 1650.0},
    {"Summoner_Transpose", 1950.0}, {"Rogue_Shadowstep", 3000.0}};

const char *const kExpirePhases[2] = {"Expire", "End"};
const char *const kTickPhases[1] = {"Tick"};
const char *const kApplyPhases[1] = {"Apply"};

namespace {

constexpr bool timings_sorted() {
  for (size_t i = 1; i < std::size(kAbilityTimings); ++i) {
    const AbilityTiming &a = kAbilityTimings[i - 1];
    const AbilityTiming &b = kAbilityTimings[i];
    if (a.ability > b.ability || (a.ability == b.ability && a.aimed >= b.aimed))
      return false;
  }
  return true;
}
static_assert(timings_sorted(),
              "ability_timings.h is sorted by ability, then aimed");

constexpr bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

void apply_step_flags(std::string_view tok, StepToken &t) {
  if (tok == "Proc")
    t.proc = true;
  if (tok == "Finisher")
    t.finisher = true;
  if (tok == "Beam" || tok == "Beam2")
    t.beam = true;
}

int parse_step_digits(std::string_view tok, size_t &j) {
  int n = 0;
  while (j < tok.size() && is_ascii_digit(tok[j]) && n < 1000) {
    n = n * 10 + (tok[j++] - '0');
  }
  return n;
}

void parse_step_piece(std::string_view tok, size_t idx, int &last_digits,
                      StepToken &t) {
  apply_step_flags(tok, t);
  const size_t d = tok.find_first_of("0123456789");
  if (d == std::string_view::npos)
    return;
  if (tok == "2" && last_digits >= 0 &&
      idx == static_cast<size_t>(last_digits) + 1) {
    t.variant = 2;
    return;
  }
  size_t j = d;
  t.step = parse_step_digits(tok, j);
  if (j < tok.size() && tok[j] == '+')
    t.proc = true;
  last_digits = static_cast<int>(idx);
}

const content::MontageRow *find_combo_step_montage(const abilities::Catalog &cat, const abilities::AbilityDef &def, int step) {
  const content::MontageRow *take = nullptr;
  bool take_proc = true;
  for (const content::MontageRow &m : def.montages) {
    const StepToken s = step_of_montage(cat, m);
    if (!s.ok || s.step != step || (take && !(take_proc && !s.proc))) continue;
    take = &m;
    take_proc = s.proc;
  }
  return take;
}

struct SwingSlotFilter {
  bool ranged = false, beam = false;
  StepToken token{};
  std::set<int64_t> arrow_hits{};
};

SwingSlotFilter build_swing_slot_filter(const abilities::Catalog &cat, const abilities::AbilityDef &def, int32_t anim_index, double &min_gap) {
  SwingSlotFilter f{def.row.name.rfind("WeaponCombo_Ranged", 0) == 0};
  if (!is_weapon_combo(def.row)) return f;
  for (const content::MontageRow &m : def.montages) {
    if (m.anim_index != anim_index) continue;
    f.token = step_of_montage(cat, m);
    if (f.token.beam) { f.beam = true; min_gap = AbilityPipeline::kBeamRequestGap; }
  }
  for (const content::AbilityHitRow &h : def.hits) {
    if (h.source == "projectile") f.arrow_hits.insert(h.hit_guid);
  }
  return f;
}

bool allow_swing_slot_hit(const SwingSlotFilter &f, int64_t hit_guid, std::string_view hit_name, int &max_requests) {
  max_requests = -1;
  if (!f.ranged) return true;
  if (!f.beam || (f.arrow_hits.count(hit_guid) && !same_step(f.token, step_token(hit_name)))) return false;
  max_requests = beam_hit_requests(hit_name);
  return max_requests > 0;
}

} // namespace

std::string_view group_of_name(std::string_view name) {
  if (name == "Bard_Misc_Flourish")
    return "EmoteGroup";
  if (name.rfind("Bard_Music_", 0) == 0)
    return "SongGroup";
  return "DefaultGroup";
}

bool is_none_tag(const std::string &s) { return s.empty() || s == "None"; }

std::string group_key(const content::AbilityRow &row) {
  return is_none_tag(row.shared_cooldown_tag) ? std::to_string(row.guid)
                                              : row.shared_cooldown_tag;
}

const AbilityTiming *find_timing(int64_t ability, int aimed) {
  const AbilityTiming *lo = std::begin(kAbilityTimings);
  const AbilityTiming *hi = std::end(kAbilityTimings);
  for (const int kind : {aimed, 1 - aimed}) {
    const AbilityTiming *it = std::lower_bound(
        lo, hi, std::make_pair(ability, kind),
        [](const AbilityTiming &x, const std::pair<int64_t, int> &key) {
          return x.ability != key.first ? x.ability < key.first
                                        : x.aimed < key.second;
        });
    if (it != hi && it->ability == ability && it->aimed == kind)
      return it;
  }
  return nullptr;
}

StepToken step_token(std::string_view name) {
  StepToken t;
  if (name.rfind("DEPR_", 0) == 0)
    return t;
  if (name.size() > 8 && name.compare(name.size() - 8, 8, "_Montage") == 0)
    name.remove_suffix(8);
  int last_digits = -1;
  size_t idx = 0;
  for (size_t start = 0; start <= name.size(); ++idx) {
    const size_t end = name.find('_', start);
    const std::string_view tok = name.substr(
        start, (end == std::string_view::npos ? name.size() : end) - start);
    parse_step_piece(tok, idx, last_digits, t);
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  t.ok = true;
  return t;
}

bool same_step(const StepToken &a, const StepToken &b) {
  return a.ok && b.ok && a.step == b.step && a.proc == b.proc;
}

// Shortbow montage file names omit or misnumber the step; the select expression comment is authoritative.
bool step_of_expression(std::string_view text, int &step, bool &proc) {
  const size_t slashes = text.find("//");
  if (slashes == std::string_view::npos)
    return false;
  const size_t eol = text.find_first_of("\r\n", slashes);
  const std::string_view line = text.substr(
      slashes + 2, eol == std::string_view::npos ? std::string_view::npos
                                                 : eol - (slashes + 2));
  const size_t hit = line.find("Hit ");
  if (hit == std::string_view::npos || hit > 12)
    return false;
  size_t p = hit + 4;
  if (p >= line.size() || !is_ascii_digit(line[p]))
    return false;
  step = parse_step_digits(line, p);
  proc = line.compare(p, 5, " Proc") == 0;
  return true;
}

std::string_view leaf_of_path(const std::string &path) {
  const size_t dot = path.rfind('.');
  const size_t slash = path.rfind('/');
  const size_t start = std::max(dot == std::string::npos ? 0 : dot + 1,
                                slash == std::string::npos ? 0 : slash + 1);
  return std::string_view(path).substr(start);
}

bool is_weapon_combo(const content::AbilityRow &row) {
  for (const std::string &t : row.tags) {
    if (t == "ability.weaponcombo")
      return true;
  }
  return row.name.rfind("WeaponCombo_", 0) == 0;
}

bool is_melee_ability(const content::AbilityRow &row) {
  return row.name.rfind("WeaponCombo_Melee", 0) == 0 ||
         std::find(row.tags.begin(), row.tags.end(), "ability.Melee") !=
             row.tags.end();
}

bool is_melee_ability(const abilities::AbilityDef *def) {
  return def && is_melee_ability(def->row);
}

int64_t projectile_of(const abilities::AbilityDef &def, int key) {
  for (const content::AbilityHitRow &h : def.hits) {
    if (h.source == "projectile" && h.key == key)
      return h.owner_guid;
  }
  return 0;
}

int64_t first_projectile_guid(const abilities::AbilityDef &def) {
  for (const content::AbilityHitRow &h : def.hits) {
    if (h.source == "projectile")
      return h.owner_guid;
  }
  return 0;
}

int64_t projectile_hit_key(const abilities::AbilityDef &def,
                           int64_t projectile) {
  int64_t key = 0;
  for (const content::AbilityHitRow &h : def.hits) {
    if (h.source == "projectile" && h.owner_guid == projectile)
      key = h.key;
  }
  return key;
}

const char *gathered_log_suffix(size_t count) {
  if (count == 0)
    return ", nothing is hit";
  return count == 1 ? ", a projectile flies at it"
                    : ", a projectile flies at each";
}

double AbilityPipeline::Host::effect_count_by_instigator(
    const stats::RecordRef &r) {
  return p_.stacks_of(r.guid);
}

double AbilityPipeline::Host::rand_double() { return p_.rng_.rand_double(); }

double AbilityPipeline::Host::ability_progress(const std::string &group) {
  return p_.progress_of(group);
}

std::optional<bool> AbilityPipeline::Host::eval_target_program(
    AbilityPipeline &self, int program, combat::Fighter &tf,
    const combat::TargetInfo &ti, std::string &why) {
  std::string err;
  auto target_eval = tf.stats().evaluation(tf.worn(), err, true);
  if (!target_eval)
    return std::nullopt;
  const combat::EvalView self_view(*self.evaluation_);
  const combat::EvalView target_view(*target_eval);
  combat::HitContext ctx;
  ctx.instigator = &self_view;
  ctx.target = &target_view;
  ctx.distance_cm =
      ti.surface_distance_cm + ti.radius_cm + self.combat_->self().radius_cm();
  ctx.enemy = ti.relation == combat::Relation::Hostile;
  ctx.neutral = ti.relation == combat::Relation::Neutral;
  Host host(self);
  host.set_context(&ctx);
  double v = 0;
  if (!self.evaluation_->eval(program, host, v, err)) {
    why = err;
    return false;
  }
  return v != 0;
}

void AbilityPipeline::Host::populate_event_hit_context(
    const AbilityPipeline &self, const ReactionContext &rc,
    combat::HitContext &ctx) {
  if (!rc.hit || !rc.hit->hit)
    return;
  const abilities::HitDef &hd = *rc.hit->hit;
  if (hd.use_item_tags && self.weapon_tags_)
    ctx.tags = self.weapon_tags_();
  ctx.tags.insert(ctx.tags.end(), hd.tags.begin(), hd.tags.end());
  if (rc.hit->ability)
    ctx.tags.insert(ctx.tags.end(), rc.hit->ability->row.tags.begin(),
                    rc.hit->ability->row.tags.end());
  ctx.source_type = rc.hit->src.type;
  ctx.targets =
      static_cast<double>(rc.hit->targets.size() + (rc.hit->on_self ? 1 : 0));
  ctx.hit.flags = rc.hit->flags;
}

bool AbilityPipeline::Host::is_replicated_stat(const stats::StatValue &v) {
  if (v.float_array() || v.guid == content::kGuidStamina)
    return false;
  return v.replication == 1 || v.replication == 5 || v.replication == 7;
}

void AbilityPipeline::Host::upsert_stat_change(bootstrap::AbilityChange &c,
                                               const stats::StatValue &v) {
  for (bootstrap::StatChange &s : c.stats) {
    if (s.record != v.guid)
      continue;
    s.value = v.value;
    s.buckets = v.buckets();
    s.base = v.base;
    s.equipment = v.equip;
    return;
  }
  bootstrap::StatChange sc;
  sc.record = v.guid;
  sc.value = v.value;
  sc.buckets = v.buckets();
  sc.base = v.base;
  sc.equipment = v.equip;
  c.stats.push_back(sc);
}

void AbilityPipeline::Host::record_recomputed_stat(
    AbilityPipeline &self, bootstrap::AbilityChange &c,
    const stats::StatValue &v) {
  if (!is_replicated_stat(v))
    return;
  if (self.a7_.tracks(v.guid)) {
    self.a7_.gear_stat(c, v.guid, v.value);
    return;
  }
  upsert_stat_change(c, v);
}

void AbilityPipeline::Host::sync_sprint_mods(
    AbilityPipeline &self, int64_t sprint_guid,
    const abilities::EffectDef &sprint, double now) {
  for (const content::ModRow &m : sprint.mods) {
    if (m.value_expr == 0 || m.shield_absorb)
      continue;
    double v = 0;
    std::string why;
    if (self.eval(m.value_expr, v, why))
      self.a7_.set_effect_value(sprint_guid, m.stat_guid, v, now);
  }
}

void AbilityPipeline::Host::remove_guid_by_hit(AbilityPipeline &self,
                                               bootstrap::AbilityChange &c,
                                               int64_t guid, int &left) {
  for (size_t i = 0; i < self.effects_.size() && left > 0;) {
    if (self.effects_[i].def->row.guid != guid) {
      ++i;
      continue;
    }
    const int take = std::min(left, self.effects_[i].stacks);
    left -= take;
    if (take >= self.effects_[i].stacks) {
      self.remove_instance(c, i);
    } else {
      self.effects_[i].stacks -= take;
      c.effects_changed.push_back(self.effect_item(self.effects_[i]));
      ++i;
    }
  }
}

double AbilityPipeline::Host::other_fn(int fn, const std::string &name,
                                       const double *args, int) {
  switch (fn) {
  case stats::kHostRemainingCharges:
    return p_.remaining_charges(std::strtoll(name.c_str(), nullptr, 10));
  case stats::kHostApplicableTarget:
    return applicable_target(name);
  case stats::kHostMovementKey:
    return p_.moving_ ? 1.0 : 0.0;
  case stats::kHostStatDelta:
    return p_.stat_delta_over_time(std::strtoll(name.c_str(), nullptr, 10),
                                   args[0]);
  case stats::kHostNow:
    return p_.clock_;
  case stats::kHostProgressDuration:
    return name == "DefaultGroup" ? kCastSeconds : 0.0;
  case stats::kHostRandIntRange:
    return rand_int_range(args[0], args[1]);
  default:
    return reaction_fn(fn, name);
  }
}

bool AbilityPipeline::Host::pred_consumed_has_tag(const ReactionContext &rc,
                                                  const std::string &name) {
  if (!rc.consumed)
    return false;
  for (const std::string &t : rc.consumed->row.tags) {
    if (combat::tag_matches(t, name))
      return true;
  }
  return false;
}

bool AbilityPipeline::Host::pred_consumed_name_has(const ReactionContext &rc,
                                                   const std::string &name) {
  return rc.consumed && rc.consumed->row.name.find(name) != std::string::npos;
}

bool AbilityPipeline::Host::pred_consumed_is(const ReactionContext &rc,
                                             const std::string &name) {
  return rc.consumed &&
         rc.consumed->row.guid == std::strtoll(name.c_str(), nullptr, 10);
}

bool AbilityPipeline::Host::pred_hit_name_is(const ReactionContext &rc,
                                             const std::string &name) {
  return rc.hit && rc.hit->hit && rc.hit->hit->row.name == name;
}

bool AbilityPipeline::Host::pred_hit_is(const ReactionContext &rc,
                                        const std::string &name) {
  return rc.hit && rc.hit->hit &&
         rc.hit->hit->row.guid == std::strtoll(name.c_str(), nullptr, 10);
}

double AbilityPipeline::Host::rand_int_range(double a0, double a1) {
  const double lo = std::floor(a0);
  const double hi = std::floor(a1);
  return hi < lo ? lo
                 : lo + std::floor(p_.rng_.rand_double() * (hi - lo + 1.0));
}

double AbilityPipeline::Host::applicable_target(const std::string &name) const {
  if (!ctx_ || !ctx_->target)
    return static_cast<double>(name == "TARGET_Self");
  if (name == "TARGET_Enemy")
    return static_cast<double>(ctx_->enemy);
  if (name == "TARGET_Neutral")
    return static_cast<double>(ctx_->neutral);
  if (name == "TARGET_Self")
    return 0.0;
  return static_cast<double>(!ctx_->enemy && !ctx_->neutral);
}

double AbilityPipeline::Host::hit_stat_value(int fn,
                                             const std::string &name) const {
  if (!rc_ || !rc_->hit)
    return 0.0;
  const std::map<int64_t, double> &m =
      fn == stats::kHostHitStatMod ? rc_->hit->mods : rc_->hit->effective;
  const auto it = m.find(std::strtoll(name.c_str(), nullptr, 10));
  return it == m.end() ? 0.0 : it->second;
}

double AbilityPipeline::Host::eval_reaction_pred(
    bool (*pred)(const ReactionContext &, const std::string &),
    const std::string &name) const {
  return (rc_ && pred(*rc_, name)) ? 1.0 : 0.0;
}

void AbilityPipeline::Host::note_unserved_reaction(int fn,
                                                   const std::string &name) {
  static constexpr int kUnservedReactionFns[] = {
      stats::kHostEffectHasTag,   stats::kHostEffectIs,
      stats::kHostEffectDuration, stats::kHostBlackboardFloat,
      stats::kHostStatBase,       stats::kHostHitHasHitMod,
  };
  const auto *end = std::end(kUnservedReactionFns);
  if (std::find(std::begin(kUnservedReactionFns), end, fn) != end &&
      p_.unserved_host_.insert(fn).second) {
    spdlog::info(
        "[abilities] the host function {} ({}) is asked by an expression "
        "of a reaction and is not served yet: it answers 0",
        fn, name);
  }
}

double AbilityPipeline::Host::reaction_fn(int fn, const std::string &name) {
  if (fn == stats::kHostHitStatMod || fn == stats::kHostHitStatEffective)
    return hit_stat_value(fn, name);
  struct ReactionPred {
    int fn;
    bool (*pred)(const ReactionContext &rc, const std::string &name);
  };
  static constexpr ReactionPred kReactionPreds[] = {
      {stats::kHostConsumedAbilityHasTag, &pred_consumed_has_tag},
      {stats::kHostConsumedAbilityNameHas, &pred_consumed_name_has},
      {stats::kHostConsumedAbilityIs, &pred_consumed_is},
      {stats::kHostHitRecordNameIs, &pred_hit_name_is},
      {stats::kHostHitRecordIs, &pred_hit_is},
  };
  for (const ReactionPred &rp : kReactionPreds) {
    if (rp.fn == fn)
      return eval_reaction_pred(rp.pred, name);
  }
  note_unserved_reaction(fn, name);
  return 0.0;
}

bool AbilityPipeline::Host::has_pending_slot_requests(const CastRecord &rec) {
  for (const HitSlot &s : rec.slots) {
    if (!s.due.empty())
      return true;
  }
  return false;
}

bool AbilityPipeline::Host::has_pending_slot_requests(const CastRecord *rec) {
  return rec && has_pending_slot_requests(*rec);
}

void AbilityPipeline::Host::enqueue_effect_hits(const abilities::Catalog *cat,
                                                int64_t guid,
                                                std::set<int64_t> &seen,
                                                std::vector<int64_t> &todo) {
  if (!cat || !seen.insert(guid).second)
    return;
  if (const abilities::EffectDef *e = cat->effect(guid)) {
    for (const auto &[phase, hit] : e->hits)
      todo.push_back(hit);
  }
}

void AbilityPipeline::Host::enqueue_hit_effects(const abilities::Catalog *cat,
                                                int64_t g,
                                                std::set<int64_t> *out_effects,
                                                std::set<int64_t> &seen,
                                                std::vector<int64_t> &todo) {
  const abilities::HitDef *h = cat ? cat->hit(g) : nullptr;
  if (!h)
    return;
  for (const content::HitEffectRow &r : h->effects) {
    if (out_effects)
      out_effects->insert(r.effect_guid);
    enqueue_effect_hits(cat, r.effect_guid, seen, todo);
  }
}

std::set<int64_t> AbilityPipeline::Host::collect_ability_hit_graph(
    const abilities::Catalog *cat, const abilities::AbilityDef &def,
    std::set<int64_t> *out_effects) {
  std::set<int64_t> hits, seen;
  std::vector<int64_t> todo;
  for (const content::AbilityHitRow &h : def.hits)
    todo.push_back(h.hit_guid);
  for (const auto &[key, effect] : def.effects)
    enqueue_effect_hits(cat, effect, seen, todo);
  while (!todo.empty() && hits.size() < 512) {
    const int64_t g = todo.back();
    todo.pop_back();
    if (hits.insert(g).second)
      enqueue_hit_effects(cat, g, out_effects, seen, todo);
  }
  return hits;
}

bool AbilityPipeline::Host::flight_matches(const Cast &cast, const Flight &f) {
  return cast.def->row.name == f.ability &&
         (f.animation < 0 || f.animation == cast.animation_index);
}

const Flight *AbilityPipeline::Host::find_cast_flight(const Cast &cast) {
  for (const Flight &f : kFlights) {
    if (flight_matches(cast, f))
      return &f;
  }
  return nullptr;
}

bool AbilityPipeline::Host::resolve_flight_due(
    const FlightCalc &fc, int64_t key, const content::ProjectileRow &p,
    double &out_due) {
  for (const Flight &f : kFlights) {
    if (!flight_matches(fc.cast, f) || f.key != key)
      continue;
    double at = fc.cast.heard_at + f.fire / fc.rate +
                fc.distance / (f.speed > 0 ? f.speed : p.speed);
    if (fc.cast.cost_due)
      at = std::max(at, *fc.cast.cost_due);
    out_due = std::max(at, fc.t);
    return true;
  }
  return false;
}

void AbilityPipeline::stop_swing(int64_t instance) {
  for (size_t i = casts_.size(); i-- > 0;) {
    Cast &cast = casts_[i];
    if (cast.instance != instance || !is_weapon_combo(cast.def->row)) continue;
    if (cast.flight_pending) {
      cast.flight_pending = false;
      cast.projectile_due.reset();
    }
    const auto is_fly = [](const Cast::Missile &m) { return m.kind == Cast::Missile::Kind::Fly && m.depth == 0; };
    cast.missiles.erase(std::remove_if(cast.missiles.begin(), cast.missiles.end(), is_fly), cast.missiles.end());
    finish_cast_if_done(i);
  }
}

int AbilityPipeline::swing_montage(const abilities::AbilityDef &def, int requested, int &step) const {
  step = 0;
  if (!catalog_ || def.montages.empty() || def.row.name.rfind("WeaponCombo_Ranged", 0) != 0) return requested;
  const content::MontageRow *asked = nullptr;
  for (const content::MontageRow &m : def.montages) {
    if (m.anim_index == requested) asked = &m;
  }
  const auto last = swing_steps_.find(def.row.guid);
  if (asked) {
    const StepToken named = step_of_montage(*catalog_, *asked);
    if (!named.ok) return requested;
    if (last == swing_steps_.end() || named.step == 1 || named.step == last->second + 1) {
      step = named.step;
      return requested;
    }
  }
  int next = last == swing_steps_.end() ? 1 : last->second + 1;
  const content::MontageRow *take = find_combo_step_montage(*catalog_, def, next);
  if (!take) {
    next = 1;
    take = find_combo_step_montage(*catalog_, def, next);
  }
  if (!take) return requested;
  spdlog::info("[abilities] {}: the client named montage {}, but the combo is at step {}: the swing plays montage {}",
               def.row.name, requested, next, take->anim_index);
  step = next;
  return take->anim_index;
}

void AbilityPipeline::Host::register_cast_hit_slots(AbilityPipeline &self, const abilities::AbilityDef &def, const Cast &cast, CastRecord &rec) {
  const SwingSlotFilter filter = build_swing_slot_filter(*self.catalog_, def, cast.animation_index, rec.min_gap);
  std::set<int64_t> seen;
  for (const content::AbilityHitRow &h : def.hits) {
    if (h.source != "hit" || !seen.insert(h.hit_guid).second) continue;
    const abilities::HitDef *hit = self.catalog_->hit(h.hit_guid);
    if (!hit || hit->row.net_type.find("ServerVerify") == std::string::npos) continue;
    int max_requests = -1;
    if (!allow_swing_slot_hit(filter, h.hit_guid, hit->row.name, max_requests) || carried_by_ball(def, h.hit_guid)) continue;
    HitSlot slot;
    slot.hit = hit;
    slot.targeting = self.targeting_of(def, static_cast<int>(h.key));
    slot.max_requests = max_requests;
    slot.reaches = self.hits_caster(hit->row);
    slot.aims_others = self.combat_ != nullptr && (hit->row.applicable_targets & (2 | 4 | 8 | 16)) != 0;
    slot.server_self_use = pets::for_ability(def.row.guid) && hit->row.applicable_targets == 1 &&
                           hit->row.fallback_target.find("SelfOnly") != std::string::npos;
    rec.slots.push_back(std::move(slot));
  }
}

} // namespace aoc::worldd
