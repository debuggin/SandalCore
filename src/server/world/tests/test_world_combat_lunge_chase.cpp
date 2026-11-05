#include "combat_test_fixture.h"

namespace sandal::worldd::tests {
namespace {

void check_lunge_pause_standing(TestHarness& h, const combat::NpcAbilityData& tables,
                                int64_t bite_guid, double r_wolf, double freeze_dist) {
    Fixture& f = h.f;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    std::vector<LungeSample> stand;
    LungeFightSpec spec;
    spec.path = wolf_path;
    spec.seconds = 30.0;
    spec.player_y = 800.0;
    CHECK(run_lunge_fight(f, tables, spec, stand), "a fight against a player who stands still runs");
    const std::vector<LungeRun> runs = lunge_runs(stand, bite_guid);
    size_t held_runs = 0, creeping = 0, uncleared = 0, silent = 0;
    double closest = 1e9, farthest = 0, slowest_creep = 0;
    for (const LungeRun& r : runs) {
        bool held = false;
        for (size_t i = r.first; i <= r.last; ++i) {
            if (stand[i].obstructed != 1) continue;
            held = true;
            const double gap = std::hypot(stand[i].wx - stand[i].px, stand[i].wy - stand[i].py) - r_wolf - 34.0;
            closest = std::min(closest, gap);
            farthest = std::max(farthest, gap);
            if (i > r.first && stand[i - 1].obstructed == 1) {
                const double d = std::hypot(stand[i].wx - stand[i - 1].wx, stand[i].wy - stand[i - 1].wy);
                slowest_creep = std::max(slowest_creep, d);
                if (d > 0.5) ++creeping;
            }
        }
        if (held) ++held_runs;
        if (stand[r.last + 1].obstructed != 0) ++uncleared;
        if (!held) ++silent;
    }
    std::printf("  %zu lunges of the Bite against a player who stands still: %zu held (flag 1) ahead of him, %.1f to %.1f cm from his surface, the pause distance %.0f cm\n",
                runs.size(), held_runs, closest, farthest, freeze_dist);
    CHECKF(runs.size() >= 5 && silent == 0, "every lunge of the Bite is held by the player in its box, with the flag going out (%zu of %zu lunges never carried it)", silent, runs.size());
    CHECKF(closest >= freeze_dist - 2.0 && farthest <= freeze_dist + 0.5,
           "the Wolf is held at the pause distance, at the box's edge, and does not come nearer: %.1f to %.1f cm of surface for a pause distance of %.0f cm",
           closest, farthest, freeze_dist);
    CHECKF(creeping == 0, "while the flag is on the Wolf does not move (%zu ticks moved, the most %.2f cm)", creeping, slowest_creep);
    CHECKF(uncleared == 0, "the flag is off again when the carry ends (%zu lunges end with it on)", uncleared);
}

void check_lunge_pause_step_out(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& wolf_path, int64_t bite_guid) {
    Fixture& f = h.f;
    double leave_at = -1, side = 700.0;
    std::vector<LungeSample> leave;
    LungeFightSpec spec;
    spec.path = wolf_path;
    spec.seconds = 40.0;
    spec.player_y = 800.0;
    spec.steer = [&](double now, const LungeSample* prev, double& px, double& py, const world::WorldActor&) {
        (void)py;
        if (leave_at < 0 && prev && prev->active == 1 && prev->obstructed == 1 && prev->ability == bite_guid) leave_at = now;
        if (leave_at < 0 || now < leave_at) return false;
        leave_at = -1;
        px += side;
        side = -side;
        return true;
    };
    CHECK(run_lunge_fight(f, tables, spec, leave), "a fight against a player who steps out of the box once the Wolf is held runs");
    size_t cleared = 0, slow_clear = 0, moved_on = 0, wrong_travel = 0;
    double worst_travel = 0;
    for (const LungeRun& r : lunge_runs(leave, bite_guid)) {
        size_t held = r.last + 1, gone = r.last + 1, clear = r.last + 1;
        for (size_t i = r.first; i <= r.last; ++i) {
            if (held > r.last && leave[i].obstructed == 1) held = i;
            if (held <= r.last && gone > r.last && leave[i].px != leave[held].px) gone = i;
            if (held <= r.last && clear > r.last && i > held && leave[i].obstructed == 0) clear = i;
        }
        if (held > r.last || clear > r.last) continue;
        ++cleared;
        if (gone > r.last || clear > gone + 2) ++slow_clear;
        const combat::MontageData* md = montage_of_cast(f, tables, leave[r.first].ability, leave[r.first].montage);
        if (!md || leave[clear - 1].position < 0) continue;
        const combat::MontageData::Step from = md->root_at(leave[clear - 1].position), to = md->root_at(md->length - 0.25);
        const double expected = std::hypot(to.x - from.x, to.y - from.y);
        const double observed = std::hypot(leave[r.last + 1].wx - leave[clear - 1].wx, leave[r.last + 1].wy - leave[clear - 1].wy);
        worst_travel = std::max(worst_travel, std::fabs(observed - expected));
        if (observed > 5.0) ++moved_on;
        if (std::fabs(observed - expected) > 12.0) ++wrong_travel;
    }
    std::printf("  %zu lunges had the player step out while the Wolf was held: the flag cleared within two ticks in %zu, the Wolf went on in %zu, its travel off the montage's by %.1f cm at the most\n",
                cleared, cleared - slow_clear, moved_on, worst_travel);
    CHECKF(cleared >= 3 && slow_clear == 0, "once the player is out of the box the flag clears within two ticks (%zu of %zu cleared late)", slow_clear, cleared);
    CHECKF(moved_on >= 1 && wrong_travel == 0,
           "the lunge then goes on by what the montage still travels (%zu went on; %zu travelled other than the montage's remainder, the worst %.1f cm)",
           moved_on, wrong_travel, worst_travel);
}

void test_lunge_pauses_at_the_player(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a Wolf's lunge pauses on contact: held ahead of the player while he is in its box, with bRootMotionObstructed going out; when the box is clear the flag clears and the lunge goes on by what the montage still travels\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const auto bp = f.motion.find(wolf_path);
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(!wolf_path.empty() && bp && tables.abilities_of(wolf_path) && bite,
          "the Blueprint table has the Wolf, the creature tables list its abilities and the ability data has the Bite");
    if (wolf_path.empty() || !bp || !tables.abilities_of(wolf_path) || !bite) return;
    const combat::RootMotionFreezeRule* freeze = tables.freeze_of(bite->row.guid);
    CHECK(freeze && freeze->kind == combat::RootMotionFreezeRule::Kind::Pause && freeze->distance > 0, "the Bite's ability record pauses its lunge on contact");
    if (!freeze) return;
    check_lunge_pause_standing(h, tables, bite->row.guid, bp->radius, freeze->distance);
    check_lunge_pause_step_out(h, tables, wolf_path, bite->row.guid);
}

void test_swing_is_not_cut_by_walk_orders(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a Bear's swings carry it to the end of their travel even when the player side-steps and the AI wants to walk: the walk order waits for the carry to end\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string bear_path = creature_path(f, "/Beast/Bear/Bear.Default__Bear_C");
    CHECK(!bear_path.empty() && f.motion.find(bear_path) && tables.abilities_of(bear_path),
          "the Blueprint table has the Bear and the creature tables list its abilities");
    if (bear_path.empty() || !f.motion.find(bear_path) || !tables.abilities_of(bear_path)) return;
    double step_at = -1, side = 300.0;
    size_t steps = 0;
    std::vector<LungeSample> samples;
    LungeFightSpec spec;
    spec.path = bear_path;
    spec.seconds = 45.0;
    spec.player_y = 800.0;
    spec.steer = [&](double now, const LungeSample* prev, double& px, double& py, const world::WorldActor&) {
        (void)py;
        if (prev && prev->cast_began) step_at = now + 0.1;
        if (step_at < 0 || now < step_at) return false;
        step_at = -1;
        px += side;
        side = -side;
        ++steps;
        return true;
    };
    CHECK(run_lunge_fight(f, tables, spec, samples), "a fight against a player who side-steps at every swing runs");
    size_t lunges = 0, cut = 0;
    double worst = 0;
    for (const LungeRun& r : lunge_runs(samples)) {
        const combat::MontageData* md = montage_of_cast(f, tables, samples[r.first].ability, samples[r.first].montage);
        if (!md) continue;
        ++lunges;
        const double expected = (md->length - 0.25) / md->rate_scale;
        const double played = samples[r.last + 1].t - samples[r.first].t;
        worst = std::max(worst, expected - played);
        if (played < expected - 0.2) ++cut;
    }
    std::printf("  %zu side-steps, %zu lunges in 45 s: %zu carried less than the montage's travel (the worst short by %.2f s)\n", steps, lunges, cut, worst);
    CHECKF(lunges >= 4 && cut == 0, "every lunge is carried to the end of the montage's travel, whatever the AI orders (%zu of %zu cut short, the worst by %.2f s)", cut, lunges, worst);
}

void test_timid_raven_swings_back(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a Timid Raven (a Blueprint no capture opened, so the generic open that lists none of its tree's abilities): it starts no fight, and once it is hit it swings back with the single swing every creature has\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string raven_path = creature_path(f, "/Raven/Raven_Timid.Default__Raven_Timid_C");
    const abilities::AbilityDef* peck = f.catalog->ability_named("Raven_Peck");
    CHECK(!raven_path.empty() && f.motion.find(raven_path) && tables.abilities_of(raven_path) && peck,
          "the Blueprint table has the Timid Raven, the creature tables list its abilities and the ability data has the Peck");
    if (raven_path.empty() || !f.motion.find(raven_path) || !tables.abilities_of(raven_path) || !peck) return;
    bool lists_peck = false;
    for (const combat::CreatureAbility& t : *tables.abilities_of(raven_path)) lists_peck = lists_peck || t.ability == peck->row.guid;
    CHECK(lists_peck, "the Timid Raven's tree uses the Peck");
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    combat::CombatWorld::Stats stats;
    LungeFightSpec spec;
    spec.path = raven_path;
    spec.seconds = 20.0;
    spec.player_y = 400.0;
    spec.hits = &hits;
    spec.timid = true;
    spec.provoke_at = 8.0;
    spec.stats = &stats;
    CHECK(run_lunge_fight(f, tables, spec, samples), "a fight against a Timid Raven that is hit at 8 s runs");
    size_t before = 0, after = 0;
    for (const combat::IncomingHit& ih : hits) {
        if (ih.at < 8.0) ++before;
        else ++after;
    }
    std::printf("  hits on the player: %zu before the Raven was hit, %zu after (%llu attacks in all)\n",
                before, after, static_cast<unsigned long long>(stats.npc_attacks));
    CHECKF(before == 0, "a Timid Raven starts no fight (%zu hits before it was hit)", before);
    CHECKF(after >= 2 && stats.npc_attacks >= 2, "once it is hit it swings back at the player (%zu hits, %llu attacks)", after, static_cast<unsigned long long>(stats.npc_attacks));
}

struct ChaseReport {
    size_t halts = 0, attacks = 0, hits = 0, ticks = 0;
    double first_attack_at = -1, closest_gap = 1e9, worst_turn = 0, run_speed = 0, turn_rate = 0;
    size_t any_due = 0, any_landed = 0, any_missed = 0, any_blocked = 0, any_crit = 0;
    size_t cone_due = 0, cone_landed = 0;
};

struct ChaseParams {
    std::string path;
    double      run_cm_s = 720.0;
    double      zig_degrees = 0.0;
    double      seconds = 12.0;
};

void accumulate_chase_cast(Fixture& f, const combat::NpcAbilityData& tables, const LungeSample& s,
                           const std::vector<combat::IncomingHit>& hits, ChaseReport& out) {
    const double seconds = 12.0;
    const combat::BiteArea* area = tables.area_of(s.ability);
    const bool measures = area && area->gate() == combat::BiteArea::Gate::Validity;
    for (const double at : cast_bites(f, tables, s.ability, s.montage)) {
        if (s.t + at <= seconds - 0.2) (measures ? out.cone_due : out.any_due) += 1;
    }
    const combat::MontageData* md = montage_of_cast(f, tables, s.ability, s.montage);
    const double span = md ? md->seconds(1.0) : 0.0;
    for (const combat::IncomingHit& ih : hits) {
        if (ih.source_guid != s.ability || ih.at < s.t - 0.05 || ih.at > s.t + span + 0.25) continue;
        if (measures) {
            ++out.cone_landed;
            continue;
        }
        ++out.any_landed;
        if (ih.flags & stats::kHitMiss) ++out.any_missed;
        if (ih.flags & stats::kHitBlock) ++out.any_blocked;
        if (ih.flags & stats::kHitCrit) ++out.any_crit;
    }
}

bool run_chase(Fixture& f, const combat::NpcAbilityData& tables, const ChaseParams& p, ChaseReport& out) {
    constexpr double kPi = 3.14159265358979323846;
    const auto bp = f.motion.find(p.path);
    if (!bp) return false;
    const double dt = 1.0 / 15.0;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    LungeFightSpec spec;
    spec.path = p.path;
    spec.seconds = p.seconds;
    spec.player_y = 500.0;
    spec.hits = &hits;
    spec.leash_cm = 40000.0;
    spec.steer = [&](double now, const LungeSample*, double& x, double& y, const world::WorldActor&) {
        const double heading = 90.0 + (p.zig_degrees > 0 ? ((static_cast<int>(now) % 2) ? p.zig_degrees : -p.zig_degrees) : 0.0);
        x += p.run_cm_s * dt * std::cos(heading * kPi / 180.0);
        y += p.run_cm_s * dt * std::sin(heading * kPi / 180.0);
        return true;
    };
    if (!run_lunge_fight(f, tables, spec, samples)) return false;
    out = ChaseReport{};
    out.run_speed = bp->max_run_speed;
    out.turn_rate = bp->rotation_rate_yaw;
    out.hits = hits.size();
    double last_cast_t = -10;
    for (size_t i = 1; i < samples.size(); ++i) {
        const LungeSample& s = samples[i];
        const double speed = std::hypot(s.wx - samples[i - 1].wx, s.wy - samples[i - 1].wy) / dt;
        const double gap = std::hypot(s.wx - s.px, s.wy - s.py) - bp->radius - 34.0;
        out.closest_gap = std::min(out.closest_gap, gap);
        out.worst_turn = std::max(out.worst_turn, std::fabs(wrap180(s.yaw - samples[i - 1].yaw)));
        if (s.cast_began) {
            ++out.attacks;
            if (out.first_attack_at < 0) out.first_attack_at = s.t;
            accumulate_chase_cast(f, tables, s, hits, out);
        }
        if (s.montage != 0) last_cast_t = s.t;
        if (s.montage == 0 && s.t - last_cast_t > 0.5 && gap > 450.0 && s.t > 1.0) {
            ++out.ticks;
            if (speed < 0.05 * out.run_speed) ++out.halts;
        }
    }
    return true;
}

void log_and_check_chase_kind(TestHarness& h, const char* name, const ChaseReport& straight, const ChaseReport& zig, const ChaseReport& walk) {
    std::printf("  %-4s (runs %.0f cm/s, turns %.0f deg/s) after a player at 720 cm/s, 12 s:\n", name, straight.run_speed, straight.turn_rate);
    std::printf("       straight:   %zu attacks (the first at %.1f s), %zu hits, closest %.0f cm from his surface, stopped %zu of %zu chasing ticks, its facing turned %.1f deg at the most in a tick\n",
                straight.attacks, straight.first_attack_at, straight.hits, straight.closest_gap, straight.halts, straight.ticks, straight.worst_turn);
    std::printf("       zig-zag:    %zu attacks (the first at %.1f s), %zu hits, closest %.0f cm from his surface, stopped %zu of %zu chasing ticks, its facing turned %.1f deg at the most in a tick\n",
                zig.attacks, zig.first_attack_at, zig.hits, zig.closest_gap, zig.halts, zig.ticks, zig.worst_turn);
    std::printf("       half speed: %zu attacks (the first at %.1f s), %zu hits, closest %.0f cm from his surface, stopped %zu of %zu chasing ticks\n",
                walk.attacks, walk.first_attack_at, walk.hits, walk.closest_gap, walk.halts, walk.ticks);
    const auto bites_line = [](const char* label, const ChaseReport& r) {
        std::printf("       %-10s bites due %zu on the target wherever he stands: %zu landed (%zu of them rolled a miss, %zu a block, %zu a critical hit); inside a cone: %zu due, %zu landed\n",
                    label, r.any_due, r.any_landed, r.any_missed, r.any_blocked, r.any_crit, r.cone_due, r.cone_landed);
    };
    bites_line("straight", straight);
    bites_line("zig-zag", zig);
    bites_line("half speed", walk);
    CHECKF(straight.halts == 0 && zig.halts == 0 && walk.halts == 0,
           "the %s does not stop while it chases (%zu, %zu and %zu chasing ticks at a stand)", name, straight.halts, zig.halts, walk.halts);
    CHECKF(straight.any_landed >= straight.any_due && zig.any_landed >= zig.any_due && walk.any_landed >= walk.any_due,
           "every bite of the %s's attacks that measures nothing reaches the runner at its notify, at 720 and at 360 cm/s (%zu of %zu, %zu of %zu and %zu of %zu)",
           name, straight.any_landed, straight.any_due, zig.any_landed, zig.any_due, walk.any_landed, walk.any_due);
    CHECKF(name[0] != 'W' || (straight.attacks >= 1 && straight.any_due >= 1 && zig.any_due >= 1),
           "the Wolf's attacks at the runner have bites that fall due (%zu bites in the straight chase, %zu in the zig-zag)", straight.any_due, zig.any_due);
    const double step = name[0] == 'W' ? 720.0 / 15.0 : 360.0 / 15.0;
    CHECKF(straight.worst_turn <= step + 1.0 && zig.worst_turn <= step + 1.0,
           "its facing turns at its rotation rate and never jumps (%.1f and %.1f deg in a tick, %.1f allowed)", straight.worst_turn, zig.worst_turn, step + 1.0);
    if (straight.run_speed > 720.0) {
        const size_t wanted = straight.run_speed > 1000.0 ? 2 : 1;
        CHECKF(straight.attacks >= wanted && zig.attacks >= wanted && straight.first_attack_at >= 0 && straight.first_attack_at < 4.0,
               "the %s (faster than the player) catches up within 4 s and attacks (%zu attacks in the straight chase, the first at %.1f s; %zu in the zig-zag)",
               name, straight.attacks, straight.first_attack_at, zig.attacks);
    }
}

void test_chase_of_a_player_who_walks_away(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a creature chases a player who runs away at 720 cm/s, straight and zig-zagging: it never stops while it chases, and it catches up and attacks\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    struct Kind {
        const char* name;
        const char* leaf;
    };
    const Kind kinds[] = {{"Wolf", "/Beast/Canine/Wolf.Default__Wolf_C"}, {"Bear", "/Beast/Bear/Bear.Default__Bear_C"}, {"Grem", "/Beast/Rabid/Rabid.Default__Rabid_C"}};
    for (const Kind& k : kinds) {
        const std::string path = creature_path(f, k.leaf);
        CHECKF(!path.empty() && f.motion.find(path) && tables.abilities_of(path), "the Blueprint table has the %s and the creature tables list its abilities", k.name);
        if (path.empty() || !f.motion.find(path) || !tables.abilities_of(path)) continue;
        ChaseReport straight, zig, walk;
        const bool a = run_chase(f, tables, {path, 720.0, 0.0, 12.0}, straight);
        const bool b = run_chase(f, tables, {path, 720.0, 60.0, 12.0}, zig);
        const bool c = run_chase(f, tables, {path, 360.0, 0.0, 12.0}, walk);
        CHECKF(a && b && c, "the three chases of the %s run", k.name);
        if (a && b && c) log_and_check_chase_kind(h, k.name, straight, zig, walk);
    }
}

void check_swing_facing_samples(TestHarness& h, const combat::NpcAbilityData& tables, const std::vector<LungeSample>& samples,
                                size_t hit_count, double rot_rate_yaw) {
    constexpr double kPi = 3.14159265358979323846;
    Fixture& f = h.f;
    const double dt = 1.0 / 15.0;
    double worst_turn = 0, worst_sent = 0, worst_lunge = 0;
    for (size_t i = 1; i < samples.size(); ++i) worst_turn = std::max(worst_turn, std::fabs(wrap180(samples[i].yaw - samples[i - 1].yaw)));
    size_t swings = 0, sent_off = 0, lunges = 0, wrong_way = 0;
    for (const LungeSample& s : samples) {
        if (!s.cast_began) continue;
        ++swings;
        if (s.has_mv_yaw) {
            const double d8 = std::fabs(wrap180(s.mv_yaw - s.yaw));
            const double d16 = s.has_rrm_yaw ? std::fabs(wrap180(s.rrm_yaw - s.yaw)) : 0.0;
            worst_sent = std::max(worst_sent, std::max(d8, d16));
            if (d8 > 0.75 * 360.0 / 256.0 || d16 > 0.05) ++sent_off;
        }
    }
    for (const LungeRun& r : lunge_runs(samples)) {
        const LungeSample& from = samples[r.first];
        const LungeSample& to = samples[r.last + 1];
        if (std::hypot(to.wx - from.wx, to.wy - from.wy) < 60.0) continue;
        const combat::MontageData* md = montage_of_cast(f, tables, from.ability, from.montage);
        if (!md || md->hits.empty()) continue;
        const size_t at_hit = std::min(samples.size() - 1, r.first + static_cast<size_t>(md->hits.front().time / md->rate_scale / dt + 0.5));
        const double bearing = std::atan2(samples[at_hit].py - from.wy, samples[at_hit].px - from.wx) * 180.0 / kPi;
        const double lunge = std::atan2(to.wy - from.wy, to.wx - from.wx) * 180.0 / kPi;
        const double off = std::fabs(wrap180(lunge - bearing));
        ++lunges;
        worst_lunge = std::max(worst_lunge, off);
        if (off > 45.0) ++wrong_way;
    }
    std::printf("  %zu swings in 40 s, %zu hits on the runner: the facing a client holds at their start is the server's within %.2f deg (%zu off); %zu lunges of 60 cm or more, the worst %.0f deg off the way to the player when the hit lands (%zu over 45); its facing turned %.1f deg at the most in a tick (its rate allows %.1f)\n",
                swings, hit_count, worst_sent, sent_off, lunges, worst_lunge, wrong_way, worst_turn, rot_rate_yaw / 15.0);
    CHECKF(swings >= 8, "the Bear swings again and again at a player who runs round it (%zu swings)", swings);
    CHECKF(sent_off == 0, "the facing a client is sent when a swing begins is the server's (%zu of %zu swings differ)", sent_off, swings);
    CHECKF(lunges >= 3 && wrong_way == 0, "every lunge goes towards the player, within 45 deg of the way to him at the hit (%zu of %zu do not, the worst %.0f deg)", wrong_way, lunges, worst_lunge);
    CHECKF(worst_turn <= rot_rate_yaw / 15.0 + 1.0, "its facing turns at its rotation rate and never jumps (%.1f deg in a tick, %.1f allowed)", worst_turn, rot_rate_yaw / 15.0 + 1.0);
}

void test_swing_faces_its_target(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a Bear whose player runs round it at 720 cm/s turns to him at its rate through every swing: the facing a client is sent is the server's, and the lunge goes towards him\n");
    constexpr double kPi = 3.14159265358979323846;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string bear_path = creature_path(f, "/Beast/Bear/Bear.Default__Bear_C");
    const auto bp = f.motion.find(bear_path);
    CHECK(!bear_path.empty() && bp && tables.abilities_of(bear_path), "the Blueprint table has the Bear and the creature tables list its abilities");
    if (bear_path.empty() || !bp || !tables.abilities_of(bear_path)) return;
    const double dt = 1.0 / 15.0, radius = 450.0, angular = 720.0 / radius;
    double angle = kPi / 2.0;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> got;
    LungeFightSpec spec;
    spec.path = bear_path;
    spec.seconds = 40.0;
    spec.player_y = radius;
    spec.hits = &got;
    spec.leash_cm = 40000.0;
    spec.steer = [&](double, const LungeSample*, double& x, double& y, const world::WorldActor&) {
        angle += angular * dt;
        x = radius * std::cos(angle);
        y = radius * std::sin(angle);
        return true;
    };
    CHECK(run_lunge_fight(f, tables, spec, samples), "a fight against a player who runs round the Bear runs");
    check_swing_facing_samples(h, tables, samples, got.size(), bp->rotation_rate_yaw);
}

void test_turn_to_a_player_behind(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a Wolf with a player right behind it turns to him at its rotation rate, one step at a time (never a jump), and its bite is begun within the tree's angle and goes on turning\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const auto bp = f.motion.find(wolf_path);
    CHECK(!wolf_path.empty() && bp && tables.abilities_of(wolf_path), "the Blueprint table has the Wolf and the creature tables list its abilities");
    if (wolf_path.empty() || !bp || !tables.abilities_of(wolf_path)) return;
    std::vector<LungeSample> samples;
    LungeFightSpec spec;
    spec.path = wolf_path;
    spec.seconds = 6.0;
    spec.player_x = -300.0;
    CHECK(run_lunge_fight(f, tables, spec, samples), "a fight against a player right behind the Wolf runs");
    const double step = bp->rotation_rate_yaw / 15.0;
    double worst_turn = 0;
    size_t turned = 0, first_cast = samples.size();
    for (size_t i = 0; i < samples.size(); ++i) {
        const double d = std::fabs(wrap180(samples[i].yaw - (i == 0 ? 0.0 : samples[i - 1].yaw)));
        worst_turn = std::max(worst_turn, d);
        if (d > 0.5 && first_cast == samples.size()) ++turned;
        if (samples[i].cast_began && first_cast == samples.size()) first_cast = i;
    }
    CHECK(first_cast < samples.size(), "the Wolf bites the player who was behind it");
    if (first_cast >= samples.size()) return;
    const LungeSample& at = samples[first_cast];
    const double error_at_start = std::fabs(wrap180(std::atan2(at.py - at.wy, at.px - at.wx) * 180.0 / 3.14159265358979323846 - at.yaw));
    const size_t later = std::min(samples.size() - 1, first_cast + 8);
    const double error_later = std::fabs(wrap180(std::atan2(samples[later].py - samples[later].wy, samples[later].px - samples[later].wx) * 180.0 / 3.14159265358979323846 - samples[later].yaw));
    std::printf("  its facing turned %.1f deg at the most in a tick (its rate allows %.1f), over %zu ticks before the bite began; it began %.1f deg off the player and was %.1f deg off half a second on\n",
                worst_turn, step, turned, error_at_start, error_later);
    CHECKF(worst_turn <= step + 1.0, "its facing turns at its rotation rate and never jumps (%.1f deg in a tick, %.1f allowed)", worst_turn, step + 1.0);
    CHECKF(turned >= 3, "it turns step by step to the player behind it (%zu ticks of turning before the bite)", turned);
    CHECKF(error_at_start <= 46.0 && error_later <= 6.0, "the bite begins within the tree's 45 deg and goes on turning to the player (%.1f deg off at its start, %.1f half a second on)", error_at_start, error_later);
}

}  // namespace

void run_combat_lunge_chase_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db) {
    (void)db;
    test_lunge_pauses_at_the_player(h);
    test_swing_is_not_cut_by_walk_orders(h);
    test_timid_raven_swings_back(h);
    test_chase_of_a_player_who_walks_away(h);
    test_swing_faces_its_target(h);
    test_turn_to_a_player_behind(h);
}

}  // namespace sandal::worldd::tests
