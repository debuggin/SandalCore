// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "combat/hit_resolver.h"
#include "combat/npc_ability_data.h"
#include "combat/npc_stats.h"
#include "effects/effect_book.h"
#include "npc/ai/npc_ai.h"
#include "npc/npc_world.h"
#include "protocol/emit/intrepid_netguid.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::combat {

inline bool notices_players(bool hostile_to_players, const world::NpcInfo& npc) { return hostile_to_players && !npc.aggro_off; }

struct IncomingHit {
    uint64_t                 instigator = 0;
    int64_t                  hit_guid = 0;
    int64_t                  source_type = 0;
    int64_t                  source_guid = 0;
    uint8_t                  flags = 0;
    std::vector<ResolvedMod> mods;
    double                   at = 0;
};

struct EffectHitNote {
    uint64_t                 holder = 0;
    int64_t                  hit_guid = 0;
    int64_t                  effect_guid = 0;
    uint8_t                  flags = 0;
    bool                     predicted = false;
    std::vector<ResolvedMod> mods;
    std::vector<double>      effective;
    double                   at = 0;
};

struct KillReward {
    uint64_t    npc = 0;
    std::string name;
    std::string blueprint;
    int         level = 0;
    int64_t     instance = 0;
    int64_t     adventuring_xp = 0;
    int64_t     weapon_xp = 0;
    int64_t     node_xp = 0;
    bool        loot = false;
};

struct HitOrigin {
    const abilities::HitDef* hit = nullptr;
    int64_t                  source_type = 0, source_guid = 0;
    std::vector<std::string> ability_tags;
    bool                     landed_here = false;
};

struct DealtHit {
    int64_t                  hit_guid = 0;
    int64_t                  source_type = 0, source_guid = 0;
    uint64_t                 instigator = 0;
    uint64_t                 target = 0;
    uint8_t                  flags = 0;
    std::vector<ResolvedMod> mods;
    std::vector<double>      effective;
};

class PlayerPort {
public:
    virtual ~PlayerPort() = default;
    virtual uint64_t actor_id() const = 0;
    virtual Fighter& fighter() = 0;
    virtual void receive_hit(const IncomingHit& hit, double now) = 0;
    virtual void set_in_combat(bool in_combat, double now) = 0;
    virtual std::array<uint32_t, 4> character_guid() const { return {}; }
    virtual protocol::emit::FIntrepidNetworkGUID pawn_guid() const { return protocol::emit::FIntrepidNetworkGUID::from_u64(actor_id()); }
    virtual protocol::emit::FIntrepidNetworkGUID cylinder_guid() const { return {}; }
    virtual void receive_kill(const KillReward& /*kill*/, double /*now*/) {}
    virtual void confirm_effect_hit(const EffectHitNote& /*note*/, double /*now*/) {}
    virtual void hit_dealt(const DealtHit& /*hit*/, double /*now*/) {}
    virtual void hit_from_pet(int64_t /*hit_guid*/, int64_t /*ability*/, double /*now*/) {}
};

struct TargetInfo {
    bool     exists = false;
    bool     alive = false;
    Relation relation = Relation::Neutral;
    bool     in_line_of_sight = true;
    double   surface_distance_cm = 0;
    Vec3     position;
    double   radius_cm = 50.0;
    bool     is_player = false;
};

struct GatheredTarget {
    uint64_t actor = 0;
    double   distance_cm = 0;
};

struct Delivery {
    bool                applied = false;
    uint8_t             flags = 0;
    std::vector<double> effective;
    bool                lethal = false;
    uint64_t            target_object = 0;
};

struct DeathEvent {
    uint64_t               npc = 0;
    uint64_t               killer = 0;
    double                 at = 0;
    int                    level = 0;
    Vec3                   position;
    std::string            blueprint;
    const world::NpcInfo*  info = nullptr;
    const npc::MotionStats* motion = nullptr;
    std::set<uint64_t>     attackers;
    bool                   gm = false;
};

