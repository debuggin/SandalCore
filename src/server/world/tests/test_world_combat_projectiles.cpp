#include "combat_test_fixture.h"

#include <filesystem>

namespace sandal::worldd::tests {
namespace {

bool load_creature_tables(TestHarness& h, combat::NpcAbilityData& tables) {
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    const std::string files[3] = {dir + "npc_abilities.tsv", dir + "npc_montages.tsv", dir + "npc_targeting.tsv"};
    std::string err;
    if (tables.load(files[0], files[1], files[2], err)) return true;
    std::error_code ec;
    bool any_there = false;
    for (const std::string& file : files) any_there = any_there || std::filesystem::exists(file, ec);
    if (!any_there) {
        std::printf("  [skip] the creature tables (tools/npcabilities, the npcabilities extractor step): %s\n", err.c_str());
        return false;
    }
    CHECKF(false, "the creature tables are there but do not read: %s", err.c_str());
    return false;
}

struct RaiderCast {
    double      t;
    int64_t     ability;
    uint64_t    montage;
    std::string label;
};

void check_open_lists_all_tree_abilities(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& path,
                                         const spawns::OpenTemplate* open, bool check_cylinder) {
    const auto bp = h.f.motion.find(path);
    const double half_height = bp ? bp->half_height : 88.0;
    const std::string name = spawns::blueprint_leaf(path);
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
    CHECKF(a && wanted >= 5 && listed == wanted, "the %s's open lists the %zu abilities of its tree (%zu listed)", name.c_str(), wanted, listed);
    if (check_cylinder) {
        CHECKF(a && a->npc.collision_cylinder != 0, "a hit on the %s is confirmed against its collision cylinder, which its open names", name.c_str());
    }
}

void log_and_check_raider_cast_cadence(TestHarness& h, const std::string& name, double chase_began, const std::vector<RaiderCast>& casts,
                                       bool has_specials) {
    constexpr double kDt = 1.0 / 15.0, kSeconds = 60.0;
    std::map<std::string, std::vector<double>> by_name;
    for (const RaiderCast& c : casts) by_name[c.label].push_back(c.t);
    std::string summary;
    for (const auto& [label, times] : by_name) summary += (summary.empty() ? "" : ", ") + label + " " + std::to_string(times.size());
    std::printf("  %s, %.0f s: %zu casts (%s); the chase began at %.2f s\n", name.c_str(), kSeconds, casts.size(), summary.c_str(), chase_began);
    std::string line;
    for (size_t i = 0; i < casts.size() && i < 24; ++i) {
        std::string label = casts[i].label;
        if (label.rfind("Bandit_Raider_", 0) == 0) label.erase(0, 14);
        line += (line.empty() ? "" : " ") + label + "@" + std::to_string(casts[i].t - std::max(0.0, chase_began)).substr(0, 5);
    }
    std::printf("  casts, seconds after the chase began: %s\n", line.c_str());
    std::vector<double> gaps;
    for (size_t i = 1; i < casts.size(); ++i) {
        if (casts[i].label == "Bandit_Raider_Strike" && casts[i - 1].label == "Bandit_Raider_Strike") gaps.push_back(casts[i].t - casts[i - 1].t);
    }
    std::sort(gaps.begin(), gaps.end());
    if (!gaps.empty()) std::printf("  strike to strike: %zu gaps, %.2f s at least, %.2f s in the middle, %.2f s at most\n", gaps.size(), gaps.front(), gaps[gaps.size() / 2], gaps.back());
    CHECKF(gaps.size() >= 5 && gaps.front() >= 2.0 - 1e-6 && gaps.back() <= 2.4,
           "strikes come 2.0 to 2.4 s apart, not at the montage's length (%.2f to %.2f s)", gaps.empty() ? 0.0 : gaps.front(), gaps.empty() ? 0.0 : gaps.back());
    const auto count = [&](const char* label) {
        const auto it = by_name.find(label);
        return it != by_name.end() ? it->second.size() : size_t{0};
    };
    CHECKF(casts.size() >= 12, "it plays its own animations (%zu casts in %.0f s; with the single swing it would play none)", casts.size(), kSeconds);
    CHECKF(count("Bandit_Raider_Strike") >= 8, "it strikes over and over (%zu strikes)", count("Bandit_Raider_Strike"));
    CHECKF(count("Bandit_Raider_HeavySlash") + count("Bandit_Raider_Whirlwind") >= 1,
           "it uses a special attack once the tree's timer for it has run (%zu heavy slashes, %zu whirlwinds)",
           count("Bandit_Raider_HeavySlash"), count("Bandit_Raider_Whirlwind"));
    if (has_specials) {
        const auto it = by_name.find("Bandit_Raider_BrutalCleave");
        const std::vector<double> empty_vec;
        const std::vector<double>& cleaves = it != by_name.end() ? it->second : empty_vec;
        CHECKF(cleaves.size() >= 3, "it cleaves several times in %.0f s (%zu)", kSeconds, cleaves.size());
        if (!cleaves.empty()) {
            std::printf("  the first cleave came %.2f s into the chase; the cleaves came %.2f s apart on average\n", cleaves.front() - chase_began,
                        cleaves.size() > 1 ? (cleaves.back() - cleaves.front()) / static_cast<double>(cleaves.size() - 1) : 0.0);
            CHECKF(cleaves.front() - chase_began >= 8.0 - 2 * kDt && cleaves.front() - chase_began <= 14.0,
                   "the first cleave comes after the fight's 8th second (%.2f s into the chase)", cleaves.front() - chase_began);
            for (size_t i = 1; i < cleaves.size(); ++i) {
                CHECKF(cleaves[i] - cleaves[i - 1] >= 8.0, "the cleave's own cooldown of 8 s holds (%.2f s between two)", cleaves[i] - cleaves[i - 1]);
            }
        }
    } else {
        CHECKF(count("Bandit_Raider_BrutalCleave") == 0 && count("Bandit_Raider_Whirlwind") == 0,
               "its tree switches the cleave and the whirlwind off (%zu, %zu)", count("Bandit_Raider_BrutalCleave"), count("Bandit_Raider_Whirlwind"));
    }
    std::vector<double> specials;
    for (const RaiderCast& c : casts) {
        if (c.label == "Bandit_Raider_HeavySlash" || c.label == "Bandit_Raider_Whirlwind") specials.push_back(c.t);
    }
    for (size_t i = 1; i < specials.size(); ++i) {
        CHECKF(specials[i] - specials[i - 1] >= 8.0, "two special attacks are at least the shared cooldown of 8 s apart (%.2f s)", specials[i] - specials[i - 1]);
    }
}

void check_raider_hit_notifies(TestHarness& h, const combat::NpcAbilityData& tables, const std::vector<RaiderCast>& casts,
                               const std::vector<combat::IncomingHit>& hits) {
    constexpr double kDt = 1.0 / 15.0, kSeconds = 60.0;
    Fixture& f = h.f;
    size_t due = 0, landed = 0, stray = 0;
    double worst_delay = 0;
    std::vector<bool> used(hits.size(), false);
    for (const RaiderCast& c : casts) {
        const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
        if (!md || c.t + md->seconds(1.0) > kSeconds - 0.3) continue;
        for (const double at : cast_bites(f, tables, c.ability, c.montage)) {
            ++due;
            for (size_t i = 0; i < hits.size(); ++i) {
                if (used[i] || hits[i].source_guid != c.ability) continue;
                const double delay = hits[i].at - (c.t + at);
                if (delay < -1e-6 || delay > 2 * kDt + 1e-6) continue;
                used[i] = true;
                ++landed;
                worst_delay = std::max(worst_delay, delay);
                break;
            }
        }
    }
    for (size_t i = 0; i < hits.size(); ++i) {
        if (used[i]) continue;
        bool in_cast = false;
        for (const RaiderCast& c : casts) {
            const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
            if (md && hits[i].source_guid == c.ability && hits[i].at >= c.t - 1e-6 && hits[i].at <= c.t + md->seconds(1.0) + 2 * kDt) in_cast = true;
        }
        if (!in_cast) ++stray;
    }
    std::printf("  %zu hits were due at their notifies, %zu landed (the latest %.3f s after its notify); %zu hits outside a cast\n", due, landed, worst_delay, stray);
    CHECKF(due >= 8 && landed == due, "every hit of every cast lands at its montage's notify (%zu of %zu)", landed, due);
    CHECKF(stray == 0, "no hit lands outside a cast (%zu)", stray);
}

void check_raider_reach_and_drift(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& path,
                                  double chase_began, const std::vector<LungeSample>& samples) {
    Fixture& f = h.f;
    const auto bp = f.motion.find(path);
    const double r_creature = bp ? bp->radius : 40.0;
    double reach = 0, travel = 0;
    for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
        if (!t.unknown && !t.off && t.fight && t.min_distance <= 0 && t.max_distance > reach) reach = t.max_distance;
        const abilities::AbilityDef* def = f.catalog->ability(t.ability);
        if (!def) continue;
        for (const content::MontageRow& m : def->montages) {
            const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
            if (md && !md->root_unreadable && !md->root_motion.empty()) travel = std::max(travel, std::hypot(md->root_motion.back().x, md->root_motion.back().y));
        }
    }
    double worst_start = 0, worst_gap = 0;
    size_t drift_ticks = 0;
    for (const LungeSample& s : samples) {
        const double gap = std::hypot(s.wx - s.px, s.wy - s.py) - r_creature - 34.0;
        if (s.cast_began) worst_start = std::max(worst_start, gap);
        if (chase_began >= 0 && s.t > chase_began + 3.0) {
            worst_gap = std::max(worst_gap, gap);
            if (gap > reach + travel + 60.0) ++drift_ticks;
        }
    }
    std::printf("  reach %.0f cm, the farthest travel of a montage %.0f cm; the surface gap was at most %.0f cm when a cast began and %.0f cm at any time after the chase\n",
                reach, travel, worst_start, worst_gap);
    CHECKF(worst_start <= reach + 5.0, "it begins its casts within the tree's reach (%.0f of %.0f cm)", worst_start, reach);
    CHECKF(drift_ticks == 0, "it never drifts away from a player who stands still (%zu ticks farther than reach + travel)", drift_ticks);
    const LungeSample& last = samples.back();
    const double end_gap = std::hypot(last.wx - last.px, last.wy - last.py) - r_creature - 34.0;
    CHECKF(end_gap <= reach + travel + 60.0, "at the end of the fight it is still about the player (%.0f cm from his surface)", end_gap);
}

void run_raider_kind_case(TestHarness& h, const combat::NpcAbilityData& tables, const char* leaf, bool has_specials) {
    constexpr double kSeconds = 60.0;
    Fixture& f = h.f;
    const std::string path = creature_path(f, leaf);
    const auto bp = f.motion.find(path);
    const std::string name = spawns::blueprint_leaf(path);
    CHECKF(!path.empty() && bp && tables.abilities_of(path) != nullptr, "the Blueprint table has the %s and the creature tables list its abilities", name.c_str());
    if (path.empty() || !bp || !tables.abilities_of(path)) return;
    CHECKF(spawns::fighting_humanoid(path), "%s is a hostile humanoid whose fights are built", name.c_str());
    BanditOpen made;
    CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the %s (verdict: %s)", name.c_str(), spawns::verdict_name(made.why));
    if (!made.open) return;
    check_open_lists_all_tree_abilities(h, tables, path, made.open, true);

    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    combat::CombatWorld::Stats stats;
    LungeFightSpec spec;
    spec.path = path;
    spec.seconds = kSeconds;
    spec.player_y = 800.0;
    spec.hits = &hits;
    spec.stats = &stats;
    spec.leash_cm = 40000.0;
    spec.made = made.open;
    CHECKF(run_lunge_fight(f, tables, spec, samples), "a fight of %.0f s against a player who stands 8 m away runs", kSeconds);
    if (samples.empty()) return;
    check_targets_named_in_full(h, samples, name.c_str());

    std::vector<RaiderCast> casts;
    double chase_began = -1;
    for (const LungeSample& s : samples) {
        if (chase_began < 0 && std::hypot(s.wx, s.wy) > 1.0) chase_began = s.t;
        if (!s.cast_began) continue;
        const abilities::AbilityDef* def = f.catalog->ability(s.ability);
        casts.push_back({s.t, s.ability, s.montage, def ? def->row.name : std::string("?")});
    }
    log_and_check_raider_cast_cadence(h, name, chase_began, casts, has_specials);
    check_raider_hit_notifies(h, tables, casts, hits);
    check_raider_reach_and_drift(h, tables, path, chase_began, samples);
}

void test_bandit_raider_fights_a_standing_player(TestHarness& h) {
    std::printf("a bandit raider against a player who stands still: made from a guard's open with its tree's abilities, it strikes within reach, lands each hit at its montage's notify, "
                "holds its specials back by the tree's timers and does not drift\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(h, tables)) return;
    run_raider_kind_case(h, tables, "/Bandit_Raider/CSB_Bandit_Raider_StateTree.Default__CSB_Bandit_Raider_StateTree_C", true);
    run_raider_kind_case(h, tables, "/Bandit_Raider/Bandit_Raider.Default__Bandit_Raider_C", false);
}

struct ShotCast {
    double      t;
    int64_t     ability;
    uint64_t    montage;
    std::string name;
};

std::vector<ShotCast> shot_casts(Fixture& f, const std::vector<LungeSample>& samples) {
    std::vector<ShotCast> out;
    for (const LungeSample& s : samples) {
        if (!s.cast_began) continue;
        const abilities::AbilityDef* def = f.catalog->ability(s.ability);
        out.push_back({s.t, s.ability, s.montage, def ? def->row.name : std::string("?")});
    }
    return out;
}

struct ShotReport {
    size_t              due = 0, landed = 0, stray = 0, not_projectile = 0;
    std::vector<double> after_cast;
    std::vector<double> flight;
    double              earliest_after_launch = 1e9;
};

ShotReport shot_report(Fixture& f, const combat::NpcAbilityData& tables, const std::vector<ShotCast>& casts,
                       const std::vector<combat::IncomingHit>& hits, double flight_limit) {
    const double seconds = flight_limit < 1.2 ? 30.0 : 60.0;
    ShotReport r;
    std::vector<bool> used(hits.size(), false);
    for (const ShotCast& c : casts) {
        const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
        if (!md) continue;
        for (const combat::MontageData::Fire& fire : md->fires) {
            const bool counted = c.t + md->seconds(1.0) + flight_limit <= seconds - 0.3;
            const double leaves = c.t + fire.time / md->rate_scale;
            if (counted) ++r.due;
            for (size_t i = 0; i < hits.size(); ++i) {
                if (used[i] || hits[i].source_type != content::kSourceTypeProjectile || hits[i].at < leaves - 1e-6 || hits[i].at > leaves + flight_limit) continue;
                used[i] = true;
                if (counted) ++r.landed;
                r.after_cast.push_back(hits[i].at - c.t);
                r.flight.push_back(hits[i].at - leaves);
                r.earliest_after_launch = std::min(r.earliest_after_launch, hits[i].at - leaves);
                break;
            }
        }
    }
    for (size_t i = 0; i < hits.size(); ++i) {
        if (hits[i].source_type != content::kSourceTypeProjectile) ++r.not_projectile;
        else if (!used[i]) ++r.stray;
    }
    return r;
}

void check_arsonist_tree_gates(TestHarness& h, const combat::NpcAbilityData& tables, const std::string& path,
                               const std::string& name, bool has_combo) {
    Fixture& f = h.f;
    std::map<std::string, std::pair<bool, std::string>> by_name;
    for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
        const abilities::AbilityDef* def = f.catalog->ability(t.ability);
        if (def && t.fight) by_name[def->row.name] = {!t.unknown && !t.off, t.why};
    }
    CHECKF(by_name["Bandit_Arsonist_Firebolt"].first, "the %s's tree can use the fire bolt", name.c_str());
    CHECKF(by_name["Bandit_Arsonist_FireboltCombo"].first == has_combo, "the %s's tree %s the fire bolt combo (%s)",
           name.c_str(), has_combo ? "switches on" : "switches off", by_name["Bandit_Arsonist_FireboltCombo"].second.c_str());
    for (const char* waits : {"Bandit_Arsonist_FlameBurst", "Bandit_Arsonist_FlameThrow", "Bandit_Arsonist_FlameRing"}) {
        CHECKF(by_name.count(waits) && !by_name[waits].first && by_name[waits].second.find("waits for") != std::string::npos,
               "%s stays off in the %s's tree (%s)", waits, name.c_str(), by_name[waits].second.c_str());
    }
}

void check_arsonist_shots_and_cadence(TestHarness& h, const combat::NpcAbilityData& tables, const char* leaf,
                                      const std::vector<LungeSample>& samples, const std::vector<combat::IncomingHit>& hits) {
    constexpr double kSeconds = 60.0;
    Fixture& f = h.f;
    const std::string name = spawns::blueprint_leaf(creature_path(f, leaf));
    const bool has_combo = std::string(leaf).find("Blister") != std::string::npos;
    const std::vector<ShotCast> casts = shot_casts(f, samples);
    std::map<std::string, std::vector<double>> by_name;
    for (const ShotCast& c : casts) by_name[c.name].push_back(c.t);
    std::string summary;
    for (const auto& [label, times] : by_name) summary += (summary.empty() ? "" : ", ") + label + " " + std::to_string(times.size());
    std::printf("  %s, %.0f s: %zu casts (%s), %zu hits\n", name.c_str(), kSeconds, casts.size(), summary.c_str(), hits.size());
    const auto count = [&](const char* label) { return by_name.count(label) ? by_name[label].size() : size_t{0}; };
    CHECKF(count("Bandit_Arsonist_Firebolt") >= 12, "it casts its fire bolt over and over (%zu)", count("Bandit_Arsonist_Firebolt"));
    if (has_combo) {
        CHECKF(count("Bandit_Arsonist_FireboltCombo") >= 2, "it casts its combo now and then (%zu)", count("Bandit_Arsonist_FireboltCombo"));
    } else {
        CHECKF(count("Bandit_Arsonist_FireboltCombo") == 0, "its tree switches the combo off (%zu)", count("Bandit_Arsonist_FireboltCombo"));
    }
    CHECKF(count("Bandit_Arsonist_FlameBurst") + count("Bandit_Arsonist_FlameThrow") + count("Bandit_Arsonist_FlameRing") == 0,
           "it never casts its spray, stream or ring of fire (%zu, %zu, %zu)",
           count("Bandit_Arsonist_FlameBurst"), count("Bandit_Arsonist_FlameThrow"), count("Bandit_Arsonist_FlameRing"));
    std::vector<double> gaps;
    for (size_t i = 1; i < casts.size(); ++i) gaps.push_back(casts[i].t - casts[i - 1].t);
    std::sort(gaps.begin(), gaps.end());
    if (!gaps.empty()) std::printf("  cast to cast: %zu gaps, %.2f s at least, %.2f s in the middle, %.2f s at most\n", gaps.size(), gaps.front(), gaps[gaps.size() / 2], gaps.back());
    CHECKF(gaps.size() >= 12 && gaps.front() >= 2.0 - 1e-6 && gaps.back() <= 2.4, "casts come 2.0 to 2.4 s apart (%.2f to %.2f s)",
           gaps.empty() ? 0.0 : gaps.front(), gaps.empty() ? 0.0 : gaps.back());
    const std::vector<double>& combos = by_name["Bandit_Arsonist_FireboltCombo"];
    if (!casts.empty() && !combos.empty()) {
        CHECKF(combos.front() - casts.front().t >= 1.7, "the first combo comes after the 2nd second of the fight (%.2f s after the first cast)", combos.front() - casts.front().t);
    }
    for (size_t i = 1; i < combos.size(); ++i) {
        CHECKF(combos[i] - combos[i - 1] >= 5.99, "two combos are at least 6 s apart (the cast's 1 s and the timer's 5 s: %.2f s)", combos[i] - combos[i - 1]);
    }
    const ShotReport report = shot_report(f, tables, casts, hits, 1.5);
    double single_min = 1e9, single_max = 0;
    for (const ShotCast& c : casts) {
        if (c.name != "Bandit_Arsonist_Firebolt") continue;
        const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
        if (!md || md->fires.size() != 1) continue;
        const double leaves = c.t + md->fires.front().time / md->rate_scale;
        for (const combat::IncomingHit& ih : hits) {
            if (ih.source_type == content::kSourceTypeProjectile && ih.at >= leaves && ih.at <= leaves + 1.5) {
                single_min = std::min(single_min, ih.at - c.t);
                single_max = std::max(single_max, ih.at - c.t);
                break;
            }
        }
    }
    std::printf("  %zu bolts due, %zu landed; a single bolt landed %.2f to %.2f s after its cast; %zu hits without a bolt, %zu hits not made by a projectile\n",
                report.due, report.landed, single_min, single_max, report.stray, report.not_projectile);
    CHECKF(report.due >= 20 && report.landed == report.due, "every bolt lands (%zu of %zu)", report.landed, report.due);
    CHECKF(report.stray == 0 && report.not_projectile == 0, "no hit comes without a bolt, and every hit names its projectile as its source (%zu, %zu)", report.stray, report.not_projectile);
    CHECKF(single_min >= 1.15 && single_max <= 1.45, "a bolt lands 1.15 to 1.45 s after its cast, with a cast 0.57 s long before it leaves and 20 m to fly (%.2f to %.2f s)", single_min, single_max);
    CHECKF(report.earliest_after_launch >= 0.4, "a bolt takes time to arrive (the quickest %.2f s after it left)", report.earliest_after_launch);
}

void run_arsonist_kind_case(TestHarness& h, const combat::NpcAbilityData& tables, const char* leaf, bool has_combo) {
    constexpr double kSeconds = 60.0;
    Fixture& f = h.f;
    const std::string path = creature_path(f, leaf);
    const auto bp = f.motion.find(path);
    const std::string name = spawns::blueprint_leaf(path);
    CHECKF(!path.empty() && bp && tables.abilities_of(path) != nullptr, "the Blueprint table has the %s and the creature tables list its abilities", name.c_str());
    if (path.empty() || !bp || !tables.abilities_of(path)) return;
    CHECKF(spawns::fighting_humanoid(path), "%s is a hostile humanoid whose fights are built", name.c_str());
    BanditOpen made;
    CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the %s (verdict: %s)", name.c_str(), spawns::verdict_name(made.why));
    if (!made.open) return;
    check_open_lists_all_tree_abilities(h, tables, path, made.open, false);
    check_arsonist_tree_gates(h, tables, path, name, has_combo);

    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    combat::CombatWorld::Stats stats;
    LungeFightSpec spec;
    spec.path = path;
    spec.seconds = kSeconds;
    spec.player_y = 2000.0;
    spec.hits = &hits;
    spec.provoke_at = 0.5;
    spec.stats = &stats;
    spec.leash_cm = 40000.0;
    spec.made = made.open;
    CHECKF(run_lunge_fight(f, tables, spec, samples), "a fight of %.0f s against a player who stands 20 m away runs", kSeconds);
    if (samples.empty()) return;
    check_targets_named_in_full(h, samples, name.c_str());
    check_arsonist_shots_and_cadence(h, tables, leaf, samples, hits);
    const LungeSample& last = samples.back();
    CHECKF(std::hypot(last.wx, last.wy) < 60.0, "it stays where it stood (%.0f cm from its place)", std::hypot(last.wx, last.wy));
}

void check_arsonist_closing_from_33m(TestHarness& h, const combat::NpcAbilityData& tables) {
    Fixture& f = h.f;
    const std::string path = creature_path(f, "/Bandit_Arsonist/Bandit_Arsonist.Default__Bandit_Arsonist_C");
    const auto bp = f.motion.find(path);
    BanditOpen made;
    if (path.empty() || !bp || !make_bandit_open(f, tables, path, made)) return;
    std::vector<LungeSample> samples;
    LungeFightSpec spec;
    spec.path = path;
    spec.seconds = 25.0;
    spec.player_y = 3300.0;
    spec.provoke_at = 0.5;
    spec.leash_cm = 40000.0;
    spec.made = made.open;
    CHECKF(run_lunge_fight(f, tables, spec, samples), "a fight against a player 33 m away runs");
    double first_gap = -1, second_gap = -1, nearest = 1e9;
    for (const LungeSample& s : samples) {
        const double gap = std::hypot(s.wx - s.px, s.wy - s.py) - bp->radius - 34.0;
        nearest = std::min(nearest, gap);
        if (!s.cast_began) continue;
        if (first_gap < 0) first_gap = gap;
        else if (second_gap < 0) second_gap = gap;
    }
    std::printf("  from 33 m: its first cast began %.0f cm from the player's surface, its second %.0f cm, it came as near as %.0f cm\n", first_gap, second_gap, nearest);
    CHECKF(first_gap >= 2800.0 && first_gap <= 3005.0, "it begins casting once the player is within 30 m, the reach of its bolt (%.0f cm)", first_gap);
    CHECKF(second_gap >= 2400.0 && second_gap <= 2560.0, "between its first two casts it closes in to 25 m, its tree's arrival distance (%.0f cm)", second_gap);
    CHECKF(nearest >= 2400.0, "it does not come nearer than that (%.0f cm)", nearest);
}

void test_arsonist_fires_at_a_standing_player(TestHarness& h) {
    std::printf("a bandit arsonist against a player who stands 20 m away: it casts its fire bolt about every 2 s and its combo now and then, each bolt leaves at its montage's notify and lands "
                "when it arrives, no hit comes without a bolt, and its spray, stream and ring of fire stay off\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(h, tables)) return;
    run_arsonist_kind_case(h, tables, "/Bandit_Arsonist/Bandit_Arsonist.Default__Bandit_Arsonist_C", false);
    run_arsonist_kind_case(h, tables, "/Bandit_Arsonist/CSB_Arsonist_StateTree.Default__CSB_Arsonist_StateTree_C", false);
    run_arsonist_kind_case(h, tables, "/Bandit_Arsonist/Bandit_Arsonist_Blister.Default__Bandit_Arsonist_Blister_C", true);
    check_arsonist_closing_from_33m(h, tables);
}

void test_plain_goblin_shaman_throws_its_bomb(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a plain goblin shaman against a player 2.5 m away: its oozing bomb leaves at its montage's notify, flies, and lands its damage when it arrives (it was an instant hit)\n");
    constexpr double kSeconds = 30.0;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(h, tables)) return;
    const std::string path = creature_path(f, "/GoblinShaman/Goblin_Shaman.Default__Goblin_Shaman_C");
    const auto bp = f.motion.find(path);
    CHECKF(!path.empty() && bp && tables.abilities_of(path) != nullptr, "the Blueprint table has the plain goblin shaman and the creature tables list its abilities");
    if (path.empty() || !bp || !tables.abilities_of(path)) return;
    BanditOpen made;
    CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the shaman (verdict: %s)", spawns::verdict_name(made.why));
    if (!made.open) return;
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    LungeFightSpec spec;
    spec.path = path;
    spec.seconds = kSeconds;
    spec.player_y = 250.0;
    spec.hits = &hits;
    spec.provoke_at = 0.5;
    spec.leash_cm = 40000.0;
    spec.made = made.open;
    CHECKF(run_lunge_fight(f, tables, spec, samples), "a fight of %.0f s against a player 2.5 m away runs", kSeconds);
    if (samples.empty()) return;
    const std::vector<ShotCast> casts = shot_casts(f, samples);
    size_t bombs = 0;
    for (const ShotCast& c : casts) bombs += c.name == "GoblinShaman_OozingBomb" ? 1 : 0;
    const ShotReport report = shot_report(f, tables, casts, hits, 1.0);
    std::printf("  %zu casts, %zu of them the bomb; %zu bombs due, %zu landed, the quickest %.2f s after it left; %zu hits without a bomb\n",
                casts.size(), bombs, report.due, report.landed, report.earliest_after_launch, report.stray);
    CHECKF(bombs >= 3, "it throws its bomb over and over (%zu)", bombs);
    CHECKF(report.due >= 3 && report.landed == report.due, "every bomb lands (%zu of %zu)", report.landed, report.due);
    size_t other_hits = 0;
    for (const combat::IncomingHit& ih : hits) {
        if (ih.source_type == content::kSourceTypeProjectile) continue;
        const abilities::AbilityDef* def = f.catalog->ability(ih.source_guid);
        if (!def || def->row.name.find("Bludgeon") == std::string::npos) ++other_hits;
    }
    CHECKF(report.stray == 0 && other_hits == 0, "no hit comes without a bomb but the Bludgeon's swings (%zu hits of a projectile that never left, %zu of another ability)",
           report.stray, other_hits);
    double earliest_after_cast = 1e9;
    for (double t : report.after_cast) earliest_after_cast = std::min(earliest_after_cast, t);
    CHECKF(earliest_after_cast >= 1.8, "no damage comes before the bomb has left the shaman's hand (%.2f s after the cast)", earliest_after_cast);
}

const talents::Node* finisher_node_of(const talents::Tree& tree, const char* skill) {
    for (const talents::Node& n : tree.nodes) {
        if (n.skill_name == skill && !n.ranks.empty()) return &n;
    }
    return nullptr;
}

bool marker_holds(const std::vector<stats::SkillRank>& held, uint64_t type, int64_t guid) {
    for (const stats::SkillRank& s : held) {
        if (s.cls == stats::kWeaponTreeRef && s.skill.type_id == type && s.skill.guid == guid) return true;
    }
    return false;
}

void check_weapon_book_marker(TestHarness& h, Rig& rig, const talents::Tree& wand, const talents::Tree& axe) {
    const talents::Node* burning = finisher_node_of(wand, "Weapon_Finisher_Burning");
    rig.book->load(wand.guid, {burning->ranks.front().guid});
    const auto with_wand = rig.book->held_ranks(wand.weapon_line);
    CHECK(marker_holds(with_wand, stats::kRefTypeSkill, burning->skill) && marker_holds(with_wand, stats::kRefTypeSkillRank, burning->ranks.front().guid),
          "the wand in hand: the Burning pick is held under the weapon-in-hand marker, as its skill and as its rank");
    CHECK(!marker_holds(rig.book->held_ranks(axe.weapon_line), stats::kRefTypeSkill, burning->skill) &&
              !marker_holds(rig.book->held_ranks(0), stats::kRefTypeSkill, burning->skill) &&
              !marker_holds(rig.book->held_ranks(-1), stats::kRefTypeSkill, burning->skill),
          "...not with the axe in hand, with none in hand (0), nor with a weapon line that no weapon tree has (-1)");
    size_t under_marker = 0, in_wand_tree = 0;
    bool in_tree = false;
    for (const stats::SkillRank& s : with_wand) {
        under_marker += s.cls == stats::kWeaponTreeRef ? 1 : 0;
        in_wand_tree += s.cls.type_id == stats::kRefTypeSkillTree && s.cls.guid == wand.guid ? 1 : 0;
        if (s.cls.type_id == stats::kRefTypeSkillTree && s.cls.guid == wand.guid && s.skill.type_id == stats::kRefTypeSkill && s.skill.guid == burning->skill) {
            in_tree = true;
        }
    }
    CHECKF(under_marker == in_wand_tree && under_marker > 0, "the marker holds exactly what the wand's tree holds (%zu of %zu)", under_marker, in_wand_tree);
    CHECK(in_tree, "(the pick is still held under its own tree, as before)");
}

int64_t finisher_targeting_of(const abilities::AbilityDef& def, const abilities::HitDef& hit) {
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& h : def.hits) {
        if (h.hit_guid != hit.row.guid || h.source != "hit") continue;
        for (const content::TargetingRow& tg : def.targetings) {
            if (tg.key == h.key) targeting = tg.guid;
        }
    }
    return targeting;
}

