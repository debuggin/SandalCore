// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_gather_common.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace emit = aoc::protocol::emit;
namespace cg = capture_gather;
using std::chrono::milliseconds;
using std::chrono::seconds;
using tsupport::Bits;
using tsupport::bits_of_hex;

static bool payload_is(const repl::ContentBlock &b, const cg::Block &want) {
    if (b.has_rep_layout != want.rep)
        return false;
    const Bits mine(b.payload);
    const Bits theirs = bits_of_hex(want.hex, want.bits);
    if (mine == theirs)
        return true;
    size_t at = 0;
    while (at < mine.size() && at < theirs.size() && mine.b[at] == theirs.b[at])
        ++at;
    std::printf("      (differs: %zu bits against the capture's %zu, the first difference at bit %zu)\n", mine.size(), theirs.size(), at);
    return false;
}

static std::optional<repl::ObjectState> load(const cg::Block &b, const char *cls) {
    return repl::load_object_state_hex(tsupport::schema_named(cls), b.hex, b.bits, b.rep);
}

static const repl::ElementState *element(const repl::ObjectState &o, const char *path, size_t index) {
    const repl::Value *v = o.state.get(*o.schema, path);
    const auto *a = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!a)
        return nullptr;
    return index < a->elements.size() ? &a->elements[index] : nullptr;
}

static void setup_node_as_1427(World &w, uint64_t node_id, const repl::ObjectState &cap_actor, const repl::ObjectState &cap_comp) {
    w.registry().modify(node_id, world::Change::State, [&](world::WorldActor &a) {
        repl::ObjectState &o = a.actor.obj;
        for (const char *path : {"AuthServerIDReplicated", "RemoteRole", "Role", "GatherableGuid.A", "GatherableGuid.B", "GatherableGuid.C", "GatherableGuid.D"}) {
            if (const repl::Value *v = cap_actor.state.get(*cap_actor.schema, path))
                o.state.set(*o.schema, path, *v);
        }
        o.state.erase(7);
        for (repl::Subobject &s : a.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
                continue;
            if (const repl::Value *v = cap_comp.state.get(*cap_comp.schema, "InteractionInputData"))
                s.obj.state.set(*s.obj.schema, "InteractionInputData", *v);
            s.obj.arrays = repl::FastArrays();
            if (const repl::Value *v = cap_comp.state.get(*cap_comp.schema, "CurrentInputStates"))
                s.obj.state.set(*s.obj.schema, "CurrentInputStates", *v);
        }
    });
}

static repl::Actor player_view_of(World &w, uint64_t node_id, uint64_t player) {
    const world::WorldActor *a = w.registry().find(node_id);
    repl::Actor v = a->actor;
    gather::show_interacting_player(*a, player, v);
    return v;
}

static std::optional<uint32_t> interacting_player_id_in_view(const repl::Actor &view) {
    for (const repl::Subobject &s : view.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
            continue;
        const repl::ElementState *el = element(s.obj, "InputEffectStatuses", 0);
        if (!el)
            return 0xffffffffu;
        const auto it = el->values.find(3);
        const auto *g = it != el->values.end() ? it->second.get_if<emit::FIntrepidNetworkGUID>() : nullptr;
        return g ? static_cast<uint32_t>(g->ObjectId) : 0xffffffffu;
    }
    return std::nullopt;
}

static void setup_node_as_1454(World &w, uint64_t node_id) {
    const auto cap_comp = load(cg::kGatheredComponent, "UInteractableComponent");
    w.registry().modify(node_id, world::Change::State, [&](world::WorldActor &a) {
        for (repl::Subobject &s : a.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
                continue;
            if (cap_comp) {
                if (const repl::Value *v = cap_comp->state.get(*cap_comp->schema, "InteractionInputData"))
                    s.obj.state.set(*s.obj.schema, "InteractionInputData", *v);
            }
            s.obj.arrays = repl::FastArrays();
        }
    });
}

static bool is_gathering_profession(int64_t p) {
    return p == id::kMining || p == id::kLumberjacking || p == id::kHerbalism;
}