struct CombatWorldConfig {
    bool   enabled = true;
    double think_seconds = 0.2;
    double scan_radius_cm = 3500.0;
    double stats_radius_cm = 15000.0;
    double stats_seconds = 1.0;
    double default_aggro_cm = 800.0;
    double default_leash_cm = 3000.0;
    double default_attack_range_cm = 220.0;
    double default_attack_interval = 2.5;
    double default_windup = 0.5;
    double attack_variance = 0.25;
    double player_reach_cm = 4500.0;
    double melee_reach_cm = 1500.0;
    uint64_t seed = 1;
    double npc_damage_mult = 1.0;
    std::string fallback_ability = "Ramkin_Hunter_SpearStrike";
};

inline constexpr int64_t kCcDrWindowMs = 15000;
inline constexpr uint8_t kCcDrMaxStage = 3;
inline constexpr int kMaxProcChainDepth = 1;

struct CcDrResult {
    int64_t effective_duration_ms = 0;
    double  scale = 1.0;
    uint8_t stage = 0;
    bool    immune = false;
};

struct CcDrTracker {
    uint8_t stage = 0;
    int64_t last_applied_ms = -kCcDrWindowMs;

    static constexpr double stage_scale(uint8_t s) noexcept {
        switch (s) {
            case 0: return 1.0;
            case 1: return 0.5;
            case 2: return 0.25;
            default: return 0.0;
        }
    }

    CcDrResult apply(int64_t base_duration_ms, int64_t now_ms) noexcept {
        if (base_duration_ms <= 0) {
            return {0, 0.0, stage, stage >= kCcDrMaxStage};
        }
        if (now_ms - last_applied_ms >= kCcDrWindowMs) {
            stage = 0;
        }
        const uint8_t applied_stage = stage;
        const double scale = stage_scale(applied_stage);
        const bool is_immune = (applied_stage >= kCcDrMaxStage);
        const int64_t effective_ms = is_immune ? 0 : static_cast<int64_t>(base_duration_ms * scale);
        last_applied_ms = now_ms;
        if (stage < kCcDrMaxStage) {
            ++stage;
        }
        return {effective_ms, scale, applied_stage, is_immune};
    }

    void reset() noexcept {
        stage = 0;
        last_applied_ms = -kCcDrWindowMs;
    }
};

[[nodiscard]] inline constexpr bool can_trigger_proc_at_depth(int proc_depth) noexcept {
    return proc_depth >= 0 && proc_depth < kMaxProcChainDepth;
}

class ProcDepthScope {
public:
    explicit ProcDepthScope(int& depth) noexcept
        : depth_(depth), allowed_(can_trigger_proc_at_depth(depth)) {
        if (allowed_) {
            ++depth_;
        }
    }
    ~ProcDepthScope() noexcept {
        if (allowed_) {
            --depth_;
        }
    }
    ProcDepthScope(const ProcDepthScope&) = delete;
    ProcDepthScope& operator=(const ProcDepthScope&) = delete;

    [[nodiscard]] bool allowed() const noexcept { return allowed_; }
    [[nodiscard]] int depth() const noexcept { return depth_; }

private:
    int& depth_;
    bool allowed_ = false;
};

struct ProcGateState {
    int proc_depth = 0;
    std::unordered_map<int64_t, int64_t> last_proc_ms;

    [[nodiscard]] bool can_fire(int64_t proc_guid, int64_t icd_ms, int64_t now_ms) const noexcept {
        if (proc_guid <= 0 || !can_trigger_proc_at_depth(proc_depth)) {
            return false;
        }
        if (icd_ms <= 0) {
            return true;
        }
        const auto it = last_proc_ms.find(proc_guid);
        if (it == last_proc_ms.end()) {
            return true;
        }
        return now_ms >= it->second && (now_ms - it->second) >= icd_ms;
    }

    bool try_fire(int64_t proc_guid, int64_t icd_ms, int64_t now_ms,
                  double roll01 = 0.0, double proc_chance = 1.0) {
        if (proc_chance <= 0.0 || roll01 < 0.0 || roll01 >= proc_chance) {
            return false;
        }
        if (!can_fire(proc_guid, icd_ms, now_ms)) {
            return false;
        }
        last_proc_ms[proc_guid] = now_ms;
        return true;
    }
};

class CombatWorld {
public:
    using DeathHook = std::function<void(const DeathEvent&)>;

