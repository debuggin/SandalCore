// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_foliage_common.h"
#include "spawns/area.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace fol = aoc::worldd::foliage;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using std::chrono::seconds;

using ClientName = std::pair<std::string, int32_t>;

static ClientName client_fname(const std::string &s) {
    const size_t us = s.rfind('_');
    if (us == std::string::npos || us == 0 || us + 1 >= s.size()) {
        return {s, 0};
    }
    const std::string tail = s.substr(us + 1);
    const bool bad = tail.size() > 9 || (tail.size() > 1 && tail[0] == '0') || tail.find_first_not_of("0123456789") != std::string::npos;
    if (bad) {
        return {s, 0};
    }
    return {s.substr(0, us), std::stoi(tail) + 1};
}

struct WireCell {
    ClientName name;
    std::map<ClientName, bool> components;
    size_t blocks = 0;
};

static std::optional<ClientName> name_of(const aoc::protocol::repl::Value *v) {
    const aoc::protocol::repl::NameValue *n = v ? v->get_if<aoc::protocol::repl::NameValue>() : nullptr;
    if (!n || n->hardcoded) {
        return std::nullopt;
    }
    return ClientName{n->text, n->number};
}

static bool read_comp_from_wire(const aoc::protocol::schema::ClassSchema &comp, const aoc::protocol::repl::ContentBlock &blk, WireCell &out) {
    size_t p = 0;
    const auto c = aoc::protocol::repl::read_rep_block(comp, blk.payload.data(), blk.payload.bit_pos(), p);
    if (!c) {
        return false;
    }
    const auto uuid = name_of(c->get(comp, "FoliageComponentUUID"));
    if (!uuid) {
        return false;
    }
    const aoc::protocol::repl::Value *init = c->get(comp, "bInitialized");
    out.components[*uuid] = init && init->get_if<bool>() && *init->get_if<bool>();
    return true;
}

static bool read_cell_from_wire(const aoc::protocol::repl::Actor &actor, WireCell &out) {
    const aoc::protocol::schema::ClassSchema *ifa = aoc::protocol::schema::gen::find_class_schema("AIntrepidStateIFA");
    const aoc::protocol::schema::ClassSchema *comp = aoc::protocol::schema::gen::find_class_schema("UIntrepidFoliageStateComponent");
    if (!ifa || !comp) {
        return false;
    }
    aoc::protocol::repl::ActorShadow shadow;
    std::vector<aoc::protocol::repl::ContentBlock> blocks;
    if (!aoc::protocol::repl::collect_open_blocks(actor, shadow, blocks) || blocks.empty() || blocks[0].sub) {
        return false;
    }
    out.blocks = blocks.size();
    size_t pos = 0;
    const auto a = aoc::protocol::repl::read_rep_block(*ifa, blocks[0].payload.data(), blocks[0].payload.bit_pos(), pos);
    const auto name = a ? name_of(a->get(*ifa, "FoliageName")) : std::nullopt;
    if (!name) {
        return false;
    }
    out.name = *name;
    for (size_t i = 1; i < blocks.size(); ++i) {
        if (!read_comp_from_wire(*comp, blocks[i], out)) {
            return false;
        }
    }
    return true;
}

struct TsvRowStats {
    size_t rows = 0;
    size_t resolved = 0;
    size_t no_actor = 0;
    size_t no_component = 0;
    size_t uninitialized = 0;
    size_t trees = 0;
    size_t plants = 0;
    size_t stones = 0;
    size_t trees_ok = 0;
    size_t plants_ok = 0;
    size_t stones_ok = 0;
    size_t foreign = 0;
};

static std::vector<std::string> split_six_cols(const std::string &line) {
    std::vector<std::string> col;
    size_t from = 0;
    for (int i = 0; i < 6; ++i) {
        const size_t t = line.find('\t', from);
        col.push_back(line.substr(from, t == std::string::npos ? std::string::npos : t - from));
        if (t == std::string::npos) {
            break;
        }
        from = t + 1;
    }
    return col;
}

