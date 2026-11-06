// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "session/ability_pipeline.h"

#include "bootstrap/constants.h"
#include "content/content_db.h"
#include "session/ability_timings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace aoc::worldd {

inline constexpr double kInf = std::numeric_limits<double>::infinity();
inline constexpr double kHoldStart = 0.2;
inline constexpr double kMuzzleCm = 40.0;
inline constexpr int64_t kStatMagicalSpeed = 109343399106257ll;
inline constexpr int64_t kStatPhysicalSpeed = 109343399171808ll;

struct ClassResource { int class_enum; const char *effect; };
struct Timing { const char *ability; double item; double cost; double hit; };
struct MissileLanding { const char *ability; int key; double after_cost; };
struct Flight { const char *ability; double bar; int key; double fire; int animation = -1; double speed = 0; };
struct Dash { const char *ability; double speed; };
struct StepToken { bool ok = false; int step = 1; bool proc = false; int variant = 0; bool finisher = false; bool beam = false; };

extern const ClassResource kClassResources[1];
extern const Timing kTimings[9];
extern const BranchedCast kBranchedCasts[27];
extern const GatheredCast kGatheredCasts[1];
extern const TimedCast kTimedCasts[3];
extern const MissileLanding kMissileLandings[7];
extern const Flight kFlights[12];
extern const ShotCast kShotCasts[42];
extern const NoteCast kNoteCasts[26];
extern const BounceRule kBounces[3];
extern const LoopedCast kLoopedCasts[1];
extern const Dash kDashes[6];
extern const char *const kExpirePhases[2];
extern const char *const kTickPhases[1];
extern const char *const kApplyPhases[1];

bool swing_flies(std::string_view ability, int animation);
bool swing_flies(const std::string &ability, int animation);
std::string_view group_of_name(std::string_view name);
bool is_none_tag(const std::string &s);
std::string group_key(const content::AbilityRow &row);
const AbilityTiming *find_timing(int64_t ability, int aimed);
StepToken step_token(std::string_view name);
bool same_step(const StepToken &a, const StepToken &b);
bool step_of_expression(std::string_view text, int &step, bool &proc);
std::string_view leaf_of_path(const std::string &path);
bool is_weapon_combo(const content::AbilityRow &row);
bool is_melee_ability(const content::AbilityRow &row);
bool is_melee_ability(const abilities::AbilityDef *def);
int64_t projectile_of(const abilities::AbilityDef &def, int key);
int64_t first_projectile_guid(const abilities::AbilityDef &def);
int64_t projectile_hit_key(const abilities::AbilityDef &def, int64_t projectile);
const char *gathered_log_suffix(size_t count);
bool carried_by_ball(const abilities::AbilityDef &def, int64_t hit_guid);
int beam_hit_requests(std::string_view hit);
StepToken step_of_montage(const abilities::Catalog &catalog, const content::MontageRow &montage);

class AbilityPipeline::Host final : public combat::ContextHost {
public:
  struct Core;
  struct Damage;
  struct Hits;
  struct Casts;
  struct Zones;
  struct Projectiles;
  struct Pets;
  struct FlightCalc { const Cast &cast; double distance = 0.0; double rate = 1.0; double t = 0.0; };

  explicit Host(AbilityPipeline &p) : p_(p) {}
  double effect_count_by_instigator(const stats::RecordRef &r) override;
  double rand_double() override;
  double ability_progress(const std::string &group) override;
  void set_reaction(const ReactionContext *rc) { rc_ = rc; }

  static std::optional<bool> eval_target_program(AbilityPipeline &self, int program, combat::Fighter &tf, const combat::TargetInfo &ti, std::string &why);
  static void populate_event_hit_context(const AbilityPipeline &self, const ReactionContext &rc, combat::HitContext &ctx);
  static bool is_replicated_stat(const stats::StatValue &v);
  static void upsert_stat_change(bootstrap::AbilityChange &c, const stats::StatValue &v);
  static void record_recomputed_stat(AbilityPipeline &self, bootstrap::AbilityChange &c, const stats::StatValue &v);
  static void sync_sprint_mods(AbilityPipeline &self, int64_t sprint_guid, const abilities::EffectDef &sprint, double now);
  static void remove_guid_by_hit(AbilityPipeline &self, bootstrap::AbilityChange &c, int64_t guid, int &left);
  static bool has_pending_slot_requests(const CastRecord &rec);
  static bool has_pending_slot_requests(const CastRecord *rec);
  static void register_cast_hit_slots(AbilityPipeline &self, const abilities::AbilityDef &def, const Cast &cast, CastRecord &rec);
  static void enqueue_effect_hits(const abilities::Catalog *cat, int64_t guid, std::set<int64_t> &seen, std::vector<int64_t> &todo);
  static void enqueue_hit_effects(const abilities::Catalog *cat, int64_t g, std::set<int64_t> *out_effects, std::set<int64_t> &seen, std::vector<int64_t> &todo);
  static std::set<int64_t> collect_ability_hit_graph(const abilities::Catalog *cat, const abilities::AbilityDef &def, std::set<int64_t> *out_effects);
  static bool flight_matches(const Cast &cast, const Flight &f);
  static const Flight *find_cast_flight(const Cast &cast);
  static bool resolve_flight_due(const FlightCalc &fc, int64_t key, const content::ProjectileRow &p, double &out_due);

protected:
  double other_fn(int fn, const std::string &name, const double *args, int) override;

private:
  static bool pred_consumed_has_tag(const ReactionContext &rc, const std::string &name);
  static bool pred_consumed_name_has(const ReactionContext &rc, const std::string &name);
  static bool pred_consumed_is(const ReactionContext &rc, const std::string &name);
  static bool pred_hit_name_is(const ReactionContext &rc, const std::string &name);
  static bool pred_hit_is(const ReactionContext &rc, const std::string &name);
  double rand_int_range(double a0, double a1);
  double applicable_target(const std::string &name) const;
  double hit_stat_value(int fn, const std::string &name) const;
  double eval_reaction_pred(bool (*pred)(const ReactionContext &, const std::string &), const std::string &name) const;
  void note_unserved_reaction(int fn, const std::string &name);
  double reaction_fn(int fn, const std::string &name);

public:
  AbilityPipeline &p_;
  const ReactionContext *rc_ = nullptr;
};

} // namespace aoc::worldd