struct FinisherOut {
    int    reacted = 0;
    double value = 0;
    int    flags = -1;
};

void tally_finisher_confirm(const bootstrap::HitConfirm& c, int64_t hit_guid, int64_t react_guid, int& landed, FinisherOut& out) {
    if (c.record_guid == hit_guid && c.source_type == content::kSourceTypeAbility) {
        ++landed;
        if (!c.targets.empty()) {
            out.flags = c.targets[0].flags;
            out.value = c.targets[0].mods.empty() ? 0.0 : c.targets[0].mods[0].value;
        }
    }
    if (c.record_guid == react_guid && c.source_type == content::kSourceTypeReaction) ++out.reacted;
}

struct FinisherBench {
    TestHarness&                 h;
    Rig&                         rig;
    uint64_t                     wolf = 0;
    double                       t = 100.0;
    int32_t                      id = 500;
    const talents::Tree*         wand = nullptr;
    const talents::Tree*         axe = nullptr;
    const talents::Node*         burning = nullptr;
    const talents::Node*         bleed = nullptr;
    const abilities::AbilityDef* wand_combo = nullptr;
    const abilities::AbilityDef* axe_combo = nullptr;
    const abilities::HitDef*     beam_end = nullptr;
    const abilities::HitDef*     axe_finisher = nullptr;
    const abilities::HitDef*     burning_hit = nullptr;
    const abilities::HitDef*     bleeding_hit = nullptr;