static std::vector<std::string> parse_tsv_row_cols(const std::string &raw) {
    if (raw.empty() || raw[0] == '#') {
        return {};
    }
    std::string line = raw;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    if (line.empty()) {
        return {};
    }
    return split_six_cols(line);
}

static void bump_tsv_category(const std::string &def_col, size_t &stones, size_t &trees, size_t &plants) {
    if (def_col.rfind("Stone_", 0) == 0 || def_col.rfind("Riverlands_Rock", 0) == 0) {
        ++stones;
    } else if (def_col.rfind("Tree_", 0) == 0) {
        ++trees;
    } else {
        ++plants;
    }
}

static bool check_retail_actor_block(const aoc::worldd::world::WorldActor &a, const capture_foliage::IfaOpen &open, std::set<uint64_t> &server_ids) {
    const tsupport::Bits theirs = tsupport::bits_of_hex(open.actor.hex, open.actor.bits);
    const aoc::protocol::schema::ClassSchema &ifa = *aoc::protocol::schema::gen::find_class_schema("AIntrepidStateIFA");
    const auto retail_state = tsupport::with_exact(theirs, [&](const uint8_t *d, size_t, size_t n) {
        size_t pos = 0;
        return aoc::protocol::repl::read_rep_block(ifa, d, n, pos);
    });
    const aoc::protocol::repl::Value *server = retail_state ? retail_state->get(ifa, "AuthServerIDReplicated") : nullptr;
    const uint64_t *server_id = server ? server->get_if<uint64_t>() : nullptr;
    if (!server_id) {
        return false;
    }
    aoc::protocol::repl::Actor patched = a.actor;
    const aoc::protocol::repl::Value *now = patched.obj.state.get(*patched.obj.schema, "AuthServerIDReplicated");
    const bool ours_zero = now && now->get_if<uint64_t>() && *now->get_if<uint64_t>() == 0;
    patched.obj.state.set(*patched.obj.schema, "AuthServerIDReplicated", aoc::protocol::repl::Value::u64(*server_id));
    aoc::protocol::repl::ActorShadow patched_shadow;
    std::vector<aoc::protocol::repl::ContentBlock> patched_blocks;
    if (!aoc::protocol::repl::collect_open_blocks(patched, patched_shadow, patched_blocks) || patched_blocks.empty()) {
        return false;
    }
    if (!ours_zero || !(tsupport::Bits(patched_blocks[0].payload) == theirs)) {
        return false;
    }
    server_ids.insert(*server_id);
    return true;
}

struct RetailCompStats {
    size_t order_ok = 0;
    size_t comps_total = 0;
    size_t comps_ok = 0;
    size_t whole_total = 0;
    size_t whole_ok = 0;
};

static void check_retail_components(const aoc::worldd::world::WorldActor &a, const capture_foliage::IfaOpen &open, const std::vector<aoc::protocol::repl::ContentBlock> &blocks, RetailCompStats &st) {
    bool order = true;
    for (uint32_t i = 0; i < open.component_count; ++i) {
        const capture_foliage::Component &c = open.components[i];
        ++st.comps_total;
        const tsupport::Bits ours(blocks[1 + i].payload);
        const aoc::protocol::repl::Subobject &sub = a.actor.subobjects[i];
        const aoc::protocol::repl::Value *uuid = sub.obj.schema ? sub.obj.state.get(*sub.obj.schema, "FoliageComponentUUID") : nullptr;
        const aoc::protocol::repl::NameValue *uuid_name = uuid ? uuid->get_if<aoc::protocol::repl::NameValue>() : nullptr;
        if (!sub.dynamic_class.has_value() || !uuid_name || aoc::protocol::repl::fname_text(*uuid_name) != c.uuid) {
            order = false;
        }
        if (c.has_states) {
            if (ours.size() <= static_cast<size_t>(c.block.bits) && ours == tsupport::bits_of_hex(c.block.hex, ours.size())) {
                ++st.comps_ok;
            }
        } else {
            ++st.whole_total;
            if (ours == tsupport::bits_of_hex(c.block.hex, c.block.bits)) {
                ++st.whole_ok;
                ++st.comps_ok;
            }
        }
    }
    if (order) {
        ++st.order_ok;
    }
}

