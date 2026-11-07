// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "combat/combat_world.h"

#include "bootstrap/constants.h"
#include "combat/combat_world_npc.h"
#include "combat/npc_ability_replicator.h"
#include "combat/npc_kits.h"
#include "npc/area_npcs.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace aoc::worldd::combat {

namespace {

nav::Vec3 to_nav(const Vec3& v) { return nav::Vec3{v.x, v.y, v.z}; }

bool is_attack(const abilities::AbilityDef& d, const abilities::Catalog& catalog, int64_t health_stat) {
    for (const content::AbilityHitRow& h : d.hits) {
        const abilities::HitDef* hit = catalog.hit(h.hit_guid);
        if (!hit || (hit->row.applicable_targets & 2) == 0) continue;
        for (const content::ModRow& m : hit->mods) {
            if (m.stat_guid == health_stat && m.value_expr != 0 && catalog.program(m.value_expr) >= 0) return true;
        }
    }
    return false;
}

class SelectHost final : public stats::ExprHost {
public:
    explicit SelectHost(stats::SplitMix& rng) : rng_(rng) {}
    double rand_double() override { return rng_.rand_double(); }
    double host_fn(int fn, const std::string&, const double* args, int) override {
        if (fn != stats::kHostRandIntRange) return 0;
        const double lo = std::floor(args[0]), hi = std::floor(args[1]);
        return hi < lo ? lo : lo + std::floor(rng_.rand_double() * (hi - lo + 1.0));
    }

private:
    stats::SplitMix& rng_;
};

constexpr double kMinHitDelay = 0.1;

struct AttackWalkSpeed {
    const char* ability;
    double      share;
};
constexpr AttackWalkSpeed kAttackWalkSpeeds[] = {
    {"Bandit_Raider_Whirlwind", 0.45},
    {"GoblinFlayer_SeveringSpin", 1.6},
};

double attack_walk_share(const std::string& ability) {
    for (const AttackWalkSpeed& row : kAttackWalkSpeeds) {
        if (ability == row.ability) return row.share;
    }
    return 1.0;
}

bool attack_keeps_its_stop(const std::string& ability) { return ability == "PitchfireRaider_PitchfireWhirlwind"; }

constexpr const char* kAttacksThatKeepTurning[] = {
    "Minotaur_Berserker_AxeStrike",
};

bool attack_keeps_turning(const std::string& ability) {
    for (const char* name : kAttacksThatKeepTurning) {
        if (ability == name) return true;
    }
    return false;
}

template <typename NpcT>
void set_attack_walk_share(NpcT& n, double share) {
    if constexpr (requires { n.attack_walk_share = share; }) {
        n.attack_walk_share = share;
    }
}

double angle_between_degrees(double a, double b) {
    double d = std::fmod(a - b, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d < -180.0) d += 360.0;
    return std::fabs(d);
}

const abilities::AbilityDef* pick_kit_ability(const npc_kits::Kit& kit, const abilities::Catalog& catalog, int64_t health_stat, stats::SplitMix& rng,
                                              const npc_kits::KitAbility*& out_k) {
    struct Cand {
        const abilities::AbilityDef* def;
        const npc_kits::KitAbility*  k;
    };
    std::vector<Cand> cands;
    double total = 0;
    for (size_t i = 0; i < kit.count; ++i) {
        const npc_kits::KitAbility& k = kit.abilities[i];
        const abilities::AbilityDef* d = catalog.ability_named(k.name);
        if (!d || !is_attack(*d, catalog, health_stat)) continue;
        cands.push_back({d, &k});
        total += std::max(1, k.uses);
    }
    if (cands.empty()) return nullptr;
    double r = rng.rand_double() * total;
    for (const Cand& c : cands) {
        r -= std::max(1, c.k->uses);
        if (r <= 0 || &c == &cands.back()) {
            out_k = c.k;
            return c.def;
        }
    }
    return nullptr;
}

void populate_target_geometry(const world::WorldActor& w, double self_radius_cm, const Fighter& target, CondContext& ctx) {
    const Vec3 at = target.position();
    ctx.surface_distance = distance_xy_cm(Vec3{w.x, w.y, w.z}, at) - self_radius_cm - target.radius_cm();
    ctx.angle = angle_between_degrees(std::atan2(at.y - w.y, at.x - w.x) * 180.0 / 3.14159265358979323846, w.yaw_degrees);
    ctx.centre_distance = std::sqrt((at.x - w.x) * (at.x - w.x) + (at.y - w.y) * (at.y - w.y) + (at.z - w.z) * (at.z - w.z));
}

double eval_fight_roll(const CondAtom& a, stats::SplitMix& rng, std::map<std::string, double>& fight_rolls) {
    const auto known = fight_rolls.find(a.name);
    if (known != fight_rolls.end()) return known->second;
    double v = a.value;
    if (rng.rand_double() < a.chance) v = a.low + (a.high - a.low) * rng.rand_double();
    fight_rolls.emplace(a.name, v);
    return v;
}

bool task_gates_ready(const CreatureAbility& t, const std::map<std::string, double>& cooldown_until, double now) {
    for (const std::string& g : t.gates) {
        const auto until = cooldown_until.find(g);
        if (until != cooldown_until.end() && now < until->second) return false;
    }
    return true;
}

bool task_groups_hold(const CreatureAbility& t, const CondContext& ctx) {
    for (const CondGroup& g : t.groups) {
        if (!eval_group(g, ctx)) return false;
    }
    return true;
}

bool task_geometry_allows(const CreatureAbility& t, const CondContext& ctx, bool& too_far, bool& off_angle, double& reach_cm) {
    if (!ctx.has_target) return t.min_distance <= 0;
    if (t.max_distance >= 0 && ctx.surface_distance > t.max_distance) {
        too_far = true;
        if (t.min_distance <= 0 && (reach_cm < 0 || t.max_distance < reach_cm)) reach_cm = t.max_distance;
        return false;
    }
    if (t.min_distance >= 0 && ctx.surface_distance < t.min_distance) return false;
    if (t.max_angle >= 0 && ctx.angle > t.max_angle) {
        off_angle = true;
        return false;
    }
    return true;
}

bool first_enemy_hit_gate(const abilities::AbilityDef& d, const abilities::Catalog& catalog, const NpcAbilityData& anim, BiteArea::Gate& gate) {
    for (const content::AbilityHitRow& r : d.hits) {
        const abilities::HitDef* hit = catalog.hit(r.hit_guid);
        if (r.source != "hit" || !hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
        const BiteArea* area = anim.area_of(d.row.guid, static_cast<int>(r.key));
        if (area) gate = area->gate();
        return true;
    }
    return false;
}

const MontageData* choose_playable_montage(const abilities::AbilityDef& def, const NpcAbilityData& anim, const abilities::Catalog& catalog,
                                           Fighter& caster, stats::SplitMix& rng) {
    const content::MontageRow* chosen = nullptr;
    std::string err;
    std::unique_ptr<stats::CharacterStats::Evaluation> evaluation;
    SelectHost host(rng);
    for (const content::MontageRow& m : def.montages) {
        if (m.soft_path.empty()) continue;
        const MontageData* playable = anim.montage(m.soft_path);
        if (!playable || playable->root_unreadable) continue;
        chosen = &m;
        if (m.select_expr == 0) break;
        const content::ExprRow* row = catalog.expr(m.select_expr);
        if (!row || row->program < 0) continue;
        if (!evaluation) evaluation = caster.stats().evaluation(caster.worn(), err, true);
        double v = 0;
        std::string e;
        if (evaluation && evaluation->eval(row->program, host, v, e) && v != 0.0) break;
    }
    return chosen ? anim.montage(chosen->soft_path) : nullptr;
}

NpcCast make_npc_cast(int64_t ability_guid, uint64_t target, PlayerPort& port, uint64_t montage_guid) {
    NpcCast cast;
    cast.ability = ability_guid;
    cast.target = target;
    cast.target_guid = port.cylinder_guid();
    const Vec3 at = port.fighter().position();
    cast.tx = at.x;
    cast.ty = at.y;
    cast.tz = at.z;
    cast.montage = montage_guid;
    return cast;
}

bool start_montage_cast(world::ActorRegistry& registry, uint64_t actor_id, const MontageData& montage, const NpcCast& cast, NpcCastState& state) {
    const auto m = bootstrap::montage_for(montage.path, montage.guid, montage.package_guid);
    return m && start_npc_cast(registry, actor_id, state, cast, m->chain);
}

npc::NpcWorld::RootMotionPlay build_root_motion_play(const world::WorldActor* me, double base_radius, const MontageData& montage,
                                                     const RootMotionFreezeRule* rule) {
    const double scale = me ? npc::actor_scale_z(*me) : 1.0;
    npc::NpcWorld::RootMotionPlay play;
    play.montage = montage.guid;
    play.length = montage.length;
    play.rate = montage.rate_scale;
    for (const MontageData::Step& s : montage.root_motion) play.steps.push_back({s.time, s.x * scale, s.y * scale});
    play.own_radius = base_radius * scale;
    if (rule) {
        play.freeze_stop = rule->kind == RootMotionFreezeRule::Kind::Stop;
        play.freeze_pause = rule->kind == RootMotionFreezeRule::Kind::Pause;
        play.freeze_distance = rule->distance;
        play.freeze_scale = rule->radius_scale;
        play.ignore_non_target = rule->ignore_non_target;
    }
    return play;
}

bool begin_player_blocked_root_motion(npc::NpcWorld* npcs_world, uint64_t actor_id, const std::vector<PlayerPort*>& players, uint64_t target,
                                      npc::NpcWorld::RootMotionPlay play) {
    if (!npcs_world) return false;
    const double own_radius = play.own_radius;
    play.blockers = [&players, own_radius, target](std::vector<npc::NpcWorld::RootMotionBlock>& out) {
        for (PlayerPort* p : players) {
            if (!p->fighter().alive()) continue;
            const Vec3 at = p->fighter().position();
            out.push_back({at.x, at.y, own_radius + p->fighter().radius_cm(), p->fighter().radius_cm(), p->actor_id() == target});
        }
    };
    return npcs_world->begin_root_motion(actor_id, std::move(play));
}

void record_task_cooldowns(const CreatureAbility& task, double base_time, stats::SplitMix& rng, std::map<std::string, double>& cooldown_until) {
    if (!task.task.empty() && task.cooldown > 0) cooldown_until[task.task] = base_time + task.cooldown;
    for (const CooldownStart& c : task.starts) {
        cooldown_until[c.name] = base_time + (c.high > c.low ? c.low + (c.high - c.low) * rng.rand_double() : c.low);
    }
}

template <typename AttackT>
void begin_task_rotation(npc::NpcWorld* npcs_world, uint64_t actor_id, const AttackT& a, double until,
                         std::function<bool(nav::Vec3&)> target_pos) {
    if (!a.task || !npcs_world || (!a.task->rotates && !attack_keeps_turning(a.ability->row.name))) return;
    const double rate = a.task->rotates ? a.task->rotate_speed : 0.0;
    npcs_world->track_target(actor_id, std::move(target_pos), rate, until);
}

double compute_unanimated_next_attack(const npc_kits::Kit* kit, const CombatWorldConfig& cfg, double now, stats::SplitMix& rng) {
    const double interval = (kit && kit->interval_ms > 0 ? kit->interval_ms / 1000.0 : cfg.default_attack_interval);
    return now + interval * (1.0 + cfg.attack_variance * (2.0 * rng.rand_double() - 1.0));
}

template <typename T>
void clamp_schedule_times(std::vector<std::pair<double, T>>& items, double& due) {
    for (size_t i = 1; i < items.size(); ++i) items[i].first = std::max(items[i].first, items[i - 1].first);
    if (!items.empty()) due = std::min(due, items.front().first);
}

void collect_unanimated_hits(const abilities::AbilityDef& ability, std::vector<int64_t>& out) {
    std::set<int64_t> seen;
    for (const content::AbilityHitRow& h : ability.hits) {
        if ((h.source == "hit" || h.source == "projectile") && seen.insert(h.hit_guid).second) out.push_back(h.hit_guid);
    }
}

template <typename NpcT, typename AttackT, typename PlanT>
void apply_animated_timing(NpcT& n, const AttackT& a, const PlanT& plan, double now, stats::SplitMix& rng) {
    const double done = plan.montage ? plan.montage->task_done_after(1.0) : 0.0;
    if (a.task) {
        record_task_cooldowns(*a.task, now + done, rng, n.cooldown_until);
        if (a.task->stop_movement || attack_keeps_its_stop(a.ability->row.name)) n.hold_until = now + plan.seconds;
    }
    n.next_attack_at = now + plan.seconds;
    set_attack_walk_share(n, attack_walk_share(a.ability->row.name));
}

template <typename NpcT, typename AttackT, typename PlanT>
bool maybe_log_npc_cast(size_t& logged, size_t limit, const NpcT& n, const AttackT& a, const PlanT& plan) {
    if (logged >= limit) return false;
    ++logged;
    spdlog::info("[combat] {} ({}) plays {}: {} hit(s) and {} projectile(s) over {:.2f} s{}", n.actor_id, n.leaf, a.ability->row.name,
                 plan.hits.size(), plan.fires.size(), plan.seconds, a.task && a.task->stop_movement ? ", standing still" : "");
    return true;
}

template <typename PendingT, typename PlanT>
void populate_animated_pending(PendingT& p, const PlanT& plan, const NpcAbilityData* anim, int64_t ability_guid, double now) {
    p.animated = true;
    for (const auto& h : plan.hits) {
        p.timed_hits.emplace_back(now + std::max(h.delay, kMinHitDelay), h.hit);
        const BiteArea* area = anim ? anim->area_of(ability_guid, h.key) : nullptr;
        p.timed_areas.push_back(area ? *area : BiteArea{});
    }
    for (const auto& fire : plan.fires) p.timed_fires.emplace_back(now + std::max(fire.delay, kMinHitDelay), fire);
    p.end_at = p.due = now + plan.seconds;
    clamp_schedule_times(p.timed_hits, p.due);
    clamp_schedule_times(p.timed_fires, p.due);
}

}  // namespace

CombatWorld::Attack CombatWorld::choose_attack(Npc& n, bool single_swing) {
    Attack a;
    a.windup_ms = static_cast<int>(cfg_.default_windup * 1000);
    a.range_cm = static_cast<int>(cfg_.default_attack_range_cm);
    if (n.anim && !single_swing) {
        const Attack b = choose_animated(n, nullptr);
        if (b.ability) return b;
    } else if (n.kit) {
        const npc_kits::KitAbility* picked_k = nullptr;
        if (const abilities::AbilityDef* def = pick_kit_ability(*n.kit, catalog_, data_.ids().health, rng_, picked_k)) {
            a.ability = def;
            a.windup_ms = picked_k->windup_ms;
            a.range_cm = picked_k->range_cm;
            return a;
        }
    }
    if (const abilities::AbilityDef* d = catalog_.ability_named(cfg_.fallback_ability)) {
        if (is_attack(*d, catalog_, data_.ids().health)) a.ability = d;
    }
    return a;
}

CombatWorld::Attack CombatWorld::choose_animated(Npc& n, const Fighter* target, TreeWhy* why) {
    Attack a;
    a.windup_ms = static_cast<int>(cfg_.default_windup * 1000);
    a.range_cm = static_cast<int>(cfg_.default_attack_range_cm);
    if (!n.anim) return a;
    CondContext ctx;
    ctx.preview = target == nullptr;
    ctx.has_target = target != nullptr;
    if (target) {
        const world::WorldActor* w = registry_.find(n.actor_id);
        if (!w) return a;
        populate_target_geometry(*w, n.radius_cm(), *target, ctx);
        const Vec3 from{w->x, w->y, w->z};
        const Vec3 at = target->position();
        ctx.sees = [this, from, at]() { return line_clear(from, at); };
    }
    ctx.health_fraction = n.max_health() > 0 ? std::clamp(n.health() / n.max_health(), 0.0, 1.0) : 1.0;
    ctx.fight_seconds = std::max(0.0, now_ - n.fight_began);
    ctx.cooldown_over = [&n, this](const std::string& g) { return task_gates_ready(CreatureAbility{.gates = {g}}, n.cooldown_until, now_); };
    ctx.roll = [this]() { return rng_.rand_double(); };
    ctx.fight_roll = [&n, this](const CondAtom& atom) { return eval_fight_roll(atom, rng_, n.fight_rolls); };
    CondContext timers = ctx;
    timers.timers_only = true;
    size_t playable = 0, shut_by_timers = 0;
    bool dummy_far = false, dummy_angle = false;
    double dummy_reach = -1.0;
    for (const CreatureAbility& t : *n.anim) {
        if (t.unknown || t.off || !t.fight) continue;
        const abilities::AbilityDef* d = catalog_.ability(t.ability);
        if (!d || !is_attack(*d, catalog_, data_.ids().health) || !animates(n, *d)) continue;
        ++playable;
        if (!task_groups_hold(t, timers)) ++shut_by_timers;
        if (!task_gates_ready(t, n.cooldown_until, now_)) continue;
        if (!task_geometry_allows(t, ctx, why ? why->too_far : dummy_far, why ? why->off_angle : dummy_angle, why ? why->reach_cm : dummy_reach)) continue;
        if (!task_groups_hold(t, ctx)) continue;
        a.ability = d;
        a.task = &t;
        if (t.max_distance >= 0) a.range_cm = static_cast<int>(t.max_distance);
        return a;
    }
    if (why) why->waits_on_timers = playable > 0 && shut_by_timers == playable;
    return a;
}

bool CombatWorld::line_clear(const Vec3& from, const Vec3& to) const {
    if (!nav_) return true;
    const nav::Vec3 p = to_nav(from), q = to_nav(to);
    if (!nav_->on_navmesh(p, 150, 300) || !nav_->on_navmesh(q, 150, 300)) return true;
    return nav_->line_of_sight(p, q);
}

bool CombatWorld::hit_reaches(const Npc& n, const Fighter& body, const BiteArea& area, bool is_target) const {
    const world::WorldActor* w = registry_.find(n.actor_id);
    if (!w) return false;
    const Vec3 at = body.position();
    HitSubject s;
    s.cx = w->x;
    s.cy = w->y;
    s.yaw = w->yaw_degrees;
    s.tx = at.x;
    s.ty = at.y;
    s.tradius = body.radius_cm();
    const Vec3 from{w->x, w->y, w->z};
    return hit_lands(area, s, is_target, [this, from, at]() { return line_clear(from, at); });
}

uint64_t CombatWorld::aimed_at(const Npc& n, const BiteArea& area, uint64_t cast_target) {
    using D = BiteArea::Discovery;
    PlayerPort* locked = player(cast_target);
    const bool locked_ok = locked && locked->fighter().alive();
    const uint64_t fighting = n.ai.target();
    PlayerPort* active = fighting ? player(fighting) : nullptr;
    const bool active_ok = active && active->fighter().alive();
    switch (area.discovery) {
        case D::PrefersActive: return active_ok ? fighting : (locked_ok ? cast_target : 0);
        case D::OnlyActive: return active_ok ? fighting : 0;
        case D::OnlyLocked: return locked_ok ? cast_target : 0;
        case D::PrefersLocked: return locked_ok ? cast_target : (active_ok ? fighting : 0);
        case D::None: break;
    }
    return 0;
}

bool CombatWorld::can_animate(const Npc& n) const {
    if (!anim_ || !n.anim) return false;
    for (const CreatureAbility& t : *n.anim) {
        if (t.unknown || t.off || !t.fight) continue;
        const abilities::AbilityDef* d = catalog_.ability(t.ability);
        if (d && is_attack(*d, catalog_, data_.ids().health) && animates(n, *d)) return true;
    }
    return false;
}

bool CombatWorld::launches_projectiles(const abilities::AbilityDef& def) const {
    if (!anim_) return false;
    for (const content::MontageRow& m : def.montages) {
        const MontageData* md = m.soft_path.empty() ? nullptr : anim_->montage(m.soft_path);
        if (!md) continue;
        for (const MontageData::Fire& fire : md->fires) {
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source == "projectile" && r.key == fire.key && catalog_.projectile(r.owner_guid)) return true;
            }
        }
    }
    return false;
}