static bool is_valid_tool_slot(int32_t s) {
    static const int32_t slots[] = {16, 26, 29, 33, 34, 35, 36, 37, 38};
    for (int32_t v : slots) {
        if (s == v)
            return true;
    }
    return false;
}

static gather::Channeler channeler(uint64_t player, uint32_t n) {
    gather::Channeler by;
    by.player_id = player;
    by.character = repl::GuidValue{n, n + 1, n + 2, n + 3};
    by.pawn.ObjectId = 1000 + player * 2;
    return by;
}

static std::optional<std::vector<std::optional<bool>>> can_input_on_wire(const repl::ContentBlock &b) {
    if (!b.sub || !b.sub->obj.schema || !b.has_rep_layout)
        return std::nullopt;
    if (std::string(b.sub->obj.schema->name) != "UInteractableComponent")
        return std::nullopt;
    const Bits bits(b.payload);
    return tsupport::with_exact(bits, [&](const uint8_t *d, size_t, size_t n) -> std::optional<std::vector<std::optional<bool>>> {
        size_t pos = 0;
        const auto st = repl::read_rep_block(*b.sub->obj.schema, d, n, pos, {});
        const repl::Value *v = st ? st->get(*b.sub->obj.schema, "CanInputActivate") : nullptr;
        const auto *a = v ? v->get_if<repl::ArrayValue>() : nullptr;
        if (!a)
            return std::nullopt;
        std::vector<std::optional<bool>> out;
        for (const repl::ElementState &e : a->elements) {
            const auto it = e.values.find(1);
            const bool *x = it == e.values.end() ? nullptr : it->second.get_if<bool>();
            out.push_back(x ? std::optional<bool>(*x) : std::nullopt);
        }
        return out;
    });
}

static std::optional<std::vector<std::optional<bool>>> read_can_input_update(World &w, uint64_t a, repl::ActorShadow &shadow) {
    const world::WorldActor *n = w.registry().find(a);
    std::vector<repl::ContentBlock> update;
    if (!n || !repl::collect_update_blocks(n->actor, shadow, update))
        return std::nullopt;
    for (const repl::ContentBlock &b : update) {
        if (auto c = can_input_on_wire(b))
            return c;
    }
    return std::nullopt;
}

static bool four_and_first_is(const std::optional<std::vector<std::optional<bool>>> &c, bool first) {
    if (!c || c->size() != 4)
        return false;
    return (*c)[0].has_value() && *(*c)[0] == first;
}

static size_t registry_can_input_size(const world::WorldActor *n) {
    if (!n)
        return 99;
    for (const repl::Subobject &s : n->actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
            continue;
        const repl::Value *v = s.obj.state.get(*s.obj.schema, "CanInputActivate");
        const auto *arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
        return arr ? arr->elements.size() : 0;
    }
    return 99;
}

static void check_1427_beats_and_done(World &w, gather::NodeBoard &board, uint64_t node_id, repl::ActorShadow &shadow) {
    size_t same = 0;
    size_t one_block = 0;
    Clock::time_point now = t0;
    for (size_t i = 0; i < cg::kRunBeatCount; ++i) {
        now += milliseconds(100);
        if (!board.beat(node_id, now))
            break;
        repl::Actor view = player_view_of(w, node_id, 7);
        std::vector<repl::ContentBlock> update;
        if (!repl::collect_update_blocks(view, shadow, update))
            break;
        if (update.size() != 1)
            continue;
        ++one_block;
        same += payload_is(update[0], cg::kRunBeats[i].block) ? 1 : 0;
    }
    CHECK(one_block == cg::kRunBeatCount, "every heartbeat is one block: the interactable's");
    CHECK(same == cg::kRunBeatCount, "all 96 heartbeats are the capture's, to the bit (81 bits each; the first, 217, also takes InteractingPlayer off)");
    now += milliseconds(100);
    std::string why;
    CHECK(board.complete(node_id, now, seconds(1800), why), "the board completes the channel");
    repl::Actor view = player_view_of(w, node_id, 7);
    std::vector<repl::ContentBlock> update;
    const bool two_done = repl::collect_update_blocks(view, shadow, update) && update.size() == 2;
    CHECK(two_done, "the done update is two blocks");
    if (two_done) {
        CHECK(payload_is(update[0], cg::kRunDoneActor), "the actor block (bGathered) is the capture's, to the bit (18 bits)");
        CHECK(payload_is(update[1], cg::kRunDoneComponent), "the component block (the status 0x20000004, the pawn, nobody channels, CanInputActivate, the state 0) is the capture's, to the bit (334 bits)");
    }
    CHECK(board.state_of(node_id) == gather::NodeState::Gathered, "the node is Gathered");
    CHECK(repl::collect_update_blocks(player_view_of(w, node_id, 7), shadow, update) && update.empty(), "and nothing changes until the respawn");
    const auto o_comp = interacting_player_id_in_view(player_view_of(w, node_id, 99));
    CHECK(o_comp && *o_comp == 0, "another player's connection sees InteractingPlayer as none");
}