    bool ready() const {
        if (wolf == 0 || !wand_combo || !axe_combo) return false;
        return beam_end && axe_finisher && burning_hit && bleeding_hit;
    }

    void hold(int64_t ranks_line, int64_t carriers_line) const {
        rig.cs->set_passives(rig.book->passives(carriers_line));
        rig.cs->set_ranks(rig.book->held_ranks(ranks_line));
        rig.pipe->set_reaction_carriers(rig.book->reaction_carriers(carriers_line));
        rig.pipe->refresh(rig.clock, "the weapon in hand", true);
        rig.take();
    }

    FinisherOut finish(const abilities::AbilityDef& combo, int anim, const abilities::HitDef& hit, const abilities::HitDef& reaction_hit, bool check = true) {
        FinisherOut out;
        t += 5.0;
        if (anim == 4 && combo.row.name == "WeaponCombo_Ranged_Wand") {
            for (int before = 0; before < 4; ++before, t += 0.6) {
                TriggerInfo lead;
                lead.animation_index = before;
                rig.pipe->trigger(combo.row.guid, id++, t, lead);
                rig.run_to(t + 0.5);
            }
        }
        TriggerInfo info;
        info.animation_index = anim;
        info.target_count = 1;
        info.target_actors = {wolf};
        const Refusal refusal = rig.pipe->trigger(combo.row.guid, id++, t, info);
        if (check) CHECKF(refusal == Refusal::None, "%s is accepted (%d)", combo.row.name.c_str(), static_cast<int>(refusal));
        rig.run_to(t + 0.4);
        const auto verdict = rig.pipe->hit_request(combo.row.guid, finisher_targeting_of(combo, hit), hit.row.guid, 1, 0, t + 0.8, {wolf});
        if (check) CHECKF(verdict == AbilityPipeline::HitVerdict::Scheduled, "the request for %s is scheduled", hit.row.name.c_str());
        int landed = 0;
        for (const Seen& s : rig.run_to(t + 1.6)) {
            for (const bootstrap::HitConfirm& c : s.confirmations) {
                tally_finisher_confirm(c, hit.row.guid, reaction_hit.row.guid, landed, out);
            }
        }
        if (check) CHECKF(landed == 1, "%s landed once (%d)", hit.row.name.c_str(), landed);
        return out;
    }
};