    CombatWorld(world::ActorRegistry& registry, npc::NpcWorld* npcs, const nav::AreaNav* nav, const CombatData& data, const abilities::Catalog& catalog,
                const NpcStatBook& book, const npc::MotionTable& motion, CombatWorldConfig config = {});

    ~CombatWorld();
    CombatWorld(const CombatWorld&) = delete;
    CombatWorld& operator=(const CombatWorld&) = delete;

    const CombatWorldConfig& config() const { return cfg_; }
    const CombatData& data() const { return data_; }
    HitResolver& resolver() { return resolver_; }

    void set_death_hook(DeathHook hook) { death_hook_ = std::move(hook); }
    void set_ability_data(const NpcAbilityData* data) { anim_ = data; }
    bool launches_projectiles(const abilities::AbilityDef& def) const;
    void set_senses_data(const npc::SensesTable* data) { senses_ = data; }

    struct GateCensus {
        size_t any = 0, validity = 0, volumes = 0, kept_swing = 0, unplayed = 0;
        std::vector<std::string> any_names, validity_names, volume_names, kept_names, unplayed_names;
        std::string text() const;
    };
    GateCensus gate_census() const;

    using HuntablePredicate = std::function<bool(const world::NpcInfo&)>;
    void set_huntable(HuntablePredicate fn) { huntable_ = std::move(fn); }

    void add_player(PlayerPort* port);
    void remove_player(PlayerPort* port);
    PlayerPort* find_player(uint64_t actor_id) { return player(actor_id); }
    const std::vector<PlayerPort*>& players() const { return players_; }

    Fighter* fighter_of(uint64_t actor_id);
    uint64_t actor_of_object(uint64_t object_id) const;
    TargetInfo inspect(uint64_t target_actor, const Fighter& from);
    std::vector<GatheredTarget> enemies_in_cone(const Fighter& from, double angle_degrees, double length_cm, double behind_cm, size_t max);
    std::vector<GatheredTarget> enemies_in_line(const Fighter& from, double length_cm, double radius_cm, size_t max);
    std::vector<GatheredTarget> enemies_around(const Fighter& from, const Vec3& centre, double radius_cm, size_t max);
    bool ground_height(double x, double y, double z_hint, double* z) const;
    uint64_t hit_component_of(uint64_t actor_id) const;

    Delivery apply_hit_to_npc(uint64_t npc_actor, uint64_t attacker, const Resolved& resolved, double now, uint64_t threat_to = 0, const HitOrigin* origin = nullptr);

    void apply_effects(uint64_t npc_actor, uint64_t instigator, const std::vector<effects::Applied>& effects, double now);
    double effect_remaining(uint64_t npc_actor, int64_t effect_guid, double now) const;
    int    effect_stacks(uint64_t npc_actor, int64_t effect_guid) const;
    size_t effect_count(uint64_t npc_actor) const;
    void remove_npc_effect(uint64_t npc_actor, int64_t effect_guid, double now);
    void remove_effects_from_npc(uint64_t npc_actor, const abilities::HitDef& hit, double now);

    // Pet hits resolve against owner stats; aggro targets the pet while kill credit and loot belong to the owner.
    bool pet_attack(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target);
    void pet_stop(uint64_t pet);
    uint64_t pet_of(uint64_t owner) const;
    uint64_t owner_attacker(uint64_t player_actor, uint64_t pet, double radius_cm) const;
    bool pet_can_act(uint64_t pet) const;
    uint64_t take_pet_stand_down(uint64_t pet);
    bool pet_fighting(uint64_t pet) const { return pet_fights_.count(pet) != 0; }
    bool is_pet(uint64_t actor_id) const { return pet_ports_.count(actor_id) != 0; }
    bool pet_use(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target, double reach_cm);
    bool pet_can_use(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target, std::string* why = nullptr);
    bool pet_casting(uint64_t pet) const { return pet_casts_.count(pet) != 0; }
    uint64_t pet_target(uint64_t pet) const {
        const auto it = pet_fights_.find(pet);
        return it == pet_fights_.end() ? 0 : it->second.target;
    }
    uint64_t creature_target(uint64_t npc_actor) const;
    bool holds_dispellable(uint64_t npc_actor) const;
    bool pet_summoned(uint64_t pet, uint64_t owner) { return pet_join(pet, owner); }
    void pet_gone(uint64_t pet);
    uint64_t attacker_of(uint64_t player_actor, uint64_t pet, double radius_cm, bool owner_too = true) const;
    bool fought_by(uint64_t npc_actor, uint64_t player_actor) const;

