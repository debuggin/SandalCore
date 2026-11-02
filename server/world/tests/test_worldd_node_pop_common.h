// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "fake_client.h"
#include "nav_chunk_fixture.h"
#include "node_pop_golden.h"
#include "protocol/schema/generated/index.h"
#include "session/player_world.h"
#include "spawns/npc_looks.h"
#include "world/node_origins.h"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace aoc::worldd::test_node_pop {

namespace bs = aoc::worldd::bootstrap;
namespace fs = std::filesystem;
namespace conn = aoc::protocol::conn;
namespace repl = aoc::protocol::repl;
using aoc::protocol::emit::FIntrepidNetworkGUID;
using spawns::Why;

inline int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            std::printf("  [ok ] %s\n", msg);                                                                          \
            g_pass++;                                                                                                  \
        } else {                                                                                                       \
            std::printf("  [FAIL] %s\n", msg);                                                                         \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)
#define CHECKF(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            g_pass++;                                                                                                  \
        } else {                                                                                                       \
            std::printf("  [FAIL] ");                                                                                  \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

#ifndef NODES_CONTENT_DB_PATH
#define NODES_CONTENT_DB_PATH "dist/content/content.db"
#endif
#ifndef NODES_SEED_PATH
#define NODES_SEED_PATH "deploy/citynode_seed.json"
#endif
#ifndef NODES_MOTION_PATH
#define NODES_MOTION_PATH "dist/content/npc_motion.tsv"
#endif
#ifndef NODES_ROADS_LEVELS_PATH
#define NODES_ROADS_LEVELS_PATH "dist/content/roads_levels.tsv"
#endif
#ifndef NODES_NAV_PATH
#define NODES_NAV_PATH "dist/content/area_nav.bin"
#endif

inline std::string db_path() {
    const char *env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? env : NODES_CONTENT_DB_PATH;
}
inline std::string motion_path() {
    const char *env = std::getenv("AOC_NPC_MOTION");
    return env && *env ? env : NODES_MOTION_PATH;
}

inline constexpr int64_t kJoeva = 108646121231993ll, kHalcyon = 108646610851452ll, kWinstead = 108646108518008ll;

struct Env {
    std::optional<content::ContentDb> db;
    std::shared_ptr<const content::PopulationData> pop;
    std::optional<spawns::CapturedData> captured;
    npc::MotionTable table;
    std::optional<spawns::GenericNpcs> generic;
    std::unique_ptr<spawns::NpcLooks> looks;
    std::unique_ptr<world::CityNodes> nodes;
    std::unique_ptr<world::NodeWire> wire;
    std::unique_ptr<world::CityNodes> shipped_nodes;
    std::unique_ptr<world::NodeWire> shipped_wire;
    std::unique_ptr<spawns::NodePopulationData> data;
    spawns::SpawnContext base;
    std::set<std::string> shown;
    spawns::TemplateSource templates;
};
inline Env g_env;

inline std::string seed_without_plots() {
    std::ifstream in(NODES_SEED_PATH);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::printf("  [FAIL] the seed file does not read: %s\n", NODES_SEED_PATH);
        std::exit(2);
    }
    j.erase("plots");
    const std::string path = (std::filesystem::temp_directory_path() / "node_pop_seed_no_plots.json").string();
    std::ofstream(path) << j.dump();
    return path;
}

inline std::unique_ptr<world::CityNodes> fresh_nodes(bool with_plots = false) {
    std::string error;
    auto nodes = world::CityNodes::load(db_path(), error);
    if (!nodes) {
        std::printf("  [FAIL] city nodes: %s\n", error.c_str());
        std::exit(2);
    }
    size_t applied = 0;
    nodes->apply_seed_file(with_plots ? std::string(NODES_SEED_PATH) : seed_without_plots(), error, &applied);
    return nodes;
}

inline size_t people_of(const world::ActorRegistry &registry) {
    return registry.count(world::ActorKind::Npc) + registry.count(world::ActorKind::Station);
}

