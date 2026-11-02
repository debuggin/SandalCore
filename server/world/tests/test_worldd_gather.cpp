// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_gather_common.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace emit = aoc::protocol::emit;
namespace cg = capture_gather;
using std::chrono::milliseconds;
using std::chrono::seconds;
using tsupport::Bits;
using tsupport::bits_of_hex;

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

static void check_rules_promotions_and_combat(World &w, const professions::Book &book, const professions::Professions &novice, const content::GatherDefRow &basalt, const content::GatherDefRow &halcyonite) {
    const content::GatherDefRow &rividium = w.g->data().defs.at(id::kRividium);
    professions::Professions apprentice = novice;
    apprentice.raise_xp_to(id::kMining, book.max_xp(*book.find(id::kMining), 0));
    apprentice.promote(id::kMining);
    CHECK(apprentice.tier(id::kMining) == 1 && apprentice.level(id::kMining) == 10, "an Apprentice Miner at level 10 (a Promote at the Novice cap)");
    CHECK(gather::check_requirements(halcyonite, apprentice, make_tool(w, id::kT1Pickaxe), false).why == gather::Refusal::ToolTierTooLow, "an Apprentice with a T1 pickaxe at an Apprentice node: the tool's tier is too low");
    CHECK(gather::check_requirements(halcyonite, apprentice, make_tool(w, id::kT2Pickaxe), false).ok(), "with a T2 pickaxe it harvests");
    CHECK(gather::check_requirements(halcyonite, apprentice, make_tool(w, id::kT3Pickaxe), false).why == gather::Refusal::ToolNotUsable, "a T3 pickaxe needs Journeyman: the Apprentice cannot use it (worldd's equip rules let it be worn)");
    CHECK(gather::check_requirements(rividium, apprentice, make_tool(w, id::kT2Pickaxe), false).why == gather::Refusal::NotCertified, "an Apprentice at a Journeyman node: not certified");
    professions::Professions journeyman = apprentice;
    journeyman.raise_xp_to(id::kMining, book.max_xp(*book.find(id::kMining), 1));
    journeyman.promote(id::kMining);
    CHECK(journeyman.tier(id::kMining) == 2 && gather::check_requirements(rividium, journeyman, make_tool(w, id::kT3Pickaxe), false).ok(), "a Journeyman with the T3 pickaxe harvests a Journeyman node");
    CHECK(gather::check_requirements(halcyonite, journeyman, make_tool(w, id::kT3Pickaxe), false).ok(), "...and an Apprentice node (a higher tool reaches lower nodes)");
    CHECK(gather::check_requirements(rividium, journeyman, make_tool(w, id::kT2Pickaxe), false).why == gather::Refusal::ToolTierTooLow, "...but not a Journeyman node with a T2 pickaxe");
    content::GatherDefRow no_combat = basalt;
    no_combat.allow_in_combat = false;
    const bool combat_ok = gather::check_requirements(no_combat, novice, make_tool(w, id::kT1Pickaxe), true).why == gather::Refusal::InCombat && gather::check_requirements(basalt, novice, make_tool(w, id::kT1Pickaxe), true).ok();
    CHECK(combat_ok, "in combat: refused where the interaction does not allow it, harvested where it does");
    CHECK(gather::check_requirements(basalt, professions::Professions(), make_tool(w, id::kT1Pickaxe), false).why == gather::Refusal::NoProfessions, "a world with no profession tables refuses");
    CHECK(gather::reach_cm(basalt) == basalt.interact_radius + basalt.interact_tolerance + 100.0, "the reach is the radius, the tolerance and 100 cm");
}