    void tick(double now);

    bool is_combatant(uint64_t actor_id) const { return npcs_.count(actor_id) != 0; }
    std::optional<npc::ai::State> ai_state(uint64_t actor_id) const;
    const npc::ai::NpcAi* ai_of(uint64_t actor_id) const;
    double health_of(uint64_t actor_id) const;
    double max_health_of(uint64_t actor_id) const;
    bool   dead(uint64_t actor_id) const;

    struct Summary {
        uint64_t id = 0;
        uint8_t  health_pct = 100;
        bool     in_combat = false;
        bool     dead = false;
    };
    void summaries(std::vector<Summary>& out) const;
    size_t combatants() const { return npcs_.size(); }
    struct Stats {
        uint64_t aggros = 0, resets = 0, kills = 0, npc_attacks = 0, npc_hits = 0, npc_misses = 0, player_hits = 0, refused_no_attack = 0;
    };
    const Stats& stats() const { return stats_; }

    std::string next_attack_name(uint64_t actor_id) const;
    void add_threat(uint64_t npc_actor, uint64_t attacker, double amount, double now);
    void add_shield(uint64_t npc_actor, double amount, double seconds, double now);
    double shield_of(uint64_t npc_actor, double now) const;
    void forget(uint64_t actor_id);

    CcDrResult apply_cc_with_dr(uint64_t target_actor, int64_t base_duration_ms, int64_t now_ms) {
        return cc_dr_by_target_[target_actor].apply(base_duration_ms, now_ms);
    }

    const CcDrTracker* cc_dr_of(uint64_t target_actor) const {
        const auto it = cc_dr_by_target_.find(target_actor);
        return it == cc_dr_by_target_.end() ? nullptr : &it->second;
    }

    bool try_trigger_proc(uint64_t actor_id, int64_t proc_guid, int64_t icd_ms, int64_t now_ms,
                          double proc_chance = 1.0) {
        ProcGateState& gate = proc_gates_by_actor_[actor_id];
        const double roll01 = (proc_chance >= 1.0) ? 0.0 : rng_.rand_double();
        return gate.try_fire(proc_guid, icd_ms, now_ms, roll01, proc_chance);
    }

    ProcGateState& proc_gate_of(uint64_t actor_id) { return proc_gates_by_actor_[actor_id]; }

