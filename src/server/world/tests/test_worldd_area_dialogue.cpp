// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "area_session_test_harness.h"
#include "protocol/repl/craft_rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "recordings.h"

namespace {

constexpr int64_t kTownCrierRoot = 6064634349883577684ll;
constexpr int64_t kTownCrierGreeting = 6064634349883577686ll;
constexpr int64_t kLionhold = 6064630651307950121ll;
constexpr int64_t kDialogueGreeting = 6064634349883573705ll;
constexpr int64_t kDialogueSecond = 6064634349883573706ll;

struct CrierFacts : dl::Facts {
    int64_t node = 6064630651307950001ll, npc_node = 6064630651307950001ll;
    bool citizen = false;
    std::optional<dl::ExprValue> call(const std::string& name, const std::vector<dl::ExprValue>& args) const override {
        if (name == "GetPlayer") return dl::ExprValue::of(1);
        if (name == "GetInteractionTarget") return dl::ExprValue::of(2);
        if (name == "GetCurrentNode") return dl::ExprValue::of(node);
        if (name == "Node_GetActorCityNode") return dl::ExprValue::of(!args.empty() && args[0].i == 2 ? npc_node : node);
        if (name == "Node_IsPlayerCitizen") return dl::ExprValue::of(citizen ? 1 : 0);
        if (name.rfind("Node_", 0) == 0) return dl::ExprValue::of(0);
        return std::nullopt;
    }
};

std::vector<int32_t> line_indexes(const dl::Talk& t) {
    std::vector<int32_t> out;
    for (const dl::Talk::Line& l : t.state().lines) out.push_back(l.index);
    return out;
}

void verify_dialogue_population() {
    std::string e;
    auto db = content::ContentDb::open(dialogue_db_path(), e);
    const auto pop = db ? db->population_data(e) : std::nullopt;
    size_t inst = 0, asset = 0;
    bool crier = false;
    if (pop) {
        for (const auto& [g, i] : pop->instances) {
            inst += i.dialogue_root != 0 ? 1 : 0;
            crier = crier || i.dialogue_root == kTownCrierRoot;
        }
        for (const auto& [g, a] : pop->assets) asset += a.dialogue_root != 0 ? 1 : 0;
    }
    CHECK(pop && inst == 1294 && asset == 48 && crier, "1,294 instances and 48 assets open a conversation, the Town Crier's among them");
}

void test_dialogue_book() {
    std::printf("the conversations of the data\n");
    std::string why;
    const auto book = dialogue_book(why);
    if (!book) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    CHECK(book->roots() == 1190 && book->nodes() == 5317, "the book holds the data's 1,190 roots and 5,317 nodes");
    size_t lines = 0, gated = 0, dangling = 0, empty_automatic = 0;
    for (const auto& [guid, n] : book->data().nodes) {
        lines += n.choices.size();
        if (n.type == "Automatic" && n.choices.empty()) ++empty_automatic;
        for (const auto& c : n.choices) {
            gated += c.requirement.empty() ? 0 : 1;
            if (c.next != 0 && !book->node(c.next)) ++dangling;
        }
    }
    CHECK(lines == 6090 && gated == 1882, "6,090 lines, 1,882 of them with a condition");
    CHECK(dangling == 0, "every line leads to a node of the data (or ends the conversation)");
    size_t bad_roots = 0;
    for (const auto& [root, top] : book->data().roots) bad_roots += (top != 0 && !book->node(top)) ? 1 : 0;
    CHECK(bad_roots == 0, "every root starts at a node of the data");
    CHECK(empty_automatic == 19, "19 automatic nodes have no line (a conversation that reaches one shows nothing)");
    CrierFacts facts;
    std::set<std::string> gaps;
    dl::Evaluator ev(*book, facts, [&](const std::string& s) { gaps.insert(s); });
    size_t holding = 0;
    for (const auto& [guid, n] : book->data().nodes) {
        for (const auto& c : n.choices) holding += ev.holds(c.requirement) ? 1 : 0;
    }
    CHECK(holding >= 4208, "every condition evaluates (the 4,208 lines with none always show)");
    std::printf("  [info] %zu of 6,090 lines show to a character with nothing; %zu kinds of gap\n", holding, gaps.size());
    verify_dialogue_population();
    const auto* n = book->node(6064634349883578023ll);
    CHECK(n && n->choices.size() == 3 && n->type == "Response", "node 6064634349883578023 has exactly 3 lines (the 3 retail sent), indexes 0 to 2");
}

void test_dialogue_town_crier() {
    std::printf("the Town Crier's lines for a character with no quests\n");
    std::string why;
    const auto book = dialogue_book(why);
    if (!book) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    const auto* greeting = book->node(kTownCrierGreeting);
    size_t plain = 0, gated = 0;
    for (const auto& c : greeting ? greeting->choices : std::vector<content::DialogueChoiceRow>{}) (c.requirement.empty() ? plain : gated)++;
    CHECK(greeting && greeting->choices.size() == 9 && plain == 4 && gated == 5, "9 lines: 4 with no condition and 5 with one");
    const auto* top = book->node(book->top_node(kTownCrierRoot));
    CHECK(top && top->type == "Automatic" && top->choices.size() == 1 && top->choices[0].next == kTownCrierGreeting, "the root starts at an automatic node whose one line leads to those nine");

    const auto visible = [&](CrierFacts facts) {
        dl::Evaluator ev(*book, facts);
        dl::Talk t;
        const dl::Result r = t.begin(*book, ev, 77, kTownCrierRoot);
        CHECK(r.outcome == dl::Outcome::Changed && t.state().shown_node == kTownCrierGreeting && t.state().greeting, "the click shows the greeting, not the automatic node");
        return line_indexes(t);
    };
    CHECK(visible(CrierFacts{}) == std::vector<int32_t>({0, 5, 6, 7, 8}), "a new character in a town: lines 0, 5, 6, 7 and 8");
    CrierFacts in_lionhold;
    in_lionhold.node = kLionhold;
    CHECK(visible(in_lionhold) == std::vector<int32_t>({5, 6, 7, 8}), "in Lionhold the first line is hidden");
    CrierFacts citizen;
    citizen.citizen = true;
    CHECK(visible(citizen) == std::vector<int32_t>({5, 6, 7, 8}), "a citizen does not get the first line, and the election line needs an election");

    CrierFacts f;
    dl::Evaluator ev(*book, f);
    dl::Talk t;
    t.begin(*book, ev, 77, kTownCrierRoot);
    dl::Result r = t.choose(*book, ev, choice_of(kTownCrierGreeting, 7));
    CHECK(r.outcome == dl::Outcome::Nothing && t.open() && !r.skipped.empty(), "the bulletin line opens a window: skipped, the conversation stays as it is");
    r = t.choose(*book, ev, choice_of(kTownCrierGreeting, 5));
    CHECK(r.outcome == dl::Outcome::Changed && t.state().shown_node == 6064634349883577707ll && !t.state().greeting && t.state().previous_choice_node == kTownCrierGreeting &&
              t.state().previous_choice_index == 5 && t.state().current_node == 6064634349883577707ll && t.state().previous_node == 0,
          "\"about this settlement\" leads on: the greeting is the line's node, the next node the current one");
    CHECK(line_indexes(t) == std::vector<int32_t>({0, 1, 2}), "its three lines show");
    r = t.choose(*book, ev, choice_of(6064634349883577707ll, 1));
    CHECK(r.outcome == dl::Outcome::Changed && t.state().shown_node == kTownCrierGreeting && t.state().previous_node == 6064634349883577707ll, "\"another question\" goes back, the node left is the previous node");
    r = t.choose(*book, ev, choice_of(kTownCrierGreeting, 8));
    CHECK(r.outcome == dl::Outcome::Changed && !t.open() && t.state().lines.empty(), "\"Exit.\" ends the conversation");
    t.begin(*book, ev, 77, kTownCrierRoot);
    CHECK(t.choose(*book, ev, choice_of(kTownCrierGreeting, 4)).outcome == dl::Outcome::Refused && t.open() && t.state().shown_node == kTownCrierGreeting, "a line that is not shown is refused, nothing changes");
    CHECK(t.choose(*book, ev, choice_of(kTownCrierGreeting, 99)).outcome == dl::Outcome::Refused, "an index beyond the node is refused");
    CHECK(t.choose(*book, ev, choice_of(6064634349883577707ll, 0)).outcome == dl::Outcome::Refused, "a line of another node is refused");
    CHECK(t.begin(*book, ev, 78, kTownCrierRoot).outcome == dl::Outcome::Refused && t.state().source == 77, "a second click while the window is open is refused");
}

struct RecordedBlock {
    bool rep = false;
    DlgBits payload;
};

std::map<std::string, RecordedBlock> recorded_dialogue() {
    std::map<std::string, RecordedBlock> out;
    const std::string path = recordings::find("dialogue_blocks.txt");
    if (path.empty()) return out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string label, dir, hex;
        int rep = 0;
        size_t bits = 0;
        if (!(ss >> label >> dir >> rep >> bits >> hex)) continue;
        if (dir != "S>C") continue;
        out[label] = {rep != 0, DlgBits::of_hex(hex, bits)};
    }
    return out;
}

