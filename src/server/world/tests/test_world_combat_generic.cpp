#include "combat_test_fixture.h"

#include <chrono>
#include <fstream>

namespace sandal::worldd::tests {
namespace {

struct GenericFightPlan {
    const char* leaf;
    const char* name;
    double      seconds;
    bool        timid;
    size_t      montages;
    double      travel;
    size_t      casts;
    size_t      due;
};

struct GenericCast {
    double      t;
    int64_t     ability;
    uint64_t    montage;
    std::string label;
};

void inspect_ability_montages(const abilities::AbilityDef& def, const combat::NpcAbilityData& tables, size_t& montages, size_t& unreadable, double& travel) {
    for (const content::MontageRow& m : def.montages) {
        const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
        if (!md) continue;
        ++montages;
        if (md->root_unreadable) {
            ++unreadable;
        } else if (!md->root_motion.empty()) {
            travel = std::max(travel, std::hypot(md->root_motion.back().x, md->root_motion.back().y));
        }
    }
}

double check_generic_montages(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& path, const GenericFightPlan& plan) {
    Fixture& f = h.f;
    double travel = 0;
    size_t montages = 0, unreadable = 0, absent = 0;
    for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
        if (t.unknown || t.off || !t.fight) continue;
        const abilities::AbilityDef* def = f.catalog->ability(t.ability);
        if (!def) {
            ++absent;
            continue;
        }
        inspect_ability_montages(*def, tables, montages, unreadable, travel);
    }
    std::printf("  %zu montages, %zu unreadable, %zu usable abilities missing from the ability data; the farthest montage carries it %.0f cm\n", montages, unreadable, absent, travel);
    CHECKF(absent == 0, "every ability of its usable tasks is in the ability data (%zu missing)", absent);
    CHECKF(montages >= plan.montages && unreadable == 0 && travel >= plan.travel, "its montages read and one carries the creature forward (%zu montages, %zu unreadable, %.0f cm)", montages, unreadable, travel);
    return travel;
}

void check_generic_open_records(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& path, double half_height, const spawns::OpenTemplate* open) {
    combat_rig::World probe;
    if (!probe.problem.empty()) return;
    const uint64_t id = probe.spawn(path, 0, 0, 7, 900, half_height, open);
    const world::WorldActor* a = probe.registry.find(id);
    size_t listed = 0, wanted = 0;
    std::set<int64_t> seen;
    for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
        if (!seen.insert(t.ability).second) continue;
        ++wanted;
        if (a && combat::npc_knows_ability(*a, t.ability)) ++listed;
    }
    CHECKF(a && wanted >= 1 && listed == wanted, "its open lists the %zu abilities of its tree (%zu listed)", wanted, listed);
}

bool match_cast_bite_hit(const GenericCast& c, double at, const std::vector<combat::IncomingHit>& hits, std::vector<bool>& used, double& worst_delay) {
    constexpr double kDt = 1.0 / 15.0;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (used[i] || hits[i].source_guid != c.ability) continue;
        const double delay = hits[i].at - (c.t + at);
        if (delay < -1e-6 || delay > 2 * kDt + 1e-6) continue;
        used[i] = true;
        worst_delay = std::max(worst_delay, delay);
        return true;
    }
    return false;
}

bool hit_falls_in_any_cast(Fixture& f, const combat::NpcAbilityData& tables, const std::vector<GenericCast>& casts, const combat::IncomingHit& hit) {
    constexpr double kDt = 1.0 / 15.0;
    for (const GenericCast& c : casts) {
        const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
        if (md && hit.source_guid == c.ability && hit.at >= c.t - 1e-6 && hit.at <= c.t + md->seconds(1.0) + 2 * kDt) return true;
    }
    return false;
}

void check_generic_cast_hits(TestHarness& h, const combat::NpcAbilityData& tables, const GenericFightPlan& plan, const std::vector<GenericCast>& casts, const std::vector<combat::IncomingHit>& hits) {
    Fixture& f = h.f;
    size_t due = 0, landed = 0, stray = 0;
    double worst_delay = 0;
    std::vector<bool> used(hits.size(), false);
    for (const GenericCast& c : casts) {
        const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
        if (!md || c.t + md->seconds(1.0) > plan.seconds - 0.3) continue;
        for (const double at : cast_bites(f, tables, c.ability, c.montage)) {
            ++due;
            if (match_cast_bite_hit(c, at, hits, used, worst_delay)) ++landed;
        }
    }
    for (size_t i = 0; i < hits.size(); ++i) {
        if (!used[i] && !hit_falls_in_any_cast(f, tables, casts, hits[i])) ++stray;
    }
    std::printf("  %zu hits were due at their notifies, %zu landed (the latest %.3f s after its notify); %zu hits outside a cast\n", due, landed, worst_delay, stray);
    CHECKF(due >= plan.due && landed == due, "every hit of every cast lands at its montage's notify (%zu of %zu)", landed, due);
    CHECKF(stray == 0, "no hit lands outside a cast (%zu)", stray);
}

