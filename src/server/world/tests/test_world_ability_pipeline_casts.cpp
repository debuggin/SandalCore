// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_world_ability_pipeline_common.h"

#include "combat/combat_data.h"
#include "combat/hit_port.h"
#include "combat/hit_resolver.h"

using namespace ability_rig;

namespace {

struct CastFighter final : combat::Fighter {
    Rig&                 rig;
    uint64_t             actor_id = 0;
    combat::Vec3         pos{};
    std::vector<int64_t> aligns;

    CastFighter(Rig& r, uint64_t id, combat::Vec3 p, std::vector<int64_t> a)
        : rig(r), actor_id(id), pos(p), aligns(std::move(a)) {}

    uint64_t id() const override { return actor_id; }
    bool is_player() const override { return actor_id == 77; }
    int level() const override { return 25; }
    combat::Vec3 position() const override { return pos; }
    bool alive() const override { return true; }
    const std::vector<int64_t>& alignments() const override { return aligns; }
    stats::CharacterStats& stats() override { return *rig.cs; }
};

struct CastPort final : combat::HitPort {
    Rig                                    caster;
    Rig                                    target_rig;
    std::optional<combat::CombatData>      combat_data;
    std::unique_ptr<combat::HitResolver>   hit_resolver;
    CastFighter                            self_fighter;
    CastFighter                            enemy_fighter;
    double                                 health_lost = 0.0;
    std::string                            problem;

    CastPort(uint64_t seed, double enemy_x)
        : caster(8, 25, {}, true, seed),
          target_rig(4, 25, {}, true, seed),
          self_fighter(caster, 77, {0.0, 0.0, 1000.0}, combat::CombatData::player_alignments()),
          enemy_fighter(target_rig, 900, {enemy_x, 0.0, 1000.0}, {}) {
        if (!caster.ok() || !target_rig.ok()) {
            problem = caster.problem + target_rig.problem;
            return;
        }
        const auto db = content::ContentDb::open(real_db_path(), problem);
        const auto rows = db ? db->combat_rows(problem) : std::nullopt;
        if (rows) combat_data = combat::CombatData::build(*rows, *caster.ctx->bundle, problem);
        if (!combat_data) return;
        hit_resolver = std::make_unique<combat::HitResolver>(*caster.catalog, *combat_data, seed);
        caster.pipe->set_combat(this);
    }

    bool ok() const { return problem.empty() && combat_data.has_value() && hit_resolver != nullptr; }

    combat::TargetInfo inspect(uint64_t actor) override {
        if (actor != enemy_fighter.actor_id) return {};
        const double center_dist = combat::distance_cm(self_fighter.position(), enemy_fighter.position());
        return {true, true, combat::Relation::Hostile, true, std::max(0.0, center_dist - 100.0), enemy_fighter.position(), 50.0, false};
    }

    combat::Fighter* fighter(uint64_t actor) override {
        return actor == enemy_fighter.actor_id ? &enemy_fighter : nullptr;
    }

    combat::HitResolver& resolver() override { return *hit_resolver; }
    const combat::CombatData& data() const override { return *combat_data; }

    combat::Delivery deliver(uint64_t actor, const combat::Resolved& hit, double /*now*/) override {
        combat::Delivery d;
        d.applied = (actor == enemy_fighter.actor_id);
        d.flags = hit.flags;
        d.target_object = actor + 4;
        for (const combat::ResolvedMod& m : hit.mods) {
            d.effective.push_back(m.value);
            if (m.value < 0.0) health_lost += -m.value;
        }
        return d;
    }

    void shield(uint64_t /*actor*/, double /*amount*/, double /*seconds*/, double /*now*/) override {}

    aoc::protocol::emit::FIntrepidNetworkGUID guid_of(uint64_t actor) override {
        return aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(actor);
    }

    aoc::protocol::emit::FIntrepidNetworkGUID hit_component_of(uint64_t actor) override {
        return aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(actor + 4);
    }

    double reach_cm(bool melee) const override { return melee ? 1500.0 : 4500.0; }
    const combat::Fighter& self() const override { return self_fighter; }