void CombatWorld::stand_off_of(const Npc& n, double& arrival, double& reach) const {
    arrival = reach = -1;
    if (!n.anim) return;
    for (const CreatureAbility& t : *n.anim) {
        if (t.unknown || t.off || !t.fight || !(t.arrival_cm > 0)) continue;
        const abilities::AbilityDef* d = catalog_.ability(t.ability);
        if (!d || !is_attack(*d, catalog_, data_.ids().health) || !animates(n, *d) || !launches_projectiles(*d)) continue;
        arrival = t.arrival_cm;
        reach = std::max(reach, t.max_distance > 0 ? t.max_distance : t.arrival_cm);
    }
}

bool CombatWorld::montages_readable(const abilities::AbilityDef& def) const {
    if (!anim_) return false;
    bool data = false;
    for (const content::MontageRow& m : def.montages) {
        if (m.soft_path.empty()) continue;
        const MontageData* md = anim_->montage(m.soft_path);
        if (!md) return false;
        if (md->root_unreadable) continue;
        data = true;
    }
    return data;
}

bool CombatWorld::areas_usable(const abilities::AbilityDef& def) const {
    if (!anim_) return false;
    for (const content::AbilityHitRow& r : def.hits) {
        if (r.source != "hit") continue;
        const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
        if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
        const BiteArea* area = anim_->area_of(def.row.guid, static_cast<int>(r.key));
        if (!area || !area->usable()) return false;
    }
    return true;
}