void check_generic_lunges_and_drift(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& path, const std::vector<LungeSample>& samples, double travel) {
    const auto bp = h.f.motion.find(path);
    const double scale = bp->default_scale;
    size_t far = 0, over = 0;
    double most = 0;
    const std::vector<LungeRun> runs = lunge_runs(samples);
    for (const LungeRun& r : runs) {
        const LungeSample& from = samples[r.first > 0 ? r.first - 1 : r.first];
        const LungeSample& to = samples[r.last + 1];
        const double went = std::hypot(to.wx - from.wx, to.wy - from.wy);
        most = std::max(most, went);
        if (went >= 100.0) ++far;
        if (went > travel * scale + 30.0) ++over;
    }
    std::printf("  %zu lunges, %zu of a metre or more, the longest %.0f cm\n", runs.size(), far, most);
    CHECKF(runs.size() >= 3 && far >= 1, "the montages carry it along their root motion (%zu lunges, %zu of a metre or more)", runs.size(), far);
    CHECKF(over == 0, "no lunge carries it farther than its montage does at its scale %.2f (%zu lunges over %.0f cm)", scale, over, travel * scale + 30.0);

    const double r_creature = bp->radius * scale, r_player = 34.0;
    double reach = 0, chase_began = -1, worst_gap = 0;
    for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
        if (!t.unknown && !t.off && t.fight && t.min_distance <= 0 && t.max_distance > reach) reach = t.max_distance;
    }
    size_t drift_ticks = 0;
    for (const LungeSample& s : samples) {
        if (chase_began < 0 && std::hypot(s.wx, s.wy) > 1.0) chase_began = s.t;
        const double gap = std::hypot(s.wx - s.px, s.wy - s.py) - r_creature - r_player;
        if (chase_began >= 0 && s.t > chase_began + 12.0) {
            worst_gap = std::max(worst_gap, gap);
            if (gap > reach + travel * scale + 60.0) ++drift_ticks;
        }
    }
    std::printf("  reach %.0f cm; the surface gap was at most %.0f cm at any time twelve seconds after the chase began\n", reach, worst_gap);
    CHECKF(drift_ticks == 0, "once it is at him it never drifts away from a player who stands still (%zu ticks farther than reach + travel)", drift_ticks);
}

double max_single_montage_speed(const combat::MontageData& md, double scale) {
    double v_max = 0;
    for (size_t i = 1; i < md.root_motion.size(); ++i) {
        const double dt = md.root_motion[i].time - md.root_motion[i - 1].time;
        if (dt >= 0.01) v_max = std::max(v_max, std::hypot(md.root_motion[i].x - md.root_motion[i - 1].x, md.root_motion[i].y - md.root_motion[i - 1].y) / dt * scale);
    }
    return v_max;
}

double max_montage_root_speed(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, double scale) {
    double v_max = 0;
    for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
        if (t.unknown || t.off || !t.fight) continue;
        const abilities::AbilityDef* def = f.catalog->ability(t.ability);
        if (!def) continue;
        for (const content::MontageRow& m : def->montages) {
            const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
            if (md) v_max = std::max(v_max, max_single_montage_speed(*md, scale));
        }
    }
    return v_max;
}

struct Smooth {
    size_t ticks = 0;
    double step = 0, dz = 0, vz = 0, zspan = 0;
    size_t vz_seen = 0;
};

Smooth measure_smooth(const std::vector<LungeSample>& s) {
    Smooth m;
    double zlo = 1e18, zhi = -1e18;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i].active != 1 || s[i - 1].active != 1) continue;
        ++m.ticks;
        const double step = std::hypot(s[i].wx - s[i - 1].wx, s[i].wy - s[i - 1].wy);
        m.step = std::max(m.step, step);
        m.dz = std::max(m.dz, std::fabs(s[i].wz - s[i - 1].wz));
        zlo = std::min(zlo, s[i].wz);
        zhi = std::max(zhi, s[i].wz);
        if (s[i].has_rrm_vz) {
            ++m.vz_seen;
            m.vz = std::max(m.vz, std::fabs(s[i].rrm_vz));
        }
    }
    m.zspan = zhi >= zlo ? zhi - zlo : 0;
    return m;
}

void check_generic_free_lunge(TestHarness& h, const combat::NpcAbilityData& tables, const GenericFightPlan& plan, const spawns::OpenTemplate* open, double travel) {
    Fixture& f = h.f;
    const std::string path = creature_path(f, plan.leaf);
    const double scale = f.motion.find(path)->default_scale;
    if (scale <= 1.05) return;
    constexpr double kPiFree = 3.14159265358979323846;
    LungeFightSpec spec;
    spec.path = path;
    spec.seconds = plan.seconds;
    spec.player_y = 800;
    spec.timid = plan.timid;
    spec.provoke_at = plan.timid ? 1.0 : -1.0;
    spec.leash_cm = 40000.0;
    spec.made = open;
    spec.steer = [](double, const LungeSample* prev, double& px, double& py, const world::WorldActor& c) {
        if (!prev || prev->active != 1) return false;
        px = c.x + std::cos(c.yaw_degrees * kPiFree / 180.0) * 700.0;
        py = c.y + std::sin(c.yaw_degrees * kPiFree / 180.0) * 700.0;
        return true;
    };
    std::vector<LungeSample> free_run;
    if (!run_lunge_fight(f, tables, spec, free_run)) {
        CHECK(false, "the fight with nobody in the way runs");
        return;
    }
    double longest = 0;
    for (const LungeRun& r : lunge_runs(free_run)) {
        const LungeSample& from = free_run[r.first > 0 ? r.first - 1 : r.first];
        const LungeSample& to = free_run[r.last + 1];
        longest = std::max(longest, std::hypot(to.wx - from.wx, to.wy - from.wy));
    }
    std::printf("  with nobody in the way the longest lunge goes %.0f cm: the farthest montage's %.0f cm at scale %.2f is %.0f\n", longest, travel, scale, travel * scale);
    CHECKF(longest >= 0.9 * travel * scale && longest <= travel * scale + 30.0, "a lunge nobody stands in the way of goes its montage's travel at the creature's scale (%.0f cm of %.0f)", longest, travel * scale);
}