static std::string name_text(const aoc::protocol::repl::ObjectState &o, const char *path) {
    const aoc::protocol::repl::Value *v = o.state.get(*o.schema, path);
    const aoc::protocol::repl::NameValue *n = v ? v->get_if<aoc::protocol::repl::NameValue>() : nullptr;
    return n ? aoc::protocol::repl::fname_text(*n) : std::string();
}

static size_t subobject_states_listed(const aoc::protocol::repl::Subobject &sub, uint64_t state) {
    const aoc::protocol::repl::FastArrayState *fa = sub.obj.arrays.find(aoc::protocol::repl::fast_array_layout(*sub.obj.schema, "IndexedStates")->field);
    if (!fa) {
        return 0;
    }
    size_t n = 0;
    for (const auto &item : fa->items()) {
        const auto s = item.values.values.find(1);
        if (s != item.values.values.end() && *s->second.get_if<uint64_t>() == state) {
            ++n;
        }
    }
    return n;
}

static size_t states_listed(const aoc::protocol::repl::Actor &a, uint64_t state) {
    size_t n = 0;
    for (const aoc::protocol::repl::Subobject &sub : a.subobjects) {
        n += subobject_states_listed(sub, state);
    }
    return n;
}

static aoc::worldd::Clock::time_point after(aoc::worldd::Clock::time_point t, double seconds_later) {
    return t + std::chrono::duration_cast<aoc::worldd::Clock::duration>(std::chrono::duration<double>(seconds_later));
}

static size_t working_set_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        return static_cast<size_t>(pmc.WorkingSetSize);
    }
#endif
    return 0;
}

static void record_tsv_row(const std::string &raw, const std::map<ClientName, WireCell> &client, TsvRowStats &st) {
    const std::vector<std::string> col = parse_tsv_row_cols(raw);
    if (col.size() < 6) {
        return;
    }
    ++st.rows;
    bump_tsv_category(col[1], st.stones, st.trees, st.plants);
    if (col[4].rfind("IntrepidGatherableFISMC_", 0) != 0) {
        ++st.foreign;
    }
    const auto cell = client.find(client_fname(col[3]));
    if (cell == client.end()) {
        ++st.no_actor;
        return;
    }
    const auto comp = cell->second.components.find(client_fname(col[4]));
    if (comp == cell->second.components.end()) {
        ++st.no_component;
        return;
    }
    if (!comp->second) {
        ++st.uninitialized;
        return;
    }
    ++st.resolved;
    bump_tsv_category(col[1], st.stones_ok, st.trees_ok, st.plants_ok);
}

static void test_every_instance_has_its_state(World &w) {
    std::printf("the cells: every instance of the map resolves the way the client looks it up\n");
    std::map<ClientName, WireCell> client;
    size_t actors = 0;
    size_t unreadable = 0;
    size_t doubled = 0;
    size_t components = 0;
    size_t not_init = 0;
    w.world->registry().for_each([&](const world::WorldActor &a) {
        if (a.kind != world::ActorKind::Foliage) {
            return;
        }
        ++actors;
        WireCell cell;
        if (!read_cell_from_wire(a.actor, cell)) {
            ++unreadable;
            return;
        }
        components += cell.components.size();
        for (const auto &[n, init] : cell.components) {
            if (!init) {
                ++not_init;
            }
        }
        if (!client.emplace(cell.name, cell).second) {
            ++doubled;
        }
    });
    char msg[240];
    std::snprintf(msg, sizeof msg, "%zu state actors in the world, %zu read back from their bits, %zu FoliageName twice, %zu components, %zu not initialized", actors, actors - unreadable, doubled, components, not_init);
    CHECK(actors == 3761 && unreadable == 0 && doubled == 0 && components == 42267 && not_init == 0 && client.size() == 3761, msg);
    TsvRowStats st;
    FILE *f = std::fopen(tsv_path(), "rb");
    char buf[2048];
    while (f && std::fgets(buf, sizeof buf, f)) {
        record_tsv_row(buf, client, st);
    }
    if (f) {
        std::fclose(f);
    }
    std::snprintf(msg, sizeof msg, "%zu of the %zu rows resolve to an initialized component (%zu without a state actor, %zu without a component, %zu not initialized)", st.resolved, st.rows, st.no_actor, st.no_component, st.uninitialized);
    CHECK(st.rows == 588362 && st.resolved == st.rows, msg);
    std::snprintf(msg, sizeof msg, "... %zu of %zu trees, %zu of %zu plants, %zu of %zu stones (and the four rock pillars)", st.trees_ok, st.trees, st.plants_ok, st.plants, st.stones_ok, st.stones);
    CHECK(st.trees == 294820 && st.trees_ok == st.trees && st.plants == 148686 && st.plants_ok == st.plants && st.stones == 144856 && st.stones_ok == st.stones, msg);
    CHECK(st.foreign == 0, "every row, the 144,856 stones among them, is in an IntrepidGatherableFISMC_<n> component: the class the client's target scan accepts");
}