static void test_rules(World &w) {
    std::printf("the requirements of a harvest\n");
    const professions::Book &book = *w.world->professions();
    professions::Professions novice(book);
    const content::GatherDefRow *basalt = w.def_named("Stone_Basalt_Large_3");
    const content::GatherDefRow *halcyonite = &w.g->data().defs.at(id::kHalcyonite);
    const content::GatherDefRow *rividium = &w.g->data().defs.at(id::kRividium);
    CHECK(basalt && basalt->cert_tier == 0 && halcyonite->cert_tier == 1 && rividium->cert_tier == 2, "Novice, Apprentice and Journeyman Mining nodes");
    CHECK(gather::check_requirements(*basalt, novice, make_tool(w, id::kT1Pickaxe), false).ok(), "a Novice with a T1 pickaxe harvests a Novice node");
    CHECK(gather::check_requirements(*basalt, novice, gather::ToolInfo{}, false).why == gather::Refusal::NoTool, "no tool in the slot: refused");
    gather::ToolInfo junk;
    junk.worn = true;
    junk.durability = 10;
    CHECK(gather::check_requirements(*basalt, novice, junk, false).why == gather::Refusal::NotATool, "a worn item that is no gathering tool: refused");
    CHECK(gather::check_requirements(*basalt, novice, make_tool(w, id::kT1Axe), false).why == gather::Refusal::WrongToolProfession, "an axe in the pickaxe's place: refused (the tool is another profession's)");
    CHECK(gather::check_requirements(*basalt, novice, make_tool(w, id::kT1Pickaxe, 0.0f), false).why == gather::Refusal::ToolBroken, "a tool with no durability: refused");
    CHECK(gather::check_requirements(*halcyonite, novice, make_tool(w, id::kT1Pickaxe), false).why == gather::Refusal::NotCertified, "a Novice at an Apprentice node: not certified");
    check_rules_promotions_and_combat(w, book, novice, *basalt, *halcyonite);
}

static void expect_dist(const char *name, const int counts[7], int n, const double want[6]) {
    bool ok = true;
    char line[512];
    int len = std::snprintf(line, sizeof line, "%s:", name);
    for (int i = 0; i < 6; ++i) {
        const double p = want[i] / 100.0;
        const double got = static_cast<double>(counts[i]) / n;
        const double tol = 3.0 * std::sqrt(p * (1 - p) / n) + 0.0005;
        if (std::fabs(got - p) > tol)
            ok = false;
        len += std::snprintf(line + len, sizeof line - static_cast<size_t>(len), " %.2f", got * 100.0);
    }
    std::snprintf(line + len, sizeof line - static_cast<size_t>(len), " (want %.2f %.2f %.2f %.2f %.3f %.3f)", want[0], want[1], want[2], want[3], want[4], want[5]);
    CHECK(ok && counts[6] == 0, line);
}

static void test_rarity(World &w) {
    std::printf("the rarity of a harvest: 100,000 harvests per case against the analytic tables\n");
    constexpr int N = 100000;
    struct Case {
        const char *name;
        int64_t def;
        double rating;
        double want[6];
    };
    const Case cases[] = {
        {"standard, rating 0", id::kMoonbell, 0, {81.9, 11.056, 3.437, 2.096, 1.01, 0.501}},
        {"standard, rating 8409", id::kMoonbell, 8409, {45.5, 43.278, 7.186, 2.481, 1.049, 0.505}},
        {"_T1_Only, rating 0", id::kDaffodil, 0, {85.365, 10.805, 2.221, 1.098, 0.51, 0.001}},
        {"_T1_Only, rating 8409", id::kDaffodil, 8409, {47.4, 44.4, 6.1, 1.5, 0.55, 0.005}},
    };
    Rig rig(w, 77);
    for (const Case &c : cases) {
        w.rate_all(c.rating);
        int counts[7] = {0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < N; ++i) {
            const auto h = rig.harvest(c.def);
            const int band = (!h || h->items.empty()) ? 6 : std::min(h->primary_rarity, 6);
            ++counts[band];
        }
        expect_dist(c.name, counts, N, c.want);
    }
    w.rate_all(0);
    w.g->set_ratings({});
}

