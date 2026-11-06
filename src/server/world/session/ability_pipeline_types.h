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
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd {

struct ConsumableTiming;
struct ItemCast;

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
    double      fire = 0;
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
using GatheredShots = GatheredCast;

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
    const char* ask = "";
    double      ask_gap = 0;
    int         key = 0;
};
using ShotFlight = ShotCast;

struct ShotAnchor {
    const char* ability;
    const char* hit;
    int         animation;
    double      gap;
    bool        magical;
};

struct NoteCast {
    const char* ability = "";
    int         animation = -1;
    int         key = 0;
    double      fire = 0;
    bool        magical = false;

    constexpr NoteCast() = default;
    constexpr NoteCast(const char* a, int anim, int k, double f, bool m)
        : ability(a), animation(anim), key(k), fire(f), magical(m) {}
    constexpr NoteCast(const char* a, int k, double f, bool m, int anim = -1)
        : ability(a), animation(anim), key(k), fire(f), magical(m) {}
};
using NoteFlight = NoteCast;

struct BounceRule {
    const char* child_hit = "";
    double      radius = 0;
    int         max_bounces = 0;
    bool        random = false;
    int         limit = 0;

    constexpr BounceRule() = default;
    constexpr BounceRule(const char* ch, double r, int mb, bool rnd)
        : child_hit(ch), radius(r), max_bounces(mb), random(rnd), limit(mb) {}
};

struct BallCast {
    const char* ability;
    double      bar;
    double      fire;
    double      period;
    double      ahead;
    double      up;
};