static void test_cells_equal_retail(World &w) {
    std::printf("the cells: an untouched cell's open against the retail opens (1198's login, 1428)\n");
    fol::FoliageCells &cells = w.f->cells();
    size_t painted = 0;
    size_t found = 0;
    size_t actor_ok = 0;
    size_t spawn_ok = 0;
    RetailCompStats cst;
    std::set<uint64_t> server_ids;
    for (const auto &open : capture_foliage::kIfaOpens) {
        const auto owner = w.f->map().owner_of(open.name);
        if (!owner) {
            continue;
        }
        ++painted;
        const world::WorldActor *a = w.world->registry().find(cells.actor_of(*owner));
        if (!a) {
            continue;
        }
        ++found;
        repl::ActorShadow shadow;
        std::vector<repl::ContentBlock> blocks;
        if (!repl::collect_open_blocks(a->actor, shadow, blocks) || blocks.size() != 1 + open.component_count) {
            continue;
        }
        if (check_retail_actor_block(*a, open, server_ids)) {
            ++actor_ok;
        }
        const auto &q = a->actor.open.spawn.location;
        if (q && q->mode == repl::QuantVec::Mode::Scaled && open.spawn_scaled && q->x == open.spawn[0] && q->y == open.spawn[1] && q->z == open.spawn[2]) {
            ++spawn_ok;
        }
        check_retail_components(*a, open, blocks, cst);
    }
    CHECK(painted == 10, "ten of the twelve retail opens are painted cells of the map (the other two are a town's runtime foliage)");
    CHECK(found == painted, "... and each has a state actor in the world from the start");
    char actor_msg[300];
    std::snprintf(actor_msg, sizeof actor_msg, "the actor block (520 bits: AuthServerIDReplicated, ReplicatedMovement, RemoteRole, Role, FoliageName) is the retail block to the last bit in each, but for that one server id (%zu values in the ten)", server_ids.size());
    CHECK(actor_ok == painted && server_ids == std::set<uint64_t>({0, 37, 48}), actor_msg);
    CHECK(spawn_ok == painted, "SerializeNewActor's location (x and y the cell's centre, z the middle of its instances' lowest and highest) is the retail location in each");
    CHECK(cst.order_ok == painted, "the components are the same, in the same byte order (_0, _1, _10 ... _19, _2 ...), in each");
    char msg[200];
    std::snprintf(msg, sizeof msg, "all %zu component blocks are the retail blocks (%zu of them with no IndexedStates: equal to the last bit; the rest up to the IndexedStates field)", cst.comps_total, cst.whole_total);
    CHECK(cst.comps_total == 166 && cst.comps_ok == cst.comps_total && cst.whole_ok == cst.whole_total && cst.whole_total > 0, msg);
}