static void test_fresh_seed() {
    std::printf("the seed of the harvest rolls: a new number on every start of the server\n");
    std::set<uint64_t> seen;
    bool never_zero = true;
    for (int i = 0; i < 64; ++i) {
        const uint64_t s = gather::fresh_seed();
        if (s == 0)
            never_zero = false;
        seen.insert(s);
    }
    CHECK(never_zero, "a fresh seed is never 0 (0 is how --gather-seed asks for a new one)");
    CHECK(seen.size() == 64, "64 fresh seeds in a row are all different");
    CHECK(gather::Config{}.seed == 0x6A7E5EEDull, "a Config made by hand keeps the fixed seed, so the tests' rolls repeat");
    stats::SplitMix a;
    stats::SplitMix b;
    a.state = gather::fresh_seed();
    b.state = gather::fresh_seed();
    bool differ = false;
    for (int i = 0; i < 8; ++i) {
        if (a.rand_double() != b.rand_double())
            differ = true;
    }
    CHECK(differ, "two fresh seeds start two different streams of rolls");
}

static void check_tier_rolls(Rig &rig, const std::vector<double> &common_roll) {
    const struct {
        int64_t def;
        const char *name;
        int xp;
        int64_t quantity;
    } tiers[] = {
        {id::kDaffodil, "Daffodil (Novice)", 80, 3},
        {id::kMoonbell, "Moonbell (Apprentice)", 90, 3},
        {id::kHalcyonite, "Halcyonite (Apprentice)", 90, 5},
        {id::kRividium, "Rividium (Journeyman)", 100, 5},
    };
    for (const auto &t : tiers) {
        rig.rng.script = &common_roll;
        rig.rng.scripted = 0;
        std::string why;
        const auto h = rig.harvest(t.def, &why);
        const bool ok = valid_common_harvest(h, t.quantity, t.xp);
        const long long got_q = (h && !h->items.empty()) ? static_cast<long long>(h->items[0].quantity) : -1ll;
        const long long got_xp = h ? static_cast<long long>(h->profession_xp) : -1ll;
        char msg[200];
        std::snprintf(msg, sizeof msg, "%s: x%lld q1, +%lld profession XP (want x%lld, +%d)", t.name, got_q, got_xp, static_cast<long long>(t.quantity), t.xp);
        CHECK(ok, msg);
        CHECK(h && h->adventuring_xp > 0, (std::string(t.name) + ": and Adventuring experience").c_str());
        CHECK(rig.rng.scripted == common_roll.size(), (std::string(t.name) + ": the three scripted draws are all it used (the quality formula's two, the quantity's one)").c_str());
    }
}

static void check_quality_readings(World &w) {
    for (const bool literal : {false, true}) {
        gather::Config cfg;
        cfg.quality_literal = literal;
        std::string error;
        auto g2 = gather::Gathering::make(*w.db, w.registry(), *w.world->ability_catalog(), cfg, error);
        CHECK(g2 != nullptr, literal ? "a Gathering with the literal reading" : "a Gathering with the band reading");
        if (!g2)
            continue;
        std::map<int64_t, double> r;
        for (const int64_t guid : w.ratings)
            r[guid] = 8409;
        g2->set_ratings(r);
        const std::vector<double> bonus = {0.99, 0.5, 0.0, 0.0};
        stats::SplitMix r2;
        r2.script = &bonus;
        const content::GatherDefRow *oak = w.def_named("Tree_Oak_Large_2");
        std::string why;
        auto run = w.make_stats();
        auto ev = run->evaluation({}, why);
        const auto x = oak ? g2->harvest(*oak, *ev, 25, world::NodeSite{}, nullptr, r2, why) : std::nullopt;
        const long long got_q = (x && !x->items.empty()) ? static_cast<long long>(x->items[0].quantity) : -1ll;
        char msg[160];
        std::snprintf(msg, sizeof msg, "an Oak at rating 8409 with a lucky quantity roll gives %lld (5 + 1), %s reading", got_q, literal ? "literal" : "band");
        const bool ok = x && x->items.size() == 1 && x->items[0].quantity == 6 && x->items[0].quality == 1;
        CHECK(ok, msg);
    }
}

