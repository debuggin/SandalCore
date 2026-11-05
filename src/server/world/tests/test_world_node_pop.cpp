// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.

#include "test_world_node_pop_common.h"

using namespace aoc::worldd;
using namespace aoc::worldd::test_node_pop;

namespace {

static const char *why_id(Why w) {
    struct Entry {
        Why w;
        const char *name;
    };
    static constexpr Entry kTable[] = {
        {Why::Spawns, "Spawns"},
        {Why::DevOnly, "DevOnly"},
        {Why::NotWorldMap, "NotWorldMap"},
        {Why::Blacklisted, "Blacklisted"},
        {Why::Gated, "Gated"},
        {Why::NodeLevel, "NodeLevel"},
        {Why::Culture, "Culture"},
        {Why::NodeOrZone, "NodeOrZone"},
        {Why::NodeType, "NodeType"},
        {Why::Biome, "Biome"},
        {Why::Season, "Season"},
        {Why::Weather, "Weather"},
        {Why::TimeOfDay, "TimeOfDay"},
        {Why::RequiredTag, "RequiredTag"},
        {Why::Prop, "Prop"},
        {Why::Hidden, "Hidden"},
        {Why::Placeholder, "Placeholder"},
        {Why::NoMesh, "NoMesh"},
        {Why::NoAnimation, "NoAnimation"},
        {Why::Appearance, "Appearance"},
    };
    for (const Entry &e : kTable) {
        if (e.w == w) return e.name;
    }
    return "?";
}
static uint64_t digest_of(const std::vector<spawns::NodeSpawn> &list) {
    uint64_t total = 0;
    for (const spawns::NodeSpawn &n : list) {
        const spawns::DataSpawn &s = n.spawn;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%lld,%lld,%d,%s", static_cast<long long>(std::nearbyint(s.x)),
                      static_cast<long long>(std::nearbyint(s.y)), s.level,
                      spawns::archetype_leaf(s.asset->archetype_path).c_str());
        uint64_t h = 0xCBF29CE484222325ull;
        for (const char *p = buf; *p; ++p) h = (h ^ static_cast<unsigned char>(*p)) * 0x100000001B3ull;
        total += h;
    }
    return total;
}
static std::string join_counts(const std::map<std::string, size_t> &m) {
    std::string out;
    for (const auto &[k, v] : m) {
        if (v == 0) continue;
        out += (out.empty() ? "" : ";") + k + ":" + std::to_string(v);
    }
    return out;
}
static std::string diff_golden_case(const tests::NodePopCase &c, const spawns::NodePopStats &st,
                                    const std::vector<spawns::NodeSpawn> &list) {
    std::map<std::string, size_t> filtered, blueprints;
    for (const auto &[why, n] : st.pop.filtered) filtered[why_id(why)] += n;
    for (const spawns::NodeSpawn &n : list) ++blueprints[spawns::archetype_leaf(n.spawn.asset->archetype_path)];
    std::string diff;
    const auto cmp = [&](const char *name, uint64_t got, uint64_t want) {
        if (got != want)
            diff += std::string(" ") + name + "=" + std::to_string(got) + " (golden " + std::to_string(want) + ")";
    };
    cmp("sets_layout", st.sets[0], c.sets_layout);
    cmp("sets_building", st.sets[1], c.sets_building);
    cmp("points_layout", st.points[0], c.points_layout);
    cmp("points_building", st.points[1], c.points_building);
    cmp("spawned_layout", st.spawned[0], c.spawned_layout);
    cmp("spawned_building", st.spawned[1], c.spawned_building);
    cmp("no_template", st.pop.no_template, c.no_template);
    cmp("no_asset", st.pop.no_asset, c.no_asset);
    cmp("outside", st.outside, c.outside);
    if (join_counts(filtered) != c.filtered)
        diff += " filtered=[" + join_counts(filtered) + "] (golden [" + c.filtered + "])";
    if (join_counts(blueprints) != c.blueprints) diff += " blueprints differ";
    const uint64_t d = digest_of(list);
    if (d != c.digest) diff += " digest " + std::to_string(d) + " (golden " + std::to_string(c.digest) + ")";
    return diff;
}
struct WalkerCount {
    size_t total = 0, patrol = 0, wander = 0, stand = 0, guards_patrol = 0, service_walking = 0, patrol_unplaced = 0;
    size_t stationary_routed = 0, villager_patrol = 0, carriers_walk = 0, workers_walk = 0, farmers_stand = 0;
    size_t services = 0, crate_vendors = 0, crate_vendors_walking = 0, building_wanderers = 0, workers_off_plot = 0,
           workers_far = 0;
};
struct WalkerSpawnArgs {
    const world::WorldActor &actor;
    const content::NpcInstanceRow *inst = nullptr;
    const content::NpcRouteRow *route = nullptr;
    const world::CityNodeInfo &node;
    int64_t plot = 0;
};
static bool is_service_leaf(const content::NpcInstanceRow *inst, const std::string &leaf) {
    static const char *const kServiceWords[] = {"Vendor",          "Clerk",     "Manager", "_KAE",
                                                "Salesman",        "Caravan",   "Station", "Board",
                                                "ServiceBuilding", "TurnInBox", "Mailbox", "ServiceDesk"};
    if (inst && inst->vendor != 0) return true;
    for (const char *w : kServiceWords) {
        if (leaf.find(w) != std::string::npos) return true;
    }
    return false;
}
static void record_workshop_wanderer(WalkerCount &c, const world::WorldActor &a, const npc::NpcMotionSetup &s,
                                     const world::CityNodeInfo &node, int64_t plot) {
    double ox = node.x, oy = node.y;
    bool found = false;
    const double yaw = node.yaw * 3.14159265358979323846 / 180.0;
    const int lvl = g_env.shipped_nodes->level_of(node.guid);
    for (const world::PlotRow &r : g_env.shipped_wire->plot_rows_of(node.guid, lvl)) {
        if (r.plot != plot) continue;
        ox = node.x + r.tx * std::cos(yaw) - r.ty * std::sin(yaw);
        oy = node.y + r.tx * std::sin(yaw) + r.ty * std::cos(yaw);
        found = true;
    }
    ++c.building_wanderers;
    if (!found || std::hypot(a.x - ox, a.y - oy) > 3000.0) ++c.workers_off_plot;
    if (s.wander_max > npc::kWorkshopRadiusCm) ++c.workers_far;
}
static void record_workshop_role(WalkerCount &c, const WalkerSpawnArgs &args, const npc::NpcMotionSetup &s,
                                 const std::string &leaf) {
    if (args.plot != 0 && s.ambient == npc::Ambient::Wander)
        record_workshop_wanderer(c, args.actor, s, args.node, args.plot);
    if (!args.inst || args.inst->name.rfind("Node_Filler_Villager", 0) != 0) return;
    if (s.ambient == npc::Ambient::Wander && leaf.find("Shovel") == std::string::npos) ++c.workers_walk;
    if (s.ambient == npc::Ambient::Stationary && leaf.find("Shovel") != std::string::npos) ++c.farmers_stand;
}
static void record_walker_service(WalkerCount &c, const WalkerSpawnArgs &args, const npc::NpcMotionSetup &s,
                                  const std::string &leaf) {
    if (is_service_leaf(args.inst, leaf)) {
        ++c.services;
        if (s.ambient != npc::Ambient::Stationary) ++c.service_walking;
    }
    if (leaf == "ServiceBuilding_Caravansary_CrateSalesman") {
        ++c.crate_vendors;
        c.crate_vendors_walking += s.ambient != npc::Ambient::Stationary;
    }
    record_workshop_role(c, args, s, leaf);
}
static void record_walker_patrol(WalkerCount &c, const world::CityNodeInfo &node, const npc::NpcMotionSetup &s,
                                 const std::string &leaf) {
    ++c.patrol;
    (npc::is_guard_leaf(leaf) ? c.guards_patrol : c.villager_patrol)++;
    if (leaf.find("Carrying") != std::string::npos) ++c.carriers_walk;
    if (std::hypot(s.route.front().x - node.x, s.route.front().y - node.y) > 60000.0) ++c.patrol_unplaced;
}
static void record_walker_spawn(WalkerCount &c, const WalkerSpawnArgs &args) {
    const npc::SpeedModel speeds;
    npc::NpcMotionSetup s =
        npc::make_setup(args.actor, g_env.table, args.inst, args.route, speeds, nullptr, &args.node);
    if (args.plot != 0) npc::keep_to_workshop(&s, npc::kWorkshopRadiusCm);
    const std::string leaf = spawns::blueprint_leaf(args.actor.npc.archetype);
    ++c.total;
    record_walker_service(c, args, s, leaf);
    if (s.ambient == npc::Ambient::Patrol && s.route.size() >= 2) {
        record_walker_patrol(c, args.node, s, leaf);
    } else if (s.ambient == npc::Ambient::Stationary) {
        ++c.stand;
        if (!s.route.empty()) ++c.stationary_routed;
    } else {
        ++c.wander;
    }
    if (npc::stands_by_kind(leaf) && s.ambient != npc::Ambient::Stationary) ++c.service_walking;
}