    bool gm_kill(uint64_t actor_id, double now, std::string& why);
    bool gm_heal(uint64_t actor_id);
    enum class GmReset : uint8_t { Walking, Standing, Dead };
    GmReset gm_reset(uint64_t actor_id, double now);
    class Npc;

private:
    struct Attack {
        const abilities::AbilityDef* ability = nullptr;
        int    windup_ms = 500;
        int    range_cm = 220;
        const CreatureAbility* task = nullptr;
    };
    struct PlannedFire {
        double               delay = 0;
        int64_t              projectile = 0;
        std::vector<int64_t> hits;
    };
    struct Pending {
        double   due = 0;
        uint64_t npc = 0, target = 0;
        const abilities::AbilityDef* ability = nullptr;
        std::vector<int64_t> hits;
        bool     animated = false;
        std::vector<std::pair<double, int64_t>> timed_hits;
        size_t   next_hit = 0;
        double   end_at = 0;
        bool     logged = false;
        std::vector<BiteArea> timed_areas;
        std::vector<std::pair<double, PlannedFire>> timed_fires;
        size_t   next_fire = 0;
    };
    struct Flight {
        uint64_t             npc = 0, target = 0;
        const abilities::AbilityDef* ability = nullptr;
        int64_t              projectile = 0;
        std::vector<int64_t> hits;
        double               speed = 0, radius = 0, range = 0, life = 0;
        bool                 homing = true, stops_at_aim = false;
        Vec3                 at, dir;
        double               aim_distance = 0, launched = 0, last = 0, travelled = 0;
        int                  hits_left = 1;
        std::set<uint64_t>   struck;
    };
    struct TreeWhy {
        bool   too_far = false, off_angle = false;
        double reach_cm = -1;
    };
    struct PlannedHit {
        double  delay = 0;
        int64_t hit = 0;
        int     key = 0;
    };
    struct CastPlan {
        const MontageData* montage = nullptr;
        double             seconds = 0;
        std::vector<PlannedHit> hits;
        std::vector<PlannedFire> fires;
    };
    Npc*  combatant(uint64_t actor_id);
    Npc*  make_combatant(const world::WorldActor& a);
    Attack choose_attack(Npc& n);
    Attack choose_animated(Npc& n, const Fighter* target, TreeWhy* why = nullptr);
    bool  hit_reaches(const Npc& n, const Fighter& body, const BiteArea& area, bool is_target) const;
    uint64_t aimed_at(const Npc& n, const BiteArea& area, uint64_t cast_target);
    bool  line_clear(const Vec3& from, const Vec3& to) const;
    bool  areas_usable(const abilities::AbilityDef& def) const;
    bool  grabs_target(const abilities::AbilityDef& def) const;
    bool  montages_readable(const abilities::AbilityDef& def) const;
    void  think(Npc& n, double now);
    void  pursue(Npc& n, uint64_t target, double speed_cm_s, double stop_distance_cm);
    bool  faces(const Npc& n, const Vec3& at, double tolerance_degrees) const;
    void  start_attack(Npc& n, uint64_t target, double now, const Attack* picked = nullptr);
    void  stop_cast(Npc& n);
    void  resolve_pending(double now);
    bool  animates(const Npc& n, const abilities::AbilityDef& def) const;
    bool  can_animate(const Npc& n) const;
    bool  plan_cast(Npc& n, const abilities::AbilityDef& def, CastPlan& out);
    void  deliver_hit(Npc& n, PlayerPort& port, const abilities::AbilityDef& ability, int64_t hit_guid, double now, int64_t projectile = 0);
    void  stand_off_of(const Npc& n, double& arrival, double& reach) const;
    void  launch(Npc& n, uint64_t target, const abilities::AbilityDef& ability, const PlannedFire& fire, double now);
    void  advance_flights(double now);
    void  kill(Npc& n, uint64_t killer, double now, bool gm = false);
    Delivery land_hit(Npc& n, uint64_t attacker, const Resolved& resolved, double now, bool log, uint64_t threat_to = 0, const HitOrigin* origin = nullptr);

    class PetPort;
    struct LandSource;
    struct HolderEvent;
    class EventHost;
    struct PetFight {
        uint64_t owner = 0, target = 0;
        const abilities::AbilityDef* ability = nullptr;
        double   reach_cm = 250;
        double   next_at = 0, end_at = 0;
        bool     swinging = false;
        bool     pursuing = false;
        std::vector<std::pair<double, int64_t>> due;
    };
    struct PetCast {
        uint64_t owner = 0, target = 0;
        const abilities::AbilityDef* ability = nullptr;
        double   reach_cm = 250;
        bool     started = false, pursuing = false;
        double   end_at = 0;
        std::vector<std::pair<double, int64_t>> due;
    };
    bool  pet_join(uint64_t pet, uint64_t owner);
    // PetPort references the Npc combatant, so pet_cleanup must tear down the port before the combatant is erased.
    void  pet_cleanup(uint64_t pet);
    void  tick_pets(double now);
    void  tick_pet_casts(double now);
    void  pet_start_cast(Npc& pet, PetCast& cast, double now);
    void  pet_swing(Npc& pet, PetFight& fight, const Npc& target, double now);
    void  pet_land(Npc& pet, const PetCast& cast, const abilities::HitDef& hit, double now, int depth = 0);
    void  pet_land_on_self(Npc& pet, uint64_t owner, const abilities::HitDef& hit, double now);
    void  land_for_player(uint64_t by, uint64_t target, const abilities::HitDef& hit, const LandSource& src, double now, int depth = 0);
    void  land_children(uint64_t by, uint64_t target, const abilities::HitDef& hit, const LandSource& src, double now, int depth);
    void  land_effects_of_hit(Npc& n, PlayerPort& port, const abilities::HitDef& hit, const HitMeta& meta, double now);
    void  effect_hit_fires(Npc& n, uint64_t by, const abilities::HitDef& hit, int64_t effect_guid, double now);
    HolderEvent event_of_hit(const Npc& n, uint64_t attacker, const Resolved& r, const Delivery& d, bool lethal, const HitOrigin* origin) const;
    void  hit_reacts(uint64_t npc_actor, const HolderEvent& event, uint64_t threat_to, const HitOrigin* origin, double now);
    void  holder_reacts(uint64_t holder, const HolderEvent& event, double now, int hit_bit = 2, const std::vector<int64_t>* carried = nullptr);
    void  pet_hit_reacts(uint64_t pet, const HolderEvent& event, double now);
    void  pet_was_hit(Npc& pet, const IncomingHit& hit, double taken, double now);
    void  pet_effects_changed(Npc& pet);
    uint64_t pet_of_owner(uint64_t owner) const;
    double flight_seconds(const content::AbilityHitRow& row, const Vec3& from, const Vec3& to) const;

