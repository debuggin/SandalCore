// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "combat_test_fixture.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

namespace sandal::worldd::tests {
namespace {

struct CcSpeed {
    double walked = 0, wire = 0;
    size_t ticks = 0, wire_ticks = 0;
};

CcSpeed cc_speed(const std::vector<LungeSample>& s, double from, double to) {
    CcSpeed out;
    double path = 0, time = 0, wire = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i].t <= from || s[i].t > to) continue;
        path += std::hypot(s[i].wx - s[i - 1].wx, s[i].wy - s[i - 1].wy);
        time += s[i].t - s[i - 1].t;
        ++out.ticks;
        if (s[i].wire_speed >= 0) {
            wire += s[i].wire_speed;
            ++out.wire_ticks;
        }
    }
    out.walked = time > 0 ? path / time : 0.0;
    out.wire = out.wire_ticks ? wire / static_cast<double>(out.wire_ticks) : 0.0;
    return out;
}

double cc_step(const std::vector<LungeSample>& s, size_t i) {
    return i > 0 && i < s.size() ? std::hypot(s[i].wx - s[i - 1].wx, s[i].wy - s[i - 1].wy) : 0.0;
}

double cc_land(LungeStepContext& c, const abilities::HitDef& hit) {
    combat::Fighter* target = c.world.fighter_of(c.creature);
    if (!target) return 0.0;
    combat::HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    combat::Resolved gate;
    std::string why;
    if (!c.world.resolver().resolve_effects(c.player.fighter(), *target, hit, meta, gate, why) || gate.effects.empty()) return 0.0;
    const double seconds = gate.effects.front().duration;
    c.world.apply_effects(c.creature, c.player.actor_id(), gate.effects, c.now);
    return seconds;
}

struct CcEvent {
    double                                 at = 0;
    std::function<void(LungeStepContext&)> act;
    bool                                   done = false;
};

struct CcRun {
    std::vector<LungeSample> s;
    std::vector<double>      chill_left;
    double                   lit = -1, chill_seconds = 0, snared_at = -1, snare_seconds = 0;
    double                   reset_at = -1, left_at = -1, died_at = -1;
    bool                     walking = false, ran = false;
};

struct CcFightArgs {
    std::string          path;
    double               seconds = 0, player_x = 0, player_y = 0, provoke_at = -1;
    LungeSteer           steer = nullptr;
    std::vector<CcEvent> events;
};

bool cc_fight(Fixture& f, const combat::NpcAbilityData& tables, CcFightArgs args, CcRun& out) {
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    LungeFightSpec spec;
    spec.path = args.path;
    spec.seconds = args.seconds;
    spec.player_x = args.player_x;
    spec.player_y = args.player_y;
    spec.provoke_at = args.provoke_at;
    spec.steer = args.steer;
    spec.leash_cm = 40000.0;
    spec.on_step = [&](LungeStepContext& c) {
        if (c.after) {
            out.chill_left.push_back(chilled ? c.world.effect_remaining(c.creature, chilled->row.guid, c.now) : 0.0);
            return;
        }
        for (CcEvent& e : args.events) {
            if (e.done || c.now < e.at) continue;
            e.done = true;
            e.act(c);
        }
    };
    out.ran = run_lunge_fight(f, tables, spec, out.s);
    return out.ran;
}

size_t cc_at(const std::vector<LungeSample>& s, double t) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].t >= t - 1e-9) return i;
    }
    return s.size();
}

struct CcLine {
    double snare = -1, speed = -1;
};