void check_wand_and_axe_finisher_reactions(FinisherBench& b) {
    TestHarness& h = b.h;
    Rig& rig = b.rig;
    b.hold(b.wand->weapon_line, b.wand->weapon_line);
    const size_t fired0 = rig.pipe->reactions_fired();
    int burns = 0;
    for (int i = 0; i < 3; ++i) burns += b.finish(*b.wand_combo, 4, *b.beam_end, *b.burning_hit).reacted;
    CHECKF(burns == 0 && rig.pipe->reactions_fired() == fired0 + 3,
           "the wand in hand with the Burning pick: Weapon_Finisher_Burning fires at each of 3 beam ends; its hit only raises the burn, so none is confirmed (%d confirmed, %zu reactions)",
           burns, rig.pipe->reactions_fired() - fired0);

    b.hold(b.axe->weapon_line, b.wand->weapon_line);
    const size_t fired1 = rig.pipe->reactions_fired();
    const int after_swap = b.finish(*b.wand_combo, 4, *b.beam_end, *b.burning_hit).reacted;
    CHECKF(after_swap == 0 && rig.pipe->reactions_fired() == fired1, "...the same beam end with the axe's line in hand: nothing fires (%d)", after_swap);
    b.hold(0, b.wand->weapon_line);
    const size_t fired2 = rig.pipe->reactions_fired();
    const int none_in_hand = b.finish(*b.wand_combo, 4, *b.beam_end, *b.burning_hit).reacted;
    CHECKF(none_in_hand == 0 && rig.pipe->reactions_fired() == fired2, "...and with no weapon in hand: nothing fires (%d)", none_in_hand);
    b.hold(b.wand->weapon_line, b.wand->weapon_line);
    const size_t fired3 = rig.pipe->reactions_fired();
    CHECK(b.finish(*b.wand_combo, 4, *b.beam_end, *b.burning_hit).reacted == 0 && rig.pipe->reactions_fired() == fired3 + 1, "...back to the wand: it fires again");

    rig.book->load(b.wand->guid, {});
    b.hold(b.wand->weapon_line, b.wand->weapon_line);
    const size_t fired4 = rig.pipe->reactions_fired();
    const int no_pick = b.finish(*b.wand_combo, 4, *b.beam_end, *b.burning_hit).reacted;
    CHECKF(no_pick == 0 && rig.pipe->reactions_fired() == fired4, "without the pick: nothing fires (%d)", no_pick);

    rig.book->load(b.axe->guid, {b.bleed->ranks.front().guid});
    b.hold(b.axe->weapon_line, b.axe->weapon_line);
    const size_t fired5 = rig.pipe->reactions_fired();
    const FinisherOut plain = b.finish(*b.axe_combo, 0, *b.axe_finisher, *b.bleeding_hit);
    const size_t with_pick = rig.pipe->reactions_fired() - fired5;
    CHECKF(plain.reacted == 0 && with_pick >= 1,
           "the axe in hand with the Bleed pick: Weapon_Finisher_Bleeding fires at the proc'd finishing hit; its hit only applies the bleed, so it is not confirmed (%d confirmed, %zu reactions)",
           plain.reacted, with_pick);
    b.hold(b.wand->weapon_line, b.axe->weapon_line);
    const size_t fired6 = rig.pipe->reactions_fired();
    const int bleeds_wand = b.finish(*b.axe_combo, 0, *b.axe_finisher, *b.bleeding_hit).reacted;
    const size_t without = rig.pipe->reactions_fired() - fired6;
    CHECKF(bleeds_wand == 0 && with_pick == without + 1,
           "...but not with the wand's line in hand (%d confirmed; %zu reactions against %zu: the Fighter's own reactions fire either way)",
           bleeds_wand, without, with_pick);
}