bool CombatWorld::grabs_target(const abilities::AbilityDef& def) const {
    for (const content::AbilityHitRow& r : def.hits) {
        if (r.source != "hit") continue;
        const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
        if (!hit) continue;
        for (const content::HitEffectRow& applied : hit->effects) {
            if (applied.kind != "apply") continue;
            const abilities::EffectDef* effect = catalog_.effect(applied.effect_guid);
            if (effect && std::find(effect->row.tags.begin(), effect->row.tags.end(), "Debuff.Tripped") != effect->row.tags.end()) return true;
        }
    }
    return false;
}

bool CombatWorld::animates(const Npc& n, const abilities::AbilityDef& def) const {
    if (!anim_ || !n.anim) return false;
    bool listed = false;
    for (const CreatureAbility& t : *n.anim) listed = listed || t.ability == def.row.guid;
    if (!listed) return false;
    const world::WorldActor* a = registry_.find(n.actor_id);
    if (!a || !npc_knows_ability(*a, def.row.guid)) return false;
    return montages_readable(def) && areas_usable(def) && !grabs_target(def);
}

std::string CombatWorld::GateCensus::text() const {
    return "creature attacks by where their hits land: " + std::to_string(any) + " on the target wherever it stands, " + std::to_string(validity) + " inside a cone, " + std::to_string(volumes) +
           " by volumes; " + std::to_string(kept_swing) + " keep the single swing (a volume that cannot be placed); " + std::to_string(unplayed) + " have no hit of their montage to decide by (a thrown rock, a bolt)";
}