static void test_filter_tags_and_zones(const content::PopFilter &open, const std::set<std::string> &none,
                                       const spawns::SpawnContext &ctx, const spawns::SpawnFilter &f) {
    content::PopFilter pf = open;
    pf.required_tags = {"War.Siege.States.SiegeDeclared", "War.Siege.States.SiegeInactive",
                        "War.Siege.States.SiegePrep"};
    CHECK(f.filter(pf, none) == Why::Spawns, "a filter that requires Declared, Inactive and Prep passes in a world "
                                             "that carries SiegeInactive (any-of; E1's all-of never passed)");
    pf.required_tags = {"War.Siege.States.SiegeActive"};
    CHECK(f.filter(pf, none) == Why::RequiredTag, "a filter that requires Active does not");
    pf.required_tags = {"War.Siege.States.SiegeDeclared", "War.Siege.States.SiegePrep"};
    CHECK(f.filter(pf, none) == Why::RequiredTag, "...nor one that requires only states the world does not carry");
    pf.required_tags = {"War.Siege.States.SiegeInactive", "npc.stature.rareSpawn"};
    CHECK(f.filter(pf, none) == Why::RequiredTag, "every other required tag is still all-of: the rare spawn tag is "
                                                  "missing");
    CHECK(f.filter(pf, {"npc.stature.rareSpawn"}) == Why::Spawns, "...and the spawner's own tags count");
    pf.required_tags = {"War.Siege.States.SiegeDeclared", "War.Siege.States.SiegeInactive", "npc.stature.rareSpawn"};
    CHECK(f.filter(pf, {"npc.stature.rareSpawn"}) == Why::Spawns, "siege states any-of and the other tag present");
    spawns::SpawnContext war = ctx;
    war.world_tags = {"War.Siege.States.SiegeActive"};
    pf.required_tags = {"War.Siege.States.SiegeDeclared", "War.Siege.States.SiegeInactive",
                        "War.Siege.States.SiegePrep"};
    CHECK(spawns::SpawnFilter(war, nullptr).filter(pf, none) == Why::RequiredTag,
          "a world at war (SiegeActive) does not spawn the idle-state filters");
    pf.required_tags = {"War.Siege.States.SiegeActive"};
    CHECK(spawns::SpawnFilter(war, nullptr).filter(pf, none) == Why::Spawns,
          "...and spawns the ones that require Active");

    content::PopFilter zf = open;
    zf.zones = {42};
    CHECK(f.filter(zf, none, 0) == Why::NodeOrZone,
          "a filter that names a zone never passes for a point with none (E1's "
          "rule, the area's circle)");
    CHECK(f.filter(zf, none, 42) == Why::Spawns && f.filter(zf, none, 43) == Why::NodeOrZone,
          "...a point in the zone passes, in another zone does not");
    spawns::SpawnContext in42 = ctx;
    in42.zone = 42;
    spawns::SpawnFilter g(in42, nullptr);
    CHECK(g.filter(zf, none, 0) == Why::Spawns, "a context that stands in a zone (a node's) answers for a point with "
                                                "none: the zone refusal is dropped");
    CHECK(g.filter(zf, none, 43) == Why::NodeOrZone, "...but a point's own zone wins");
}