static void test_rules_of_rolls(World &w) {
    std::printf("what a harvest gives: the quantity, the experience, the wear\n");
    w.g->set_ratings({});
    Rig rig(w, 5);
    const std::vector<double> common_roll = {0.99, 0.5, 0.99};
    check_tier_rolls(rig, common_roll);
    const std::vector<double> uncommon = {0.99, 0.925, 0.99};
    rig.rng.script = &uncommon;
    rig.rng.scripted = 0;
    auto h = rig.harvest(id::kMoonbell);
    const bool unc_ok = h && h->items.size() == 1 && h->items[0].quality == 6 && h->primary_rarity == 1 && h->profession_xp == 135;
    CHECK(unc_ok, "an Uncommon (q6) Moonbell gives +135 (90 x 1.5)");
    check_quality_readings(w);
    rig.rng.script = &common_roll;
    rig.rng.scripted = 0;
    h = rig.harvest(id::kDaffodil);
    CHECK(h && h->durability_loss == 1.0, "a plant wears the tool by 1");
    size_t twos = 0;
    size_t ones = 0;
    size_t others = 0;
    for (const auto &[guid, d] : w.g->data().defs) {
        rig.rng.script = nullptr;
        const auto x = rig.harvest(guid);
        if (!x)
            continue;
        if (x->durability_loss == 2.0) {
            ++twos;
        } else if (x->durability_loss == 1.0) {
            ++ones;
        } else {
            ++others;
        }
    }
    CHECK(ones > 300 && others == 0, "every node wears the tool by 1 or 2");
    std::printf("      (%zu nodes wear the tool by 2, %zu by 1)\n", twos, ones);
}

static void check_bonus_1470_odds(World &w) {
    Rig stat(w, 31);
    int coal = 0;
    int sapphire = 0;
    int iron = 0;
    constexpr int N = 60000;
    for (int i = 0; i < N; ++i) {
        const auto x = stat.harvest(id::kLimestone);
        if (!x)
            continue;
        for (const combat::RolledItem &it : x->items) {
            coal += it.item == 109162169652923ll;
            sapphire += it.item == 6064629252182507530ll;
            iron += it.item == 109162169652928ll;
        }
    }
    char msg[200];
    std::snprintf(msg, sizeof msg, "over %d harvests: Coal %.2f %% (3 %%), Iron %.2f %% and Sapphire %.2f %% (about 50 %% each: two of three picks)", N, coal * 100.0 / N, iron * 100.0 / N, sapphire * 100.0 / N);
    const bool coal_ok = coal > N * 0.025 && coal < N * 0.036;
    const bool gem_ok = iron > N * 0.40 && iron < N * 0.60 && sapphire > N * 0.40 && sapphire < N * 0.60;
    CHECK(coal_ok && gem_ok, msg);
}