void check_generic_smoothness(TestHarness& h, const combat::NpcAbilityData& tables, const GenericFightPlan& plan, const spawns::OpenTemplate* open, const std::vector<LungeSample>& samples) {
    Fixture& f = h.f;
    constexpr double kDt = 1.0 / 15.0;
    const std::string path = creature_path(f, plan.leaf);
    const double scale = f.motion.find(path)->default_scale;
    const double v_max = max_montage_root_speed(f, tables, path, scale);
    const double step_limit = v_max * kDt * 1.05 + 1.0;
    const Smooth flat = measure_smooth(samples);
    std::printf("  on the plain: %zu ticks carried, the longest step %.1f cm (at most %.1f), the height moves at most %.2f cm a tick, the velocity's vertical part at most %.2f cm/s over %zu updates\n",
                flat.ticks, flat.step, step_limit, flat.dz, flat.vz, flat.vz_seen);
    CHECKF(flat.ticks >= 8 && flat.step <= step_limit, "its steps along the montage stay smooth: none is longer than the fastest the montages carry it (%.1f of %.1f cm)", flat.step, step_limit);
    CHECKF(flat.dz < 0.5 && flat.vz < 0.5 && flat.vz_seen >= 4, "on the plain its height stays where the ground is, and the updates say the velocity has no vertical part (%.2f cm a tick, %.2f cm/s)", flat.dz, flat.vz);

    LungeFightSpec hill_spec;
    hill_spec.path = path;
    hill_spec.seconds = plan.seconds;
    hill_spec.player_x = 4300;
    hill_spec.timid = plan.timid;
    hill_spec.provoke_at = plan.timid ? 1.0 : -1.0;
    hill_spec.leash_cm = 40000.0;
    hill_spec.made = open;
    hill_spec.creature_x = 3500;
    std::vector<LungeSample> hill;
    if (!run_lunge_fight(f, tables, hill_spec, hill)) {
        CHECK(false, "the fight on the hill's flank runs");
        return;
    }
    const Smooth up = measure_smooth(hill);
    const double slope = 400.0 / 3000.0;
    std::printf("  on the hill: %zu ticks carried, the longest step %.1f cm, the height moves at most %.2f cm a tick (slope %.3f: at most %.2f), %.1f cm in all, the velocity's vertical part at most %.2f cm/s\n",
                up.ticks, up.step, up.dz, slope, slope * up.step + 1.0, up.zspan, up.vz);
    CHECKF(up.ticks >= 8 && up.step <= step_limit, "on the hill its steps stay smooth too (%.1f of %.1f cm)", up.step, step_limit);
    CHECKF(up.dz <= slope * up.step + 1.0 && up.zspan > 5.0, "its height follows the ground's slope and nothing else (%.2f cm a tick at most for steps of %.1f cm, %.1f cm in all)", up.dz, up.step, up.zspan);
    CHECKF(up.vz < 0.5 && up.vz_seen >= 4, "and the updates of a lunge up the hill still say the velocity has no vertical part (%.2f cm/s over %zu updates)", up.vz, up.vz_seen);
}

void check_generic_donor_swing(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& path, const GenericFightPlan& plan) {
    Fixture& f = h.f;
    BanditOpen donor;
    CHECKF(make_bandit_open(f, tables, path, donor, false), "the world makes an open for the %s without its tree's abilities (verdict: %s)", plan.name, spawns::verdict_name(donor.why));
    if (!donor.open) return;
    std::vector<LungeSample> plain;
    combat::CombatWorld::Stats plain_stats;
    LungeFightSpec spec;
    spec.path = path;
    spec.seconds = plan.seconds;
    spec.player_y = 800;
    spec.timid = plan.timid;
    spec.provoke_at = plan.timid ? 1.0 : -1.0;
    spec.stats = &plain_stats;
    spec.leash_cm = 40000.0;
    spec.made = donor.open;
    CHECK(run_lunge_fight(f, tables, spec, plain), "the same fight with the donor's own list of records runs");
    size_t plain_casts = 0;
    for (const LungeSample& s : plain) plain_casts += s.cast_began ? 1 : 0;
    std::printf("  with the donor's own records: %zu casts, %llu single swings\n", plain_casts, static_cast<unsigned long long>(plain_stats.npc_attacks));
    CHECKF(plain_casts == 0 && plain_stats.npc_attacks >= 3, "with the donor's own list it plays no montage and keeps the single swing (%zu casts, %llu swings)", plain_casts, static_cast<unsigned long long>(plain_stats.npc_attacks));
}