static const content::NpcSetRow *find_layout_gated_set() {
    for (const auto &[guid, sources] : g_env.pop->gates) {
        if (sources.size() != 1 || sources[0] != spawns::kLayoutGate) continue;
        const auto set = g_env.pop->sets.find(guid);
        if (set == g_env.pop->sets.end() || set->second.blacklisted || set->second.dev_only ||
            set->second.members.empty()) {
            continue;
        }
        const auto inst = g_env.pop->instances.find(set->second.members.front().instance);
        if (inst == g_env.pop->instances.end() || inst->second.blacklisted || inst->second.dev_only) continue;
        return &set->second;
    }
    return nullptr;
}

static void test_filter() {
    std::printf("the spawn filter: the siege tags are any-of, the context's "
                "zone, the sources\n");
    const content::PopFilter open{};
    const std::set<std::string> none;
    spawns::SpawnContext ctx;
    spawns::SpawnFilter f(ctx, nullptr);
    test_filter_tags_and_zones(open, none, ctx, f);

    const content::NpcSetRow *gated = find_layout_gated_set();
    CHECK(gated != nullptr, "the data has sets that only the Layout source spawns");
    if (gated) {
        const content::NpcInstanceRow &inst = g_env.pop->instances.at(gated->members.front().instance);
        CHECK(f.point(*g_env.pop, nullptr, *gated, inst) == Why::Gated,
              "...the world does not spawn one (no layout source runs)");
        spawns::SpawnContext layout = ctx;
        layout.active_gates = {spawns::kLayoutGate};
        CHECK(spawns::SpawnFilter(layout, nullptr).point(*g_env.pop, nullptr, *gated, inst) == Why::Spawns,
              "...a node's context does");
        spawns::SpawnContext building = ctx;
        building.active_gates = {spawns::kBuildingGate};
        CHECK(spawns::SpawnFilter(building, nullptr).point(*g_env.pop, nullptr, *gated, inst) == Why::Gated,
              "...and the building source does not run a layout's set");
    }
    const bool names_ok =
        std::string(spawns::node_level_name(4)) == "Town" && std::string(spawns::node_type_name(1)) == "Scientific" &&
        std::string(spawns::biome_name(14)) == "Riverlands" && std::string(spawns::node_level_name(9)).empty() &&
        std::string(spawns::biome_name(-1)).empty();
    CHECK(names_ok, "ENodeLevel, ENodeType and EBiomeType by number");
    const spawns::NodeSite site = spawns::node_site_of(*g_env.nodes, *g_env.wire, *g_env.nodes->by_guid(kJoeva), 4);
    const spawns::SpawnContext jc = spawns::node_spawn_context(g_env.base, site, {"Node_Villager_F"});
    const bool jc_ok = jc.node_level == "Town" && jc.node_id == kJoeva && jc.node_type == "Scientific" &&
                       jc.biome == "Riverlands" && jc.zone == site.zoi && jc.zone != 0 &&
                       jc.active_gates.count(spawns::kLayoutGate) && jc.active_gates.count(spawns::kBuildingGate) &&
                       jc.shown_humanoids.count("Node_Villager_F") && jc.shown_humanoids.count("Guard_Soldier");
    CHECK(jc_ok, "Joeva's context: Town, its id, Scientific, the Riverlands, its "
                 "zone, the two sources, the shown "
                 "humanoids added to the base's");
}

static void match_plot_building_frames(const world::CityNodeInfo &joeva, const world::PlotRow &row,
                                       const std::vector<spawns::PlacedSet> &placed, bool &checked,
                                       bool &building_frame) {
    const double kPi = 3.14159265358979323846;
    const double yaw = joeva.yaw * kPi / 180.0;
    const double ex = joeva.x + row.tx * std::cos(yaw) - row.ty * std::sin(yaw);
    const double ey = joeva.y + row.tx * std::sin(yaw) + row.ty * std::cos(yaw);
    const double eyaw = joeva.yaw + spawns::quaternion_yaw(row.qx, row.qy, row.qz, row.qw);
    for (const spawns::PlacedSet &p : placed) {
        if (p.plot != row.plot) continue;
        building_frame = building_frame && std::fabs(p.x - ex) < 1e-6 && std::fabs(p.y - ey) < 1e-6 &&
                         std::fabs(p.z - (joeva.z + row.tz)) < 1e-6 && std::fabs(p.yaw - eyaw) < 1e-9;
        checked = true;
    }
}

static void check_building_frames(const world::CityNodeInfo &joeva, int64_t layout,
                                  const std::vector<spawns::PlacedSet> &placed) {
    bool checked = false, building_frame = true;
    for (const world::PlotRow &row : g_env.wire->plot_rows(layout, 4)) {
        if (row.building == 0 || (row.state != 1 && row.state != 2)) continue;
        const int64_t spawn = g_env.data->spawn_for(row.building, 4);
        if (spawn == 0 || g_env.data->spawn_sets(spawn, row.state).empty()) continue;
        match_plot_building_frames(joeva, row, placed, checked, building_frame);
    }
    CHECK(checked && building_frame, "a building's sets are placed at the plot's translation rotated by the "
                                     "node's yaw, at the node's height plus "
                                     "the plot's, with the plot's yaw added to the node's");
}