std::optional<DlgBits> without_object(const DlgBits& bits, const emit::FIntrepidNetworkGUID& guid) {
    BunchWriter w;
    emit::write_intrepid_guid(w, guid);
    if (w.bit_pos() != 128) return std::nullopt;
    const DlgBits needle = DlgBits::of_writer(w);
    for (size_t at = 0; at + needle.size() <= bits.size(); ++at) {
        if (bits.slice(at, needle.size()) == needle) {
            DlgBits cut = bits;
            cut.erase(at, needle.size());
            return cut;
        }
    }
    return std::nullopt;
}

bool wire_ids_exist(const dl::Book& book, const ClientDialogue& c) {
    bool ok = c.shown_node == 0 || book.node(c.shown_node) != nullptr;
    for (const auto& l : c.lines) ok = ok && book.node(l.node) != nullptr && l.type_id == repl::kDialogueNodeTypeId;
    return ok;
}

bool lines_follow_the_rules(const dl::Book& book, const ClientDialogue& c) {
    bool ok = true;
    for (const auto& l : c.lines) {
        const auto* n = book.node(l.node);
        ok = ok && l.node == c.shown_node && n && l.index >= 0 && static_cast<size_t>(l.index) < n->choices.size() && l.kind == 0;
    }
    return ok;
}

