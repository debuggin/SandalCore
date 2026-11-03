// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "combat/hit_port.h"
#include "combat/hit_resolver.h"
#include "session/abilities.h"
#include "session/ability_catalog.h"
#include "stats/character_stats.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aoc::worldd {

// Cast-bar abilities wait for ServerRequestBranchVerification before jumping to the branch section and taking cost.
struct BranchedCast {
    const char* ability;
    int64_t     branch;
    float       position;
    double      item;
    double      request;
    double      answer;
    double      cost;
    double      end;
    int         animation = -1;
    bool        cost_first = false;
    bool        cancels = true;
    double      full = 0;
    int64_t     held_skill = 0;
    double      hold_limit = 0;
    double      release = 0;
    int64_t     charged_branch = 0;
    double      charged_answer = 0;
    double      charged_cost = 0;
};

struct LoopedCast {
    const char* ability;
    float       position;
    double      item;
    double      first_shot;
    double      period;
    double      jump;
    double      end;
};

struct GatheredCast {
    const char* ability;
    int         projectile;
    double      angle;
    double      length;
    double      behind;
    double      speed;
    int         charged_projectile = 0;
    double      charged_length = 0;
    double      charged_radius = 0;
    double      charged_speed = 0;
    const char* charged_montage = "";
    uint64_t    charged_montage_guid = 0;
};

struct TimedCast {
    enum class Kind : uint8_t {
        Volley,
        Strike,
        Barrage,
    };
    const char* ability;
    Kind        kind;
    int         projectile;
    double      first;
    double      gap;
    int         count;
    double      speed;
    double      angle = 0;
    double      length = 0;
    double      behind = 0;
    double      radius = 0;
    double      pitch[3] = {0, 0, 0};
    float       jump_position = 0;
    double      jump_at = 0;
    double      end_at = 0;
};

struct ShotCast {
    const char* ability;
    int         animation;
    double      fire;
    bool        magical;
    const char* ask;
    double      ask_gap;
};

// Generic data-driven ability runtime operating alongside AbilityRuntime (Sprint/Dodge/Rest) on the session clock.
class AbilityPipeline {
public:
    static constexpr double kItemDelay = 0.300;
    static constexpr double kCostDelay = 0.300;
    static constexpr double kHitDelay = 0.035;
    static constexpr double kProjectileDelay = 0.340;
    static constexpr double kRegenInterval = 1.0;
    static constexpr int    kMaxRegenSteps = 5;
    static constexpr double kConfirmDelay = 0.080;
    static constexpr double kRequestWindow = 5.0;
    static constexpr int    kMaxRequestsPerHit = 8;
    static constexpr double kMinRequestGap = 0.100;
    static constexpr int    kBeamRequestsPerHit = 24;
    static constexpr double kBeamRequestGap = 0.050;
    static constexpr size_t kMaxRecords = 16;
    static constexpr size_t kMaxCasts = 16;
    static constexpr size_t kMaxGathered = 40;
    static constexpr double kLoopDelay = 2.3;
    static constexpr double kSongProgressSeconds = 15.0;
    static constexpr double kCastSeconds = 3.0;
    static constexpr double kHeldEndDelay = 0.059;
    static constexpr double kEndDelay = 0.259;
    static constexpr double kEarliestBranchRequest = 0.6;
    static constexpr int    kMaxApplyDepth = 4;
    static constexpr double kMovementWindow = 1.0;
    static constexpr double kMovementMargin = 1.5;
    static constexpr double kZoneOpenDelay = 0.035;
    static constexpr double kZoneFadeDelay = 0.06;
    static constexpr double kZoneCloseDelay = 0.70;
    static constexpr double kZoneRequestSlack = 1.5;
    static constexpr int    kZoneExtraRequests = 8;
    static constexpr size_t kMaxZones = 16;
    static constexpr size_t kRecentIds = 256;
    static constexpr size_t kMaxEffects = 256;
    static constexpr size_t kMaxStepsPerAdvance = 100000;

    using WornFn = std::function<std::vector<stats::WornItem>()>;
    using WeaponTagsFn = std::function<std::vector<std::string>()>;
    using ActiveTargetFn = std::function<uint64_t()>;

    AbilityPipeline(const abilities::Catalog* catalog, stats::CharacterStats& stats, AbilityRuntime& a7, WornFn worn, double world_time_at_zero,
                    uint64_t seed = 1);

    void set_known(std::vector<int64_t> known);
    bool known(int64_t ability) const { return known_.count(ability) != 0; }
    bool handles(int64_t ability) const;