FinisherOut plain_axe_finisher_hit(FinisherBench& b, int64_t ranks_line, int64_t carriers_line) {
    TestHarness& h = b.h;
    b.hold(ranks_line, carriers_line);
    FinisherOut r;
    for (int tries = 0; tries < 12; ++tries) {
        r = b.finish(*b.axe_combo, 0, *b.axe_finisher, *b.bleeding_hit, false);
        if (r.flags == 0 && r.value != 0.0) break;
    }
    CHECKF(r.flags == 0 && r.value != 0.0, "a hit that was not missed, blocked or critical within 12 tries (flags %d)", r.flags);
    return r;
}

void check_axe_finisher_damage_talents(FinisherBench& b) {
    TestHarness& h = b.h;
    Rig& rig = b.rig;
    const FinisherOut base = plain_axe_finisher_hit(b, b.axe->weapon_line, b.axe->weapon_line);
    std::vector<int64_t> bought = {b.bleed->ranks.front().guid};
    for (const char* skill : {"Weapon_Passive_DamageFocus_1", "Weapon_Passive_DamageFocus_2", "Weapon_Passive_DamageFocus_3",
                              "Weapon_Passive_Finisher_Damage_1", "Weapon_Passive_Finisher_Damage_2", "Weapon_Passive_Finisher_Damage_3"}) {
        const talents::Node* n = finisher_node_of(*b.axe, skill);
        if (n) bought.push_back(n->ranks.front().guid);
    }
    CHECKF(bought.size() == 7, "the axe tree has the six damage talents (%zu bought)", bought.size());
    rig.book->load(b.axe->guid, bought);
    const FinisherOut boosted = plain_axe_finisher_hit(b, b.axe->weapon_line, b.axe->weapon_line);
    CHECKF(base.value != 0.0 && boosted.value != 0.0 && near(boosted.value / base.value, 1.12, 1e-3),
           "the axe's finishing hit with the six damage talents deals 12 %% more (%.3f over %.3f = %.4f)",
           boosted.value, base.value, base.value != 0.0 ? boosted.value / base.value : 0.0);
    const FinisherOut elsewhere = plain_axe_finisher_hit(b, b.wand->weapon_line, b.axe->weapon_line);
    CHECKF(elsewhere.value != 0.0 && near(elsewhere.value / base.value, 1.0, 1e-3),
           "...and the same as without them while the wand's line is in hand (%.4f)", base.value != 0.0 ? elsewhere.value / base.value : 0.0);
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}