static void test_run_1427(World &w) {
    std::printf("the retail capture, replayed: the node's open, 96 heartbeats and the done update, to the bit\n");
    const auto cap_actor = load(cg::kRunOpenActor, "AGatherableActor");
    const auto cap_comp = load(cg::kRunOpenComponent, "UInteractableComponent");
    const bool loaded = cap_actor && cap_comp;
    CHECK(loaded, "the captured blocks load");
    if (!loaded)
        return;
    std::string why;
    const uint64_t node_id = w.add_node(id::kDaffodil, 1427, 0, 0, &why);
    CHECK(node_id != 0, "a Plant_Daffodil_Medium_1 node is made from the captured template");
    if (!node_id)
        return;
    setup_node_as_1427(w, node_id, *cap_actor, *cap_comp);
    const repl::ElementState *statuses = element(*cap_comp, "InputEffectStatuses", 0);
    const auto *pawn_ptr = (statuses && statuses->values.count(3)) ? statuses->values.at(3).get_if<emit::FIntrepidNetworkGUID>() : nullptr;
    CHECK(pawn_ptr != nullptr, "the capture's pawn reference is in the open");
    if (!pawn_ptr)
        return;
    gather::Channeler by{7, repl::GuidValue{cg::kRunCharacter[0], cg::kRunCharacter[1], cg::kRunCharacter[2], cg::kRunCharacter[3]}, *pawn_ptr};
    gather::NodeBoard board(w.registry(), gather::NodeBoard::Config{});
    CHECK(board.begin(node_id, by, t0, seconds(12), why), "the board starts the channel");
    repl::ActorShadow shadow;
    std::vector<repl::ContentBlock> blocks;
    repl::Actor view = player_view_of(w, node_id, 7);
    const bool open_ok = repl::collect_open_blocks(view, shadow, blocks) && blocks.size() == 2;
    CHECK(open_ok, "the open has two blocks: the actor's and the interactable's");
    if (!open_ok)
        return;
    CHECK(payload_is(blocks[0], cg::kRunOpenActor), "the actor block is the capture's, to the bit (303 bits)");
    CHECK(payload_is(blocks[1], cg::kRunOpenComponent), "the component block is the capture's, to the bit (625 bits): the status 0x08000001, the pawn, who channels");
    check_1427_beats_and_done(w, board, node_id, shadow);
}