    void  land_effect(Npc& n, const effects::Applied& applied, uint64_t instigator, double now);
    void  run_effect_hits(Npc& n, const effects::Instance& e, const char* const* phases, size_t count, double at);
    void  advance_effects(double now);
    void  send_effects(Npc& n, const effects::Book::Changes& changes);
    void  clear_effects(Npc& n);
    void  drop_effects_of(uint64_t instigator);
    void  begin_reset(Npc& n, double now);
    void  set_flags(Npc& n, bool in_combat, uint64_t target_actor);
    bool  fill_stats(const world::WorldActor& a);
    PlayerPort* player(uint64_t actor_id);
    void  player_exchange(uint64_t player_id, double now);

    world::ActorRegistry&  registry_;
    npc::NpcWorld*         npcs_world_;
    const nav::AreaNav*    nav_;
    const CombatData&      data_;
    const abilities::Catalog& catalog_;
    const NpcStatBook&     book_;
    const npc::MotionTable& motion_;
    CombatWorldConfig      cfg_;
    HitResolver            resolver_;
    DeathHook              death_hook_;
    std::unordered_map<uint64_t, std::unique_ptr<Npc>> npcs_;
    std::vector<PlayerPort*> players_;
    std::map<uint64_t, double> player_combat_until_;
    std::vector<Pending>   pending_;
    std::vector<Flight>    flights_;
    std::map<uint64_t, PetFight> pet_fights_;
    std::map<uint64_t, PetCast> pet_casts_;
    std::map<uint64_t, uint64_t> pet_owners_;
    std::map<uint64_t, uint64_t> pet_stand_downs_;
    std::map<uint64_t, std::unique_ptr<PlayerPort>> pet_ports_;
    std::set<int64_t>      reaction_noted_;
    double                 pet_regen_at_ = 0;
    const NpcAbilityData*  anim_ = nullptr;
    const npc::SensesTable* senses_ = nullptr;
    std::unordered_map<std::string, bool> hostile_by_archetype_;
    HuntablePredicate      huntable_;
    std::set<uint64_t>     stats_filled_;
    std::set<uint64_t>     effect_holders_;
    std::set<int64_t>      refused_effects_;
    int                    effect_depth_ = 0;
    size_t                 effect_hits_logged_ = 0;
    static constexpr size_t kLoggedEffectHits = 120;
    size_t                 effects_logged_ = 0;
    static constexpr size_t kLoggedEffects = 80;
    static constexpr int    kMaxEffectDepth = 4;
    static constexpr size_t kMaxEffectSteps = 4096;
    double                 next_stats_ = 0;
    double                 next_think_ = 0;
    double                 now_ = 0;
    Stats                  stats_;
    stats::SplitMix        rng_;
    size_t                 npc_hits_logged_ = 0;
    static constexpr size_t kLoggedNpcHits = 400;
    size_t                 npc_casts_logged_ = 0;
    static constexpr size_t kLoggedNpcCasts = 200;
    size_t                 projectiles_logged_ = 0;
    static constexpr size_t kLoggedProjectiles = 200;
    std::unordered_map<uint64_t, CcDrTracker> cc_dr_by_target_;
    std::unordered_map<uint64_t, ProcGateState> proc_gates_by_actor_;
};

}  // namespace aoc::worldd::combat