inline void set_level(world::CityNodes &nodes, int64_t guid, int level) {
    world::LiveNode ln;
    ln.guid = guid;
    ln.level = level;
    nodes.apply_change(ln);
}
inline std::vector<spawns::NodeSpawn> select(const world::CityNodes &nodes, const world::NodeWire &wire,
                                             const world::CityNodeInfo &info, int level, spawns::NodePopStats &st) {
    const spawns::NodeSite site = spawns::node_site_of(nodes, wire, info, level);
    const spawns::SpawnContext ctx = spawns::node_spawn_context(g_env.base, site, g_env.shown);
    const spawns::SpawnFilter filter(ctx, &g_env.table);
    return spawns::select_node_spawns(*g_env.pop, g_env.templates, *g_env.data, wire, site,
                                      spawns::AreaConfig::riverlands(), filter, st);
}
inline std::vector<spawns::NodeSpawn> select_in_area(const world::CityNodes &nodes, const world::NodeWire &wire,
                                                     const world::CityNodeInfo &info, int level,
                                                     const spawns::AreaConfig &area) {
    spawns::NodePopStats st;
    const spawns::NodeSite site = spawns::node_site_of(nodes, wire, info, level);
    const spawns::SpawnContext ctx = spawns::node_spawn_context(g_env.base, site, g_env.shown);
    const spawns::SpawnFilter filter(ctx, &g_env.table);
    return spawns::select_node_spawns(*g_env.pop, g_env.templates, *g_env.data, wire, site, area, filter, st);
}
inline std::vector<spawns::NodeSpawn> select_real(const world::CityNodeInfo &info, int level,
                                                  spawns::NodePopStats &st) {
    return select(*g_env.nodes, *g_env.wire, info, level, st);
}
inline const char *why_id(Why w) {
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
inline uint64_t digest_of(const std::vector<spawns::NodeSpawn> &list) {
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
inline std::string join_counts(const std::map<std::string, size_t> &m) {
    std::string out;
    for (const auto &[k, v] : m) {
        if (v == 0) continue;
        out += (out.empty() ? "" : ";") + k + ":" + std::to_string(v);
    }
    return out;
}
inline const tests::NodePopCase *golden_of(const std::string &node, int level) {
    for (size_t i = 0; i < tests::kNodePopCaseCount; ++i) {
        if (node == tests::kNodePopCases[i].node && level == tests::kNodePopCases[i].level)
            return &tests::kNodePopCases[i];
    }
    return nullptr;
}
inline size_t golden_npcs(const std::string &node, int level) {
    const tests::NodePopCase *c = golden_of(node, level);
    return c ? c->spawned_layout + c->spawned_building : 0;
}
inline uint64_t g_next = 8192;
inline FIntrepidNetworkGUID mint() {
    FIntrepidNetworkGUID g;
    g.ObjectId = g_next;
    g_next += 2;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4u;
    return g;
}
struct Rig {
    world::ActorRegistry registry;
    std::vector<uint64_t> spawned, despawned;
    std::unique_ptr<spawns::NodePopulations> pops;
    std::function<void(uint64_t, const content::NpcInstanceRow *, const content::NpcRouteRow *)> on_extra;
    std::function<void(uint64_t, const content::NpcInstanceRow *, const content::NpcRouteRow *,
                       const world::CityNodeInfo &, int64_t)>
        on_spawn;
    Rig(const world::CityNodes &nodes, const world::NodeWire &wire, const spawns::AreaConfig &area,
        std::function<void(uint64_t, const content::NpcInstanceRow *, const content::NpcRouteRow *)> extra = {})
        : on_extra(std::move(extra)) {
        init_pops(nodes, wire, area, g_env.pop, g_env.data.get());
    }
    Rig(const world::CityNodes &nodes, const world::NodeWire &wire, const spawns::AreaConfig &area,
        std::shared_ptr<const content::PopulationData> population, const spawns::NodePopulationData *data) {
        init_pops(nodes, wire, area, std::move(population), data);
    }
    void init_pops(const world::CityNodes &nodes, const world::NodeWire &wire, const spawns::AreaConfig &area,
                   std::shared_ptr<const content::PopulationData> population, const spawns::NodePopulationData *data) {
        spawns::NodePopulations::Hooks hooks;
        hooks.spawned = [this](uint64_t id, const content::NpcInstanceRow *inst, const content::NpcRouteRow *route,
                               const world::CityNodeInfo &node, int64_t plot) {
            spawned.push_back(id);
            if (on_extra) on_extra(id, inst, route);
            if (on_spawn) on_spawn(id, inst, route, node, plot);
        };
        hooks.despawned = [this](uint64_t id) { despawned.push_back(id); };
        spawns::NodePopulations::Config cfg;
        cfg.shown_humanoids = g_env.shown;
        pops = std::make_unique<spawns::NodePopulations>(
            registry, nodes, wire, data ? *data : *g_env.data, population ? population : g_env.pop, g_env.templates,
            g_env.base, &g_env.table, area, [] { return mint(); }, 60, std::move(hooks), cfg);
    }
    size_t npcs_of(int64_t node) const { return pops->actors_of(node).size(); }
};
inline std::set<uint64_t> as_set(const std::vector<uint64_t> &v) {
    return {v.begin(), v.end()};
}
struct RoadFile {
    struct Road {
        int64_t zoi = 0;
        int tiers[7] = {0};
    };
    std::vector<Road> roads;
};
inline bool parse_road_level_line(const std::string &line, RoadFile::Road &r) {
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '\t')) f.push_back(cell);
    if (f.size() < 6 || f[0] != "roadn") return false;
    r.zoi = static_cast<int64_t>(std::strtoull(f[1].c_str(), nullptr, 10));
    std::stringstream ts(f[2]);
    std::string t;
    int k = 0;
    while (std::getline(ts, t, ',') && k < 7) r.tiers[k++] = std::atoi(t.c_str());
    return true;
}
inline bool read_roads_levels(const std::string &path, RoadFile &out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        RoadFile::Road r;
        if (!parse_road_level_line(line, r)) return false;
        out.roads.push_back(r);
    }
    return true;
}
inline void extract_stat_array(const repl::FastArrayState &arr, std::map<int64_t, float> &out) {
    for (const repl::FastArrayItem &it : arr.items()) {
        const auto v = it.values.values.find(1);
        const auto *s = v == it.values.values.end() ? nullptr : v->second.get_if<repl::StatInt32Inst>();
        if (s) out[s->record] = std::bit_cast<float>(s->value);
    }
}
inline std::map<int64_t, float> stat_items(const world::WorldActor &a) {
    std::map<int64_t, float> out;
    for (const repl::Subobject &sub : a.actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAoCStatsComponent") continue;
        const auto *arr =
            const_cast<repl::FastArrays &>(sub.obj.arrays).get(*sub.obj.schema, "StatRepInt32EveryoneProxy");
        if (arr) extract_stat_array(*arr, out);
    }
    return out;
}
inline bool has_full_health(const std::map<int64_t, float> &items) {
    const auto h = items.find(content::kGuidHealth);
    const auto m = items.find(content::kGuidMaxHealth);
    return h != items.end() && m != items.end() && m->second > 0 && h->second == m->second;
}
inline std::optional<std::vector<repl::BlockView>> find_chain_blocks(const fake::Delivered &d,
                                                                     const world::WorldActor &a) {
    const size_t end = d.content.bit_pos();
    for (size_t start = 0; start < (d.open ? 2000u : 1u) && start < end; ++start) {
        auto got = repl::read_content_blocks(d.content.data(), end, start);
        if (!got || got->empty() || !(*got)[0].header.is_actor) continue;
        if (d.open && got->size() != a.actor.subobjects.size() + 1) continue;
        return got;
    }
    return std::nullopt;
}
inline void collect_fast_array_stats(const std::vector<repl::FastArrayField> &fields, uint32_t field_idx,
                                     std::map<int64_t, float> &out) {
    for (const repl::FastArrayField &field : fields) {
        if (field.field != field_idx) continue;
        for (const auto &ch : field.delta.changed) {
            const auto v = ch.values.values.find(1);
            const auto *s = v == ch.values.values.end() ? nullptr : v->second.get_if<repl::StatInt32Inst>();
            if (s) out[s->record] = std::bit_cast<float>(s->value);
        }
    }
}
inline bool read_comp_stat_block(const fake::Delivered &d, const repl::Subobject &comp, const repl::BlockView &b,
                                 std::map<int64_t, float> &out) {
    if (b.header.is_actor || b.header.object.ObjectId != comp.guid.ObjectId) return true;
    const size_t block_end = b.payload_pos + b.payload_bits;
    size_t pos = b.payload_pos;
    if (b.header.has_rep_layout &&
        !repl::read_rep_block(*comp.obj.schema, d.content.data(), block_end, pos, comp.obj.opts)) {
        return false;
    }
    if (pos >= block_end) return false;
    const auto fields = repl::read_fast_array_fields(*comp.obj.schema, d.content.data(), block_end, pos, comp.obj.opts);
    const auto *f = comp.obj.schema->field("StatRepInt32EveryoneProxy");
    if (!fields || !f) return false;
    collect_fast_array_stats(*fields, f->index, out);
    return true;
}
inline std::map<int64_t, float> stat_items_in_chain(const fake::Delivered &d, const world::WorldActor &a) {
    std::map<int64_t, float> out;
    const repl::Subobject *comp = nullptr;
    for (const repl::Subobject &s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "UAoCStatsComponent") comp = &s;
    }
    if (!comp || d.close) return out;
    const auto blocks = find_chain_blocks(d, a);
    if (!blocks) return out;
    for (const repl::BlockView &b : *blocks) {
        if (!read_comp_stat_block(d, *comp, b, out)) return out;
    }
    return out;
}
inline void check_town_health(PlayerWorld &world, const char *when) {
    size_t total = 0, lacking = 0, bad = 0, wizards = 0, villagers = 0, unequal = 0;
    std::map<std::pair<std::string, int>, float> by_kind;
    world.registry().for_each([&](const world::WorldActor &a) {
        if (a.kind != world::ActorKind::Npc || a.npc.spawn_ref < spawns::kNodeRefBase) return;
        ++total;
        const std::string leaf = spawns::blueprint_leaf(a.npc.archetype);
        wizards += leaf == "Guard_Wizard" ? 1 : 0;
        villagers += leaf.rfind("Node_Villager", 0) == 0 ? 1 : 0;
        const auto items = stat_items(a);
        if (!items.count(content::kGuidHealth) || !items.count(content::kGuidMaxHealth)) {
            ++lacking;
            return;
        }
        if (!has_full_health(items)) ++bad;
        const auto key = std::make_pair(leaf, static_cast<int>(a.npc.level));
        const auto [it, fresh] = by_kind.emplace(key, items.at(content::kGuidMaxHealth));
        if (!fresh && it->second != items.at(content::kGuidMaxHealth)) ++unequal;
    });
    for (const auto &[kind, health] : by_kind) {
        if (kind.first == "Guard_Wizard" || kind.first == "Guard_Captain" ||
            kind.first.rfind("Node_Villager", 0) == 0) {
            std::printf("    %s L%d: Health %.1f\n", kind.first.c_str(), kind.second, health);
        }
    }
    std::printf("    %s: %zu town people, %zu without a Health item, %zu with "
                "Health != MaxHealth or 0, %zu guard wizards, %zu villagers\n",
                when, total, lacking, bad, wizards, villagers);
    CHECK(total > 0 && wizards > 0 && villagers > 0,
          "(the town's people include guard wizards and villagers, two templates "
          "that have no stats of their own)");
    CHECK(lacking == 0, "after the node people start, no node NPC lacks a Health item");
    CHECK(bad == 0, "every node NPC has Health == MaxHealth > 0");
    CHECK(unequal == 0, "people of one Blueprint and level hold one and the same MaxHealth");
}
inline void pump_wizard_view(ActorView &view, Connection &server, fake::Client &client, const world::WorldActor &wiz,
                             std::vector<fake::Delivered> &got) {
    Clock::time_point t{std::chrono::seconds(5000)};
    auto pass = [&] {
        t += std::chrono::milliseconds(60);
        view.tick(server, wiz.x, wiz.y, t);
        for (const auto &pkt : server.flush(t)) {
            for (const auto &d : client.receive(pkt).delivered) got.push_back(d);
        }
        for (const auto &pkt : client.packets({})) server.receive(pkt, t);
        for (const AckEvent &e : server.take_ack_events()) view.on_ack(e);
    };
    for (int i = 0; i < 400 && view.channel_of(wiz.id) == 0; ++i) pass();
    for (int i = 0; i < 20; ++i) pass();
}
inline void check_open_carries_health(PlayerWorld &world) {
    const world::WorldActor *wiz = nullptr;
    world.registry().for_each([&](const world::WorldActor &a) {
        if (!wiz && a.kind == world::ActorKind::Npc && a.npc.spawn_ref >= spawns::kNodeRefBase &&
            spawns::blueprint_leaf(a.npc.archetype) == "Guard_Wizard") {
            wiz = &a;
        }
    });
    CHECK(wiz != nullptr, "(a guard wizard stands in the town)");
    if (!wiz) return;
    const conn::OuterHeader outer{0x500c7696u, 0, 5};
    const conn::CustomField cf{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d};
    const Clock::time_point t0{std::chrono::seconds(5000)};
    Connection server{ConnectionConfig{outer, cf, 14265, 15870}, t0};
    fake::Client client{outer, cf, 14265, 15870};
    ActorView view(world.registry(), ActorView::Config{}, [](uint32_t i) { return i >= 3 && i <= 6; });
    std::vector<fake::Delivered> got;
    pump_wizard_view(view, server, client, *wiz, got);
    const uint32_t channel = view.channel_of(wiz->id);
    CHECK(channel != 0, "the view opens the guard wizard for a client standing at it");
    std::map<int64_t, float> seen;
    for (const fake::Delivered &d : got) {
        if (d.channel != channel || !d.open) continue;
        seen = stat_items_in_chain(d, *wiz);
        break;
    }
    std::printf("    the open of the guard wizard on channel %u carries %zu stat items\n", channel, seen.size());
    CHECK(has_full_health(seen), "the open a fake client receives for a guard "
                                 "wizard carries Health == MaxHealth > 0");
}
inline bool init_node_pop_db(std::string &error) {
    g_env.db = content::ContentDb::open(db_path(), error);
    if (!g_env.db) {
        std::printf("  [FAIL] the real content.db does not open (%s): %s\n", db_path().c_str(), error.c_str());
        return false;
    }
    auto pop = g_env.db->population_data(error);
    if (!pop) {
        std::printf("  [FAIL] the population chain does not read: %s\n", error.c_str());
        return false;
    }
    g_env.pop = std::make_shared<const content::PopulationData>(std::move(*pop));
    g_env.captured = spawns::CapturedData::load(error);
    if (!g_env.captured) {
        std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
        return false;
    }
    if (!g_env.table.load(motion_path(), error)) {
        std::printf("  [FAIL] the Blueprint table does not load (%s): %s\n", motion_path().c_str(), error.c_str());
        return false;
    }
    g_env.generic.emplace(*g_env.captured, &g_env.table, spawns::GenericConfig{});
    g_env.templates = spawns::TemplateSource{&*g_env.captured, &*g_env.generic};
    return true;
}
inline int init_node_pop_env() {
    spdlog::set_level(std::getenv("AOC_TEST_LOG") ? spdlog::level::debug : spdlog::level::off);
    std::string error;
    if (!init_node_pop_db(error)) return 2;
    g_env.nodes = fresh_nodes();
    g_env.wire = world::NodeWire::load(db_path(), *g_env.nodes, error);
    if (!g_env.wire) {
        std::printf("  [FAIL] the node wire does not load: %s\n", error.c_str());
        return 2;
    }
    g_env.shipped_nodes = fresh_nodes(true);
    g_env.shipped_wire = world::NodeWire::load(db_path(), *g_env.shipped_nodes, error);
    if (!g_env.shipped_wire) {
        std::printf("  [FAIL] the node wire of the shipped seed does not load: %s\n", error.c_str());
        return 2;
    }
    g_env.data = spawns::NodePopulationData::load(db_path(), error);
    if (!g_env.data) {
        std::printf("  [FAIL] the node population data does not load: %s\n", error.c_str());
        return 2;
    }
    g_env.base.hour = 11.0;
    g_env.shown = g_env.captured->npc_leaves();
    std::string looks_error;
    const auto combat = g_env.db->combat_rows(looks_error);
    if (combat)
        g_env.looks = spawns::NpcLooks::load(fs::path(motion_path()).parent_path().string(), &g_env.table, &*combat,
                                             &*g_env.captured, looks_error);
    if (!g_env.looks) {
        std::printf("  [FAIL] the people's looks do not load (%s): %s\n", motion_path().c_str(), looks_error.c_str());
        return 2;
    }
    g_env.base.looks_ok = [](const content::NpcAssetRow &a) { return g_env.looks->can_dress(a); };
    return 0;
}
struct PlotFolk {
    size_t total = 0, villager_f = 0, hammer = 0, shovel = 0;
};
inline void classify_plot_folk(PlotFolk &f, const std::string &id) {
    ++f.total;
    if (id.find("Villager_M_Hammer") != std::string::npos) {
        ++f.hammer;
        return;
    }
    if (id.find("Villager_M_Shovel") != std::string::npos) {
        ++f.shovel;
        return;
    }
    if (id.find("Villager_F") != std::string::npos || id.find("Villager_M") != std::string::npos) ++f.villager_f;
}
inline PlotFolk folk_at(const std::vector<spawns::NodeSpawn> &list, int64_t plot) {
    PlotFolk f;
    for (const spawns::NodeSpawn &s : list) {
        if (plot == 0 || s.plot != plot || !s.spawn.asset) continue;
        classify_plot_folk(f, s.spawn.asset->name + " " + s.spawn.asset->archetype_path);
    }
    return f;
}
inline std::string diff_golden_case(const tests::NodePopCase &c, const spawns::NodePopStats &st,
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
inline bool is_service_leaf(const content::NpcInstanceRow *inst, const std::string &leaf) {
    static const char *const kServiceWords[] = {"Vendor",          "Clerk",     "Manager", "_KAE",
                                                "Salesman",        "Caravan",   "Station", "Board",
                                                "ServiceBuilding", "TurnInBox", "Mailbox", "ServiceDesk"};
    if (inst && inst->vendor != 0) return true;
    for (const char *w : kServiceWords) {
        if (leaf.find(w) != std::string::npos) return true;
    }
    return false;
}
inline void record_workshop_wanderer(WalkerCount &c, const world::WorldActor &a, const npc::NpcMotionSetup &s,
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
inline void record_workshop_role(WalkerCount &c, const WalkerSpawnArgs &args, const npc::NpcMotionSetup &s,
                                 const std::string &leaf) {
    if (args.plot != 0 && s.ambient == npc::Ambient::Wander)
        record_workshop_wanderer(c, args.actor, s, args.node, args.plot);
    if (!args.inst || args.inst->name.rfind("Node_Filler_Villager", 0) != 0) return;
    if (s.ambient == npc::Ambient::Wander && leaf.find("Shovel") == std::string::npos) ++c.workers_walk;
    if (s.ambient == npc::Ambient::Stationary && leaf.find("Shovel") != std::string::npos) ++c.farmers_stand;
}
inline void record_walker_service(WalkerCount &c, const WalkerSpawnArgs &args, const npc::NpcMotionSetup &s,
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
inline void record_walker_patrol(WalkerCount &c, const world::CityNodeInfo &node, const npc::NpcMotionSetup &s,
                                 const std::string &leaf) {
    ++c.patrol;
    (npc::is_guard_leaf(leaf) ? c.guards_patrol : c.villager_patrol)++;
    if (leaf.find("Carrying") != std::string::npos) ++c.carriers_walk;
    if (std::hypot(s.route.front().x - node.x, s.route.front().y - node.y) > 60000.0) ++c.patrol_unplaced;
}
inline void record_walker_spawn(WalkerCount &c, const WalkerSpawnArgs &args) {
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
} // namespace aoc::worldd::test_node_pop