static void verify_1454_blocks(const world::WorldActor &a, const repl::Actor &view, const std::vector<repl::ContentBlock> &blocks) {
    const std::vector<uint8_t> cap = tsupport::bytes_of_hex(cg::kGatheredComponent.hex);
    size_t pos = 0;
    const auto st = repl::read_rep_block(tsupport::schema_named("UInteractableComponent"), cap.data(), cg::kGatheredComponent.bits, pos, {});
    const bool read_ok = st.has_value() && pos > 0 && pos < cg::kGatheredComponent.bits;
    CHECK(read_ok, "the captured block reads: a RepLayout block and the state maps after it");
    if (!st)
        return;
    const Bits mine(blocks[1].payload);
    const Bits theirs = bits_of_hex(cg::kGatheredComponent.hex, cg::kGatheredComponent.bits);
    CHECK(mine.size() >= pos && mine.slice(0, pos) == theirs.slice(0, pos), "our RepLayout block is the capture's, to the bit");
    repl::Actor with_maps = view;
    world::set_player(with_maps, repl::GuidValue{1, 2, 3, 4}, true);
    repl::ActorShadow shadow2;
    std::vector<repl::ContentBlock> blocks2;
    const bool collected = repl::collect_open_blocks(with_maps, shadow2, blocks2) && blocks2.size() == 2;
    const bool maps_ok = collected && blocks2[1].payload.bit_pos() > blocks[1].payload.bit_pos() && Bits(blocks2[1].payload).slice(0, pos) == theirs.slice(0, pos);
    CHECK(maps_ok, "with the viewer's guid in the two state maps the block is our RepLayout block, which is the capture's, and the maps after it");
    const auto actor_state = repl::load_object_state(*a.actor.obj.schema, blocks[0].payload.bytes().data(), blocks[0].payload.bit_pos(), blocks[0].has_rep_layout);
    const repl::Value *gathered = actor_state ? actor_state->state.get(*actor_state->schema, "bGathered") : nullptr;
    const bool b_gathered = gathered && gathered->get_if<bool>() && *gathered->get_if<bool>();
    CHECK(b_gathered, "the actor block carries bGathered");
}

static void test_gathered_1454(World &w) {
    std::printf("the retail capture, replayed: a gathered node as another client opens it\n");
    std::string why;
    const uint64_t node_id = w.add_node(6064631483095515243ll, 1454, 0, 0, &why);
    if (!node_id)
        return;
    setup_node_as_1454(w, node_id);
    gather::NodeBoard board(w.registry(), gather::NodeBoard::Config{});
    gather::Channeler by;
    by.player_id = 5;
    by.character = repl::GuidValue{1, 2, 3, 4};
    by.pawn.ObjectId = 4242;
    Clock::time_point now = t0;
    CHECK(board.begin(node_id, by, now, seconds(12), why), "a channel starts");
    for (int i = 0; i < 6; ++i) {
        now += milliseconds(100);
        board.beat(node_id, now);
    }
    CHECK(board.complete(node_id, now + milliseconds(100), seconds(100), why), "and completes after six heartbeats: the done step is 7");
    const world::WorldActor *a = w.registry().find(node_id);
    const bool status_ok = a && gather::node_status(*a) == (((1u + 3u * 7u) & 31u) << 27 | 4u) && gather::node_status(*a) == 2952790020u;
    CHECK(status_ok, "the status is 0xB0000004, the capture's");
    repl::Actor view = a->actor;
    gather::show_interacting_player(*a, 99, view);
    repl::ActorShadow shadow;
    std::vector<repl::ContentBlock> blocks;
    const bool open_ok = repl::collect_open_blocks(view, shadow, blocks) && blocks.size() == 2;
    CHECK(open_ok, "the open has two blocks");
    if (open_ok)
        verify_1454_blocks(*a, view, blocks);
}

static void check_data_spawns(World &w) {
    size_t placed = 0;
    size_t with_row = 0;
    size_t garden = 0;
    size_t garden_rows = 0;
    for (const spawns::CapturedSpawn &s : w.captured->spawns()) {
        if (!s.open || s.open->category != "gatherable")
            continue;
        ++placed;
        const auto actor = s.open->actor_block();
        const repl::Value *v = actor.state.state.get(*actor.state.schema, "GatherableActorDef.Guid");
        const int64_t guid = (v && v->get_if<int64_t>()) ? *v->get_if<int64_t>() : 0;
        with_row += w.g->def(guid) != nullptr;
    }
    CHECK(placed >= 439 && with_row == placed, "every captured node's def has a gather row (the 439 of the Riverlands and the E1 area's)");
    for (size_t i = 0; i < spawns::garden::kGardenDefCount; ++i) {
        ++garden;
        garden_rows += w.g->def(static_cast<int64_t>(spawns::garden::kGardenDefs[i].guid)) != nullptr;
    }
    CHECK(garden == 10 && garden_rows == 10, "the GM garden's 10 defs have gather rows");
}