static void check_cell_actor_meta(const world::WorldActor &a, const fol::FoliageMap &map, uint16_t owner) {
    CHECK(a.actor.subobjects.size() == map.owners()[owner].components.size() && a.actor.subobjects.size() == 28, "it has a component for each FISMC of the owner: 28, as the retail open of that cell");
    double cx = 0;
    double cy = 0;
    map.cell_center(owner, cx, cy);
    CHECK(a.x == cx && a.y == cy && a.x == -1011200.0 && a.y == 499200.0, "it stands at the cell's centre (-1,011,200, 499,200), where the retail open puts it");
    const bool arch_ok = a.actor.open.archetype.size() == 2 && a.actor.open.archetype[0].guid.ObjectId == 18292086725290287385ull && a.actor.subobjects[0].dynamic_class && a.actor.subobjects[0].dynamic_class->front().guid.ObjectId == 8959184318458680037ull;
    CHECK(arch_ok, "its archetype is Default__IntrepidStateIFA and its components are IntrepidFoliageStateComponent (the ids of the retail capture's export)");
    CHECK(name_text(a.actor.obj, "FoliageName") == "InstancedFoliageActor_25600_-40_19_0", "FoliageName is the owner");
    const bool comp_ok = name_text(a.actor.subobjects[0].obj, "FoliageComponentUUID") == "IntrepidGatherableFISMC_0" && name_text(a.actor.subobjects[1].obj, "FoliageComponentUUID") == "IntrepidGatherableFISMC_1" && name_text(a.actor.subobjects[2].obj, "FoliageComponentUUID") == "IntrepidGatherableFISMC_10";
    CHECK(comp_ok, "the components are in byte order: _0, _1, _10 ... (the retail open's)");
}

static void test_cell_actor(World &w) {
    std::printf("the cells: the state actor of the cell the retail capture opened\n");
    fol::FoliageCells &cells = w.f->cells();
    const auto &map = w.f->map();
    const uint32_t stone_row = w.row_of("InstancedFoliageActor_25600_-40_19_0", "IntrepidGatherableFISMC_18", 0);
    const uint16_t owner = map.rows()[stone_row].owner;
    CHECK(w.f->name_of(stone_row) == "Stone_Granite_Large_3", "(a Granite stone)");
    const uint64_t actor_id = cells.actor_of(owner);
    CHECK(actor_id != 0 && cells.actors() == 3761 && cells.items_of(owner) == 0, "every cell has its state actor before anything in it is cut: 3,761, this one with no item");
    const size_t in_registry = w.world->registry().size();
    const Clock::time_point t0{seconds(1000)};
    const auto change = w.f->harvested(stone_row, t0);
    CHECK(change && change->state == fol::kEmpty, "the harvest is Empty");
    CHECK(cells.actor_of(owner) == actor_id && cells.actors() == 3761 && w.world->registry().size() == in_registry, "the first cut of the cell changed its actor and made no other");
    const world::WorldActor *a = w.world->registry().find(actor_id);
    const bool flags_ok = a && a->kind == world::ActorKind::Foliage && a->snapshot_only && a->keep_fast_shadow && a->dormancy_delay == fol::kStateActorOpen && !a->never_dormant;
    CHECK(flags_ok, "it is a snapshot-only foliage actor with a 265 ms open, whose fast arrays the view keeps");
    if (!a) {
        return;
    }
    check_cell_actor_meta(*a, map, owner);
    CHECK(cells.shown_state(stone_row) == 3u && cells.items_of(owner) == 1, "one item: the instance, Empty (3)");
    const double back = w.f->store().regrow_seconds(stone_row);
    w.f->tick(after(t0, back - 1.0));
    CHECK(cells.shown_state(stone_row) == 3u, "(a stone is still Empty a second before its time)");
    const uint32_t version = w.world->registry().find(actor_id)->version;
    w.f->tick(after(t0, back + 1.0));
    CHECK(!cells.shown_state(stone_row) && cells.items_of(owner) == 0 && w.world->registry().find(actor_id)->version > version, "then it is whole: the item leaves the list and the actor's version moved");
}