static size_t expected_building_sets(int64_t layout) {
    size_t total = 0;
    for (const world::PlotRow &row : g_env.wire->plot_rows(layout, 4)) {
        if (row.building == 0 || (row.state != 1 && row.state != 2)) continue;
        const int64_t spawn = g_env.data->spawn_for(row.building, 4);
        if (spawn != 0) total += g_env.data->spawn_sets(spawn, row.state).size();
    }
    return total;
}

static void check_joeva_placed_sets(const world::CityNodeInfo &joeva, int64_t layout) {
    const std::vector<spawns::PlacedSet> placed =
        spawns::placed_sets(*g_env.data, *g_env.wire, spawns::node_site_of(*g_env.nodes, *g_env.wire, joeva, 4));
    size_t layout_sets = 0, building_sets = 0;
    bool layout_at_node = true, ids = true;
    for (const spawns::PlacedSet &p : placed) {
        if (p.source == spawns::PopSource::Layout) {
            ++layout_sets;
            layout_at_node = layout_at_node && p.x == joeva.x && p.y == joeva.y && p.z == joeva.z &&
                             p.yaw == joeva.yaw && p.placement == kJoeva && p.plot == 0;
        } else {
            ++building_sets;
            ids = ids && p.plot != 0 && p.placement != kJoeva;
        }
    }
    const size_t expect_building = expected_building_sets(layout);
    CHECK(layout_sets == 68 && layout_at_node, "Joeva at Town: its 68 layout sets are placed at the node record's "
                                               "location and yaw, placement id the node's guid");
    CHECK(building_sets == expect_building && building_sets > 0 && ids,
          "...and a set for each population set of each plot's building in its "
          "state, with the plot in the placement id");
    check_building_frames(joeva, layout, placed);
}

static void test_data() {
    std::printf("the node tables and the frames\n");
    const double r2 = std::sqrt(0.5);
    CHECK(std::fabs(spawns::quaternion_yaw(0, 0, r2, r2) - 90.0) < 1e-9 &&
              std::fabs(spawns::quaternion_yaw(0, 0, 1, 0) - 180.0) < 1e-9 &&
              std::fabs(spawns::quaternion_yaw(0, 0, -r2, r2) + 90.0) < 1e-9 &&
              std::fabs(spawns::quaternion_yaw(0, 0, 0, 1)) < 1e-12,
          "a quaternion's yaw: 90, 180, -90 and 0 degrees");
    CHECK(g_env.data->layouts() == 44 && g_env.data->buildings() == 59,
          "the layouts' population sets and the buildings' populations read (44 "
          "layouts with sets, 59 buildings with "
          "populations)");
    const world::CityNodeInfo *joeva = g_env.nodes->by_guid(kJoeva);
    const int64_t layout = g_env.wire->layout_of(kJoeva);
    CHECK(joeva && layout != 0 && g_env.data->layout_sets(layout).size() == 68,
          "Joeva's layout (Flat_D) names 68 population sets");
    bool keyed = false;
    for (const world::PlotRow &row : g_env.wire->plot_rows(layout, 4)) {
        const std::vector<int> levels = row.building ? g_env.data->levels_of(row.building) : std::vector<int>();
        if (levels.size() < 2 || levels.front() < 1) continue;
        const int lo = levels.front(), hi = levels.back();
        keyed = g_env.data->spawn_for(row.building, lo - 1) == 0 && g_env.data->spawn_for(row.building, lo) != 0 &&
                g_env.data->spawn_for(row.building, 6) == g_env.data->spawn_for(row.building, hi);
        if (keyed) break;
    }
    CHECK(keyed, "a building runs the BuildingLevelSpawn of the highest level "
                 "key at or below the node's: nothing "
                 "below its first key");
    check_joeva_placed_sets(*joeva, layout);
}

struct Anchor {
    const char *leaf;
    double x, y, z;
    bool building;
};

static std::pair<double, double> closest_anchor_match(const std::vector<spawns::NodeSpawn> &list, const Anchor &a) {
    double best = 1e18, best_dz = 1e18;
    for (const spawns::NodeSpawn &n : list) {
        if (spawns::archetype_leaf(n.spawn.asset->archetype_path) != a.leaf) continue;
        if ((n.source == spawns::PopSource::Building) != a.building) continue;
        const double d = std::hypot(n.spawn.x - a.x, n.spawn.y - a.y);
        if (d < best) {
            best = d;
            best_dz = n.spawn.z - a.z;
        }
    }
    return {best, best_dz};
}

static void test_anchors() {
    std::printf("retail's NPCs at Joeva (the retail capture, a Town)\n");
    const Anchor anchors[] = {
        {"Guard_Wizard", -1081432, 516570, 14753, false},
        {"Node_Villager_M", -1097474, 509458, 14024, false},
        {"Guard_Soldier", -1091333, 510020, 13408, false},
        {"Cow_StateTree", -1085254, 503627, 12608, false},
        {"Horse_withCollision", -1089840, 503696, 12625, false},
        {"Pig_0", -1078157, 512855, 13046, false},
        {"Chicken_Base", -1087121, 515470, 14042, false},
        {"Node_Warehouse_StorageClerk_KAE", -1081577, 505744, 12638, true},
        {"General_Gear_Base_Vendor", -1079600, 509538, 13290, true},
    };
    spawns::NodePopStats st;
    const auto list = select_real(*g_env.nodes->by_guid(kJoeva), 4, st);
    CHECK(list.size() == golden_npcs("Verra_RVR_Joeva", 4) && !list.empty(),
          "Joeva at Town has the oracle's number of NPCs");
    for (const Anchor &a : anchors) {
        const auto [best, best_dz] = closest_anchor_match(list, a);
        char msg[240];
        std::snprintf(msg, sizeof(msg),
                      "a %s of our %s sets stands within 1 m of where retail's "
                      "stood (%.2f m)",
                      a.leaf, a.building ? "building" : "layout", best / 100.0);
        CHECK(best < 100.0, msg);
        std::snprintf(msg, sizeof(msg), "...and at retail's height: %.0f cm, ours %+.0f cm (within 2 m)", a.z, best_dz);
        CHECK(std::fabs(best_dz) < 200.0, msg);
    }
}