void check_generic_creature_animates(TestHarness& h, const GenericFightPlan& plan, const std::function<void(TestHarness&, const std::map<std::string, size_t>&)>& more = nullptr) {
    Fixture& f = h.f;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, plan.leaf);
    const auto bp = f.motion.find(path);
    CHECKF(!path.empty() && bp && tables.abilities_of(path) != nullptr, "the Blueprint table has the %s and the creature tables list its abilities", plan.name);
    if (path.empty() || !bp || !tables.abilities_of(path)) return;
    CHECKF(!spawns::fighting_humanoid(path), "the %s is a creature, not one of the humanoids whose fights are built by name", plan.name);

    const double travel = check_generic_montages(h, tables, path, plan);
    BanditOpen made;
    CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the %s (verdict: %s)", plan.name, spawns::verdict_name(made.why));
    if (!made.open) return;
    check_generic_open_records(h, tables, path, bp->half_height, made.open);

    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    combat::CombatWorld::Stats stats;
    LungeFightSpec spec;
    spec.path = path;
    spec.seconds = plan.seconds;
    spec.player_y = 800;
    spec.hits = &hits;
    spec.timid = plan.timid;
    spec.provoke_at = plan.timid ? 1.0 : -1.0;
    spec.stats = &stats;
    spec.leash_cm = 40000.0;
    spec.made = made.open;
    CHECKF(run_lunge_fight(f, tables, spec, samples), "a fight of %.0f s against a player who stands 8 m away runs", plan.seconds);
    if (samples.empty()) return;
    check_targets_named_in_full(h, samples, plan.name);

    std::vector<GenericCast> casts;
    double chase_began = -1;
    std::map<std::string, size_t> by_name;
    for (const LungeSample& s : samples) {
        if (chase_began < 0 && std::hypot(s.wx, s.wy) > 1.0) chase_began = s.t;
        if (!s.cast_began) continue;
        const abilities::AbilityDef* def = f.catalog->ability(s.ability);
        const std::string label = def ? def->row.name : std::string("?");
        casts.push_back({s.t, s.ability, s.montage, label});
        ++by_name[label];
    }
    std::string summary;
    for (const auto& [label, n] : by_name) summary += (summary.empty() ? "" : ", ") + label + " " + std::to_string(n);
    std::printf("  %zu casts in %.0f s (%s); %llu attacks; the chase began at %.2f s\n", casts.size(), plan.seconds, summary.c_str(), static_cast<unsigned long long>(stats.npc_attacks), chase_began);
    CHECKF(casts.size() >= plan.casts, "it plays its own animations (%zu casts in %.0f s; with the single swing it would play none)", casts.size(), plan.seconds);
    if (more) more(h, by_name);

    check_generic_cast_hits(h, tables, plan, casts, hits);
    check_generic_lunges_and_drift(h, tables, path, samples, travel);
    check_generic_smoothness(h, tables, plan, made.open, samples);
    check_generic_free_lunge(h, tables, plan, made.open, travel);
    check_generic_donor_swing(h, tables, path, plan);
}

void test_goblin_flayer_animates_its_attacks(TestHarness& h) {
    std::printf("a Goblin Flayer (made from a generic Blueprint) against a player who stands still: with its tree's abilities in its open it plays its overhead slashes and its cleave, lunges at him along "
                "their root motion (unreadable before), lands each hit at its notify and stays about him; with the donor's own list of records it keeps the single swing\n");
    check_generic_creature_animates(h, {"/GoblinFlayer/Goblin_Flayer_Gravehide.Default__Goblin_Flayer_Gravehide_C", "Gravehide Flayer", 60.0, false, 3, 100.0, 8, 6}, [](TestHarness& h, const std::map<std::string, size_t>& by_name) {
        const auto count = [&](const char* name) { const auto it = by_name.find(name); return it == by_name.end() ? size_t{0} : it->second; };
        CHECKF(count("GoblinFlayer_OverheadSlash") + count("GoblinFlayer_OverheadSlash2") >= 4, "it plays the overhead slashes, whose root motion could not be read before (%zu + %zu)", count("GoblinFlayer_OverheadSlash"),
               count("GoblinFlayer_OverheadSlash2"));
    });
}

void test_languid_goblin_animates_its_swing(TestHarness& h) {
    std::printf("a Languid Goblin (made from a generic Blueprint, whose only ability was missing from the ability data) that a player hits: it plays its melee swing, lunges at him along the montage's root motion, lands each "
                "hit at its notify and stays about him; with the donor's own list of records it keeps the single swing\n");
    check_generic_creature_animates(h, {"/Goblin/Goblin_LanguidGoblin.Default__Goblin_LanguidGoblin_C", "Languid Goblin", 40.0, true, 2, 150.0, 6, 6}, [](TestHarness& h, const std::map<std::string, size_t>& by_name) {
        size_t all = 0, swings = 0;
        for (const auto& [label, n] : by_name) {
            all += n;
            if (label == "Goblin_Languid_Swing") swings = n;
        }
        CHECKF(swings >= 6 && swings == all, "every cast is its swing (%zu of %zu casts)", swings, all);
    });
}

struct ChaseNumbers {
    double first_move = -1, first_cast = -1, gap_at_cast = -1, speed = 0, end_gap = -1;
    size_t far_ticks = 0;
};

ChaseNumbers chase_numbers(const std::vector<LungeSample>& s, double dt) {
    ChaseNumbers n;
    std::vector<double> speeds;
    for (size_t i = 1; i < s.size(); ++i) {
        const double step = std::hypot(s[i].wx - s[i - 1].wx, s[i].wy - s[i - 1].wy) / dt;
        const double gap = std::hypot(s[i].wx - s[i].px, s[i].wy - s[i].py);
        if (n.first_move < 0 && step > 20.0) n.first_move = s[i].t;
        if (n.first_cast < 0 && s[i].cast_began) {
            n.first_cast = s[i].t;
            n.gap_at_cast = gap;
        }
        if (s[i].montage == 0 && gap > 700.0 && s[i].t > 1.0) speeds.push_back(step);
    }
    std::sort(speeds.begin(), speeds.end());
    n.far_ticks = speeds.size();
    n.speed = speeds.empty() ? 0.0 : speeds[speeds.size() / 2];
    if (!s.empty()) n.end_gap = std::hypot(s.back().wx - s.back().px, s.back().wy - s.back().py);
    return n;
}