void verify_greeting_block_parity(const PcBlock& blk, const world::WorldActor& npc) {
    const auto rec = recorded_dialogue();
    const auto r54 = rec.find("greeting_one_line");
    if (r54 == rec.end()) {
        recordings::print_skip("dialogue_blocks.txt");
        return;
    }
    size_t pos = 0;
    const std::vector<uint8_t> raw = r54->second.payload.bytes();
    const auto theirs_state = repl::read_rep_block(class_named("AoCPlayerControllerBP_C"), raw.data(), r54->second.payload.size(), pos);
    const repl::Value* ov = theirs_state ? theirs_state->get(156) : nullptr;
    const auto* their_guid = ov ? ov->get_if<emit::FIntrepidNetworkGUID>() : nullptr;
    const auto mine = without_object(blk.bits, npc.actor.guid);
    const auto theirs = their_guid ? without_object(r54->second.payload, *their_guid) : std::nullopt;
    CHECK(mine && theirs && *mine == *theirs && blk.bits.size() == r54->second.payload.size(),
          "R: the greeting equals the recorded block bit for bit (503 bits), the NPC's object reference apart");
}

void verify_dialogue_open_and_step(const dl::Book& book, DialogueHarness& h, const world::WorldActor& npc, ClientDialogue& client) {
    size_t from = h.got.size();
    h.client_request(interact(npc.npc.interact_component), h.pawn_ch);
    auto blocks = pc_blocks_since(h, from);
    CHECK(blocks.size() == 1 && h.s->talk_actor() == h.talker && h.s->dialogue_opens() == 1, "the click is answered with one block and the session talks to the NPC");
    if (blocks.size() == 1) {
        CHECK(blocks[0].handles == std::vector<uint16_t>({154, 156, 166, 170}), "the greeting carries CurrentChoices, ObjectSource, the node shown and bIsInGreeting: handles 154, 156, 166, 170 (K4)");
        client.apply(blocks[0].state);
        CHECK(wire_ids_exist(book, client), "K1: the node and the lines name nodes of the data, by the node table's TypeId");
        CHECK(lines_follow_the_rules(book, client) && client.lines.size() == 1 && client.greeting && client.shown_node == kDialogueGreeting, "K2: one line, naming the node shown, index 0, type Default; the greeting flag is on");
        CHECK(client.source == npc.actor.guid, "K3: the source is the NPC the connection has open");
        verify_greeting_block_parity(blocks[0], npc);
    }
    from = h.got.size();
    h.client_request(interact(npc.npc.interact_component), h.pawn_ch);
    CHECK(pc_blocks_since(h, from).empty() && h.s->dialogue_opens() == 1, "a second click while the window is open changes nothing");

    from = h.got.size();
    h.client_request(dialogue_choice(choice_of(kDialogueGreeting, 0)), h.pc_ch);
    blocks = pc_blocks_since(h, from);
    CHECK(blocks.size() == 1 && h.s->dialogue_steps() == 1, "the line is answered with one block");
    if (blocks.size() == 1) {
        CHECK(blocks[0].handles == std::vector<uint16_t>({146, 151, 154, 166, 170}), "the step carries the line chosen, the node now, its lines, the node shown and the cleared greeting flag: handles 146, 151, 154, 166, 170 (K4)");
        client.apply(blocks[0].state);
        CHECK(client.shown_node == kDialogueSecond && client.current_node == kDialogueSecond && client.previous_choice_node == kDialogueGreeting && !client.greeting && client.lines.size() == 6,
              "the client shows the second node with its six lines");
        CHECK(wire_ids_exist(book, client) && lines_follow_the_rules(book, client), "K1, K2: the ids exist, every line names the node shown with an index below its count");
        const auto rec = recorded_dialogue();
        const auto r94 = rec.find("step_six_lines");
        if (r94 != rec.end()) CHECK(blocks[0].bits == r94->second.payload, "R: the step equals the recorded block bit for bit (1,443 bits)");
    }
}