CombatWorld::GateCensus CombatWorld::gate_census() const {
    GateCensus out;
    if (!anim_) return out;
    using G = BiteArea::Gate;
    std::set<int64_t> seen;
    for (const auto& [archetype, tasks] : anim_->by_kind()) {
        for (const CreatureAbility& t : tasks) {
            if (t.unknown || t.off || !t.fight || !seen.insert(t.ability).second) continue;
            const abilities::AbilityDef* d = catalog_.ability(t.ability);
            if (!d || !is_attack(*d, catalog_, data_.ids().health) || !montages_readable(*d)) continue;
            if (!areas_usable(*d)) {
                ++out.kept_swing;
                out.kept_names.push_back(d->row.name);
                continue;
            }
            G gate = G::None;
            if (!first_enemy_hit_gate(*d, catalog_, *anim_, gate)) {
                ++out.unplayed;
                out.unplayed_names.push_back(d->row.name);
                continue;
            }
            ++(gate == G::Validity ? out.validity : gate == G::Volumes ? out.volumes : out.any);
            (gate == G::Validity ? out.validity_names : gate == G::Volumes ? out.volume_names : out.any_names).push_back(d->row.name);
        }
    }
    return out;
}

bool CombatWorld::plan_cast(Npc& n, const abilities::AbilityDef& def, CastPlan& out) {
    out = CastPlan{};
    if (!animates(n, def)) return false;
    const MontageData* montage = choose_playable_montage(def, *anim_, catalog_, n, rng_);
    if (!montage || montage->length <= 0 || montage->rate_scale <= 0) return false;
    for (const MontageData::Hit& h : montage->hits) {
        for (const content::AbilityHitRow& r : def.hits) {
            if (r.source != "hit" || r.key != h.key) continue;
            out.hits.push_back({h.time / montage->rate_scale, r.hit_guid, static_cast<int>(r.key)});
            break;
        }
    }
    for (const MontageData::Fire& f : montage->fires) {
        PlannedFire fire;
        fire.delay = f.time / montage->rate_scale;
        for (const content::AbilityHitRow& r : def.hits) {
            if (r.source != "projectile" || r.key != f.key) continue;
            fire.projectile = r.owner_guid;
            if (std::find(fire.hits.begin(), fire.hits.end(), r.hit_guid) == fire.hits.end()) fire.hits.push_back(r.hit_guid);
        }
        if (fire.projectile != 0 && catalog_.projectile(fire.projectile)) out.fires.push_back(std::move(fire));
    }
    if (out.hits.empty() && out.fires.empty()) return false;
    out.montage = montage;
    out.seconds = montage->seconds(1.0);
    return true;
}