static void check_cell_first_open_form(World &w, uint64_t actor_id, uint32_t oak_row) {
    const uint32_t other = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 1);
    const Clock::time_point t1{seconds(30000)};
    w.f->harvested(oak_row, t1);
    w.f->harvested(other, t1);
    const world::WorldActor *b = w.world->registry().find(actor_id);
    repl::Actor fresh = b->actor;
    b->first_open_form(fresh);
    CHECK(states_listed(fresh, 3) == 2 && states_listed(fresh, 11) == 0, "two instances cut a moment ago are a live harvest to a first look: still 3 (the client plays the fall)");
    w.f->tick(after(t1, 60));
    const uint32_t third = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 2);
    w.f->harvested(third, after(t1, 60));
    repl::Actor copy = b->actor;
    b->first_open_form(copy);
    CHECK(states_listed(copy, 11) == 2 && states_listed(copy, 3) == 1 && states_listed(b->actor, 3) == 3 && states_listed(b->actor, 11) == 0, "a minute later a first look lists the two older Empty instances as 11 and the new one as 3; the actor itself holds 3 for all");
    const repl::FastArrayState *live = nullptr;
    const repl::FastArrayState *shown = nullptr;
    for (size_t i = 0; i < b->actor.subobjects.size() && !live; ++i) {
        const auto layout = repl::fast_array_layout(*b->actor.subobjects[i].obj.schema, "IndexedStates");
        const repl::FastArrayState *l = b->actor.subobjects[i].obj.arrays.find(layout->field);
        if (l && !l->items().empty()) {
            live = l;
            shown = copy.subobjects[i].obj.arrays.find(layout->field);
        }
    }
    const bool keys_ok = live && shown && live->array_key() == shown->array_key() && live->items()[0].key == shown->items()[0].key && live->items()[0].id == shown->items()[0].id;
    CHECK(keys_ok, "and the copy keeps the array's key and the items' keys and IDs (only the value changed: what a connection has is the server's count)");
}

static void test_cell_regrowth(World &w) {
    std::printf("the cells: an Oak's items through its regrowth\n");
    fol::FoliageCells &cells = w.f->cells();
    const uint32_t oak_row = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 0);
    const uint16_t owner = w.f->map().rows()[oak_row].owner;
    const Clock::time_point t0{seconds(20000)};
    w.f->harvested(oak_row, t0);
    const uint64_t actor_id = cells.actor_of(owner);
    CHECK(actor_id != 0 && cells.shown_state(oak_row) == 3u, "a harvest: the instance is Empty (3) in its cell's list");
    const world::WorldActor *a = w.world->registry().find(actor_id);
    CHECK(a && a->actor.subobjects.size() == 8, "the cell (-41, 19) has 8 components");
    const uint32_t v0 = a ? a->version : 0;
    w.f->tick(after(t0, 10));
    CHECK(cells.shown_state(oak_row) == 3u, "(nothing is due after 10 s)");
    w.f->tick(after(t0, 301));
    CHECK(cells.shown_state(oak_row) == 0u && w.world->registry().find(actor_id)->version > v0, "after 300 s: Infancy (0), and the actor's version moved: the clients are sent it");
    w.f->tick(after(t0, 301 + 1800));
    CHECK(cells.shown_state(oak_row) == 1u, "after 1,800 s more: Juvenile (1)");
    w.f->tick(after(t0, 301 + 3600 + 5));
    CHECK(!cells.shown_state(oak_row) && cells.items_of(owner) == 0 && w.f->store().deviations() == 0, "after 1,800 s more: whole, the item is deleted from the list");
    check_cell_first_open_form(w, actor_id, oak_row);
}