void verify_dialogue_close_and_death(const dl::Book& book, int64_t root, DialogueHarness& h, const world::WorldActor& npc, ClientDialogue& client) {
    size_t from = h.got.size();
    h.client_request(dialogue_choice(choice_of(kDialogueSecond, 3)), h.pc_ch);
    CHECK(pc_blocks_since(h, from).empty() && h.s->talk().state().shown_node == kDialogueSecond, "a line whose action is not done yet changes nothing");

    const size_t refused = h.s->dialogue_refused();
    from = h.got.size();
    h.client_request(dialogue_choice(choice_of(kDialogueGreeting, 0)), h.pc_ch);
    h.client_request(dialogue_choice(choice_of(kDialogueSecond, 17)), h.pc_ch);
    h.client_request(dialogue_choice(choice_of(123456789, 0)), h.pc_ch);
    CHECK(h.s->dialogue_refused() == refused + 3 && pc_blocks_since(h, from).empty() && h.s->talk().state().shown_node == kDialogueSecond,
          "a stale node, an index the node lacks and a node nobody showed are refused: no block, nothing changed");

    repl::DialogueResponse close;
    close.response_type = 2;
    close.quest_response = 3;
    close.node_type_id = repl::kDialogueNodeTypeId;
    repl::DialogueResponse window;
    window.response_type = 3;
    window.node_type_id = repl::kDialogueNodeTypeId;
    window.window_type = 4;
    from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, dialogue_choice(close), true), h.client.actor(h.pc_ch, dialogue_choice(close), true), h.client.actor(h.pc_ch, dialogue_choice(window), true)});
    h.settle();
    auto blocks = pc_blocks_since(h, from);
    CHECK(blocks.size() == 1 && !h.s->talk().open() && h.s->talk_actor() == 0, "the client's three close calls are answered with one block, and the conversation is over");
    if (blocks.size() == 1) {
        CHECK(blocks[0].handles == std::vector<uint16_t>({146, 151, 154, 156, 166}), "the clear carries the line chosen, the node now, no lines, no source and no node shown: handles 146, 151, 154, 156, 166 (K4)");
        client.apply(blocks[0].state);
        CHECK(client.lines.empty() && client.shown_node == 0 && client.current_node == 0 && client.previous_choice_node == 0 && client.source == emit::FIntrepidNetworkGUID{},
              "K3: a close writes the source back to nothing and leaves no node");
        const auto rec = recorded_dialogue();
        const auto r224 = rec.find("clear_after_step");
        if (r224 != rec.end()) CHECK(blocks[0].bits == r224->second.payload, "R: the clear equals the recorded block bit for bit (433 bits)");
    }
    from = h.got.size();
    h.client_request(dialogue_choice(choice_of(kDialogueSecond, 0)), h.pc_ch);
    CHECK(pc_blocks_since(h, from).empty(), "a choice after the close changes nothing");
    h.client_request(interact(npc.npc.interact_component), h.pawn_ch);
    CHECK(h.s->talk().open() && h.s->dialogue_opens() == 2, "and a new click opens the window again");

    from = h.got.size();
    h.fx.world->registry().remove(h.talker);
    h.run(milliseconds(300));
    CHECK(!h.s->talk().open() && !pc_blocks_since(h, from).empty(), "the NPC is gone: the conversation ends and the client is told");

    DialogueHarness d(book, root);
    d.play();
    d.run(milliseconds(1500));
    const world::WorldActor* npc2 = d.fx.world->registry().find(d.talker);
    d.client_request(interact(npc2->npc.interact_component), d.pawn_ch);
    CHECK(d.s->talk().open(), "a second player talks");
    from = d.got.size();
    d.s->take_damage(d.conn, 1.0e9, d.t);
    d.settle();
    CHECK(!d.s->talk().open() && !pc_blocks_since(d, from).empty(), "the player dies: the conversation ends and the client is told");
    const size_t refused_dead = d.s->dialogue_refused();
    d.client_request(dialogue_choice(choice_of(kDialogueGreeting, 0)), d.pc_ch);
    CHECK(d.s->dialogue_refused() == refused_dead + 1, "a choice from a dead player is refused");
}