struct ChaseSpec {
    const char* name;
    double      start_gap;
    double      speed;
    bool        stat_speed;
};

void check_chase(TestHarness& h, const ChaseNumbers& n, const ChaseSpec& spec) {
    CHECKF(n.first_move >= 0 && n.far_ticks >= 10, "%s sets off after the player and walks (first move at %.2f s, %zu ticks far from him)", spec.name, n.first_move, n.far_ticks);
    CHECKF(std::fabs(n.speed - spec.speed) <= 0.02 * spec.speed, "%s walks at %.0f cm/s: its Blueprint's run speed%s (%.0f)", spec.name, spec.speed, spec.stat_speed ? " times its MoveSpeedMult" : "", n.speed);
    const double walk = n.first_cast >= 0 ? (spec.start_gap - n.gap_at_cast) / spec.speed : 1e9;
    CHECKF(n.first_cast >= 0 && n.first_cast - n.first_move <= walk + 2.0,
           "%s attacks within %.1f s of setting off: the walk to where it cast (%.0f cm away) takes %.1f s, it did not stop to wait (first cast %.2f s after it set off)", spec.name, walk + 2.0, n.gap_at_cast, walk,
           n.first_cast - n.first_move);
}

bool check_humanoid_far_walk_kind(TestHarness& h, const combat::NpcAbilityData& tables, combat::NpcStatBook& book, const char* leaf, bool humanoid) {
    Fixture& f = h.f;
    constexpr double kDt = 1.0 / 15.0, kStartGap = 2500.0;
    const std::string path = creature_path(f, leaf);
    const auto bp = f.motion.find(path);
    const std::string name = spawns::blueprint_leaf(path);
    CHECKF(!path.empty() && bp && tables.abilities_of(path) != nullptr, "the Blueprint table has the %s and the creature tables list its abilities", name.c_str());
    if (path.empty() || !bp || !tables.abilities_of(path)) return false;
    BanditOpen made;
    if (humanoid) {
        CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the %s", name.c_str());
        if (!made.open) return false;
    }
    std::string err;
    auto cs = book.make(*bp, 7, err);
    const stats::StatValue* mult = cs ? cs->find(f.data->ids().move_speed_mult) : nullptr;
    CHECKF(mult && mult->value >= 1.0, "the %s has a MoveSpeedMult of its own at level 7 (%.2f)", name.c_str(), mult ? mult->value : 0.0);
    if (!mult) return false;
    const double speed = humanoid ? bp->max_run_speed * mult->value : bp->max_run_speed;
    std::vector<LungeSample> samples;
    LungeFightSpec spec;
    spec.path = path;
    spec.seconds = 25.0;
    spec.player_y = kStartGap;
    spec.provoke_at = 0.5;
    spec.leash_cm = 60000.0;
    spec.made = made.open;
    CHECKF(run_lunge_fight(f, tables, spec, samples), "a fight of 25 s against a player 25 m away runs for the %s", name.c_str());
    if (samples.empty()) return false;
    const ChaseNumbers n = chase_numbers(samples, kDt);
    std::printf("  %-34s set off %.2f s, speed %.0f cm/s (Blueprint run %.0f x MoveSpeedMult %.2f), first cast %.2f s from %.0f cm\n",
                name.c_str(), n.first_move, n.speed, bp->max_run_speed, mult->value, n.first_cast, n.gap_at_cast);
    check_chase(h, n, {name.c_str(), kStartGap, speed, humanoid});
    return true;
}

void test_humanoids_walk_up_to_a_far_player(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a goblin, a bandit raider and a bandit captain against a player 25 m away: they run at their Blueprint's run speed times their MoveSpeedMult and walk up to the nearest reach of the attacks they can use "
                "(a special that waits for a timer does not park them), a wolf and a bear keep their Blueprint's speed\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    struct Kind {
        const char* leaf;
        bool        humanoid;
    };
    const Kind kinds[] = {
        {"/Bandit_Raider/CSB_Bandit_Raider_StateTree.Default__CSB_Bandit_Raider_StateTree_C", true},
        {"/Bandit_Raider/Bandit_Raider.Default__Bandit_Raider_C", true},
        {"/BanditCaptain/Bandit_Captain.Default__Bandit_Captain_C", true},
        {"/Goblin/Goblin_Gravehide.Default__Goblin_Gravehide_C", true},
        {"/Canine/Wolf.Default__Wolf_C", false},
        {"/Bear/Bear.Default__Bear_C", false},
    };
    for (const Kind& kind : kinds) {
        if (!check_humanoid_far_walk_kind(h, tables, book, kind.leaf, kind.humanoid)) return;
    }
}

struct RealGroundRunSpec {
    std::string             path;
    const npc::MotionStats* bp = nullptr;
    const spawns::OpenTemplate* open = nullptr;
    nav::Vec3               home{};
    nav::Vec3               target{};
};