    struct StatCost {
        int64_t     stat = 0;
        std::string name;
        double      amount = 0;
    };
    struct Inspection {
        double                  cooldown = 0;
        int                     max_charges = 1;
        int                     charge_cost = 1;
        bool                    triggers_cooldown = false;
        std::string             group;
        std::vector<StatCost>   stat_costs;
        std::vector<std::pair<int64_t, int>> effect_costs;
        std::optional<double>   cast_seconds;
        bool                    requirement = true;
        std::string             problem;
    };
    std::optional<Inspection> inspect(int64_t ability);

    bool evaluate(int64_t expr, double& out, std::string& why) {
        drop_evaluation();
        return eval(expr, out, why);
    }

    Refusal trigger(int64_t ability, int32_t client_instance_id, double now, const TriggerInfo& info = {});
    bool cancel(int32_t client_instance_id, double now, double held_seconds = 0);
    bool branch_request(int32_t client_instance_id, int64_t branch_guid, double now);
    bool end_request(int32_t server_instance_id, double now);

    enum class HitVerdict : uint8_t {
        Scheduled,
        Quiet,
        Empty,
        NoCast,
        NotItsHit,
        TooMany,
        Unsupported,
    };
    static const char* verdict_name(HitVerdict v);
    HitVerdict hit_request(int64_t ability, int64_t targeting_guid, int64_t hit_guid, size_t components, size_t own, double now, const std::vector<uint64_t>& targets = {});

    void set_dead(bool dead);
    bool dead() const { return dead_; }

    void set_combat(combat::HitPort* port) { combat_ = port; }
    void set_weapon_tags(WeaponTagsFn fn) { weapon_tags_ = std::move(fn); }
    void set_active_target(ActiveTargetFn fn) { active_target_ = std::move(fn); }
    void receive_hit(const combat::IncomingHit& hit, double now);
    void confirm_hit_on(const combat::IncomingHit& hit, double now, const protocol::emit::FIntrepidNetworkGUID& target, const protocol::emit::FIntrepidNetworkGUID& cylinder,
                        double taken, bool lethal);
    void tick(double now);
    std::vector<AbilityEvent> take_events();
    double next_due() const;

    void sync_a7_values(double now);
    bool grant_effect(int64_t effect_guid, int stacks, double now);

    void set_reaction_carriers(std::vector<int64_t> effect_guids);
    void set_reactions_enabled(bool on) {
        if (on == reactions_enabled_) return;
        reactions_enabled_ = on;
        resolve_resource();
    }
    bool reactions_enabled() const { return reactions_enabled_; }
    size_t reactions_fired() const { return reactions_fired_; }

    void set_data_timings(bool on) { data_timings_ = on; }
    bool remove_effect(int64_t effect_guid, double now);

    void set_moving(bool moving);
    void refresh(double now, const char* why, bool with_stats = true);

    double take_damage(double amount, double now);
    std::vector<std::pair<double, double>> shields() const;
    double movement_allowance(double now) const;