void test_dialogue_session() {
    std::printf("the talking window through the session\n");
    std::string why;
    auto book = dialogue_book(why);
    if (!book) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    const int64_t root = root_with_top(*book, kDialogueGreeting);
    CHECK(root != 0, "a root starts at the node the recorded conversation showed");
    DialogueHarness h(*book, root);
    h.play();
    h.run(milliseconds(1500));
    CHECK(h.s->actor_view()->state_of(h.talker) == world::ClientState::Awake, "the NPC is open on the client");
    const world::WorldActor* npc = h.fx.world->registry().find(h.talker);
    ClientDialogue client;
    verify_dialogue_open_and_step(*book, h, *npc, client);
    verify_dialogue_close_and_death(*book, root, h, *npc, client);
}

void test_dialogue_1531() {
    std::printf("the vendor's greeting as recorded\n");
    std::string why;
    auto book = dialogue_book(why);
    if (!book) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    const auto rec = recorded_dialogue();
    const auto it = rec.find("greeting_three_lines");
    if (it == rec.end()) {
        recordings::print_skip("dialogue_blocks.txt");
        return;
    }
    const int64_t root = root_with_top(*book, 6064634349883578023ll);
    CHECK(root != 0, "a root starts at the node the recorded vendor showed");
    DialogueHarness h(*book, root);
    h.play();
    h.run(milliseconds(1500));
    const world::WorldActor* npc = h.fx.world->registry().find(h.talker);
    const size_t from = h.got.size();
    h.client_request(interact(npc->npc.interact_component), h.pawn_ch);
    const auto blocks = pc_blocks_since(h, from);
    CHECK(blocks.size() == 1, "the click is answered with one block");
    if (blocks.size() != 1) return;
    size_t pos = 0;
    const std::vector<uint8_t> raw = it->second.payload.bytes();
    const auto theirs_state = repl::read_rep_block(class_named("AoCPlayerControllerBP_C"), raw.data(), it->second.payload.size(), pos);
    const repl::Value* ov = theirs_state ? theirs_state->get(156) : nullptr;
    const auto* their_guid = ov ? ov->get_if<emit::FIntrepidNetworkGUID>() : nullptr;
    const auto mine = without_object(blocks[0].bits, npc->actor.guid);
    const auto theirs = their_guid ? without_object(it->second.payload, *their_guid) : std::nullopt;
    CHECK(mine && theirs && *mine == *theirs && blocks[0].bits.size() == it->second.payload.size(),
          "R: the vendor's greeting equals the recorded block bit for bit (929 bits), the NPC's object reference apart");
}