static void test_bonus_1470(World &w) {
    std::printf("the retail capture: the bonus drops of a Limestone node, with forced draws\n");
    w.g->set_ratings({});
    Rig rig(w, 9);
    const std::vector<double> draws = {0.99, 0.925, 0.99, 0.01, 0.1, 0.99, 0.3, 0.9, 0.99, 0.975, 0.99};
    rig.rng.script = &draws;
    std::string why;
    const auto h = rig.harvest(id::kLimestone, &why);
    CHECK(h.has_value(), "the harvest rolls");
    if (!h)
        return;
    CHECK(h->items.size() == 3, "three stacks");
    if (h->items.size() == 3) {
        std::vector<std::tuple<uint64_t, int64_t, int>> got;
        std::vector<std::tuple<uint64_t, int64_t, int>> want;
        for (const combat::RolledItem &it : h->items)
            got.emplace_back(static_cast<uint64_t>(it.item), it.quantity, it.quality);
        for (const cg::FrameEvent &e : cg::kFrameEvents) {
            if (e.capture == 1470)
                want.emplace_back(e.event.record, e.event.quantity, e.event.quality);
        }
        CHECK(got == want, "Limestone x5 q6, Coal x3 q1 and Sapphire x5 q31: the captured records, quantities and qualities, in the captured order");
    }
    CHECK(rig.rng.scripted == draws.size(), "and every scripted draw was used, none more drawn");
    const bool xp_ok = h->primary_rarity == 1 && h->primary_quality == 6 && h->profession_xp == 150;
    CHECK(xp_ok, "the experience is the first item's: Uncommon at a Journeyman node, 100 x 1.5 = 150 (the Heroic bonus gem adds nothing)");
    check_bonus_1470_odds(w);
}

static void check_duration_curve(World &w, Rig &rig) {
    std::string why;
    double last = 100;
    bool monotone = true;
    for (const double rating : {0.0, 50.0, 107.5, 500.0, 2000.0, 4204.5, 10000.0}) {
        w.rate_all(rating);
        const content::GatherDefRow *d = w.g->def(id::kHalcyonite);
        const auto s = w.g->channel_seconds(*d, *rig.run, why);
        if (!s || *s > last + 1e-9 || *s < 7.5 - 1e-9)
            monotone = false;
        if (s)
            last = *s;
        if (rating == 107.5)
            CHECK(s && std::fabs(*s / 15.0 - 0.927) < 0.003, "ratings of 107.5 (the equation sums two: 215) give the 0.927 of the retail data (13.9 s of 15)");
    }
    CHECK(monotone, "a higher rating never lengthens the channel, and it never goes under half the base");
    w.g->set_ratings({});
}

static void test_durations(World &w) {
    std::printf("the channel's length against the captured harvests\n");
    Rig rig(w, 3);
    w.g->set_ratings({});
    std::string why;
    const struct {
        int64_t def;
        double base;
    } bases[] = {
        {id::kDaffodil, 11.25},
        {id::kMoonbell, 11.25},
        {id::kHalcyonite, 15.0},
        {id::kRividium, 15.0},
        {id::kLimestone, 15.0},
    };
    bool bases_ok = true;
    for (const auto &b : bases) {
        const content::GatherDefRow *d = w.g->def(b.def);
        const auto s = d ? w.g->channel_seconds(*d, *rig.run, why) : std::nullopt;
        if (!s || std::fabs(*s - b.base) >= 1e-9 || d->base_duration != b.base)
            bases_ok = false;
    }
    CHECK(bases_ok, "at rating 0 the channel is the base: 11.25 s for a plant, 15 s for a large stone (and 7.5 s for a medium one)");
    check_duration_curve(w, rig);
    bool all_in = true;
    size_t seen = 0;
    for (const cg::Harvest &h : cg::kHarvests) {
        if (h.beats < 20)
            continue;
        const content::GatherDefRow *d = w.g->def(h.def);
        if (!d) {
            all_in = false;
            continue;
        }
        ++seen;
        const double factor = ((h.frame_ms - h.open_ms + 87) / 1000.0) / d->base_duration;
        if (factor <= 0.5 || factor > 1.0)
            all_in = false;
    }
    CHECK(seen == 9 && all_in, "the nine captured channels are 0.5 to 1.0 of their node's base duration");
}

int main() {
    spdlog::set_level(spdlog::level::warn);
    const char *path = db_path();
    if (!path) {
        std::printf("[skip] test_worldd_gather: no content.db (set AOC_CONTENT_DB, or build dist/content/content.db)\n");
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
    test_rules(w);
    test_rules_of_rolls(w);
    test_bonus_1470(w);
    test_durations(w);
    test_rarity(w);
    test_fresh_seed();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