    std::vector<combat::GatheredTarget> enemies_touching(const combat::Vec3& centre, double radius_cm, size_t max) override {
        const double dist = combat::distance_cm(centre, enemy_fighter.position());
        if (max == 0 || dist > radius_cm + 50.0) return {};
        return {{enemy_fighter.actor_id, dist}};
    }
};

int64_t hit_targeting_guid(const abilities::AbilityDef& def, int64_t hit_guid) {
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& h : def.hits) {
        if (h.hit_guid != hit_guid || h.source != "hit") continue;
        for (const content::TargetingRow& tg : def.targetings) {
            if (tg.key == h.key) targeting = tg.guid;
        }
    }
    return targeting;
}

struct StrikeOutcome {
    bool    ok = false;
    bool    jumped = false;
    double  value = 0.0;
    uint8_t flags = 0;
};

StrikeOutcome run_lightning_strike(uint64_t seed, int how) {
    StrikeOutcome out;
    CastPort port(seed, 600.0);
    if (!port.ok()) return out;
    const abilities::AbilityDef* def = port.caster.catalog->ability_named("Mage_LightningStrike");
    const abilities::HitDef* hit = port.caster.catalog->hit_named("Mage_LightningStrike_Hit");
    if (!def || !hit) return out;
    port.caster.know({"Mage_LightningStrike"});
    constexpr int64_t kCastComplete = 6064628894971461633ll;
    constexpr double t0 = 1.0;
    constexpr int32_t id = 41;
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {port.enemy_fighter.actor_id};
    if (port.caster.pipe->trigger(def->row.guid, id, t0, info) != Refusal::None) return out;
    double ask = t0 + 1.2;
    if (how == 0) {
        port.caster.run_to(t0 + 2.416);
        out.jumped = port.caster.pipe->branch_request(id, kCastComplete, t0 + 2.416);
        ask = t0 + 3.2;
    } else if (how == 1) {
        port.caster.run_to(t0 + 0.171);
        out.jumped = port.caster.pipe->branch_request(id, kCastComplete, t0 + 0.171);
    } else {
        port.caster.run_to(t0 + 0.3);
        out.jumped = port.caster.pipe->cancel(id, t0 + 0.3, 1.0e6);
    }
    port.caster.run_to(ask);
    const int64_t targeting = hit_targeting_guid(*def, hit->row.guid);
    port.caster.pipe->hit_request(def->row.guid, targeting, hit->row.guid, 1, 0, ask, {port.enemy_fighter.actor_id});
    port.caster.run_to(ask + 1.0);
    for (const Seen& s : port.caster.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid != hit->row.guid || c.targets.empty() || c.targets[0].mods.empty()) continue;
            out.ok = true;
            out.value = c.targets[0].mods[0].value;
            out.flags = c.targets[0].flags;
        }
    }
    return out;
}

void test_lightning_strike() {
    std::printf("Lightning Strike: full charge, quick tap and clamped release damage scaling\n");
    bool done = false;
    for (uint64_t seed = 1; seed <= 12 && !done; ++seed) {
        const StrikeOutcome full = run_lightning_strike(seed, 0);
        const StrikeOutcome tap = run_lightning_strike(seed, 1);
        const StrikeOutcome forged = run_lightning_strike(seed, 2);
        if (!full.ok || !tap.ok || !forged.ok || full.flags != 0 || tap.flags != 0 || forged.flags != 0 || full.value == 0.0) continue;
        done = true;
        const double tap_share = tap.value / full.value;
        const double forged_share = forged.value / full.value;
        CHECK(full.jumped && tap.jumped && forged.jumped, "branch completion and release transitions are accepted");
        CHECK(full.value < 0.0, "a full hold of Lightning Strike deals damage");
        CHECKF(tap_share > 0.25 && tap_share < 0.42, "a quick tap at 171 ms deals one third of a full bar's damage: %.3f", tap_share);
        CHECKF(forged_share > 0.30 && forged_share < 0.55, "a release at 0.3 s claiming a million-second hold is clamped to 0.3 s: %.3f", forged_share);
    }
    CHECK(done, "found a deterministic seed where full hold, quick tap and clamped release all land as plain hits");
}