static void test_golden() {
    std::printf("the golden counts: %zu cases (10 nodes inside the Riverlands, 7 "
                "levels)\n",
                tests::kNodePopCaseCount);
    size_t mismatches = 0, total_npcs = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < tests::kNodePopCaseCount; ++i) {
        const tests::NodePopCase &c = tests::kNodePopCases[i];
        const world::CityNodeInfo *info = g_env.nodes->by_name(c.node);
        if (!info) {
            CHECKF(false, "no node %s", c.node);
            continue;
        }
        spawns::NodePopStats st;
        const auto list = select_real(*info, c.level, st);
        total_npcs += list.size();
        const std::string diff = diff_golden_case(c, st, list);
        CHECKF(diff.empty(), "%s at level %d differs from the oracle:%s", c.node, c.level, diff.c_str());
        if (!diff.empty() && ++mismatches <= 3) std::printf("    %s L%d:%s\n", c.node, c.level, diff.c_str());
    }
    std::printf(
        "    %zu cases selected in %lld ms (%zu NPCs in all)\n", tests::kNodePopCaseCount,
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()),
        total_npcs);
    CHECK(mismatches == 0, "every node at every level has the oracle's sets, "
                           "points, NPCs, losses, Blueprints and digest");
    CHECK(golden_npcs("Verra_RVR_Joeva", 4) > golden_npcs("Verra_RVR_Joeva", 3) &&
              golden_npcs("Verra_RVR_Joeva", 3) > golden_npcs("Verra_RVR_Joeva", 0) &&
              golden_npcs("Verra_RVR_Joeva", 5) < golden_npcs("Verra_RVR_Joeva", 3) &&
              golden_npcs("Verra_RVR_Joeva", 5) == golden_npcs("Verra_RVR_Joeva", 6),
          "(the data) Joeva has more people at Town than at Village, more at "
          "Village than at Wilderness, and at City "
          "(as at Metropolis) fewer than at Village: the Town-only sets are gone");
    CHECK(golden_of("Verra_RVR_Joeva", 4)->spawned_building > 0 &&
              golden_of("Verra_RVR_ArtisanPOI_AvensEnd", 0)->spawned_building > 0 &&
              golden_of("Verra_RVR_ArtisanPOI_AvensEnd", 0)->spawned_layout == 0,
          "(the data) a town has building NPCs (clerks, vendors) and Aven's "
          "End's NPCs, at Wilderness, are all its "
          "building's");
}

static void test_walkers() {
    std::printf("how the towns' people live (make_setup, the shipped seed's "
                "workshops)\n");
    std::map<int64_t, WalkerCount> by_node;
    Rig rig(*g_env.shipped_nodes, *g_env.shipped_wire, spawns::AreaConfig::riverlands());
    rig.on_spawn = [&](uint64_t id, const content::NpcInstanceRow *inst, const content::NpcRouteRow *route,
                       const world::CityNodeInfo &node, int64_t plot) {
        if (const world::WorldActor *a = rig.registry.find(id)) {
            record_walker_spawn(by_node[node.guid], {*a, inst, route, node, plot});
        }
    };
    rig.pops->start(0.0);
    size_t service_walking = 0, stationary_routed = 0, unplaced = 0;
    for (const auto &[guid, c] : by_node) {
        service_walking += c.service_walking;
        stationary_routed += c.stationary_routed;
        unplaced += c.patrol_unplaced;
    }
    const WalkerCount &joeva = by_node[kJoeva];
    const WalkerCount &halcyon = by_node[kHalcyon];
    CHECK(joeva.total == 624 && halcyon.total == 375, "Joeva (Town) has 603 NPCs and 21 crafting stations and Halcyon "
                                                      "(Village) 354 and 21 with the workshops");
    CHECK(joeva.patrol > 0 && joeva.guards_patrol > 0 && joeva.wander > 0 && joeva.stand > 0,
          "Joeva has people that walk a route, guards among them, people that "
          "wander and people that stand");
    CHECK(joeva.workers_walk == 16 && joeva.farmers_stand == 2 && halcyon.workers_walk == 4,
          "the workshops' people: 16 workers walk and 2 farmers stand at Joeva, "
          "4 workers walk at Halcyon");
    CHECK(joeva.carriers_walk == 0 && joeva.guards_patrol == 1 && halcyon.carriers_walk == 2 &&
              halcyon.guards_patrol == 2,
          "Joeva's carriers stand (its guard walks); Halcyon's 2 carriers and 2 "
          "guards walk their routes");
    CHECK(service_walking == 0 && joeva.services > 0 && halcyon.services > 0,
          "no vendor, clerk, merchant stand or station of any node walks "
          "(Joeva's and Halcyon's, the building sets' "
          "included)");
    CHECK(joeva.crate_vendors > 0 && joeva.crate_vendors_walking == 0 && halcyon.crate_vendors_walking == 0,
          "the construction crate vendor (the caravansary's crate salesman) "
          "stands in Joeva and Halcyon");
    CHECK(joeva.building_wanderers == 16 && halcyon.building_wanderers == 4,
          "the workshops' wanderers are the 16 of Joeva and the 4 of Halcyon");
    CHECK(joeva.workers_off_plot == 0 && halcyon.workers_off_plot == 0,
          "every workshop worker stands within 30 m of its own plot (its anchor)");
    CHECK(joeva.workers_far == 0 && halcyon.workers_far == 0,
          "every workshop worker strolls at most 6 m from its anchor");
    CHECK(stationary_routed == 0, "nobody that stands has a route");
    CHECK(unplaced == 0, "every route that is walked starts within 600 m of its "
                         "node's record (the node-local waypoints were placed)");
}