std::vector<CcLine> cc_logged(const std::function<void()>& body) {
    std::ostringstream log;
    const auto previous = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("slow_test", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    body();
    logger->flush();
    spdlog::set_default_logger(previous);
    std::vector<CcLine> out;
    std::istringstream in(log.str());
    for (std::string line; std::getline(in, line);) {
        const size_t at = line.find("[cc] ");
        if (at == std::string::npos) continue;
        CcLine l;
        char leaf[64] = {};
        unsigned long long id = 0;
        if (std::sscanf(line.c_str() + at, "[cc] %63s (%llu) snare %lf speed %lf", leaf, &id, &l.snare, &l.speed) == 4) out.push_back(l);
    }
    return out;
}

double cc_wander_speed(const std::string& wolf_path, double scale, size_t* steps) {
    combat_rig::World w;
    if (!w.problem.empty()) return -1.0;
    const uint64_t id = w.spawn(wolf_path, 0, 0, 7, 900);
    if (id == 0 || !w.npcs->remove(id)) return -1.0;
    npc::NpcMotionSetup setup;
    setup.ambient = npc::Ambient::Wander;
    setup.walk_speed = 150.0;
    if (!w.npcs->add(id, setup) || !w.npcs->set_speed_scale(id, scale)) return -1.0;
    std::vector<double> speeds;
    for (double now = 1.0 / 15.0; now < 60.0; now += 1.0 / 15.0) {
        w.npcs->note_player(77, 0, 500, now);
        w.npcs->tick(now);
        const npc::Mover* m = w.npcs->mover(id);
        if (m && m->moving()) speeds.push_back(std::hypot(m->velocity().x, m->velocity().y));
    }
    if (steps) *steps = speeds.size();
    if (speeds.size() < 20) return -1.0;
    std::sort(speeds.begin(), speeds.end());
    return speeds[speeds.size() / 2];
}

struct CcEnv {
    Fixture&                     f;
    const combat::NpcAbilityData& tables;
    std::string                  wolf_path;
    const abilities::HitDef*     frostbolt = nullptr;
    double                       snare_strength = 0.5;
    double                       kept = 0.5;
};

std::function<void(LungeStepContext&)> make_chill_on(const abilities::HitDef& frostbolt, CcRun& run) {
    return [&run, &frostbolt](LungeStepContext& c) {
        run.chill_seconds = cc_land(c, frostbolt);
        run.lit = c.now;
    };
}

std::function<void(LungeStepContext&)> make_sent_home(CcRun& run) {
    return [&run](LungeStepContext& c) {
        c.world.fighter_of(c.creature);
        const npc::NpcWorld::RelocateResult moved = c.w.npcs->relocate(c.creature, 0, 4500, 1000.0, false);
        c.w.npcs->restore_home(c.creature, nullptr);
        run.reset_at = c.now;
        run.walking = moved.status == npc::NpcWorld::Relocated::Ok && c.world.gm_reset(c.creature, c.now) == combat::CombatWorld::GmReset::Walking;
    };
}

void verify_chase_wire_and_log(TestHarness& h, const CcEnv& env, const CcRun& chase, const std::vector<CcLine>& chase_log, const CcSpeed& run_free) {
    const size_t on_i = cc_at(chase.s, chase.lit);
    size_t off_i = on_i;
    while (off_i < chase.chill_left.size() && (off_i == on_i || chase.chill_left[off_i] > 0)) ++off_i;
    const double free_step = run_free.walked / 15.0;
    CHECKF(near(cc_step(chase.s, on_i), env.kept * free_step, 0.05 * free_step) && near(cc_step(chase.s, on_i + 1), env.kept * free_step, 0.05 * free_step),
           "the slow is at once: the first two steps under it are %.1f and %.1f cm, a slowed step is %.1f", cc_step(chase.s, on_i), cc_step(chase.s, on_i + 1), env.kept * free_step);
    CHECKF(off_i < chase.s.size() && near(cc_step(chase.s, off_i + 1), free_step, 0.05 * free_step), "...and so is the end: the step after the chill came off is %.1f cm, a free step is %.1f", cc_step(chase.s, off_i + 1), free_step);
    CHECKF(on_i < chase.s.size() && chase.s[on_i].cc_flags == 16 && chase.s[on_i].has_cc_snare && near(chase.s[on_i].cc_snare, env.snare_strength, 1e-6) && chase.s[on_i].cc_items >= 1 && chase.s[on_i].cc_fields == 2,
           "the update that adds the effect's item says the state: flags %d, snare %.2f, %zu fields, %zu item(s)", on_i < chase.s.size() ? chase.s[on_i].cc_flags : -1,
           on_i < chase.s.size() ? chase.s[on_i].cc_snare : -1.0, on_i < chase.s.size() ? chase.s[on_i].cc_fields : 0, on_i < chase.s.size() ? chase.s[on_i].cc_items : 0);
    CHECKF(off_i < chase.s.size() && chase.s[off_i].cc_flags == 0 && chase.s[off_i].has_cc_snare && chase.s[off_i].cc_snare == 0.0 && chase.s[off_i].cc_fields == 2,
           "the update of the tick the chill comes off says it cleared: flags %d, snare %.2f (%zu fields)", off_i < chase.s.size() ? chase.s[off_i].cc_flags : -1, off_i < chase.s.size() ? chase.s[off_i].cc_snare : -1.0,
           off_i < chase.s.size() ? chase.s[off_i].cc_fields : 0);
    size_t said_before = 0, said_between_wrong = 0, said_after = 0;
    for (size_t i = 0; i < chase.s.size(); ++i) {
        if (chase.s[i].cc_flags < 0) continue;
        if (i < on_i) ++said_before;
        else if (i > on_i && i < off_i && (chase.s[i].cc_flags != 16 || !near(chase.s[i].cc_snare, env.snare_strength, 1e-6))) ++said_between_wrong;
        else if (i > off_i) ++said_after;
    }
    CHECKF(said_before == 0 && said_between_wrong == 0 && said_after == 0, "no other update says anything of the state (%zu before, %zu wrong during, %zu after)", said_before, said_between_wrong, said_after);
    CHECKF(chase_log.size() == 2 && near(chase_log[0].snare, env.snare_strength, 0.005) && near(chase_log[1].snare, 0.0, 0.005) && near(chase_log[0].speed, env.kept * chase_log[1].speed, 1.0) &&
               near(chase_log[1].speed, run_free.walked, 0.02 * run_free.walked),
           "the log says it once when the chill goes on and once when it comes off (%zu lines)", chase_log.size());
}

void check_cc_chase(TestHarness& h, const CcEnv& env) {
    CcRun chase;
    bool put_away = false;
    const LungeSteer far_off = [&put_away](double now, const LungeSample*, double& x, double& y, const world::WorldActor&) {
        if (put_away || now < 0.6) return false;
        put_away = true;
        x = 0;
        y = 14000;
        return true;
    };
    const std::vector<CcLine> chase_log = cc_logged([&] {
        cc_fight(env.f, env.tables, {env.wolf_path, 12.5, 0, 3000, 0.2, far_off, {{3.0, make_chill_on(*env.frostbolt, chase)}}}, chase);
    });
    CHECK(chase.ran, "a fight of the Wolf against a player who is put 140 m away runs");
    if (!chase.ran) return;
    const double chase_end = chase.lit + chase.chill_seconds;
    const size_t on_i = cc_at(chase.s, chase.lit);
    const CcSpeed run_free = cc_speed(chase.s, 1.0, chase.lit - 0.1);
    const CcSpeed run_chilled = cc_speed(chase.s, chase.lit + 0.4, chase_end - 0.4);
    const CcSpeed run_after = cc_speed(chase.s, chase_end + 0.4, chase_end + 2.4);
    CHECKF(chase.chill_seconds > 4.0 && chase.chill_seconds < 9.0 && on_i < chase.chill_left.size() && chase.chill_left[on_i] > 0.0,
           "the chill is on the Wolf after Frostbolt's hit: it lasts %.2f s by the record", chase.chill_seconds);
    CHECKF(run_free.walked > 300.0 && run_free.ticks >= 20 && near(run_free.wire, run_free.walked, 0.03 * run_free.walked),
           "the Wolf runs after the player at %.1f cm/s and its updates carry %.1f", run_free.walked, run_free.wire);
    CHECKF(near(run_chilled.walked, env.kept * run_free.walked, 0.015 * run_free.walked) && run_chilled.ticks >= 20,
           "chilled it covers %.2f of the ground it covered free (%.1f of %.1f cm/s)", run_chilled.walked / run_free.walked, run_chilled.walked, run_free.walked);
    CHECKF(near(run_chilled.wire, env.kept * run_free.wire, 0.015 * run_free.wire + 2.0) && run_chilled.wire_ticks >= 20,
           "...and the speed its updates carry is %.2f of the free one", run_chilled.wire / run_free.wire);
    CHECKF(near(run_after.walked, run_free.walked, 0.015 * run_free.walked) && near(run_after.wire, run_free.wire, 0.015 * run_free.wire + 2.0) && run_after.ticks >= 20,
           "when the chill has run out it runs at its full speed again (%.1f cm/s)", run_after.walked);
    verify_chase_wire_and_log(h, env, chase, chase_log, run_free);
}

void check_cc_walk_home_and_departures(TestHarness& h, const CcEnv& env, CcRun& home_free, CcSpeed& walk_free) {
    CcRun home_chilled;
    CHECK(cc_fight(env.f, env.tables, {env.wolf_path, 11.0, 0, 14000, -1, nullptr, {{1.0, make_sent_home(home_free)}}}, home_free) &&
              cc_fight(env.f, env.tables, {env.wolf_path, 11.0, 0, 14000, -1, nullptr, {{1.0, make_chill_on(*env.frostbolt, home_chilled)}, {1.0, make_sent_home(home_chilled)}}}, home_chilled),
          "two walks home run, one of a free Wolf and one of a chilled one");
    if (!home_free.ran || !home_chilled.ran) return;
    CHECK(home_free.walking && home_chilled.walking, "the Wolf is sent home on foot in both");
    const double home_end = home_chilled.lit + home_chilled.chill_seconds;
    walk_free = cc_speed(home_free.s, home_free.reset_at + 0.5, home_free.reset_at + 5.0);
    const CcSpeed walk_chilled = cc_speed(home_chilled.s, home_chilled.reset_at + 0.5, std::min(home_chilled.reset_at + 5.0, home_end - 0.4));
    const CcSpeed walk_free_after = cc_speed(home_free.s, home_end + 0.4, home_end + 3.4);
    const CcSpeed walk_chilled_after = cc_speed(home_chilled.s, home_end + 0.4, home_end + 3.4);
    CHECKF(walk_free.walked > 100.0 && walk_free.ticks >= 20 && near(walk_free.wire, walk_free.walked, 0.03 * walk_free.walked), "a free Wolf walks home at %.1f cm/s and its updates carry %.1f", walk_free.walked, walk_free.wire);
    CHECKF(near(walk_chilled.walked, env.kept * walk_free.walked, 0.015 * walk_free.walked) && walk_chilled.ticks >= 20, "chilled it walks home at %.2f of that", walk_chilled.walked / walk_free.walked);
    CHECKF(near(walk_chilled.wire, env.kept * walk_free.wire, 0.015 * walk_free.wire + 2.0) && walk_chilled.wire_ticks >= 20, "...and its updates carry %.2f of the free speed", walk_chilled.wire / walk_free.wire);
    CHECK(near(walk_chilled_after.walked, walk_free_after.walked, 0.015 * walk_free_after.walked) && walk_chilled_after.ticks >= 20, "when the chill has run out it walks at the full speed");

    CcRun left;
    const bool left_ran = cc_fight(env.f, env.tables,
                                   {env.wolf_path, 8.0, 0, 14000, -1, nullptr,
                                    {{1.0, make_chill_on(*env.frostbolt, left)}, {1.0, make_sent_home(left)}, {3.0, [&left](LungeStepContext& c) {
                                                                                                                  left.left_at = c.now;
                                                                                                                  c.world.remove_player(&c.player);
                                                                                                              }}}},
                                   left);
    CHECK(left_ran, "a walk home in which the player leaves runs");
    if (left_ran) {
        const size_t i = cc_at(left.s, left.left_at);
        const CcSpeed after_leaving = cc_speed(left.s, left.left_at + 0.4, left.left_at + 2.9);
        const CcSpeed free_then = cc_speed(home_free.s, left.left_at + 0.4, left.left_at + 2.9);
        CHECKF(i < left.s.size() && left.chill_left[i] == 0.0 && left.s[i].cc_flags == 0 && left.s[i].has_cc_snare && left.s[i].cc_snare == 0.0 && left.s[i].held_effects == 0,
               "the update of the tick the player leaves says the state cleared and the chill is gone");
        CHECKF(near(after_leaving.walked, free_then.walked, 0.015 * free_then.walked) && after_leaving.ticks >= 20, "...and the Wolf walks at its full speed at once");
    }
}

void check_cc_death_and_lunge(TestHarness& h, const CcEnv& env) {
    CcRun died;
    const bool died_ran = cc_fight(env.f, env.tables,
                                   {env.wolf_path, 6.0, 0, 14000, -1, nullptr,
                                    {{1.0, make_chill_on(*env.frostbolt, died)}, {1.0, make_sent_home(died)}, {3.0, [&died](LungeStepContext& c) {
                                                                                                                  died.died_at = c.now;
                                                                                                                  combat::Resolved lethal;
                                                                                                                  lethal.mods.push_back({1, content::kGuidHealth, -1.0e9});
                                                                                                                  c.world.apply_hit_to_npc(c.creature, c.player.actor_id(), lethal, c.now);
                                                                                                              }}}},
                                   died);
    CHECK(died_ran, "a walk home in which the Wolf is killed runs");
    if (died_ran) {
        const size_t i = cc_at(died.s, died.died_at);
        CHECKF(i < died.s.size() && died.chill_left[i] == 0.0 && died.s[i].cc_flags == 0 && died.s[i].has_cc_snare && died.s[i].cc_snare == 0.0 && died.s[i].held_effects == 0,
               "the update of the tick it dies says the state cleared and it holds nothing");
    }

    CcRun lunge_free, lunge_chilled;
    CHECK(cc_fight(env.f, env.tables, {env.wolf_path, 10.0, 0, 800, 0.2, nullptr, {}}, lunge_free) &&
              cc_fight(env.f, env.tables, {env.wolf_path, 10.0, 0, 800, 0.2, nullptr, {{0.1, make_chill_on(*env.frostbolt, lunge_chilled)}}}, lunge_chilled),
          "a fight against a player 8 m away runs, free and chilled");
    if (!lunge_free.ran || !lunge_chilled.ran) return;
    struct Lunge {
        bool   found = false, chilled = false;
        double at = 0, travelled = 0, longest_step = 0;
    };
    const auto first_lunge = [](const CcRun& r) {
        Lunge out;
        for (const LungeRun& lr : lunge_runs(r.s)) {
            const double travelled = std::hypot(r.s[lr.last + 1].wx - r.s[lr.first].wx, r.s[lr.last + 1].wy - r.s[lr.first].wy);
            if (travelled < 40.0) continue;
            out.found = true;
            out.at = r.s[lr.first].t;
            out.travelled = travelled;
            out.chilled = lr.first < r.chill_left.size() && r.chill_left[lr.first] > 0.0;
            for (size_t i = lr.first; i <= lr.last + 1; ++i) out.longest_step = std::max(out.longest_step, cc_step(r.s, i));
            break;
        }
        return out;
    };
    const Lunge a = first_lunge(lunge_free), b = first_lunge(lunge_chilled);
    CHECK(a.found && b.found, "both Wolves lunge at the player");
    CHECKF(b.found && b.chilled, "the chilled Wolf lunges while the chill is on it (at %.2f s)", b.at);
    CHECKF(a.found && b.found && near(b.travelled, a.travelled, 0.1 * a.travelled + 10.0) && near(b.longest_step, a.longest_step, 0.1 * a.longest_step + 2.0),
           "...and its lunge is the free one's: %.0f against %.0f cm, steps of %.1f against %.1f cm", b.travelled, a.travelled, b.longest_step, a.longest_step);
}

void check_cc_two_snares(TestHarness& h, const CcEnv& env, content::AbilityContent& ability_db, const CcSpeed& walk_free) {
    const abilities::HitDef* disengage = env.f.catalog->hit_named("Ranger_Disengage_Hit");
    const abilities::EffectDef* snared = env.f.catalog->effect_named("Status_Snared");
    CHECK(disengage && snared, "the data has the Ranger's Disengage hit and the Snared effect");
    if (!disengage || !snared) return;
    std::string err;
    const auto snared_record = ability_db.crowd_control(snared->row.crowd_control_guid, err);
    CHECKF(snared_record && snared_record->applied_status == 16 && near(snared_record->snare_strength, 0.25, 1e-9), "Snared is a snare of 0.25 and nothing else: %s", err.c_str());
    if (!snared_record) return;
    CcRun two;
    const bool ran = cc_fight(env.f, env.tables,
                              {env.wolf_path, 11.5, 0, 14000, -1, nullptr,
                               {{1.0, make_chill_on(*env.frostbolt, two)},
                                {1.0, [&two, disengage](LungeStepContext& c) {
                                     two.snare_seconds = cc_land(c, *disengage);
                                     two.snared_at = c.now;
                                 }},
                                {1.0, make_sent_home(two)}}},
                              two);
    CHECK(ran, "a walk home under a chill and a Snared runs");
    if (!ran) return;
    const double chill_off = two.lit + two.chill_seconds, snare_off = two.snared_at + two.snare_seconds, snared_keeps = 1.0 - snared_record->snare_strength;
    const CcSpeed both = cc_speed(two.s, two.reset_at + 0.5, chill_off - 0.4);
    const CcSpeed weaker = cc_speed(two.s, chill_off + 0.4, snare_off - 0.4);
    const CcSpeed none = cc_speed(two.s, snare_off + 0.4, snare_off + 1.9);
    CHECKF(snare_off > chill_off + 1.0, "the Snared outlasts the chill (%.2f s against %.2f s)", snare_off, chill_off);
    CHECKF(both.ticks >= 20 && near(both.walked, env.kept * walk_free.walked, 0.015 * walk_free.walked) && near(both.wire, env.kept * walk_free.wire, 0.015 * walk_free.wire + 2.0),
           "under both it keeps what the stronger leaves: %.2f of its speed, not the product %.3f", both.walked / walk_free.walked, env.kept * snared_keeps);
    CHECKF(weaker.ticks >= 12 && near(weaker.walked, snared_keeps * walk_free.walked, 0.015 * walk_free.walked) && near(weaker.wire, snared_keeps * walk_free.wire, 0.015 * walk_free.wire + 2.0),
           "when the chill is off the Snared holds it at %.2f", weaker.walked / walk_free.walked);
    CHECKF(none.ticks >= 10 && near(none.walked, walk_free.walked, 0.015 * walk_free.walked), "...and when that is off it is free again (%.1f cm/s)", none.walked);
    size_t chill_end_i = cc_at(two.s, two.lit);
    while (chill_end_i < two.chill_left.size() && two.chill_left[chill_end_i] > 0) ++chill_end_i;
    CHECKF(chill_end_i < two.s.size() && two.s[chill_end_i].cc_flags == -1 && two.s[chill_end_i].cc_fields == 1 && two.s[chill_end_i].has_cc_snare &&
               near(two.s[chill_end_i].cc_snare, snared_record->snare_strength, 1e-6),
           "the update of the tick the chill comes off says only that the snare is the weaker one now");
    size_t cleared_at = two.s.size();
    for (size_t i = chill_end_i + 1; i < two.s.size(); ++i) {
        if (two.s[i].cc_flags == 0) {
            cleared_at = i;
            break;
        }
    }
    CHECKF(cleared_at < two.s.size() && two.s[cleared_at].has_cc_snare && two.s[cleared_at].cc_snare == 0.0 && std::fabs(two.s[cleared_at].t - snare_off) < 0.2,
           "...and the update of the tick the Snared comes off says it cleared");
}

void check_cc_wander_and_grapple(TestHarness& h, const CcEnv& env, const CcSpeed& walk_free) {
    size_t free_steps = 0, slow_steps = 0;
    const double wander_free = cc_wander_speed(env.wolf_path, 1.0, &free_steps), wander_slow = cc_wander_speed(env.wolf_path, env.kept, &slow_steps);
    CHECKF(wander_free > 50.0 && wander_slow > 0.0 && near(wander_slow, env.kept * wander_free, 0.015 * wander_free),
           "a wandering Wolf walks its legs at %.2f of its speed when its NPC world keeps %.2f of it", wander_free > 0 ? wander_slow / wander_free : -1.0, env.kept);

    const abilities::HitDef* grapple = env.f.catalog->hit_named("Tank_Grapple_HitSnare1");
    CHECK(grapple != nullptr, "the data has the Grapple's first snare");
    if (!grapple) return;
    CcRun chain;
    const std::vector<CcLine> chain_log = cc_logged([&] {
        cc_fight(env.f, env.tables,
                 {env.wolf_path, 8.0, 0, 14000, -1, nullptr,
                  {{1.0, [&chain, grapple](LungeStepContext& c) {
                        chain.snare_seconds = cc_land(c, *grapple);
                        chain.snared_at = c.now;
                    }},
                   {1.0, make_sent_home(chain)}}},
                 chain);
    });
    CHECK(chain.ran, "a walk home under the Grapple's snares runs");
    if (!chain.ran) return;
    std::vector<size_t> says;
    for (size_t i = 0; i < chain.s.size(); ++i) {
        if (chain.s[i].has_cc_snare) says.push_back(i);
    }
    const double want[4] = {0.75, 0.5, 0.25, 0.0};
    bool in_order = says.size() == 4;
    for (size_t k = 0; in_order && k < 4; ++k) in_order = near(chain.s[says[k]].cc_snare, want[k], 1e-6);
    CHECKF(in_order, "the updates name the three snares and then the end, in order: %zu updates", says.size());
    CHECKF(chain_log.size() == 4 && near(chain_log[0].snare, 0.75, 0.005) && near(chain_log[1].snare, 0.5, 0.005) && near(chain_log[2].snare, 0.25, 0.005) && near(chain_log[3].snare, 0.0, 0.005),
           "...and so does the log, with no free Wolf between the snares: %zu lines", chain_log.size());
    if (!in_order) return;
    for (size_t k = 0; k < 3; ++k) {
        const CcSpeed under = cc_speed(chain.s, chain.s[says[k]].t + 0.15, chain.s[says[k + 1]].t - 0.15);
        CHECKF(under.ticks >= 6 && near(under.walked, (1.0 - want[k]) * walk_free.walked, 0.015 * walk_free.walked) && near(under.wire, (1.0 - want[k]) * walk_free.wire, 0.015 * walk_free.wire + 2.0),
               "under the snare of %.2f it walks at %.1f cm/s (%.2f of the free %.1f)", want[k], under.walked, 1.0 - want[k], walk_free.walked);
    }
}

void test_cc_on_creatures(TestHarness& h) {
    Fixture& f = h.f;
    if (!f.ok()) return;
    std::printf("a Wolf under a slow: half the speed in a chase and on the walk home, at once, full speed again when the chill ends, the applier leaves or it dies; its lunge goes on unslowed\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    const abilities::HitDef* frostbolt = f.catalog->hit_named("Mage_Frostbolt_Hit");
    const abilities::EffectDef* chilled = f.catalog->effect_named("Status_Chilled");
    CHECK(!wolf_path.empty() && f.motion.find(wolf_path) && tables.abilities_of(wolf_path) && frostbolt && chilled,
          "the Blueprint table has the Wolf, the creature tables list its abilities, and the data has Frostbolt's hit and the Chilled effect");
    if (wolf_path.empty() || !f.motion.find(wolf_path) || !tables.abilities_of(wolf_path) || !frostbolt || !chilled) return;
    std::string err;
    auto ability_db = content::AbilityContent::open(f.path, err);
    const auto record = ability_db ? ability_db->crowd_control(chilled->row.crowd_control_guid, err) : std::nullopt;
    CHECKF(record && record->applied_status == 16 && near(record->snare_strength, 0.5, 1e-9), "the Chilled effect's crowd control record is a snare of 0.5 and nothing else (status flags 16): %s", err.c_str());
    if (!record || !ability_db) return;
    const CcEnv env{f, tables, wolf_path, frostbolt, record->snare_strength, 1.0 - record->snare_strength};
    check_cc_chase(h, env);
    CcRun home_free;
    CcSpeed walk_free;
    check_cc_walk_home_and_departures(h, env, home_free, walk_free);
    if (!home_free.ran) return;
    check_cc_death_and_lunge(h, env);
    check_cc_two_snares(h, env, *ability_db, walk_free);
    check_cc_wander_and_grapple(h, env, walk_free);
}

}  // namespace

void run_combat_cc_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db) {
    (void)db;
    test_cc_on_creatures(h);
}

}  // namespace sandal::worldd::tests