    bool   casting() const { return !casts_.empty(); }
    size_t effect_count() const { return effects_.size(); }
    int    stacks_of(int64_t effect_guid) const;
    std::optional<double> effect_end(int64_t effect_guid) const;
    size_t cooldowns_running() const { return groups_.size(); }
    int    charges_left(int64_t ability) const;
    double now() const { return now_; }
    int    resource_class() const { return resource_class_; }
    size_t refused() const { return refused_; }
    size_t zones() const { return zones_.size(); }
    size_t casts_started() const { return casts_started_; }
    size_t ticks_regen() const { return regen_ticks_; }
    size_t unsupported_logged() const { return unsupported_.size(); }
    double world_time_at_zero() const { return world_time_at_zero_; }

private:
    struct HitSource {
        int64_t type = 0;
        int64_t guid = 0;
        bool    actor = true;
        bool    confirm = false;
        bool    response = false;
        bool    simulated = false;
        bool    autonomous = true;
        bool    source_is_target = false;
    };
    struct HitEvent {
        const abilities::HitDef*     hit = nullptr;
        const abilities::AbilityDef* ability = nullptr;
        HitSource                    src;
        std::vector<uint64_t>        targets;
        bool                         on_self = false;
        std::map<int64_t, double>    mods;
        std::map<int64_t, double>    effective;
        uint8_t                      flags = 0;
    };
    struct ReactionContext {
        const HitEvent*              hit = nullptr;
        const abilities::AbilityDef* consumed = nullptr;
    };
    struct Pending {
        double at = 0;
        bool   found = true;
        std::vector<uint64_t> targets;
    };
    struct HitSlot {
        const abilities::HitDef* hit = nullptr;
        int64_t                  targeting = 0;
        bool                     reaches = false;
        bool                     aims_others = false;
        int                      requests = 0;
        double                   last_request = -1e9;
        std::vector<Pending>     due;
        bool                     applied = false;
    };
    struct CastRecord {
        int32_t                      client_id = 0;
        int64_t                      ability = 0;
        double                       trigger_at = 0;
        double                       cost_at = -1;
        double                       fill = 1;
        double                       cost_due = 0;
        double                       window_end = 0;
        double                       hit_delay = kHitDelay;
        int                          max_requests = kMaxRequestsPerHit;
        double                       min_gap = kMinRequestGap;
        std::vector<HitSlot>         slots;
        std::set<int64_t>            reach;
        std::map<int64_t, std::pair<int, double>> asked;
    };
    struct Cast {
        int32_t                               client_id = 0;
        const abilities::AbilityDef*          def = nullptr;
        double                                trigger_at = 0;
        std::optional<double>                 item_due, cost_due, hit_due, projectile_due, loop_due, own_due;
        const BranchedCast*                   branched = nullptr;
        const LoopedCast*                     looped = nullptr;
        const GatheredCast*                   gathered = nullptr;
        int64_t                               gathered_projectile = 0;
        int64_t                               charged_projectile = 0;
        bool                                  held = false;
        bool                                  charged = false;
        int                                   shots = 0;
        int64_t                               shot_projectile = 0;
        bool                                  montage_branch = false;
        float                                 branch_position = 0;
        bool                                  branch_predicted = true;
        std::optional<double>                 branch_due, end_due;
        struct Missile {
            double   due = 0;
            int64_t  projectile = 0;
            uint64_t target = 0;
            enum class Kind : uint8_t { Land, Launch, Strike };
            Kind     kind = Kind::Land;
            int      index = 0;
            double   x = 0, y = 0, z = 0;
        };
        std::vector<Missile>                  missiles;
        const TimedCast*                      timed = nullptr;
        std::set<uint64_t>                    struck;
        bootstrap::ActiveAbilityItem          item;
        std::vector<const abilities::EffectDef*> own_effects;
        bool                                  toggle_off = false;
        int64_t                               chain_owner = 0;
        int64_t                               instance = -1;
        int32_t                               animation_index = 0;
        bool                                  target = false;
        bool                                  self_reachable = true;
        std::vector<uint64_t>                 target_actors;
        bool                                  loop = false;
        double                                cooldown = 0;
        int                                   max_charges = 1;
        int                                   charge_cost = 1;
        std::string                           group;
        std::vector<std::pair<int64_t, double>> stat_costs;
        std::vector<std::pair<int64_t, int>>  effect_costs;
        bool                                  triggers_cooldown = false;
        bool                                  clears_montage = false;
        bool                                  request_heard = false;
        double                                heard_at = -1;
        const ShotCast*                       shot = nullptr;
        bool                                  flight_pending = false;
        double                                launch_at = -1;
        bool                                  anchored = false;
        double                                flight = 0;
    };
    struct EffectInst {
        int64_t                     owner = 0;
        int64_t                     zone = 0;
        bool                        has_shield = false;
        float                       shield_base = 0, shield_value = 0;
        uint32_t                    shield_guid[4] = {0, 0, 0, 0};
        int64_t                     key = 0;
        const abilities::EffectDef* def = nullptr;
        int                         stacks = 1;
        double                      start = 0, end = 0;
        double                      next_tick = 0;
        std::vector<stats::EffectMod> mods;
    };
    struct Zone {
        enum class State : uint8_t { Pending, Open, Ended };
        int64_t id = 0;
        int64_t ability = 0, zone = 0;
        int32_t client_id = 0;
        double  open_at = 0;
        double  end_at = 0;
        double  fade_at = -1, close_at = -1;
        State   state = State::Pending;
    };
    // Chain-based cooldown recovery preserves the first use's start timestamp across consecutive charge recoveries.
    struct Group {
        int                  max_charges = 1;
        int                  charges = 1;
        double               cooldown = 0;
        double               timer_start = 0, timer_end = -1;
        double               chain_start = 0;
        int                  timers = 0;
        std::set<int64_t>    items;
        int64_t              last_ability = 0;
    };
    struct Held {
        int32_t               client_id = 0;
        int64_t               ability = 0;
        std::optional<double> end_due;
    };
    bool dead_ = false;
    class Host;

    void   advance(double now);
    void   run_due(double t);
    double sync(double t) const;
    void   emit(AbilityEvent e);
    void   refuse(int32_t client_id, Refusal why, double t, const std::string& what, bool log_always = false);