static void test_data(World &w) {
    std::printf("the gather data of the real content.db\n");
    CHECK(w.g->defs() == 339 && w.g->tools() == 87 && w.g->data().quality_expr.size() == 47, "339 node defs, 87 tools, 47 item variations");
    std::set<int64_t> roots;
    for (const auto &[guid, d] : w.g->data().defs)
        roots.insert(d.reward_guid);
    CHECK(roots.size() > 100 && w.g->tables() >= roots.size(), "the reward tables of the nodes (and their sub tables) are loaded");
    check_data_spawns(w);
    size_t formulas = 0;
    size_t rewards = 0;
    size_t tools_ok = 0;
    size_t professions_ok = 0;
    size_t compiled = 0;
    for (const auto &[guid, d] : w.g->data().defs) {
        if (d.duration_expr != 0 && w.world->ability_catalog()->program(d.duration_expr) >= 0)
            ++formulas;
        if (d.reward_guid != 0)
            ++rewards;
        if (is_gathering_profession(d.profession_guid))
            ++professions_ok;
    }
    CHECK(formulas == 339 && rewards == 339 && professions_ok == 339, "all 339 defs: a compiled channel formula, a reward table, a gathering profession");
    for (const auto &[item, t] : w.g->data().tools) {
        if (is_valid_tool_slot(t.tool_slot))
            ++tools_ok;
    }
    const bool pickaxes_ok = tools_ok > 0 && w.g->tool(id::kT1Pickaxe) && w.g->tool(id::kT1Pickaxe)->cert_tier == 0 && w.g->tool(id::kT3Pickaxe)->cert_tier == 2;
    CHECK(pickaxes_ok, "the T1 pickaxe reaches Novice nodes, the T3 pickaxe Journeyman");
    for (const auto &[var, expr] : w.g->data().quality_expr) {
        if (expr != 0 && w.world->ability_catalog()->program(expr) >= 0)
            ++compiled;
    }
    CHECK(compiled >= 44, "44 of the 47 variations' Quality formulas compile (the Hunting prototype's and the loot variations' are not all gathering)");
    const content::GatherDefRow *oak = w.def_named("Tree_Oak_Large_2");
    const content::GatherDefRow *stone = w.def_named("Stone_Basalt_Large_3");
    CHECK(oak && oak->override_respawn == 14400.0 && oak->empty_duration == 300.0, "Tree_Oak_Large_2: override 14,400 s, EmptyDuration 300 s");
    CHECK(stone && stone->override_respawn == 0.0 && stone->empty_duration == 840.0, "Stone_Basalt_Large_3: no override, EmptyDuration 840 s");
}

static void check_board_beats_and_complete(gather::NodeBoard &board, uint64_t a, uint64_t b, Clock::time_point now) {
    std::string why;
    CHECK(!board.beat(a, now + milliseconds(99)), "no heartbeat before 100 ms");
    CHECK(board.beat(a, now + milliseconds(100)) && board.channel_of(a)->step == 1, "the first heartbeat at 100 ms");
    CHECK(!board.beat(a, now + milliseconds(150)), "none at 150 ms");
    CHECK(board.beat(a, now + milliseconds(200)) && board.channel_of(a)->step == 2, "the second at 200 ms");
    const bool stall_ok = board.beat(a, now + seconds(5)) && board.channel_of(a)->step == 3 && !board.beat(a, now + seconds(5) + milliseconds(50));
    CHECK(stall_ok, "a stall makes one update, not a burst");
    CHECK(!board.beat(a, now + seconds(10)), "no heartbeat once the channel's time is over: the completion is next");
    CHECK(board.complete(a, now + seconds(10), board.respawn_of(14400.0, 300.0), why), "the channel completes: Gathered");
    const bool respawn_ok = board.state_of(a) == gather::NodeState::Gathered && board.gathered_until(a) && std::chrono::duration<double>(*board.gathered_until(a) - (now + seconds(10))).count() == 7200.0;
    CHECK(respawn_ok, "the respawn is the data's 14,400 s (the actor def's override) times 0.5");
    CHECK(!board.complete(a, now, seconds(1), why), "a second completion of the same channel is refused");
    CHECK(!board.begin(a, channeler(2, 20), now + seconds(11), seconds(10), why) && why.find("gathered") != std::string::npos, "a gathered node cannot be started");
    const bool def_respawn_ok = board.respawn_of(0.0, 840.0) == std::chrono::duration<double>(420.0) && board.respawn_of(0.0, 0.0) == std::chrono::duration<double>(1.0);
    CHECK(def_respawn_ok, "no override: the foliage's EmptyDuration; never under a second");
    CHECK(board.cancel(b, "the second player walks away") && board.state_of(b) == gather::NodeState::Available, "player 2 walks away from node b: it is Available again");
}