static size_t test_manager_start(world::CityNodes &nodes, Rig &rig) {
    const size_t made = rig.pops->start(0.0);
    size_t want = 0;
    for (const world::CityNodeInfo &n : nodes.nodes()) {
        if (const tests::NodePopCase *c = golden_of(n.name, nodes.level_of(n.guid))) {
            want += c->spawned_layout + c->spawned_building;
        }
    }
    CHECK(made == want && rig.pops->made() == want && people_of(rig.registry) == want && rig.spawned.size() == want,
          "start: every node inside the area has the oracle's number of NPCs at "
          "its seed level");
    CHECK(rig.pops->nodes() == 10 && rig.pops->start(0.0) == 0, "10 nodes inside the Riverlands; started once");
    CHECK(rig.npcs_of(kJoeva) == golden_npcs("Verra_RVR_Joeva", 4) &&
              rig.npcs_of(kHalcyon) == golden_npcs("Verra_RVR_Halcyon", 3) &&
              rig.npcs_of(kWinstead) == golden_npcs("Verra_RVR_Winstead", 0),
          "Joeva (Town), Halcyon (Village) and Winstead (Wilderness) have their "
          "level's people");
    bool tagged = true, in_range = true, grounding = true;
    size_t by_data = 0, by_terrain = 0;
    rig.registry.for_each([&](const world::WorldActor &a) {
        tagged = tagged && a.npc.spawn_ref >= spawns::kNodeRefBase && a.npc.source == world::Source::Data &&
                 (a.kind == world::ActorKind::Npc || a.kind == world::ActorKind::Station);
        in_range = in_range && a.npc.home_x == a.x && a.npc.home_y == a.y;
        grounding = grounding && a.npc.ground_by_data != a.npc.ground_by_terrain;
        by_data += a.npc.ground_by_data ? 1 : 0;
        by_terrain += a.npc.ground_by_terrain ? 1 : 0;
    });
    CHECK(tagged && in_range, "each NPC carries a spawn reference in the node range, comes from the "
                              "data and has its place as its home");
    char grounding_msg[200];
    std::snprintf(grounding_msg, sizeof(grounding_msg),
                  "each node NPC keeps its data height (ground_by_data: %zu) "
                  "unless its point is a PopulationArea's "
                  "random one (ground_by_terrain: %zu)",
                  by_data, by_terrain);
    CHECK(grounding && by_data == want - by_terrain && by_data > want / 2, grounding_msg);
    CHECK(rig.pops->tick(1.0) == 0, "nothing changed: the manager does nothing");
    return want;
}
static void check_level_change_registry(Rig &rig, const std::set<uint64_t> &b, const std::set<uint64_t> &a,
                                        const std::map<uint64_t, uint32_t> &ref_of, size_t despawned_before) {
    bool leavers_gone = true, stayers_there = true, hook_ids = true;
    for (const uint64_t id : b) {
        const world::WorldActor *act = rig.registry.find(id);
        if (!a.count(id)) {
            leavers_gone = leavers_gone && act == nullptr;
        } else {
            stayers_there = stayers_there && act != nullptr && act->npc.spawn_ref == ref_of.at(id);
        }
    }
    for (size_t i = despawned_before; i < rig.despawned.size(); ++i) {
        hook_ids = hook_ids && b.count(rig.despawned[i]) && !a.count(rig.despawned[i]);
    }
    CHECK(leavers_gone && stayers_there && hook_ids,
          "the NPCs that left are out of the registry; the ones that stay keep "
          "their actors and their references");
}

static void check_manager_respawn(Rig &rig, const std::vector<uint64_t> &before, const std::set<uint64_t> &a,
                                  const std::map<uint64_t, uint32_t> &ref_of) {
    uint64_t gone_id = 0, stay_id = 0;
    for (const uint64_t id : before) {
        if (!a.count(id) && gone_id == 0) gone_id = id;
        if (a.count(id) && stay_id == 0 && rig.registry.find(id)->npc.respawn_seconds >= 0) stay_id = id;
    }
    CHECK(rig.pops->respawn(ref_of.at(gone_id), 3.0) == 0,
          "an NPC the new level has no point for does not come back (its "
          "reference is closed)");
    CHECK(rig.pops->respawn(0x80000000u + 4000000u, 3.0) == 0 && rig.pops->respawn(5, 3.0) == 0,
          "a reference that is no slot, or not a node's, is nothing");
    const world::WorldActor stayer = *rig.registry.find(stay_id);
    rig.registry.remove(stay_id);
    const size_t npcs_without = people_of(rig.registry);
    const uint64_t back = rig.pops->respawn(ref_of.at(stay_id), 4.0);
    const world::WorldActor *b2 = rig.registry.find(back);
    const bool back_ok = back != 0 && b2 && people_of(rig.registry) == npcs_without + 1 &&
                         std::tie(b2->npc.asset_guid, b2->npc.instance_guid, b2->npc.spawn_ref, b2->x, b2->y,
                                  b2->npc.level) == std::tie(stayer.npc.asset_guid, stayer.npc.instance_guid,
                                                             ref_of.at(stay_id), stayer.x, stayer.y, stayer.npc.level);
    CHECK(back_ok, "the NPC of a point that stays comes back from the node's "
                   "selection: the same Blueprint, place, "
                   "level and reference");
}