    bool   eval(int64_t expr, double& out, std::string& why);
    bool   eval_bool(int64_t expr, bool none, bool& out, std::string& why);
    bool   eval_requirement(int64_t expr, const std::vector<uint64_t>& targets, bool& out, std::string& why);
    void   drop_evaluation() { evaluation_.reset(); }
    std::string expr_problem(int64_t expr) const;

    double stat_now(int64_t guid) const;
    void   set_held(bootstrap::AbilityChange& c, int64_t stat, double value);
    void   recompute(bootstrap::AbilityChange& c);
    bool   take_stat(bootstrap::AbilityChange& c, int64_t stat, double amount);

    const char* resolve_formulas(Cast& cast, const abilities::AbilityDef& def, std::string& why);
    void   run_cast_step(Cast& cast, double t);
    void   step_item(Cast& cast, double t);
    void   step_cost(Cast& cast, double t);
    void   step_hits(Cast& cast, double t, const char* which);
    void   cancel_swings(double t, const std::string& by);
    void   step_branch(Cast& cast, double t);
    void   launch_gathered(Cast& cast, double t);
    void   start_timed(Cast& cast, const TimedCast& timed, double t);
    void   step_timed(Cast& cast, const Cast::Missile& m, double t);
    void   launch_timed(Cast& cast, const Cast::Missile& launch, double t);
    void   land_strike(Cast& cast, const Cast::Missile& m, double t);
    void   cancel_timed(int32_t client_instance_id, double now);
    void   fire_from_hit(const CastRecord& rec, const abilities::HitDef& hit, double t);
    bool   holds_skill(int64_t skill) const;
    void   reschedule_branch(Cast& cast, double branch_at, double cost_after);
    void   abort_cast(size_t index, double t, const char* why);
    void   bring_sooner(Cast& cast, double earlier);
    void   land_by_flight(Cast& cast, double t);
    double client_rate(bool magical) const;
    void   plan_shot(Cast& cast, const ShotCast& shot, double t);
    void   launch_shot(Cast& cast, double t);
    void   anchor_shot(Cast& cast, double now);
    void   land_shot(Cast& cast, double t);
    Cast*  cast_of(int32_t client_id);
    double held_fill(const Cast& cast, double held_seconds) const;

    struct FillScope {
        AbilityPipeline& p;
        double           saved;
        FillScope(AbilityPipeline& pipeline, double fill) : p(pipeline), saved(pipeline.fill_) { p.fill_ = fill; }
        ~FillScope() { p.fill_ = saved; }
    };
    void   step_end(Cast& cast, double t);
    void   finish_cast_if_done(size_t index);
    std::string montage_group_of(const abilities::AbilityDef& def) const;

    void   apply_hit(bootstrap::AbilityChange& c, const abilities::HitDef& hit, double t, bool skip_resource, const HitSource& src);
    void   apply_effect(bootstrap::AbilityChange& c, const abilities::EffectDef& def, int stacks, std::optional<double> duration, double t, bool raw = false,
                        int64_t owner = 0);
    void   remove_instance(bootstrap::AbilityChange& c, size_t index);
    void   remove_stacks(bootstrap::AbilityChange& c, int64_t effect_guid, int stacks);
    bootstrap::EffectItem effect_item(const EffectInst& e) const;
    void   run_effect_hits(bootstrap::AbilityChange& c, const EffectInst& e, const char* const* phases, size_t n, double t);
    void   step_loop(Cast& cast, double t);
    void   spawn_zones(const Cast& cast, double open_at, double end_at);
    void   run_zone(size_t index, double t);
    double zone_due(const Zone& z) const;
    void   end_zone_effects(bootstrap::AbilityChange& c, int64_t zone_id, double t);

    void   step_own(Cast& cast, double t);
    void   end_held(size_t index, double t);
    std::set<int64_t> effects_applied_by_hits(const abilities::AbilityDef& def) const;
    void   end_loop(bootstrap::AbilityChange& c, int64_t ability, double t);
    void   remove_by_hit(bootstrap::AbilityChange& c, const abilities::HitDef& hit);
    void   sync_engine_effects();

    CastRecord* record_of(int32_t client_id);
    void   run_slot(CastRecord& rec, HitSlot& slot, double t, bool response, bool found, const std::vector<uint64_t>& targets);
    void   apply_hit_on_targets(bootstrap::AbilityChange& c, const abilities::HitDef& hit, const std::vector<uint64_t>& targets, double t, const HitSource& src,
                                const abilities::AbilityDef* ability);