vendor::GmVendors dialogue_test_vendors() {
    vendor::GmVendor gear{6064634098852102156ll, "PTR_ONLY_Vendor_Gear_0-9", "Gear", "GM Gear 0-9", {fi::kMace1H, fi::kShield, fi::kRingA, fi::kRingB, fi::kPotion, fi::kHeavyChest}, 6, 1};
    vendor::GmVendor other{6064634360064966680ll, "PTR_ONLY_Vendor_Materials", "Materials", "GM Materials", {fi::kGatherCape}, 1, 1};
    return vendor::GmVendors::from_defs({gear, other});
}

struct VendorHarness : Harness {
    uint64_t gm = 0, gm2 = 0;
    const int64_t gear_def = 6064634098852102156ll, mats_def = 6064634360064966680ll;
    explicit VendorHarness(svc::Character r = cyb_record()) : Harness(std::move(r)) {
        fx.world->set_gm_vendors(dialogue_test_vendors());
        const spawns::OpenTemplate* vendor_t = fx.captured->template_for(spawns::kGmVendorArchetype);
        if (!vendor_t) { std::printf("  [FAIL] no vendor template\n"); std::exit(2); }
        gm = fx.add_vendor(*vendor_t, kCx + 800, kCy, gear_def, "GM Gear 0-9");
        gm2 = fx.add_vendor(*vendor_t, kCx, kCy + 900, mats_def, "GM Materials");
        start();
    }
    uint64_t component_of(uint64_t actor) const {
        const world::WorldActor* a = fx.world->registry().find(actor);
        return a ? a->npc.interact_component : 0;
    }
    void talk_to(uint64_t actor) { client_request(interact(component_of(actor)), pawn_ch); }
};

BunchWriter purchase(const repl::VendorPurchaseCart& cart) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerHandleVendorPurchase");
    const auto params = repl::vendor_purchase_params(*rpc, cart);
    return params ? actor_rpc(pc, "ServerHandleVendorPurchase", *params) : BunchWriter();
}

repl::VendorPurchaseCart cart_of(int64_t vendor, std::vector<std::pair<int64_t, int32_t>> items) {
    repl::VendorPurchaseCart c;
    c.vendor = vendor;
    for (const auto& [record, qty] : items) c.items.push_back({record, qty, std::nullopt, std::nullopt});
    return c;
}

BunchWriter start_craft_for_no_station() {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerStartCraft");
    repl::StartCraft start;
    start.quantity = 1;
    const auto params = rpc ? repl::start_craft_params(*rpc, start) : std::nullopt;
    return params ? actor_rpc(pc, "ServerStartCraft", *params) : BunchWriter();
}