static void check_board_respawn_and_edge_cases(World &w, gather::NodeBoard &board, uint64_t a, uint64_t b, Clock::time_point now) {
    std::string why;
    CHECK(board.advance(now + seconds(7000)) == 0 && board.state_of(a) == gather::NodeState::Gathered, "a gathered node stays gathered until its time");
    CHECK(board.advance(now + seconds(7211)) == 1 && board.state_of(a) == gather::NodeState::Available, "it is Available again when its time has come");
    const world::WorldActor *na = w.registry().find(a);
    CHECK(na && !gather::node_gathered(*na) && gather::node_status(*na) == 0, "and the actor is the fresh node's again: bGathered off, no status");
    CHECK(board.begin(a, channeler(1, 10), now + seconds(7300), seconds(10), why), "so the node can be harvested again");
    const bool cancel_ok = board.cancel(a, "a test") && board.state_of(a) == gather::NodeState::Available && !board.cancel(a, "twice");
    CHECK(cancel_ok, "a cancel makes the node Available once");
    CHECK(board.begin(b, channeler(2, 20), now + seconds(7300), seconds(10), why), "player 2 starts at node b again");
    const bool cancel_p2 = board.cancel_player(2, "the session ends") == 1 && board.state_of(b) == gather::NodeState::Available && board.cancel_player(2, "again") == 0;
    CHECK(cancel_p2, "the channels of a player who leaves are cancelled");
    const Clock::time_point later = now + seconds(8000);
    CHECK(board.begin(a, channeler(3, 30), later, seconds(10), why), "a channel starts");
    CHECK(board.advance(later + seconds(39)) == 0 && board.state_of(a) == gather::NodeState::Channeling, "30 s after it was due it is still the channel's");
    const bool stuck_ok = board.advance(later + seconds(41)) == 1 && board.state_of(a) == gather::NodeState::Available && board.stats().stuck == 1;
    CHECK(stuck_ok, "a session that never completes it is overtaken: the node is Available");
    const uint64_t c = w.add_node(id::kDaffodil, 3);
    CHECK(board.begin(c, channeler(4, 40), later, seconds(10), why), "a channel starts on a third node");
    w.registry().remove(c);
    CHECK(!board.beat(c, later + milliseconds(200)), "a heartbeat to a node that left the world makes nothing");
    const bool gone_ok = !board.complete(c, later + seconds(10), seconds(1), why) && board.state_of(c) == gather::NodeState::Available && board.stats().gone >= 1;
    CHECK(gone_ok, "a node removed during the channel cannot be completed, and is forgotten");
}

static void test_board(World &w) {
    std::printf("the board: the shared state of the nodes\n");
    gather::NodeBoard::Config cfg;
    cfg.respawn_mult = 0.5;
    gather::NodeBoard board(w.registry(), cfg);
    std::string why;
    const uint64_t a = w.add_node(id::kDaffodil, 1);
    const uint64_t b = w.add_node(id::kMoonbell, 2);
    CHECK(board.state_of(a) == gather::NodeState::Available && board.channeling() == 0 && board.gathered() == 0, "a fresh node is Available");
    Clock::time_point now = t0;
    CHECK(board.begin(a, channeler(1, 10), now, seconds(10), why), "player 1 starts a channel on node a");
    CHECK(board.state_of(a) == gather::NodeState::Channeling && board.channel_of(a) && board.channel_of(a)->player_id == 1, "node a is Channeling for player 1");
    CHECK(!board.begin(a, channeler(2, 20), now, seconds(10), why) && why.find("channeling") != std::string::npos, "player 2 is refused: the state is shared (the node is channeling)");
    CHECK(!board.begin(a, channeler(1, 10), now, seconds(10), why), "so is player 1 asking again: nothing is started twice");
    CHECK(board.begin(b, channeler(2, 20), now, seconds(10), why), "player 2 harvests the other node");
    CHECK(board.channels_of(1) == std::vector<uint64_t>{a} && board.channels_of(2) == std::vector<uint64_t>{b}, "each player's channels");
    CHECK(!board.begin(999999, channeler(3, 30), now, seconds(1), why), "a node that is not in the world is refused");
    check_board_beats_and_complete(board, a, b, now);
    check_board_respawn_and_edge_cases(w, board, a, b, now);
}