struct BallOutcome {
    bool   ok = false;
    bool   forged_refused = false;
    int    hits = 0;
    int    after_death = 0;
    double health_lost = 0.0;
};

int64_t ball_projectile_hit_guid(const abilities::AbilityDef& def) {
    for (const content::AbilityHitRow& h : def.hits) {
        if (h.source == "projectile") return h.hit_guid;
    }
    return 0;
}

int count_new_ball_hits(const std::vector<Seen>& log, size_t& cursor, int64_t ball_hit) {
    int added = 0;
    for (; cursor < log.size(); ++cursor) {
        for (const bootstrap::HitConfirm& c : log[cursor].confirmations) {
            if (c.record_guid == ball_hit && !c.targets.empty()) ++added;
        }
    }
    return added;
}

BallOutcome run_ball_lightning(bool kill_after_first_hit) {
    BallOutcome out;
    CastPort port(7, 1000.0);
    if (!port.ok()) return out;
    const abilities::AbilityDef* def = port.caster.catalog->ability_named("Mage_BallLightning");
    const int64_t ball_hit = def ? ball_projectile_hit_guid(*def) : 0;
    if (!def || ball_hit == 0) return out;
    port.caster.know({"Mage_BallLightning"});
    constexpr int64_t kBranch = 6064629048751489028ll;
    constexpr double t0 = 1.0;
    const int32_t id = port.caster.next_client_id;
    if (port.caster.trigger("Mage_BallLightning", t0, false) != Refusal::None) return out;
    const auto forged = port.caster.pipe->hit_request(def->row.guid, content::kTargetingDefaultSelf, ball_hit, 1, 0, t0 + 0.4, {port.enemy_fighter.actor_id});
    out.forged_refused = (forged == AbilityPipeline::HitVerdict::Quiet);
    size_t seen = 0;
    bool branched = false, killed = false;
    for (int step = 1; step <= 220; ++step) {
        const double now = t0 + 0.05 * step;
        if (!branched && now - t0 >= 1.25 - 1e-9) {
            branched = true;
            port.caster.pipe->branch_request(id, kBranch, now);
        }
        port.caster.run_to(now);
        const int step_hits = count_new_ball_hits(port.caster.log, seen, ball_hit);
        out.hits += step_hits;
        if (killed) out.after_death += step_hits;
        if (kill_after_first_hit && !killed && out.hits >= 1) {
            killed = true;
            port.caster.pipe->set_dead(true);
        }
    }
    out.ok = port.caster.pawn.broken.empty();
    out.health_lost = port.health_lost;
    return out;
}

void test_ball_lightning() {
    std::printf("Ball Lightning: projectile ticks every 0.5 s and continues flying after the caster dies\n");
    const BallOutcome alive = run_ball_lightning(false);
    const BallOutcome dead = run_ball_lightning(true);
    CHECK(alive.ok && dead.ok, "Ball Lightning cast runs cleanly in both alive and post-death scenarios");
    CHECK(alive.forged_refused, "a direct client hit request for the ball's projectile hit is refused");
    CHECKF(alive.hits >= 5 && alive.health_lost > 0.0, "the launched ball hits the enemy 10 m ahead repeatedly while the Mage lives (%d hits, %.1f Health lost)", alive.hits, alive.health_lost);
    CHECKF(dead.hits == alive.hits && dead.after_death == dead.hits - 1,
           "the ball keeps flying after the Mage dies: %d total hits, %d after death", dead.hits, dead.after_death);
    CHECKF(dead.health_lost >= alive.health_lost * 0.5, "post-death ball hits continue to deal damage (%.1f vs %.1f)", dead.health_lost, alive.health_lost);
}

}  // namespace

int main() {
    if (!real_db_present()) {
        std::printf("[skip] ability pipeline cast tests: no content.db at %s: skipped\n", real_db_path().c_str());
        return 0;
    }
    test_lightning_strike();
    test_ball_lightning();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