bool run_real_ground_direction(Fixture& f, const combat::NpcAbilityData& tables, nav::AreaNav& nav, const RealGroundRunSpec& spec, std::vector<LungeSample>& samples) {
    constexpr double kDt = 1.0 / 15.0;
    Rig rig(4, 20, {});
    if (!rig.ok()) return false;
    combat_rig::World w;
    if (!w.problem.empty()) return false;
    w.npcs = std::make_unique<npc::NpcWorld>(w.registry, &nav);
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &nav, *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const double spawn_z = spec.home.z + spec.bp->half_height * spec.bp->default_scale;
    const uint64_t creature = combat_rig::spawn_npc(w.registry, w.npcs.get(), *w.captured, w.next_id, spec.path, spec.home.x, spec.home.y, spawn_z, 7, 900, spec.bp->half_height, spec.open);
    if (creature == 0) return false;
    w.registry.find(creature)->npc.leash_radius_cm = 60000.0;
    combat_rig::FakePlayer player(rig, 77, {spec.target.x, spec.target.y, spec.target.z + 98.0});
    world.add_player(&player);
    GremCastView last;
    double now = 0, next_hit = 0.5;
    while (now < 25.0) {
        now += kDt;
        if (now >= next_hit) {
            next_hit = now + 1.0;
            world.add_threat(creature, 77, 100.0, now);
        }
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);
        nav.pump(50.0);
        w.npcs->note_player(77, spec.target.x, spec.target.y, now);
        w.npcs->tick(now);
        world.tick(now);
        const world::WorldActor* g = w.registry.find(creature);
        const GremCastView view = grem_cast_view(w.registry, creature);
        LungeSample s;
        s.t = now;
        s.wx = g->x;
        s.wy = g->y;
        s.px = spec.target.x;
        s.py = spec.target.y;
        s.montage = view.montage;
        s.cast_began = view.montage != 0 && (view.element != last.element || last.montage == 0);
        last = view;
        samples.push_back(s);
    }
    return true;
}

struct RealGroundCamp {
    const char* leaf;
    double      x;
    double      y;
};

bool run_camp_directions(TestHarness& h, const combat::NpcAbilityData& tables, nav::AreaNav& nav, const RealGroundRunSpec& base_spec, double speed) {
    constexpr double kDt = 1.0 / 15.0, kPi = 3.14159265358979323846;
    const std::string name = spawns::blueprint_leaf(base_spec.path);
    size_t runs = 0;
    for (int d = 0; d < 8 && runs < 3; ++d) {
        const double a = d * kPi / 4.0;
        const nav::Vec3 want{base_spec.home.x + 2000.0 * std::cos(a), base_spec.home.y + 2000.0 * std::sin(a), base_spec.home.z};
        nav::Vec3 target;
        if (!nav.nearest_point(want, 400, 600, &target) || std::hypot(target.x - want.x, target.y - want.y) > 250.0) continue;
        const nav::PathResult route = nav.find_path(base_spec.home, target, 200, 400, 20000);
        if (!route.found || !route.complete || route.length > 2300.0) continue;
        RealGroundRunSpec spec = base_spec;
        spec.target = target;
        std::vector<LungeSample> samples;
        if (!run_real_ground_direction(h.f, tables, nav, spec, samples)) return false;
        ++runs;
        const ChaseNumbers n = chase_numbers(samples, kDt);
        const double start_gap = std::hypot(samples.front().wx - target.x, samples.front().wy - target.y);
        std::printf("  %-26s direction %d: path %.0f cm, set off %.2f s, speed %.0f cm/s, first cast %.2f s from %.0f cm\n",
                    name.c_str(), d, route.length, n.first_move, n.speed, n.first_cast, n.gap_at_cast);
        const std::string label = name + " on the real ground";
        check_chase(h, n, {label.c_str(), start_gap, speed, true});
    }
    CHECKF(runs >= 1, "the %s has a real spot 20 m from its camp that it can walk to (%zu found)", name.c_str(), runs);
    return true;
}

void test_humanoids_walk_up_on_the_real_ground(TestHarness& h) {
    Fixture& f = h.f;
    const char* env = std::getenv("AOC_REAL_GROUND");
    if (!env || std::string(env) != "1") {
        std::printf("humanoid creatures walking up to a player on the real ground: opt-in (AOC_REAL_GROUND=1 reads dist/content/area_nav.bin)\n");
        return;
    }
    std::printf("goblins, a bandit raider and a bandit captain on the real ground: each reaches a player standing 20 m away along a real path, at its speed, without standing and staring\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    std::string err;
    auto nav = nav::AreaNav::load(dir + "area_nav.bin", &err);
    if (!nav) {
        std::printf("  [skip] the real ground: %s\n", err.c_str());
        return;
    }
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    const RealGroundCamp camps[] = {
        {"/Goblin/Goblin_Gravehide.Default__Goblin_Gravehide_C", -796495, 569503},
        {"/Bandit_Raider/Bandit_Raider.Default__Bandit_Raider_C", -837337, 640605},
        {"/BanditCaptain/Bandit_Captain.Default__Bandit_Captain_C", -837337, 640605},
    };
    for (const RealGroundCamp& camp : camps) {
        const std::string path = creature_path(f, camp.leaf);
        const auto bp = f.motion.find(path);
        const std::string name = spawns::blueprint_leaf(path);
        if (path.empty() || !bp || !tables.abilities_of(path)) return;
        if (!nav->load_around(camp.x, camp.y, 2, 180)) {
            std::printf("  [skip] the ground round the %s's camp did not load\n", name.c_str());
            continue;
        }
        nav::Vec3 home;
        if (!nav->nearest_point({camp.x, camp.y, 12000.0}, 3000, 100000, &home)) continue;
        BanditOpen made;
        if (!make_bandit_open(f, tables, path, made)) return;
        std::string e2;
        auto cs = book.make(*bp, 7, e2);
        const stats::StatValue* mult = cs ? cs->find(f.data->ids().move_speed_mult) : nullptr;
        if (!mult) return;
        if (!run_camp_directions(h, tables, *nav, {path, &*bp, made.open, home, {}}, bp->max_run_speed * mult->value)) return;
    }
}

std::string unique_temp_suffix() {
    int marker = 0;
    char text[64];
    std::snprintf(text, sizeof text, "%llx_%llx", static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&marker)));
    return text;
}