void test_craft_start_for_no_station_stops_the_client() {
    std::printf("a craft start that names no station: the client's craft is stopped\n");
    VendorHarness h;
    h.play();
    h.run(milliseconds(1500));
    const size_t from = h.got.size();
    h.client_request(start_craft_for_no_station(), h.pc_ch);
    CHECK(h.s->quick_refused() + h.s->crafts_refused() == 1, "the start is refused and counted");
    CHECK(count_calls(pc_calls_since(h, from), "ClientInterruptCraft") == 1, "the client is sent ClientInterruptCraft");
}

void test_craft_start_is_not_slowed_by_the_shops() {
    std::printf("a craft start has a request budget of its own: a burst of purchases does not use it\n");
    VendorHarness h;
    h.play();
    h.run(milliseconds(1500));
    std::vector<fake::Bunch> burst;
    for (int i = 0; i < 12; ++i) burst.push_back(h.client.actor(h.pc_ch, purchase(cart_of(h.gear_def, {{fi::kPotion, 1}})), true));
    burst.push_back(h.client.actor(h.pc_ch, start_craft_for_no_station(), true));
    h.client_sends(std::move(burst));
    h.settle();
    CHECK(h.s->requests_dropped() >= 4, "the shops' budget cut the purchases of the burst");
    CHECK(h.s->quick_refused() + h.s->crafts_refused() == 1, "the craft start of the same burst was not cut: it was answered");
}

void test_gm_materials_with_a_full_item_bag() {
    std::printf("the GM vendor: materials go to the material bag when the item bag is full\n");
    std::vector<svc::InventoryItem> full = kit();
    full.erase(std::remove_if(full.begin(), full.end(), [](const svc::InventoryItem& i) { return i.container == Bag; }), full.end());
    for (int i = 0; i < items::kBackpackSlots; ++i) full.push_back(make_item(Bag, i, fi::kPotion, 500 + static_cast<uint32_t>(i)));
    VendorHarness f(cyb_record(full));
    {
        vendor::GmVendor gear{f.gear_def, "PTR_ONLY_Vendor_Gear_0-9", "Gear", "GM Gear 0-9", {fi::kMace1H, fi::kPotion}, 2, 1};
        vendor::GmVendor mats{f.mats_def, "PTR_ONLY_Vendor_Materials", "Materials", "GM Materials", {cf::resource::kCoal}, 1, 1};
        f.fx.world->set_gm_vendors(vendor::GmVendors::from_defs({gear, mats}));
    }
    f.play();
    f.run(milliseconds(1500));
    f.talk_to(f.gm2);
    CHECK(f.s->vendor_actor() == f.gm2 && bag_items(f) == 70, "the materials vendor's window is open and the item bag holds 70 stacks: no free slot");
    f.client_request(purchase(cart_of(f.mats_def, {{cf::resource::kCoal, 41}})), f.pc_ch);
    CHECK(f.s->vendor_purchases() == 1 && f.s->vendor_refused() == 0, "41 Coal (three stacks) are sold although the item bag has no free slot");
    CHECK(f.s->inventory().count(ItemContainer::Resource0) == 3 && bag_items(f) == 70, "they are in the material bag, and the item bag is as it was");
    f.run(milliseconds(2200));
    const size_t materials_before = f.s->inventory().count(ItemContainer::Resource0);
    f.client_request(purchase(cart_of(f.mats_def, {{cf::resource::kCoal, 500}})), f.pc_ch);
    CHECK(f.s->vendor_purchases() == 1 && f.s->vendor_refused() == 1 && f.s->inventory().count(ItemContainer::Resource0) == materials_before && bag_items(f) == 70,
          "a cart the material bag cannot hold is refused whole: nothing is added");
}

}  // namespace

void run_dialogue_session_suite() {
    test_dialogue_book();
    test_dialogue_town_crier();
    test_dialogue_session();
    test_dialogue_1531();
    test_craft_start_for_no_station_stops_the_client();
    test_craft_start_is_not_slowed_by_the_shops();
    test_gm_materials_with_a_full_item_bag();
}