namespace ability_types {

struct StatCost {
    int64_t     stat = 0;
    std::string name;
    double      amount = 0;
};

struct Inspection {
    double                               cooldown = 0;
    int                                  max_charges = 1;
    int                                  charge_cost = 1;
    bool                                 triggers_cooldown = false;
    std::string                          group;
    std::vector<StatCost>                stat_costs;
    std::vector<std::pair<int64_t, int>> effect_costs;
    std::optional<double>                cast_seconds;
    bool                                 requirement = true;
    std::string                          problem;
};

enum class HitVerdict : uint8_t {
    Scheduled,
    Quiet,
    Empty,
    NoCast,
    NotItsHit,
    TooMany,
    Unsupported,
};

struct PetPayment {
    std::vector<std::pair<int64_t, double>> costs;
    double                                  cooldown = 0;
};

struct BoltRecall {
    uint64_t target = 0;
    bool     behind = false;
};

struct HitSource {
    int64_t  type = 0;
    int64_t  guid = 0;
    bool     actor = true;
    bool     confirm = false;
    bool     response = false;
    bool     simulated = false;
    bool     autonomous = true;
    bool     source_is_target = false;
    uint64_t source_actor = 0;
    bool     reach_free = false;
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
    double                at = 0;
    bool                  found = true;
    std::vector<uint64_t> targets;
};

struct HitSlot {
    const abilities::HitDef* hit = nullptr;
    int64_t                  targeting = 0;
    bool                     reaches = false;
    bool                     aims_others = false;
    bool                     server_self_use = false;
    bool                     fired = false;
    int                      requests = 0;
    int                      max_requests = -1;
    double                   last_request = -1e9;
    std::vector<Pending>     due;
    bool                     applied = false;
};

struct CastRecord {
    int32_t                                   client_id = 0;
    int64_t                                   ability = 0;
    double                                    trigger_at = 0;
    double                                    cost_at = -1;
    double                                    fill = 1;
    double                                    cost_due = 0;
    double                                    window_end = 0;
    double                                    hit_delay = 0.035;
    int                                       max_requests = 8;
    double                                    min_gap = 0.100;
    std::vector<HitSlot>                      slots;
    std::set<int64_t>                         reach;
    std::map<int64_t, std::pair<int, double>> asked;
    const ShotAnchor*                         anchor = nullptr;
    std::shared_ptr<const ItemCast>           item_cast;
};

struct BallFlight {
    struct Touch {
        double next = 0;
        int    hits = 0;
    };
    int64_t                   projectile = 0;
    double                    speed = 0;
    double                    radius = 0;
    double                    lasts = 0;
    double                    rate = 1.0;
    double                    launch_at = -1;
    double                    x = 0, y = 0, z = 0;
    double                    dx = 1, dy = 0;
    std::map<uint64_t, Touch> touched;
    int                       hits = 0;
};

struct Cast {
    int32_t                                  client_id = 0;
    const abilities::AbilityDef*             def = nullptr;
    const ConsumableTiming*                  consumable = nullptr;
    double                                   trigger_at = 0;
    std::optional<double>                    item_due, cost_due, hit_due, projectile_due, loop_due, own_due;
    const BranchedCast*                      branched = nullptr;
    const LoopedCast*                        looped = nullptr;
    double                                   loop_scale = 1.0;
    const GatheredCast*                      gathered = nullptr;
    int64_t                                  gathered_projectile = 0;
    int64_t                                  charged_projectile = 0;
    bool                                     held = false;
    bool                                     charged = false;
    int                                      shots = 0;
    int64_t                                  shot_projectile = 0;
    bool                                     montage_branch = false;
    float                                    branch_position = 0;
    bool                                     branch_predicted = true;
    std::optional<double>                    branch_due, end_due, held_release_due;
    struct Missile {
        double   due = 0;
        int64_t  projectile = 0;
        uint64_t target = 0;
        enum class Kind : uint8_t { Land, Launch, Strike, Fly, Ball };
        Kind     kind = Kind::Land;
        int      index = 0;
        int      depth = 0;
        double   x = 0, y = 0, z = 0;
    };
    std::vector<Missile>                     missiles;
    const TimedCast*                         timed = nullptr;
    std::set<uint64_t>                       struck;
    std::set<uint64_t>                       bounced;
    bootstrap::ActiveAbilityItem             item;
    std::vector<const abilities::EffectDef*> own_effects;
    bool                                     toggle_off = false;
    int64_t                                  chain_owner = 0;
    int64_t                                  instance = -1;
    int32_t                                  animation_index = 0;
    bool                                     target = false;
    bool                                     self_reachable = true;
    std::vector<uint64_t>                    target_actors;
    bool                                     loop = false;
    double                                   cooldown = 0;
    int                                      max_charges = 1;
    int                                      charge_cost = 1;
    std::string                              group;
    std::vector<std::pair<int64_t, double>>  stat_costs;
    std::vector<std::pair<int64_t, int>>     effect_costs;
    bool                                     triggers_cooldown = false;
    bool                                     clears_montage = false;
    bool                                     request_heard = false;
    double                                   heard_at = -1;
    const ShotCast*                          shot = nullptr;
    bool                                     flight_pending = false;
    double                                   launch_at = -1;
    bool                                     anchored = false;
    double                                   flight = 0;
    const BallCast*                          ball = nullptr;
    BallFlight                               ball_flight;
    std::shared_ptr<const ItemCast>          item_cast;
};

struct EffectInst {
    int64_t                       owner = 0;
    int64_t                       zone = 0;
    bool                          has_shield = false;
    float                         shield_base = 0, shield_value = 0;
    uint32_t                      shield_guid[4] = {0, 0, 0, 0};
    int64_t                       key = 0;
    const abilities::EffectDef*   def = nullptr;
    int                           stacks = 1;
    double                        start = 0, end = 0;
    double                        next_tick = 0;
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
    int               max_charges = 1;
    int               charges = 1;
    double            cooldown = 0;
    double            timer_start = 0, timer_end = -1;
    double            chain_start = 0;
    int               timers = 0;
    std::set<int64_t> items;
    int64_t           last_ability = 0;
};

struct Held {
    int32_t               client_id = 0;
    int64_t               ability = 0;
    std::optional<double> end_due;
};

struct Instance {
    int64_t               server_id = 0;
    int32_t               client_id = 0;
    std::optional<double> end_due;
    int64_t               ability = 0;
};

}  // namespace ability_types

}  // namespace aoc::worldd