struct TableErrorEnv {
    enum class Table { Abilities, Montages, Targeting };
    std::filesystem::path dir;
    std::string           abilities_header;
    std::string           montages_header;
    std::string           targeting_header;
    std::string           no_abilities;
    std::string           no_montages;
    std::string           no_targeting;

    TableErrorEnv()
        : dir(std::filesystem::temp_directory_path() / ("aoc_creature_tables_columns_" + unique_temp_suffix())),
          abilities_header("archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival"),
          montages_header("montage\tguid\tpackage\tpackage_guid\tlength\trate_scale\tsections\thits\troot_motion\troot_unreadable\tanim_end\tfires"),
          targeting_header("ability\tkey\tdistance\tmin_distance\tangle\tangle_center\tcone_check\tfreeze\tfreeze_distance\tfreeze_scale\tignore_non_target\tdiscovery\tcheck_validity\tlos_check\tdistance_center\tvolumes\tshapes") {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        no_abilities = write("abilities.tsv", abilities_header, "");
        no_montages = write("montages.tsv", montages_header, "");
        no_targeting = write("targeting.tsv", targeting_header, "");
    }
    ~TableErrorEnv() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string write(const char* name, const std::string& header, const std::string& row) const {
        std::ofstream out(dir / name, std::ios::binary);
        out << header << "\n";
        if (!row.empty()) out << row << "\n";
        return (dir / name).string();
    }
    std::string error_of(Table which, const std::string& row) const {
        const std::string a = which == Table::Abilities ? write("bad_abilities.tsv", abilities_header, row) : no_abilities;
        const std::string m = which == Table::Montages ? write("bad_montages.tsv", montages_header, row) : no_montages;
        const std::string t = which == Table::Targeting ? write("bad_targeting.tsv", targeting_header, row) : no_targeting;
        combat::NpcAbilityData tables;
        std::string err;
        return tables.load(a, m, t, err) ? std::string() : err;
    }
    static std::string ability_row(const char* cooldown, const char* stop, const char* max_distance, const char* fight) {
        return std::string("Wolf\t7\tBite\t") + cooldown + "\t" + stop + "\t0\t0\t" + max_distance + "\t180\t\t0\t" + fight + "\t\t\t\t\t0";
    }
};

void test_creature_tables_are_read_strictly(TestHarness& h) {
    std::printf("the creature tables: a good row loads; a NaN or an infinite number and a flag that is neither 0 nor 1 fail the load\n");
    TableErrorEnv env;
    const auto loads = [&](const std::string& row) { return env.error_of(TableErrorEnv::Table::Abilities, row).empty(); };
    CHECK(loads(TableErrorEnv::ability_row("2.5", "1", "300", "1")), "a good row loads");
    CHECK(!loads(TableErrorEnv::ability_row("nan", "1", "300", "1")), "a NaN cooldown fails the load");
    CHECK(!loads(TableErrorEnv::ability_row("2.5", "1", "inf", "1")), "an infinite reach fails the load");
    CHECK(!loads(TableErrorEnv::ability_row("2.5", "2", "300", "1")) && !loads(TableErrorEnv::ability_row("2.5", "1", "300", "yes")),
          "a flag that is neither 0 nor 1 fails the load");
}