static void test_load(const char *db, const char *tsv) {
    std::printf("the load: the 3,761 state actors made at start\n");
    World w;
    if (!w.open(db, tsv, false)) {
        return;
    }
    const size_t registry_before = w.world->registry().size();
    CHECK(w.f->cells().actors() == 0, "(the first draft's way, an actor at a cell's first cut: none yet, for the measurement)");
    const size_t ws0 = working_set_bytes();
    const auto t0 = std::chrono::steady_clock::now();
    const size_t made = w.f->cells().make_all();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const size_t ws1 = working_set_bytes();
    size_t subobjects = 0;
    size_t total_bits = 0;
    size_t max_bits = 0;
    size_t opens = 0;
    w.world->registry().for_each([&](const world::WorldActor &a) {
        if (a.kind != world::ActorKind::Foliage) {
            return;
        }
        subobjects += a.actor.subobjects.size();
        repl::ActorShadow shadow;
        std::vector<repl::ContentBlock> blocks;
        if (!repl::collect_open_blocks(a.actor, shadow, blocks)) {
            return;
        }
        size_t bits = 0;
        for (const repl::ContentBlock &b : blocks) {
            bits += b.payload.bit_pos();
        }
        total_bits += bits;
        max_bits = std::max(max_bits, bits);
        ++opens;
    });
    const double grown_mb = ws1 > ws0 ? static_cast<double>(ws1 - ws0) / (1024.0 * 1024.0) : 0.0;
    CHECK(made == 3761 && subobjects == 42267 && w.world->registry().size() == registry_before + 3761 && opens == 3761, "3,761 actors with 42,267 components were made and each builds its open");
    CHECK(ms < 2000.0, "they are made in under two seconds, once, at start");
    CHECK(ws0 == 0 || grown_mb < 100.0, "and they hold under 100 MB of the process (the measurement: see above)");
}

static void test_worker_area_filtering(World &w) {
    std::printf("the cells: worker area filtering excludes carved-out holes and keeps only authoritative cells\n");
    const fol::FoliageMap &map = w.f->map();
    const spawns::AreaConfig saved = spawns::AreaConfig::active_worker_area();
    spawns::AreaConfig carved = spawns::AreaConfig::continent();
    carved.exclude_node_plugs = {spawns::kVerraNodePlugs[0]};
    spawns::AreaConfig::active_worker_area(&carved);
    world::ActorRegistry reg_carved;
    uint64_t next_guid = 900000;
    fol::FoliageCells cells_carved(reg_carved, map, [&] { return aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(++next_guid); });
    const size_t made_carved = cells_carved.make_all();
    size_t carved_holes = 0;
    size_t leaked_holes = 0;
    for (size_t o = 0; o < map.owners().size(); ++o) {
        double cx = 0.0, cy = 0.0;
        map.cell_center(static_cast<uint16_t>(o), cx, cy);
        const bool in_hole = carved.has_carve_out(cx, cy);
        if (in_hole) {
            ++carved_holes;
            if (cells_carved.actor_of(static_cast<uint16_t>(o)) != 0) ++leaked_holes;
        }
    }
    CHECK(carved_holes > 0 && leaked_holes == 0 && made_carved + carved_holes == map.owners().size(),
          "make_all excludes every cell inside a carved-out hole (has_carve_out) and creates the rest");
    spawns::AreaConfig plug = spawns::AreaConfig::continent();
    plug.region_mask = static_cast<uint8_t>(spawns::RegionBit::None);
    plug.region_label = "node_plug";
    plug.include_node_plugs = {spawns::kVerraNodePlugs[0]};
    spawns::AreaConfig::active_worker_area(&plug);
    world::ActorRegistry reg_plug;
    fol::FoliageCells cells_plug(reg_plug, map, [&] { return aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(++next_guid); });
    const size_t made_plug = cells_plug.make_all();
    size_t outside_plug = 0;
    for (size_t o = 0; o < map.owners().size(); ++o) {
        double cx = 0.0, cy = 0.0;
        map.cell_center(static_cast<uint16_t>(o), cx, cy);
        if (cells_plug.actor_of(static_cast<uint16_t>(o)) != 0 && !plug.contains(cx, cy)) ++outside_plug;
    }
    spawns::AreaConfig::active_worker_area(&saved);
    CHECK(made_plug == carved_holes && outside_plug == 0 && made_carved + made_plug == map.owners().size(),
          "a plug worker only creates AIntrepidStateIFA cells inside its authoritative area");
}

void run_foliage_cells_tests(World &w, const char *db, const char *tsv) {
    test_every_instance_has_its_state(w);
    test_cells_equal_retail(w);
    test_cell_actor(w);
    test_cell_regrowth(w);
    test_load(db, tsv);
    test_worker_area_filtering(w);
}