static void test_manager_level_change(world::CityNodes &nodes, Rig &rig, size_t want) {
    const std::vector<uint64_t> before = rig.pops->actors_of(kJoeva);
    const std::vector<uint64_t> halcyon_before = rig.pops->actors_of(kHalcyon);
    std::map<uint64_t, uint32_t> ref_of;
    for (const uint64_t id : before) ref_of[id] = rig.registry.find(id)->npc.spawn_ref;
    const size_t spawned_before = rig.spawned.size(), despawned_before = rig.despawned.size();
    set_level(nodes, kJoeva, 3);
    CHECK(rig.pops->entry_of(kJoeva)->level == 4, "(a level change is not followed until the world ticks)");
    CHECK(rig.pops->tick(2.0) == 1, "Joeva Town -> Village: one node rebuilt");
    const std::vector<uint64_t> after = rig.pops->actors_of(kJoeva);
    const std::set<uint64_t> b = as_set(before), a = as_set(after);
    size_t kept = 0, gone = 0, arrived = 0;
    for (const uint64_t id : b) (a.count(id) ? kept : gone)++;
    for (const uint64_t id : a) arrived += b.count(id) ? 0 : 1;
    CHECK(rig.pops->entry_of(kJoeva)->level == 3 && after.size() == golden_npcs("Verra_RVR_Joeva", 3),
          "Joeva has the oracle's people for Village");
    CHECK(kept > 0 && gone > 0 && arrived > 0 && kept + gone == before.size() && kept + arrived == after.size(),
          "some NPCs stay, some leave and some arrive (a Town's sets go, a "
          "Village's building sets come)");
    CHECK(rig.spawned.size() - spawned_before == arrived && rig.despawned.size() - despawned_before == gone,
          "the hooks saw each arrival and each departure once");
    check_level_change_registry(rig, b, a, ref_of, despawned_before);
    CHECK(as_set(rig.pops->actors_of(kHalcyon)) == as_set(halcyon_before), "Halcyon's people were not touched");
    CHECK(people_of(rig.registry) == want - golden_npcs("Verra_RVR_Joeva", 4) + golden_npcs("Verra_RVR_Joeva", 3),
          "the registry holds exactly the nodes' people (nothing leaked, nothing "
          "doubled)");
    check_manager_respawn(rig, before, a, ref_of);
    CHECK(rig.pops->respawned() == 1 &&
              rig.pops->entry_of(kJoeva)->npcs + rig.pops->entry_of(kJoeva)->stations == after.size(),
          "(counted; the node's count is whole again)");
}
static void test_manager_rebuilds(world::CityNodes &nodes, Rig &rig) {
    CHECK(rig.pops->tick(5.0) == 0, "(nothing to follow)");
    set_level(nodes, kJoeva, 0);
    rig.pops->tick(6.0);
    CHECK(rig.pops->actors_of(kJoeva).size() == golden_npcs("Verra_RVR_Joeva", 0),
          "Joeva at Wilderness has the people of a Wilderness node");
    set_level(nodes, kJoeva, 4);
    rig.pops->tick(7.0);
    CHECK(rig.pops->actors_of(kJoeva).size() == golden_npcs("Verra_RVR_Joeva", 4) &&
              rig.pops->entry_of(kJoeva)->level == 4,
          "...and at Town again, the Town's");
    size_t building_npcs = 0;
    for (const uint64_t id : rig.pops->actors_of(kJoeva)) {
        building_npcs += rig.registry.find(id)->npc.vendor_guid != 0 ? 1 : 0;
    }
    CHECK(building_npcs > 0, "(the Town's people include vendors again)");
    constexpr int64_t kTownCrierRoot = 6064634349883577684ll;
    size_t criers = 0;
    for (const uint64_t id : rig.pops->actors_of(kJoeva)) {
        criers += rig.registry.find(id)->npc.dialogue_root == kTownCrierRoot ? 1 : 0;
    }
    CHECK(criers == 2, "(and its two Town Criers open the Town Crier's conversation)");
    CHECK(rig.pops->rebuilds() == 3, "three rebuilds in all");
    set_level(nodes, kHalcyon, 4);
    set_level(nodes, kWinstead, 3);
    CHECK(rig.pops->tick(8.0) == 2 && rig.npcs_of(kHalcyon) == golden_npcs("Verra_RVR_Halcyon", 4) &&
              rig.npcs_of(kWinstead) == golden_npcs("Verra_RVR_Winstead", 3),
          "two nodes that changed together are both rebuilt");
    size_t halcyon_criers = 0;
    for (const uint64_t id : rig.pops->actors_of(kHalcyon)) {
        halcyon_criers += rig.registry.find(id)->npc.dialogue_root == kTownCrierRoot ? 1 : 0;
    }
    CHECK(halcyon_criers == 1, "(Halcyon's Town Crier opens the same conversation)");
    spawns::NodePopStats s3, s4;
    const auto l3 = select_real(*nodes.by_guid(kJoeva), 3, s3);
    const auto l4 = select_real(*nodes.by_guid(kJoeva), 4, s4);
    size_t differing = 0;
    for (const spawns::NodeSpawn &x : l4) {
        for (const spawns::NodeSpawn &y : l3) {
            if (x.ident == y.ident && x.spawn.asset->guid != y.spawn.asset->guid) ++differing;
        }
    }
    std::printf("    (%zu points of Joeva name another Blueprint at Village than "
                "at Town)\n",
                differing);
    CHECK(true, "(counted)");
}
static void test_manager() {
    std::printf("the live manager: the people follow the nodes' levels\n");
    auto nodes = fresh_nodes();
    Rig rig(*nodes, *g_env.wire, spawns::AreaConfig::riverlands());
    const size_t want = test_manager_start(*nodes, rig);
    test_manager_level_change(*nodes, rig, want);
    test_manager_rebuilds(*nodes, rig);
}
static std::shared_ptr<content::PopulationData> build_replacement_pop_data(const char *kGuard, const char *kVillager) {
    auto pd = std::make_shared<content::PopulationData>();
    const auto asset = [&](int64_t guid, const char *name, const char *path, const char *lo, const char *hi) {
        content::NpcAssetRow a;
        a.guid = guid;
        a.name = name;
        a.archetype_path = path;
        a.filter.node_level_min = lo;
        a.filter.node_level_max = hi;
        pd->assets[guid] = a;
    };
    asset(9001, "Guard", kGuard, "Wilderness", "Encampment");
    asset(9002, "Villager", kVillager, "Village", "Metropolis");
    asset(9003, "VillageOnly", kVillager, "Village", "Metropolis");
    pd->asset_sets[9100] = {{9001, 1.0}, {9002, 1.0}};
    pd->asset_sets[9101] = {{9003, 1.0}};
    const auto instance = [&](int64_t guid, int64_t asset_set) {
        content::NpcInstanceRow i;
        i.guid = guid;
        i.name = "Post";
        i.level_min = i.level_max = 1;
        i.ambient_behavior = "Stationary";
        i.respawn_time = 10;
        i.asset_sets = {asset_set};
        pd->instances[guid] = i;
    };
    instance(9200, 9100);
    instance(9201, 9101);
    content::NpcSetRow set;
    set.guid = 9300;
    set.name = "Post set";
    content::NpcSetMember m1;
    m1.key = 1;
    m1.instance = 9200;
    m1.x = 100;
    m1.y = 200;
    content::NpcSetMember m2 = m1;
    m2.key = 2;
    m2.instance = 9201;
    m2.x = 300;
    set.members = {m1, m2};
    pd->sets[9300] = set;
    return pd;
}
static void test_replacement_transitions(world::CityNodes &synth, Rig &rig) {
    CHECK(rig.pops->start(0.0) == 1, "at Encampment the post has a guard and the "
                                     "point only a Village has has nobody");
    std::vector<uint64_t> ids = rig.pops->actors_of(kJoeva);
    const world::WorldActor guard = *rig.registry.find(ids.at(0));
    const uint32_t guard_ref = guard.npc.spawn_ref;
    CHECK(guard.npc.asset_guid == 9001 && guard_ref >= spawns::kNodeRefBase, "...the guard, with a node reference");
    rig.registry.remove(guard.id);
    set_level(synth, kJoeva, 3);
    CHECK(rig.pops->tick(1.0) == 1, "Encampment -> Village");
    ids = rig.pops->actors_of(kJoeva);
    CHECK(ids.size() == 2, "the post has a villager now and the Village's own point has one too");
    uint64_t villager = 0;
    for (const uint64_t id : ids) {
        if (rig.registry.find(id)->npc.asset_guid == 9002) villager = id;
    }
    const world::WorldActor *v = rig.registry.find(villager);
    CHECK(v && v->npc.spawn_ref != guard_ref && v->npc.spawn_ref >= spawns::kNodeRefBase && v->x == guard.x &&
              v->y == guard.y,
          "the dead guard's point is a villager at the same place (a new "
          "reference: the guard's is closed)");
    CHECK(rig.pops->respawn(guard_ref, 20.0) == 0 && rig.pops->actors_of(kJoeva).size() == 2,
          "the respawn timer of the dead guard runs out: nothing comes back (no "
          "guard beside the villager)");
    const uint32_t villager_ref = v->npc.spawn_ref;
    set_level(synth, kJoeva, 2);
    CHECK(rig.pops->tick(2.0) == 1 && rig.pops->actors_of(kJoeva).size() == 1, "Village -> Encampment: one NPC");
    const world::WorldActor *g2 = rig.registry.find(rig.pops->actors_of(kJoeva).at(0));
    CHECK(g2 && g2->npc.asset_guid == 9001 && rig.registry.find(villager) == nullptr,
          "...a guard again; the villager left");
    CHECK(rig.pops->respawn(villager_ref, 21.0) == 0, "the villager's reference is closed: it does not come back");
    const uint32_t ref2 = g2->npc.spawn_ref;
    rig.registry.remove(g2->id);
    const uint64_t back = rig.pops->respawn(ref2, 22.0);
    CHECK(back != 0 && rig.registry.find(back)->npc.asset_guid == 9001 &&
              rig.registry.find(back)->npc.spawn_ref == ref2,
          "a guard that dies at Encampment comes back as a guard, with its "
          "reference");
    rig.registry.remove(back);
    set_level(synth, kJoeva, 3);
    rig.pops->tick(3.0);
    CHECK(rig.pops->actors_of(kJoeva).size() == 2 && rig.pops->respawn(ref2, 23.0) == 0 &&
              rig.pops->actors_of(kJoeva).size() == 2,
          "a guard that died, then the level rose: the villager stands there "
          "already, and the guard's respawn does "
          "nothing");
}
static void test_replacement() {
    std::printf("a point that draws another Blueprint at another level\n");
    const char *kGuard = "/Game/DesignAssets/Population/NPC/PCRaces/Guards/"
                         "Guard_Soldier.Default__Guard_Soldier_C";
    const char *kVillager = "/Game/DesignAssets/Population/NPC/PCRaces/Node/"
                            "Filler/Node_Villager_F.Default__Node_Villager_F_C";
    if (!g_env.captured->template_for(kGuard) || !g_env.captured->template_for(kVillager)) {
        CHECK(false, "the retail captures opened a Guard_Soldier and a Node_Villager_F");
        return;
    }
    auto pd = build_replacement_pop_data(kGuard, kVillager);
    std::string error;
    const world::CityNodeInfo *real = g_env.nodes->by_guid(kJoeva);
    std::vector<world::CityZoneInfo> zones{*g_env.nodes->zone_by_guid(real->zoi_guid)};
    world::CityNodes synth({*real}, zones);
    set_level(synth, kJoeva, 2);
    auto wire = world::NodeWire::load(db_path(), synth, error);
    if (!wire) {
        CHECK(false, "the node wire loads for a registry of one node");
        return;
    }
    const auto data = spawns::NodePopulationData::make({{wire->layout_of(kJoeva), {9300}}});
    spawns::AreaConfig everywhere;
    everywhere.center_x = real->x;
    everywhere.center_y = real->y;
    everywhere.radius_cm = 1.0e8;
    Rig rig(synth, *wire, everywhere, pd, data.get());
    test_replacement_transitions(synth, rig);
}
} // namespace
int main() {
    if (const int rc = init_node_pop_env(); rc != 0) return rc;
    test_filter();
    test_data();
    test_walkers();
    test_anchors();
    test_golden();
    test_manager();
    test_replacement();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