void test_creature_table_errors_name_the_column(TestHarness& h) {
    std::printf("a creature table that does not read: the error names the file, the line, the column and what the column holds\n");
    TableErrorEnv env;
    using Table = TableErrorEnv::Table;
    const auto has = [](const std::string& text, const char* part) { return text.find(part) != std::string::npos; };
    const std::string cooldown = env.error_of(Table::Abilities, TableErrorEnv::ability_row("nan", "1", "300", "1"));
    CHECK(has(cooldown, "abilities.tsv") && has(cooldown, "a line of Wolf") && has(cooldown, "the column cooldown holds \"nan\""),
          "a NaN cooldown: the error names the table, the line (Wolf) and the column (cooldown) with what it holds (nan)");
    const std::string reach = env.error_of(Table::Abilities, TableErrorEnv::ability_row("2.5", "1", "inf", "1"));
    CHECK(has(reach, "the column max_distance holds \"inf\""), "an infinite reach: the column is max_distance and it holds inf");
    const std::string flag = env.error_of(Table::Abilities, TableErrorEnv::ability_row("2.5", "1", "300", "yes"));
    CHECK(has(flag, "the column fight holds \"yes\""), "a flag that is neither 0 nor 1: the column is fight and it holds yes");
    const std::string stop = env.error_of(Table::Abilities, TableErrorEnv::ability_row("2.5", "2", "300", "1"));
    CHECK(has(stop, "the column stop_movement holds \"2\""), "another flag: the column is stop_movement and it holds 2");
    const std::string length = env.error_of(Table::Montages, "/Game/M\t5\tP\t6\tnan\t1\t\t\t\t0\t\t");
    CHECK(has(length, "montages.tsv") && has(length, "a line of /Game/M") && has(length, "the column length holds \"nan\""),
          "a montage whose length is NaN: the column is length");
    const std::string hits = env.error_of(Table::Montages, "/Game/M\t5\tP\t6\t2\t1\t\t0.5-x\t\t0\t\t");
    CHECK(has(hits, "the column hits holds \"0.5-x\""), "a montage with a hit that is no time:key pair: the column is hits and it holds the text");
    const std::string angle = env.error_of(Table::Targeting, "7\t1\t100\t0\tnan\t0\t0\tnone\t0\t1\t0\tnone\t0\t0\t0\t0\t");
    CHECK(has(angle, "targeting.tsv") && has(angle, "a line of 7") && has(angle, "the column angle holds \"nan\""),
          "a targeting record whose angle is NaN: the column is angle");
    const std::string freeze = env.error_of(Table::Targeting, "7\t1\t100\t0\t90\t0\t0\tsometimes\t0\t1\t0\tnone\t0\t0\t0\t0\t");
    CHECK(has(freeze, "the column freeze holds \"sometimes\""), "a freeze that is none, pause or stop and neither: the column is freeze");
    CHECK(env.error_of(Table::Abilities, TableErrorEnv::ability_row("2.5", "1", "300", "1")).empty(), "(a good line loads)");
}

namespace reg_check {
template <class T>
const void* type_tag() {
    static char tag = 0;
    return &tag;
}
struct Entry {
    const char*                name;
    const char*                file;
    int                        line;
    const void*                context;
    std::function<void(void*)> call;
};
const char* base_name(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}
bool is_duplicate_name(const std::vector<Entry>& list, std::size_t idx) {
    for (std::size_t j = 0; j < idx; ++j) {
        if (std::strcmp(list[j].name, list[idx].name) == 0) return true;
    }
    return false;
}
template <class Ctx>
int run_list(const std::vector<Entry>& list, Ctx& ctx, std::FILE* out) {
    const void* const want = type_tag<std::remove_const_t<Ctx>>();
    int problems = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Entry& e = list[i];
        if (is_duplicate_name(list, i)) {
            std::fprintf(out, "  [FAIL] test registry: the name %s is registered twice (%s:%d); the second one did not run\n", e.name, base_name(e.file), e.line);
            ++problems;
            continue;
        }
        if (e.context && e.context != want) {
            std::fprintf(out, "  [FAIL] test registry: %s (%s:%d) takes another fixture type than the one this program runs with; it did not run\n",
                         e.name, base_name(e.file), e.line);
            ++problems;
            continue;
        }
        try {
            e.call(e.context ? static_cast<void*>(&ctx) : nullptr);
        } catch (const std::exception& ex) {
            std::fprintf(out, "  [FAIL] %s threw: %s\n", e.name, ex.what());
            ++problems;
        }
    }
    return problems;
}
}  // namespace reg_check

void test_registry_failure_paths(TestHarness& h) {
    std::printf("the test registry reports its own failure paths\n");
    struct OtherFixture {};
    int ran = 0;
    std::vector<reg_check::Entry> list;
    list.push_back({"fine", __FILE__, __LINE__, nullptr, [&](void*) { ++ran; }});
    list.push_back({"fine", __FILE__, __LINE__, nullptr, [&](void*) { ++ran; }});
    list.push_back({"for_another_fixture", __FILE__, __LINE__, reg_check::type_tag<OtherFixture>(), [&](void*) { ++ran; }});
    list.push_back({"throws", __FILE__, __LINE__, nullptr, [](void*) { throw std::runtime_error("boom"); }});
    list.push_back({"after_the_throw", __FILE__, __LINE__, nullptr, [&](void*) { ++ran; }});
    std::FILE* out = std::tmpfile();
    CHECK(out != nullptr, "(a temporary file for the runner's messages)");
    if (!out) return;
    const int problems = reg_check::run_list(list, h.f, out);
    std::rewind(out);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof buf, out)) text += buf;
    std::fclose(out);
    CHECKF(problems == 3, "three problems are counted: a name twice, a test made for another fixture, a test that threw (%d)", problems);
    CHECK(text.find("the name fine is registered twice") != std::string::npos && text.find("for_another_fixture") != std::string::npos &&
              text.find("takes another fixture type") != std::string::npos && text.find("throws threw: boom") != std::string::npos,
          "...and each one is told with the test's name");
    CHECKF(ran == 2, "the tests that were fine ran once each, the one after the throw too (%d)", ran);
}

}  // namespace

void run_combat_generic_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db) {
    (void)db;
    test_goblin_flayer_animates_its_attacks(h);
    test_languid_goblin_animates_its_swing(h);
    test_humanoids_walk_up_to_a_far_player(h);
    test_humanoids_walk_up_on_the_real_ground(h);
    test_creature_tables_are_read_strictly(h);
    test_creature_table_errors_name_the_column(h);
    test_registry_failure_paths(h);
}

}  // namespace sandal::worldd::tests
