// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.

#include "test_world_node_pop_common.h"

using namespace aoc::worldd;
using namespace aoc::worldd::test_node_pop;

namespace {

struct RoadFile {
    struct Road {
        int64_t zoi = 0;
        int tiers[7] = {0};
    };
    std::vector<Road> roads;
};
static bool parse_road_level_line(const std::string &line, RoadFile::Road &r) {
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
static bool read_roads_levels(const std::string &path, RoadFile &out) {
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
static void extract_stat_array(const repl::FastArrayState &arr, std::map<int64_t, float> &out) {
    for (const repl::FastArrayItem &it : arr.items()) {
        const auto v = it.values.values.find(1);
        const auto *s = v == it.values.values.end() ? nullptr : v->second.get_if<repl::StatInt32Inst>();
        if (s) out[s->record] = std::bit_cast<float>(s->value);
    }
}
static std::map<int64_t, float> stat_items(const world::WorldActor &a) {
    std::map<int64_t, float> out;
    for (const repl::Subobject &sub : a.actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAoCStatsComponent") continue;
        const auto *arr =
            const_cast<repl::FastArrays &>(sub.obj.arrays).get(*sub.obj.schema, "StatRepInt32EveryoneProxy");
        if (arr) extract_stat_array(*arr, out);
    }
    return out;
}
static bool has_full_health(const std::map<int64_t, float> &items) {
    const auto h = items.find(content::kGuidHealth);
    const auto m = items.find(content::kGuidMaxHealth);
    return h != items.end() && m != items.end() && m->second > 0 && h->second == m->second;
}
static std::optional<std::vector<repl::BlockView>> find_chain_blocks(const fake::Delivered &d,
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
static void collect_fast_array_stats(const std::vector<repl::FastArrayField> &fields, uint32_t field_idx,
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
static bool read_comp_stat_block(const fake::Delivered &d, const repl::Subobject &comp, const repl::BlockView &b,
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
static std::map<int64_t, float> stat_items_in_chain(const fake::Delivered &d, const world::WorldActor &a) {
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
static void check_town_health(PlayerWorld &world, const char *when) {
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
static void pump_wizard_view(ActorView &view, Connection &server, fake::Client &client, const world::WorldActor &wiz,
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
static void check_open_carries_health(PlayerWorld &world) {
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

struct HeightKept {
    size_t npcs = 0, moved = 0, open_moved = 0;
    double worst = 0;
};
static HeightKept height_kept(const world::ActorRegistry &reg, const std::map<uint64_t, double> &data_z) {
    HeightKept h;
    reg.for_each([&](const world::WorldActor &a) {
        const auto it = data_z.find(a.id);
        if ((a.kind != world::ActorKind::Npc && a.kind != world::ActorKind::Station) || it == data_z.end()) return;
        ++h.npcs;
        const double d = std::fabs(a.z - it->second);
        h.worst = std::max(h.worst, d);
        if (d > 1.0) ++h.moved;
        if (a.actor.open.spawn.location &&
            std::fabs(aoc::protocol::repl::dequantize_vector(*a.actor.open.spawn.location, 10).z - it->second) > 1.0) {
            ++h.open_moved;
        }
    });
    return h;
}

static size_t count_waiting_actors(const world::ActorRegistry &reg, const nav::AreaNav &nav) {
    size_t w = 0;
    reg.for_each([&](const world::WorldActor &a) {
        w +=
            (a.kind == world::ActorKind::Npc || a.kind == world::ActorKind::Station) && !nav.ready_at(a.x, a.y) ? 1 : 0;
    });
    return w;
}

static void test_pending_chunks(world::CityNodes &synth, nav::AreaNav &nav, Rig &rig, npc::NpcWorld &npcs,
                                const std::map<uint64_t, double> &data_z) {
    std::vector<nav::ChunkKey> arrived;
    nav.set_interest({{500.0, 500.0}});
    for (int i = 0; i < 800; ++i) {
        const auto got = nav.pump(50);
        arrived.insert(arrived.end(), got.begin(), got.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    npcs.chunks_resident(arrived);
    CHECK(!arrived.empty() && npcs.pending_ground() == count_waiting_actors(rig.registry, nav),
          "the chunks that arrived ground the NPCs standing in them: the count "
          "is the NPCs still outside the window");
    {
        const HeightKept h = height_kept(rig.registry, data_z);
        char msg[240];
        std::snprintf(msg, sizeof(msg),
                      "bowl: after the chunks arrived, %zu people still stand at "
                      "their data height (%zu moved, %zu "
                      "opens moved, the most %.1f cm)",
                      h.npcs - h.moved, h.moved, h.open_moved, h.worst);
        CHECK(h.npcs == npcs.size() && h.moved == 0 && h.open_moved == 0, msg);
    }
    set_level(synth, kJoeva, 4);
    rig.pops->tick(2.0);
    CHECK(rig.pops->actors_of(kJoeva).size() == golden_npcs("Verra_RVR_Joeva", 4) &&
              npcs.pending_ground() == count_waiting_actors(rig.registry, nav),
          "Village -> Town: the new NPCs wait or stand on the ground, by their "
          "chunk");
    {
        const HeightKept h = height_kept(rig.registry, data_z);
        char msg[240];
        std::snprintf(msg, sizeof(msg),
                      "bowl: the people the level change brought keep their data "
                      "height too (%zu of %zu moved, the most %.1f cm)",
                      h.moved, h.npcs, h.worst);
        CHECK(h.npcs == npcs.size() && h.moved == 0 && h.open_moved == 0, msg);
    }
}

static void test_pending() {
    std::printf("the ground's window: NPCs that wait for their chunk\n");
    std::string error;
    world::CityNodeInfo j = *g_env.nodes->by_guid(kJoeva);
    j.x = 500;
    j.y = 500;
    j.z = 11000;
    std::vector<world::CityZoneInfo> zones{*g_env.nodes->zone_by_guid(j.zoi_guid)};
    world::CityNodes synth({j}, zones);
    set_level(synth, kJoeva, 4);
    auto wire = world::NodeWire::load(db_path(), synth, error);
    if (!wire) {
        CHECK(false, "the node wire loads for a registry of one node");
        return;
    }
    const std::string path = navfix::write_chunk_file("nodepop");
    auto nav = nav::AreaNav::load(path, &error);
    if (!nav) {
        CHECK(false, "the chunked fixture opens");
        return;
    }
    nav->load_around(0, 0, 0);
    world::ActorRegistry *reg = nullptr;
    spawns::AreaConfig everywhere{0, 0, 1.0e8};
    std::unique_ptr<npc::NpcWorld> npcs;
    npc::NpcMotionSetup stand;
    stand.foot_offset = 88;
    std::map<uint64_t, double> data_z;
    Rig rig(synth, *wire, everywhere, [&](uint64_t id, const content::NpcInstanceRow *, const content::NpcRouteRow *) {
        if (reg) data_z[id] = reg->find(id)->z;
        if (npcs) npcs->add(id, stand);
    });
    reg = &rig.registry;
    npcs = std::make_unique<npc::NpcWorld>(*reg, nav.get());
    const size_t made = rig.pops->start(0.0);
    const size_t waiting = count_waiting_actors(*reg, *nav);
    CHECK(made == golden_npcs("Verra_RVR_Joeva", 4) && npcs->size() == made,
          "a town beside a chunked ground: every NPC is under the NPC world's "
          "care");
    CHECK(waiting > 0 && waiting < made && npcs->pending_ground() == waiting,
          "the NPCs whose chunk is not resident wait for it (the count is the "
          "NPC world's): the others are grounded at "
          "once");
    const HeightKept h0 = height_kept(*reg, data_z);
    CHECK(h0.npcs == made && h0.moved == 0 && h0.open_moved == 0,
          "bowl: all people keep their data height while waiting for their chunk");
    set_level(synth, kJoeva, 3);
    rig.pops->tick(1.0);
    const size_t village = select_in_area(synth, *wire, j, 3, everywhere).size();
    const size_t town = rig.pops->actors_of(kJoeva).size(), held = npcs->size();
    CHECK(town == village && held == town && npcs->pending_ground() == count_waiting_actors(*reg, *nav),
          "Town -> Village: the NPC world holds exactly the town's people and "
          "its pending count follows");
    test_pending_chunks(synth, *nav, rig, *npcs, data_z);
    npcs.reset();
    nav.reset();
    std::error_code ec;
    fs::remove(path, ec);
}

static std::pair<size_t, size_t> check_world_road_tiers(const PlayerWorld &world, const world::CityNodes &nodes,
                                                        const RoadFile &roads, int64_t joeva_zoi) {
    size_t bad = 0, joeva_roads = 0;
    for (size_t i = 0; i < roads.roads.size(); ++i) {
        const RoadFile::Road &r = roads.roads[i];
        const world::CityZoneInfo *z = nodes.zone_by_guid(r.zoi);
        const int level = (z && z->node_guid != 0) ? nodes.level_of(z->node_guid) : 0;
        if (r.zoi == joeva_zoi) ++joeva_roads;
        if (world.roads().tier_of_road(i) != r.tiers[level]) ++bad;
    }
    return {bad, joeva_roads};
}

static void test_world_roads_and_respawn(PlayerWorld &world, world::CityNodes &nodes, const world::CityNodeInfo &joeva,
                                         const spawns::AreaConfig &circle, size_t before_npcs) {
    RoadFile roads;
    const bool have_roads = read_roads_levels(NODES_ROADS_LEVELS_PATH, roads);
    if (have_roads) {
        const auto [bad, joeva_roads] = check_world_road_tiers(world, nodes, roads, joeva.zoi_guid);
        CHECK(bad == 0 && joeva_roads > 0, "every road is worth what its zone's node level gives it (Joeva's "
                                           "roads at Town)");
    }
    set_level(nodes, kJoeva, 3);
    world.tick_world(10.0);
    const spawns::NodePopulations::Entry *e = world.node_people()->entry_of(kJoeva);
    const size_t expected_v3 = select_in_area(nodes, *g_env.wire, joeva, 3, circle).size();
    CHECK(e && e->level == 3 && e->npcs + e->stations == expected_v3 &&
              people_of(world.registry()) == before_npcs + e->npcs + e->stations,
          "tick_world follows Joeva's level change: Village's people, the "
          "registry holds the area's NPCs and these");
    if (have_roads) {
        const auto [bad, joeva_roads] = check_world_road_tiers(world, nodes, roads, joeva.zoi_guid);
        CHECK(bad == 0 && joeva_roads > 0, "...and the roads of Joeva's zone changed with it");
    }
    uint64_t victim = 0;
    uint32_t ref = 0;
    world.registry().for_each([&](const world::WorldActor &a) {
        if (a.npc.spawn_ref >= spawns::kNodeRefBase && victim == 0 && a.npc.vendor_guid == 0) {
            victim = a.id;
            ref = a.npc.spawn_ref;
        }
    });
    const int64_t asset = world.registry().find(victim)->npc.asset_guid;
    world.registry().remove(victim);
    const uint64_t again = world.respawn_npc(ref, 11.0);
    const world::WorldActor *back = world.registry().find(again);
    CHECK(again != 0 && back && back->npc.spawn_ref == ref && back->npc.asset_guid == asset,
          "respawn_npc brings a node NPC back through the node manager");
    CHECK(back && has_full_health(stat_items(*back)), "...and the NPC that came back has its Health, full");
    check_town_health(world, "after the level change and the respawn");
    CHECK(world.respawn_npc(spawns::kNodeRefBase + 7777777u, 11.0) == 0,
          "...and a reference that is no slot is nothing");
}

static void test_world() {
    std::printf("PlayerWorld: start_node_people, tick_world, respawn_npc and the "
                "roads\n");
    std::string error;
    aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
    PlayerWorld world(&*g_env.db, "", bs::GameStateMode::Tier3, 60, guids);
    CHECK(world.ready(), "a PlayerWorld on the real content.db is ready");
    if (!world.ready()) return;
    auto nodes = fresh_nodes();
    spawns::NodePopulations::Config cfg;
    CHECK(world.start_node_people(cfg) == 0 && world.node_people() == nullptr,
          "without an area, the nodes' wire and the population data there are no "
          "people");
    world.set_spawn_context(g_env.base);
    CHECK(world.load_blueprints(motion_path(), error), "the Blueprint table loads");
    world.set_city_nodes(nodes.get());
    world.set_node_wire(g_env.wire.get());
    world.set_node_population_data(g_env.data.get());
    const world::CityNodeInfo *joeva = nodes->by_guid(kJoeva);
    spawns::AreaConfig circle;
    circle.center_x = joeva->x;
    circle.center_y = joeva->y;
    circle.radius_cm = 150000.0;
    CHECK(world.load_area(circle, error), "a 1.5 km circle around Joeva loads");
    RoadFile roads;
    const bool have_roads = read_roads_levels(NODES_ROADS_LEVELS_PATH, roads);
    world.set_road_default_level(0);
    if (have_roads) {
        CHECK(world.load_roads(NODES_ROADS_LEVELS_PATH, error) && world.roads().level_roads() == roads.roads.size() &&
                  roads.roads.size() > 100,
              "the by-level roads load into the world (145 roads)");
    }
    const size_t before_npcs = people_of(world.registry());
    const size_t made = world.start_node_people(cfg);
    CHECK(made == select_in_area(*nodes, *g_env.wire, *joeva, 4, circle).size() && made > 400 && world.node_people() &&
              people_of(world.registry()) == before_npcs + made,
          "start_node_people: the circle holds Joeva only (a Town): the oracle's "
          "number of NPCs, added to the area's");
    CHECK(world.start_node_people(cfg) == 0, "once");
    size_t vendors = 0, with_ref = 0;
    world.registry().for_each([&](const world::WorldActor &a) {
        if (a.npc.spawn_ref >= spawns::kNodeRefBase) {
            ++with_ref;
            vendors += a.npc.vendor_guid != 0 ? 1 : 0;
        }
    });
    CHECK(with_ref == made && vendors > 0, "the town's NPCs carry node references, and some are vendors");
    check_town_health(world, "after start_node_people");
    check_open_carries_health(world);
    test_world_roads_and_respawn(world, *nodes, *joeva, circle, before_npcs);
}

static world::RoadMap::Polyline by_level_road(int64_t zoi, std::array<int, 7> tiers, std::array<double, 7> halves,
                                              std::array<double, 4> xy) {
    world::RoadMap::Polyline r;
    r.by_level = true;
    r.zoi = zoi;
    r.tiers = tiers;
    r.halves = halves;
    r.xy = {xy[0], xy[1], xy[2], xy[3]};
    return r;
}

static void test_roads_apply_levels(world::RoadMap &m, const world::RoadMap::Polyline &plain) {
    CHECK(m.add(by_level_road(11, {1, 1, 2, 3, 4, 4, 4}, {500, 500, 500, 500, 600, 900, 900}, {0, 0, 10000, 0}), 0),
          "a by-level road is added");
    CHECK(m.add(by_level_road(22, {-1, -1, -1, 2, 4, 4, 4}, {0, 0, 0, 500, 500, 500, 500}, {0, 5000, 10000, 5000}), 0),
          "...and one with nothing for a mount below Village");
    CHECK(m.add(plain), "...and a plain road");
    CHECK(m.level_roads() == 2 && m.roads() == 3 && m.level_of_road(0) == 0 && m.level_of_road(2) == -1,
          "two of the three follow a level; they start at level 0");
    CHECK(m.tier_at(5000, 0) == 1 && m.tier_at(5000, 5000) == -1 && m.tier_at(5000, 9000) == 3,
          "level 0: the artery is T1, the capillary gives a mount nothing, the "
          "plain road T3");
    CHECK(m.tier_of_road(0) == 1 && m.tier_of_road(1) == -1 && m.tier_of_road(2) == 3, "(the roads' tiers by index)");
    const auto level_of = [](int64_t zoi) { return zoi == 11 ? 4 : zoi == 22 ? 3 : -1; };
    CHECK(m.apply_levels(level_of, 0) == 2, "two roads change when their zones' nodes level up");
    CHECK(m.tier_at(5000, 0) == 4 && m.tier_at(5000, 5000) == 2 && m.tier_at(5000, 9000) == 3,
          "Town: the artery is T4, the capillary T2 (Village), the plain road is "
          "untouched");
    CHECK(m.tier_at(5000, 550) == 4 && m.tier_at(5000, 650) == -1,
          "...and an artery's half width grows with its level (600 cm at Town)");
    CHECK(m.apply_levels(level_of, 0) == 0, "nothing changes the second time");
    CHECK(m.apply_levels([](int64_t) { return 6; }, 0) == 2 && m.tier_at(5000, 700) == 4 && m.tier_at(5000, 950) == -1,
          "Metropolis: 900 cm of half width");
    CHECK(m.apply_levels([](int64_t) { return -1; }, 2) == 2 && m.level_of_road(0) == 2 && m.tier_at(5000, 0) == 2 &&
              m.tier_at(5000, 5000) == -1,
          "a zone with no node takes the default level (2): the capillary has "
          "nothing there");
    CHECK(m.apply_levels(nullptr, 0) == 1 && m.tier_at(5000, 0) == 1 && m.level_of_road(1) == 0,
          "(no function: every road at the default level; the capillary's level "
          "changed, its tier did not)");
}

static void test_roads_validation_and_bounds(const world::RoadMap::Polyline &plain) {
    world::RoadMap bad;
    CHECK(!bad.add(by_level_road(1, {5, 0, 0, 0, 0, 0, 0}, {500, 500, 500, 500, 500, 500, 500}, {0, 0, 1, 1})), "a tier above 4 is refused");
    CHECK(!bad.add(by_level_road(1, {-1, -1, -1, -1, -1, -1, -1}, {0, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 1})), "a road with nothing at any level is refused");
    CHECK(!bad.add(by_level_road(1, {1, 1, 1, 1, 1, 1, 1}, {500, 500, 0, 500, 500, 500, 500}, {0, 0, 1, 1})), "a level with a tier and no width is refused");
    CHECK(!bad.add(by_level_road(1, {1, 1, 1, 1, 1, 1, 1}, {500, 500, 500, 500, 500, 500, 500}, {0, 0, 1, 1}), 7), "a start level beyond Metropolis is refused");
    auto too_large = plain;
    too_large.xy[0] = 1e300;
    CHECK(!bad.add(too_large), "finite coordinates outside float storage are refused before indexing");
    too_large = plain;
    too_large.half_width_cm = 1e300;
    CHECK(!bad.add(too_large), "finite widths outside float storage are refused before indexing");
    CHECK(!bad.add(by_level_road(1, {1, 1, 1, 1, 1, 1, 1}, {500, 500, 1e300, 500, 500, 500, 500}, {0, 0, 1, 1})), "a future active level's width must also fit float storage");
    CHECK(bad.roads() == 0, "(none was added)");
    world::RoadMap long_road;
    CHECK(long_road.add(by_level_road(11, {1, 1, 2, 3, 4, 4, 4}, {500, 500, 500, 500, 500, 500, 500}, {0, 0, 1e30, 0})), "a long road that fits float storage is accepted without indexing its enormous box");
    CHECK(long_road.tier_at(1e25, 0) == 1 && long_road.tier_at(1e25, 501) == -1, "a road outside the integer grid is queried directly with its proper width");
    CHECK(long_road.apply_levels([](int64_t) { return 4; }, 0) == 1 && long_road.tier_at(1e25, 0) == 4, "an exceptionally long road still follows its node's level");
    auto local = plain;
    local.xy = {0, 0, 10000, 0};
    CHECK(long_road.add(local) && long_road.tier_at(5000, 0) == 4, "indexed and exceptional segments are queried together, keeping the highest tier");
    CHECK(long_road.apply_levels(nullptr, 0) == 1 && long_road.tier_at(5000, 0) == 3 && long_road.tier_at(1e25, 0) == 1, "an indexed road can provide the winning tier when the exceptional road's node level falls");
    world::RoadMap many_cells;
    auto broad_box = local;
    broad_box.xy = {0, 0, 1e8, 0};
    CHECK(many_cells.add(broad_box) && many_cells.tier_at(1e7, 0) == 3, "a segment covering more than 4096 valid grid cells is queried without filling them all");
    world::RoadMap wide_road;
    auto wide = local;
    wide.half_width_cm = 1e20;
    CHECK(wide_road.add(wide) && wide_road.tier_at(0, 1e19) == 3, "a wide road that fits float storage is accepted and queried without allocating billions of cells");
    world::RoadMap inactive_width;
    CHECK(inactive_width.add(by_level_road(1, {-1, 1, 1, 1, 1, 1, 1}, {1e300, 500, 500, 500, 500, 500, 500}, {0, 0, 10000, 0})) &&
              inactive_width.bounds().max_y == 500 && inactive_width.tier_at(5000, 0) == -1 &&
              inactive_width.apply_levels(nullptr, 1) == 1 && inactive_width.tier_at(5000, 0) == 1,
          "widths of inactive levels do not enlarge the index, while later active levels still work");
}
static void test_roads_tsv_files() {
    const fs::path dir = fs::temp_directory_path() / "world_node_pop_roads";
    fs::create_directories(dir);
    const fs::path file = dir / "roads_levels.tsv";
    {
        std::ofstream f(file);
        f << "# roads v2: test\nroadn\t11\t1,1,2,3,4,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\nroad\t3\t500\t0,9000\t10000,9000\n";
    }
    world::RoadMap fm;
    std::string error;
    CHECK(fm.load(file.string(), error, 4) && fm.roads() == 2 && fm.level_roads() == 1 && fm.level_of_road(0) == 4 &&
              fm.tier_at(5000, 0) == 4 && fm.tier_at(5000, 9000) == 3,
          "a file with a roadn line and a road line loads; the by-level road starts at the level the loader was given (Town)");
    {
        std::ofstream f(file);
        f << "roadn\t11\t1,1,2,3,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n";
    }
    world::RoadMap short_file;
    CHECK(!short_file.load(file.string(), error) && error.find("numbers each") != std::string::npos, "a roadn line with six tiers is refused");
    {
        std::ofstream f(file);
        f << "roadn\tx\t1,1,2,3,4,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n";
    }
    CHECK(!short_file.load(file.string(), error), "...and one whose zone is no number");
    for (const char *line : {"road\t3\t500\tnot-a-number,0\t10000,0\n", "road\t3\t500\t0,0\t10000,0junk\n",
                             "road\t4294967296\t500\t0,0\t10000,0\n",
                             "roadn\t-1\t1,1,2,3,4,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n",
                             "roadn\t11\t1.5,1,2,3,4,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n",
                             "roadn\t11\tnan,1,2,3,4,4,4\t500,500,500,500,600,900,900\t0,0\t10000,0\n"}) {
        {
            std::ofstream f(file);
            f << line;
        }
        world::RoadMap malformed;
        CHECK(!malformed.load(file.string(), error) && !error.empty(), "malformed road coordinates, tiers and zones are refused without numeric coercion");
    }
    CHECK(fm.tier_at(1e300, 1e300) == -1 && fm.tier_at(-1e300, -1e300) == -1, "queries far outside the road bounds avoid out-of-range cell conversions");
    fs::remove_all(dir);
}
static void sample_one_level_line(const std::string &line, const world::RoadMap &wilderness, const world::RoadMap &rm,
                                  size_t &sampled, size_t &same) {
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '\t')) f.push_back(cell);
    for (size_t i = 3; i < f.size(); i += 6) {
        const size_t comma = f[i].find(',');
        const double x = std::atof(f[i].substr(0, comma).c_str());
        const double y = std::atof(f[i].substr(comma + 1).c_str());
        ++sampled;
        same += wilderness.tier_at(x, y) == rm.tier_at(x, y) ? 1 : 0;
    }
}
static void check_wilderness_roads_match(const world::RoadMap &rm) {
    const std::string one_level =
        std::string(NODES_ROADS_LEVELS_PATH).substr(0, std::string(NODES_ROADS_LEVELS_PATH).rfind("roads_levels.tsv")) +
        "roads.tsv";
    std::string error;
    world::RoadMap wilderness;
    if (!wilderness.load(one_level, error)) return;
    size_t sampled = 0, same = 0;
    std::ifstream in(one_level);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] != '#') sample_one_level_line(line, wilderness, rm, sampled, same);
    }
    CHECK(sampled > 100 && same == sampled, "at Wilderness the by-level map and the one-level roads.tsv give the "
                                            "same tier along every road");
}
static void test_roads() {
    std::printf("roads by node level\n");
    world::RoadMap m;
    world::RoadMap::Polyline plain;
    plain.tier = 3;
    plain.half_width_cm = 500;
    plain.xy = {0, 9000, 10000, 9000};
    test_roads_apply_levels(m, plain);
    test_roads_validation_and_bounds(plain);
    test_roads_tsv_files();
    RoadFile real;
    if (!read_roads_levels(NODES_ROADS_LEVELS_PATH, real)) return;
    std::string error;
    world::RoadMap rm;
    CHECK(rm.load(NODES_ROADS_LEVELS_PATH, error, 0) && rm.roads() == real.roads.size() &&
              rm.level_roads() == rm.roads() && rm.roads() > 100,
          "the real roads_levels.tsv loads: every road follows a level");
    check_wilderness_roads_match(rm);
    const world::CityNodeInfo *joeva = g_env.nodes->by_guid(kJoeva);
    rm.apply_levels([&](int64_t zoi) { return zoi == joeva->zoi_guid ? 4 : 0; }, 0);
    size_t wrong = 0, joeva_roads = 0, changed = 0;
    for (size_t i = 0; i < real.roads.size(); ++i) {
        const int want = real.roads[i].zoi == joeva->zoi_guid ? real.roads[i].tiers[4] : real.roads[i].tiers[0];
        wrong += rm.tier_of_road(i) != want ? 1 : 0;
        if (real.roads[i].zoi == joeva->zoi_guid) {
            ++joeva_roads;
            changed += real.roads[i].tiers[4] != real.roads[i].tiers[0] ? 1 : 0;
        }
    }
    CHECK(wrong == 0 && joeva_roads > 0 && changed > 0,
          "Joeva's zone at Town: its roads are worth their Town tier (some are "
          "worth more than at Wilderness), every "
          "other road is untouched");
}
static bool compute_bowl_floor_person(PlayerWorld &world, double &median, double &px, double &py) {
    std::vector<double> overs;
    std::vector<std::array<double, 3>> people;
    world.registry().for_each([&](const world::WorldActor &a) {
        if (a.kind != world::ActorKind::Npc || a.npc.spawn_ref < spawns::kNodeRefBase) return;
        double t = 0;
        if (!world.nav()->terrain_height(a.npc.home_x, a.npc.home_y, &t)) return;
        const double over = a.npc.home_z - t;
        if (over > world::TownFloor::kBowlCm) {
            overs.push_back(over);
            people.push_back({a.npc.home_x, a.npc.home_y, over});
        }
    });
    std::sort(overs.begin(), overs.end());
    median = overs.empty() ? 0.0 : overs[overs.size() / 2];
    const auto on_floor = std::find_if(people.begin(), people.end(), [&](const std::array<double, 3> &p) {
        return std::fabs(p[2] - median) <= world::TownFloor::kSameCm;
    });
    const bool ok = !overs.empty() && on_floor != people.end();
    CHECK(ok, "(the people stand over the plain, and some of them at the "
              "median's height)");
    if (ok) {
        px = (*on_floor)[0];
        py = (*on_floor)[1];
    }
    return ok;
}
static void test_bowl_world_logging(PlayerWorld &world, world::CityNodes &synth) {
    auto count_lines = [](const std::string &text) {
        size_t n = 0;
        for (size_t at = text.find("[towns]"); at != std::string::npos; at = text.find("[towns]", at + 1)) ++n;
        return n;
    };
    std::ostringstream captured;
    auto logger =
        std::make_shared<spdlog::logger>("towns_check", std::make_shared<spdlog::sinks::ostream_sink_mt>(captured));
    logger->set_level(spdlog::level::info);
    const auto previous = spdlog::default_logger();
    spdlog::set_default_logger(logger);
    world.tick_world(100.0);
    world.tick_world(105.0);
    world.tick_world(110.0);
    const std::string first = captured.str();
    set_level(synth, kJoeva, 4);
    world.tick_world(120.0);
    world.tick_world(125.0);
    const std::string second = captured.str();
    spdlog::set_default_logger(previous);
    CHECK(count_lines(first) == 1 && first.find("Joeva (Wilderness)") != std::string::npos,
          "the towns' check makes one line for the town (Wilderness) once its "
          "numbers are still, and not another while "
          "they stay so");
    CHECK(count_lines(second) == 2 && second.find("Joeva (Town)") != std::string::npos,
          "after a level change: one more line, for the new level, as soon as "
          "its people are on the ground");
    CHECK(first.find(" people, ") != std::string::npos &&
              first.find("on a floor within 30 cm of where the data puts them") != std::string::npos &&
              first.find("would walk if they had ground") != std::string::npos,
          "the line counts the people on a floor, on a floor farther away, "
          "without a floor, with no walkable ground "
          "near, and those that would walk");
}
static void test_bowl_world_queries(PlayerWorld &world, world::CityNodes &synth, double median, double px, double py) {
    const auto terrain = [&](double x, double y) {
        double t = 0;
        world.nav()->terrain_height(x, y, &t);
        return t;
    };
    const double base = terrain(px, py);
    const auto town = world.town_ground_at(px, py);
    char msg[260];
    std::snprintf(msg, sizeof(msg),
                  "the town's floor at a person of its floor: the baked ground "
                  "there (%.0f) + the median %.0f cm of "
                  "the people's heights over it",
                  base, median);
    CHECK(town && town->node == kJoeva && std::fabs(town->z - (base + median)) < 0.01, msg);
    const auto ground = world.ground_at(px, py);
    CHECK(ground && std::fabs(*ground - (base + median + PlayerWorld::kBowlSlackCm)) < 0.01,
          "ground_at there: the town's floor + the slack (a GM teleport and the "
          "saved rider land on it), not the baked "
          "plain");
    world::Place place;
    place.name = "Joeva";
    place.x = px;
    place.y = py;
    place.z = 11000;
    CHECK(std::fabs(world.place_height(place) -
                    (base + median + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm)) < 0.01,
          "a GM teleport to a place in the bowl is put on the town's floor (+ "
          "the capsule), not 50 m under it");
    double plain = 0;
    const auto far_ground = world.ground_at(25000, 500);
    CHECK(world.nav()->terrain_height(25000, 500, &plain) && !world.town_ground_at(25000, 500) && far_ground &&
              std::fabs(*far_ground - plain) < 1e-9,
          "far from the town: the baked ground, as before");
    const auto lifted = world.lift_out_of_bowl(px, py, base + 98.0);
    CHECK(lifted && lifted->node == kJoeva &&
              std::fabs(lifted->z - (base + median + 98.0 + PlayerWorld::kBowlSlackCm)) < 0.01 &&
              std::fabs(lifted->under_cm - median) < 1.0,
          "a character saved on the baked plain under the town is put on the "
          "town's floor (+ capsule + slack)");
    CHECK(!world.lift_out_of_bowl(px, py, base + median + 98.0) &&
              !world.lift_out_of_bowl(px, py, base + median + 98.0 - 1400.0),
          "...not one that stands on the floor, nor one less than 15 m under it "
          "(a terrace)");
    CHECK(!world.lift_out_of_bowl(25000, 500, terrain(25000, 500) + 98.0),
          "...nor one saved on the open ground far from the town");
    const size_t town_persons = world.town_floor().floor_persons(kJoeva);
    set_level(synth, kJoeva, 0);
    world.tick_world(5.0);
    world.town_ground_at(px, py);
    CHECK(world.town_floor().floor_persons(kJoeva) != town_persons,
          "after a level change the floor is rebuilt from the people the new "
          "level has");
    test_bowl_world_logging(world, synth);
}
static void test_bowl_world() {
    std::printf("PlayerWorld in a bowl (the fixture's plain at 1,000; the town's "
                "people 100 m over it)\n");
    std::string error;
    world::CityNodeInfo j = *g_env.nodes->by_guid(kJoeva);
    j.x = 500;
    j.y = 500;
    j.z = 11000;
    std::vector<world::CityZoneInfo> zones{*g_env.nodes->zone_by_guid(j.zoi_guid)};
    world::CityNodes synth({j}, zones);
    set_level(synth, kJoeva, 4);
    auto wire = world::NodeWire::load(db_path(), synth, error);
    if (!wire) {
        CHECK(false, "the node wire loads for a registry of one node");
        return;
    }
    const std::string path = navfix::write_chunk_file("nodebowlworld");
    struct RemoveFile {
        const std::string &p;
        ~RemoveFile() {
            std::error_code ec;
            fs::remove(p, ec);
        }
    } remove_file{path};
    aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
    PlayerWorld world(&*g_env.db, "", bs::GameStateMode::Tier3, 60, guids);
    world.set_spawn_context(g_env.base);
    if (!world.ready() || !world.load_blueprints(motion_path(), error)) {
        CHECK(false, "the world is ready and the Blueprint table loads");
        return;
    }
    world.set_city_nodes(&synth);
    world.set_node_wire(wire.get());
    world.set_node_population_data(g_env.data.get());
    spawns::AreaConfig circle;
    circle.center_x = 500;
    circle.center_y = 500;
    circle.radius_cm = 150000.0;
    auto nav = nav::AreaNav::load(path, &error);
    if (!world.load_area(circle, error) || !nav || !world.adopt_ground(std::move(nav), error)) {
        CHECK(false, "the area loads and the fixture's ground is adopted");
        return;
    }
    CHECK(!world.town_ground_at(500, 500) && world.town_floor().towns() == 0,
          "before the node people exist, no point is in a bowl");
    spawns::NodePopulations::Config cfg;
    const size_t made = world.start_node_people(cfg);
    CHECK(made == golden_npcs("Verra_RVR_Joeva", 4), "the town's people are made on the adopted ground");
    double median = 0, px = 0, py = 0;
    if (!compute_bowl_floor_person(world, median, px, py)) return;
    test_bowl_world_queries(world, synth, median, px, py);
}
} // namespace
int main() {
    if (const int rc = init_node_pop_env(); rc != 0) return rc;
    test_pending();
    test_world();
    test_roads();
    test_bowl_world();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