static void test_input_never_empty(World &w) {
    std::printf("the node's CanInputActivate is never empty on the wire (the client indexes the array without a bound: an empty one crashed it)\n");
    gather::NodeBoard board(w.registry(), gather::NodeBoard::Config{});
    std::string why;
    const uint64_t a = w.add_node(id::kDaffodil, 41);
    repl::ActorShadow shadow;
    std::vector<repl::ContentBlock> opened;
    CHECK(a != 0 && repl::collect_open_blocks(w.registry().find(a)->actor, shadow, opened), "a fresh node opens (retail's open carries no CanInputActivate: the client keeps its class default)");
    bool open_has_it = false;
    for (const repl::ContentBlock &b : opened) {
        if (can_input_on_wire(b).has_value())
            open_has_it = true;
    }
    CHECK(!open_has_it, "a fresh node's open does not carry CanInputActivate (1427's does not)");
    Clock::time_point now = t0;
    CHECK(board.begin(a, channeler(1, 10), now, seconds(10), why), "a channel starts");
    read_can_input_update(w, a, shadow);
    CHECK(board.cancel(a, "the player walks away"), "and is cancelled");
    CHECK(four_and_first_is(read_can_input_update(w, a, shadow), true), "the cancel tells the client CanInputActivate with four entries, the first true: the interaction is open again (an empty array was the crash)");
    CHECK(board.begin(a, channeler(1, 10), now + seconds(1), seconds(10), why), "a channel starts again");
    read_can_input_update(w, a, shadow);
    CHECK(board.complete(a, now + seconds(11), seconds(100), why), "and completes");
    CHECK(four_and_first_is(read_can_input_update(w, a, shadow), false), "the done update tells the client four entries of which the first is false (retail's 33 bits)");
    CHECK(board.advance(now + seconds(112)) == 1 && board.state_of(a) == gather::NodeState::Available, "the node returns after its respawn");
    CHECK(four_and_first_is(read_can_input_update(w, a, shadow), true), "the respawn tells the client four entries, the first true: the node can be harvested again");
    CHECK(board.begin(a, channeler(2, 20), now + seconds(200), seconds(10), why), "a channel starts a third time");
    read_can_input_update(w, a, shadow);
    CHECK(board.advance(now + seconds(200 + 10 + 31)) == 1 && board.state_of(a) == gather::NodeState::Available, "its session never completes it: the board frees the node 30 s after its time");
    const auto after_release = read_can_input_update(w, a, shadow);
    CHECK(!after_release || four_and_first_is(after_release, true), "the release tells the client nothing new, or four entries with the first true: never an empty array");
    CHECK(registry_can_input_size(w.registry().find(a)) == 4, "and the node in the registry holds four entries");
}

int main() {
    spdlog::set_level(spdlog::level::warn);
    const char *path = db_path();
    if (!path) {
        std::printf("[skip] test_world_gather: no content.db (set AOC_CONTENT_DB, or build dist/content/content.db)\n");
        return 0;
    }
    std::printf("content.db: %s\n", path);
    World w;
    if (!w.open(path))
        return 1;
    std::printf("%zu artisanship rating stats in the bundle\n", w.ratings.size());
    test_data(w);
    test_run_1427(w);
    test_gathered_1454(w);
    test_board(w);
    test_input_never_empty(w);
    run_gather_rolls_tests(w);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