void CombatWorld::start_attack(Npc& n, uint64_t target, double now, const Attack* picked) {
    PlayerPort* port = player(target);
    set_attack_walk_share(n, 1.0);
    Attack a = n.next;
    if (n.anim && port) {
        a = picked ? *picked : choose_animated(n, &port->fighter());
        if (!a.ability) return;
    }
    CastPlan plan;
    bool animated = false;
    if (a.ability && n.anim && port && a.task && plan_cast(n, *a.ability, plan)) {
        if (a.task && npcs_world_) npcs_world_->halt(n.actor_id);
        animated = start_montage_cast(registry_, n.actor_id, *plan.montage, make_npc_cast(a.ability->row.guid, target, *port, plan.montage->guid), n.cast);
        if (animated && !plan.montage->root_motion.empty()) {
            const RootMotionFreezeRule* rule = anim_ ? anim_->freeze_of(a.ability->row.guid) : nullptr;
            if (!begin_player_blocked_root_motion(npcs_world_, n.actor_id, players_, target, build_root_motion_play(registry_.find(n.actor_id), n.radius_cm(), *plan.montage, rule))) {
                end_npc_cast(registry_, n.actor_id, n.cast);
                animated = false;
            }
        }
    }
    if (animated) {
        apply_animated_timing(n, a, plan, now, rng_);
        const double rot_until = now + (a.task && a.task->rotates && a.task->rotate_seconds > 0 ? std::min(a.task->rotate_seconds, plan.seconds) : plan.seconds);
        begin_task_rotation(npcs_world_, n.actor_id, a, rot_until, [this, target](nav::Vec3& out) {
            PlayerPort* q = player(target);
            if (!q || !q->fighter().alive()) return false;
            out = to_nav(q->fighter().position());
            return true;
        });
    } else {
        n.next_attack_at = compute_unanimated_next_attack(n.kit, cfg_, now, rng_);
    }
    n.next = choose_attack(n);
    if (!a.ability) {
        ++stats_.refused_no_attack;
        return;
    }
    Pending p{.npc = n.actor_id, .target = target, .ability = a.ability};
    if (animated) {
        p.logged = maybe_log_npc_cast(npc_casts_logged_, kLoggedNpcCasts, n, a, plan);
        populate_animated_pending(p, plan, anim_, a.ability->row.guid, now);
    } else {
        p.due = now + a.windup_ms / 1000.0;
        collect_unanimated_hits(*a.ability, p.hits);
    }
    pending_.push_back(std::move(p));
    ++stats_.npc_attacks;
}

}  // namespace aoc::worldd::combat