void test_weapon_finisher_picks(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the weapon talents see the weapon in hand: a finisher pick fires its reaction at that weapon's finishing hit\n");
    Rig rig(4, 25, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    const talents::Tree* wand = rig.talent_catalog->tree_named("Weapon_Ranged_Wand");
    const talents::Tree* axe = rig.talent_catalog->tree_named("Weapon_1H_Axe");
    CHECK(wand && axe && wand->weapon_line != 0 && axe->weapon_line != 0 && wand->weapon_line != axe->weapon_line,
          "the wand's and the 1H axe's weapon trees, each with a line of its own");
    if (!wand || !axe) return;
    const talents::Node* burning = finisher_node_of(*wand, "Weapon_Finisher_Burning");
    const talents::Node* bleed = finisher_node_of(*axe, "Weapon_Finisher_Bleed");
    CHECK(burning && bleed, "the wand's Burning pick and the axe's Bleed pick");
    if (!burning || !bleed) return;
    check_weapon_book_marker(h, rig, *wand, *axe);

    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 600, 0, 8, 100);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    rig.know({"WeaponCombo_Ranged_Wand", "WeaponCombo_Melee_1H_Axe"});
    FinisherBench b{h, rig, wolf, 100.0, 500, wand, axe, burning, bleed,
                    f.catalog->ability_named("WeaponCombo_Ranged_Wand"),
                    f.catalog->ability_named("WeaponCombo_Melee_1H_Axe"),
                    f.catalog->hit_named("Weapon_Wand_Beam_ComboEnd_Hit"),
                    f.catalog->hit_named("WeaponCombo_Axe_1H_Hit4+"),
                    f.catalog->hit_named("Weapon_Finisher_Burning"),
                    f.catalog->hit_named("Weapon_Finisher_Bleeding")};
    CHECK(b.ready(), "a Wolf, the two combos, their finishing hits and the two reactions' hits");
    if (!b.ready()) return;
    check_wand_and_axe_finisher_reactions(b);
    check_axe_finisher_damage_talents(b);
}

}  // namespace

void run_combat_projectile_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db) {
    (void)db;
    test_weapon_finisher_picks(h);
    test_bandit_raider_fights_a_standing_player(h);
    test_arsonist_fires_at_a_standing_player(h);
    test_plain_goblin_shaman_throws_its_bomb(h);
}

}  // namespace sandal::worldd::tests
