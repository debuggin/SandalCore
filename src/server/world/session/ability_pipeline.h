// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "session/ability_pipeline_types.h"
#include "session/item_cast.h"

namespace aoc::worldd {

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
    static constexpr double kItemRequestWindow = 15.0;
    static constexpr double kItemCastDelay = 0.065;
    static constexpr int    kMaxRequestsPerHit = 8;
    static constexpr double kMinRequestGap = 0.100;
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
    static constexpr double kEarliestChargedRequest = 1.0;
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
    static constexpr double kMaxShotFlight = 3.0;
    static constexpr double kLaunchAdvance = 0.15;

    using WornFn = std::function<std::vector<stats::WornItem>()>;
    using WeaponTagsFn = std::function<std::vector<std::string>()>;
    using ActiveTargetFn = std::function<uint64_t()>;
    using ItemTakeFn = std::function<bool(int32_t client_instance_id)>;
    using StatCost = ability_types::StatCost;
    using Inspection = ability_types::Inspection;
    using HitVerdict = ability_types::HitVerdict;
    using PetPayment = ability_types::PetPayment;
    using BoltRecall = ability_types::BoltRecall;

    AbilityPipeline(const abilities::Catalog* catalog, stats::CharacterStats& stats, AbilityRuntime& a7, WornFn worn, double world_time_at_zero,
                    uint64_t seed = 1);

    void set_known(std::vector<int64_t> known);
    bool known(int64_t ability) const { return known_.count(ability) != 0; }
    bool handles(int64_t ability) const;
    static std::string cooldown_group_of(const content::AbilityRow& row);

    std::optional<Inspection> inspect(int64_t ability);

    bool evaluate(int64_t expr, double& out, std::string& why) {
        drop_evaluation();
        return eval(expr, out, why);
    }

    Refusal trigger(int64_t ability, int32_t client_instance_id, double now, const TriggerInfo& info = {});
    bool cancel(int32_t client_instance_id, double now, double held_seconds = 0);
    bool branch_request(int32_t client_instance_id, int64_t branch_guid, double now);
    bool end_request(int32_t server_instance_id, double now);

    static const char* verdict_name(HitVerdict v);
    HitVerdict hit_request(int64_t ability, int64_t targeting_guid, int64_t hit_guid, size_t components, size_t own, double now,
                           const std::vector<uint64_t>& targets = {});

    void set_dead(bool dead);
    bool dead() const { return dead_; }

    void set_combat(combat::HitPort* port) { combat_ = port; }
    void set_weapon_tags(WeaponTagsFn fn) { weapon_tags_ = std::move(fn); }
    void set_active_target(ActiveTargetFn fn) { active_target_ = std::move(fn); }
    void set_item_taker(ItemTakeFn fn) { item_taker_ = std::move(fn); }
    void receive_hit(const combat::IncomingHit& hit, double now);
    void confirm_hit_on(const combat::IncomingHit& hit, double now, const protocol::emit::FIntrepidNetworkGUID& target,
                        const protocol::emit::FIntrepidNetworkGUID& cylinder, double taken, bool lethal);
    void confirm_effect_hit(const combat::EffectHitNote& note, double now);
    void confirm_dealt(const combat::DealtHit& dealt, double now);
    bool can_pay_pet_ability(int64_t ability, double now, PetPayment& out);
    void commit_pet_payment(int64_t ability, const PetPayment& pay, double now);
    bool pay_pet_ability(int64_t ability, double now, double& cooldown);
    void start_pet_cooldown(int64_t ability, double seconds, double now);
    void land_pet_hit(int64_t hit_guid, int64_t ability, double now);
    bool holds_dispellable() const;
    bool grant_effect(int64_t effect_guid, double now);
    bool grant_effect(int64_t effect_guid, int stacks, double now);
    uint64_t take_transpose() {
        const uint64_t id = transpose_with_;
        transpose_with_ = 0;
        return id;
    }
    BoltRecall take_bolt_recall() {
        const BoltRecall recall = bolt_recall_;
        bolt_recall_ = {};
        return recall;
    }

    void tick(double now);
    std::vector<AbilityEvent> take_events();
    double next_due() const;

    void sync_a7_values(double now);

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
    double stat_of(int64_t guid) const { return stat_now(guid); }
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
    using HitSource = ability_types::HitSource;
    using HitEvent = ability_types::HitEvent;
    using ReactionContext = ability_types::ReactionContext;
    using Pending = ability_types::Pending;
    using HitSlot = ability_types::HitSlot;
    using CastRecord = ability_types::CastRecord;
    using BallFlight = ability_types::BallFlight;
    using Cast = ability_types::Cast;
    using EffectInst = ability_types::EffectInst;
    using Zone = ability_types::Zone;
    using Group = ability_types::Group;
    using Held = ability_types::Held;
    using Instance = ability_types::Instance;

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
    void   step_item(Cast& cast, double t);
    void   step_cost(Cast& cast, double t);
    void   step_hits(Cast& cast, double t, const char* which, int depth = 0);
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
    void   end_zones_of(const Cast& cast, double t);
    bool   can_pay(const Cast& cast) const;
    void   bring_sooner(Cast& cast, double earlier);
    void   land_by_flight(Cast& cast, double t);
    void   launch_ball(Cast& cast, double t);
    void   ball_step(Cast& cast, const Cast::Missile& step, double t);
    double client_rate(bool magical) const;
    bool   plays_at_magical(const abilities::AbilityDef& def, bool row_magical) const;
    double item_rate(const abilities::AbilityDef& def) const;
    void   plan_shot(Cast& cast, const ShotCast& shot, double t);
    void   launch_shot(Cast& cast, double t);
    void   anchor_shot(Cast& cast, double now);
    void   land_shot(Cast& cast, double t);
    void   plan_notes(Cast& cast, double t);
    void   launch_note(Cast& cast, const Cast::Missile& fly, double t);
    void   bounce_from(bootstrap::AbilityChange& c, Cast& cast, const abilities::HitDef& hit, int64_t projectile, uint64_t target, int depth, double t);
    Cast*  cast_of(int32_t client_id);
    double held_fill(const Cast& cast, double held_seconds) const;