    bool   eval_in_event(int64_t expr, const ReactionContext& rc, double& out, std::string& why);
    void   raise_hit_event(bootstrap::AbilityChange& c, const HitEvent& ev, double t);
    void   raise_ability_event(bootstrap::AbilityChange& c, const abilities::AbilityDef& ability, double t);
    void   run_reactions(bootstrap::AbilityChange& c, int kind, int bit, const ReactionContext& rc, double t);
    void   fire_reaction(bootstrap::AbilityChange& c, const abilities::ReactionDef& r, const ReactionContext& rc, double t);
    bool   ensure_evaluation(std::string& why);
    double slot_due(const CastRecord& rec, const HitSlot& s) const;
    double record_reference(const CastRecord& rec) const { return rec.cost_at >= 0 ? rec.cost_at : rec.cost_due; }
    void   prune_records(double t);
    void   sync_shields(bootstrap::AbilityChange& c);
    std::set<int64_t> reach_of(const abilities::AbilityDef& def) const;
    int64_t targeting_of(const abilities::AbilityDef& def, int key) const;
    bool   absorb(bootstrap::AbilityChange& c, double& amount);
    double apply_delta(bootstrap::AbilityChange& c, int64_t stat, double amount);
    void   drop_record(int32_t client_id);
    void   effect_expires(size_t index, double t);
    void   effect_ticks(size_t index, double t);
    std::vector<stats::ActiveEffect> engine_effects() const;
    // Bit 1 (Self) or Bit 8 (Party) targets the caster since the caster is always in its own party.
    bool   hits_caster(const content::HitRow& hit) const { return (hit.applicable_targets & (1 | 8)) != 0; }

    void   use_charge(bootstrap::AbilityChange& c, const Cast& cast, double t);
    void   group_timer_ends(const std::string& key, double t);
    bootstrap::CooldownItem cooldown_item(int64_t ability, const Group& g) const;

    void   regenerate(double t);
    void   resolve_resource();

    const abilities::Catalog* catalog_;
    stats::CharacterStats&    stats_;
    AbilityRuntime&           a7_;
    WornFn                    worn_;
    WeaponTagsFn              weapon_tags_;
    ActiveTargetFn            active_target_;
    double                    world_time_at_zero_;
    stats::SplitMix           rng_;
    std::unique_ptr<stats::CharacterStats::Evaluation> evaluation_;

    double                    now_ = 0;
    double                    clock_ = 0;
    int                       apply_depth_ = 0;
    bool                      data_timings_ = true;
    bool                      moving_ = false;
    std::map<int64_t, int>    last_max_charges_;
    std::deque<std::pair<double, double>> losses_;
    double remaining_charges(int64_t ability) const;
    double stat_delta_over_time(int64_t stat, double seconds) const;
    double progress_of(const std::string& group) const;
    double last_item_at_ = -1;
    double fill_ = 1;
    std::map<int64_t, double> full_bars_;
    double                    next_regen_ = kRegenInterval;
    std::set<int64_t>         known_;
    std::vector<Cast>         casts_;
    std::vector<EffectInst>   effects_;
    std::map<std::string, Group> groups_;
    std::deque<int32_t>       recent_ids_;
    std::optional<int64_t>    item_instance_;
    int32_t                   animation_counter_ = 0;
    std::vector<std::pair<double, double>> movement_;
    std::set<std::string>     unsupported_;
    std::set<int64_t>         no_formula_;
    std::vector<AbilityEvent> events_;
    std::vector<Zone>         zones_;
    int64_t                   next_zone_id_ = 1;
    int64_t                   zone_owner_ = 0;
    std::vector<bootstrap::HitConfirm> confirmations_;
    std::deque<CastRecord>    records_;
    std::vector<Held>         held_;
    struct Instance {
        int64_t server_id = 0;
        int32_t client_id = 0;
        std::optional<double> end_due;
        int64_t ability = 0;
    };
    std::deque<Instance>      instances_;
    void   end_instance(size_t index, double t);
    stats::SplitMix           guid_rng_;
    std::vector<bootstrap::ShieldElement> shields_sent_;
    size_t                    refused_ = 0, casts_started_ = 0, regen_ticks_ = 0;
    combat::HitPort*          combat_ = nullptr;
    const abilities::EffectDef* resource_ = nullptr;
    int                       resource_class_ = 0;
    std::vector<int64_t>      reaction_carriers_;
    bool                      reactions_enabled_ = true;
    int                       reaction_depth_ = 0;
    size_t                    reactions_fired_ = 0;
    std::map<int64_t, int>    reaction_procs_;
    std::set<int64_t>         reaction_noted_;
    std::set<int64_t>         reaction_recursive_noted_;
    std::set<int>             unserved_host_;
};

}  // namespace aoc::worldd