    struct FillScope {
        AbilityPipeline& p;
        double           saved;
        FillScope(AbilityPipeline& pipeline, double fill) : p(pipeline), saved(pipeline.fill_) { p.fill_ = fill; }
        ~FillScope() { p.fill_ = saved; }
    };
    struct ConsumedScope {
        AbilityPipeline&                   p;
        const std::vector<stats::StatMod>* saved;
        ConsumedScope(AbilityPipeline& pipeline, const std::shared_ptr<const ItemCast>& item)
            : p(pipeline), saved(pipeline.consumed_) {
            p.consumed_ = item ? &item->stats : nullptr;
        }
        ~ConsumedScope() { p.consumed_ = saved; }
    };
    void   step_end(Cast& cast, double t);
    void   finish_cast_if_done(size_t index);
    std::string montage_group_of(const abilities::AbilityDef& def) const;

    void   apply_hit(bootstrap::AbilityChange& c, const abilities::HitDef& hit, double t, bool skip_resource, const HitSource& src);
    void   apply_hit_here(bootstrap::AbilityChange& c, const abilities::HitDef& hit, double t, bool skip_resource, const HitSource& src);
    void   modify_cooldowns(bootstrap::AbilityChange& c, const abilities::HitDef& hit, double t);
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
    void   release_held(int32_t client_id, double t);
    std::set<int64_t> effects_applied_by_hits(const abilities::AbilityDef& def) const;
    void   end_loop(bootstrap::AbilityChange& c, int64_t ability, double t);
    void   remove_by_hit(bootstrap::AbilityChange& c, const abilities::HitDef& hit);
    void   sync_engine_effects();

    CastRecord* record_of(int32_t client_id);
    void   run_slot(CastRecord& rec, HitSlot& slot, double t, bool response, bool found, const std::vector<uint64_t>& targets);
    void   apply_hit_on_targets(bootstrap::AbilityChange& c, const abilities::HitDef& hit, const std::vector<uint64_t>& targets, double t, const HitSource& src,
                                const abilities::AbilityDef* ability, bool* landed = nullptr);

    bool   eval_in_event(int64_t expr, const ReactionContext& rc, double& out, std::string& why);
    void   raise_hit_event(bootstrap::AbilityChange& c, const HitEvent& ev, double t);
    void   fire_carried_reactions(bootstrap::AbilityChange& c, const HitEvent& ev, double t);
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
    void   land_bolt_recall(int32_t client_id, uint64_t target, double t);

    void   regenerate(double t);
    void   resolve_resource();

    const abilities::Catalog* catalog_;
    stats::CharacterStats&    stats_;
    AbilityRuntime&           a7_;
    WornFn                    worn_;
    WeaponTagsFn              weapon_tags_;
    ActiveTargetFn            active_target_;
    ItemTakeFn                item_taker_;
    double                    world_time_at_zero_;
    stats::SplitMix           rng_;
    std::mt19937_64           bounce_rng_{0x5a17d00du};
    std::unique_ptr<stats::CharacterStats::Evaluation> evaluation_;

    double                    now_ = 0;
    double                    clock_ = 0;
    int                       apply_depth_ = 0;
    int                       carried_depth_ = 0;
    bool                      data_timings_ = true;
    bool                      moving_ = false;
    std::map<int64_t, int>    last_max_charges_;
    std::deque<std::pair<double, double>> losses_;
    double remaining_charges(int64_t ability) const;
    double stat_delta_over_time(int64_t stat, double seconds) const;
    double progress_of(const std::string& group) const;
    double last_item_at_ = -1;
    double fill_ = 1;
    const std::vector<stats::StatMod>* consumed_ = nullptr;
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
    std::deque<Instance>      instances_;
    void   end_instance(size_t index, double t);
    std::map<int64_t, int>    swing_steps_;
    int    swing_montage(const abilities::AbilityDef& def, int requested, int& step) const;
    void   stop_swing(int64_t instance);
    stats::SplitMix           guid_rng_;
    std::vector<bootstrap::ShieldElement> shields_sent_;
    size_t                    refused_ = 0, casts_started_ = 0, regen_ticks_ = 0;
    combat::HitPort*          combat_ = nullptr;
    uint64_t                  transpose_with_ = 0;
    BoltRecall                bolt_recall_;
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
    const ReactionContext*    active_reaction_ = nullptr;
};

}  // namespace aoc::worldd
