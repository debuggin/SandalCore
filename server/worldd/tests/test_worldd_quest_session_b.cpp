// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "quest_session_test_harness.h"

namespace {

constexpr int64_t kBeyamInstance = 6064634726276071434ll;
constexpr int64_t kBeyamRoot = 6064634722647147783ll;
constexpr int64_t kHungryForMore = 6064634722458600699ll;
constexpr int64_t kBeyamMeet01 = 6064634722650227976ll;
constexpr int64_t kBeyamTurnIn = 6064634722658551050ll;
constexpr int64_t kBreadObjective = 6064634722533704963ll;
constexpr int64_t kPilferedObjective = 6064634722552710404ll;
constexpr int64_t kBread = 6064634776923734016ll;

int count_sacks(const QuestHarness& h) {
    int n = 0;
    for (const svc::InventoryItem& it : h.s->inventory().items()) n += it.item_record_id == kRockSack ? it.quantity : 0;
    return n;
}

void verify_quest_gm_multi_and_items(FakeQuestApi& api, QuestHarness& h) {
    api.queue(kCyb, "give", "RVR_FTUE_QST_1400_WelcomeToVerra");
    api.queue(kCyb, "give", "CSQ_RVR_0031_ROR_RainOfRocks");
    h.commands();
    {
        std::set<std::string> guids;
        size_t expected = 0;
        for (const qs::QuestInstance& q : h.s->quest_log().instances()) {
            guids.insert(q.guid.hex());
            ++expected;
            for (const qs::ObjectiveState& o : q.objectives) {
                guids.insert(o.guid.hex());
                ++expected;
            }
        }
        CHECK(h.s->quest_log().instances().size() == 4 && guids.size() == expected, "K5: four quests in the log, every instance and objective guid different");
        std::set<int32_t> cache_ids, vars_ids;
        for (const qs::QuestInstance& q : h.s->quest_log().instances()) {
            cache_ids.insert(q.cache_id);
            vars_ids.insert(q.vars_id);
        }
        CHECK(cache_ids.size() == 4 && vars_ids.size() == 4 && cache_ids.count(repl::kIndexNone) == 0 && vars_ids.count(repl::kIndexNone) == 0,
              "K7: every item of each array has a replication id of its own");
    }

    const qs::QuestInstance* rain = h.s->quest_log().find(kRainOfRocks);
    CHECK(rain != nullptr, "Rain of Rocks is in the log");
    if (rain) {
        const qs::Guid128 g = rain->guid;
        const size_t from = h.got.size();
        h.client_request(abandon_rpc({g}), h.pc_ch);
        CHECK(h.s->quest_log().find(kRainOfRocks) == nullptr && count_sacks(h) == 0, "K11: the abandon took the quest out of the log, and the sacks out of the quest bag");
        size_t removes = 0;
        for (const repl::RpcCall& c : pc_calls_since(h, from)) {
            if (!c.rpc || std::string(c.rpc->name) != "ClientReceiveStorageEvent") continue;
            const auto ev = repl::parse_storage_event(c);
            removes += (ev && ev->type == static_cast<uint8_t>(repl::StorageEventType::Remove) && static_cast<int64_t>(ev->item.record_id) == kRockSack) ? 1 : 0;
        }
        CHECK(removes == 1, "K11: the client was told with one Remove event for the sacks");
    }
    h.end();
    bool saved_has_sacks = false;
    for (const auto& inv : h.sink.inventories) {
        for (const svc::InventoryItem& it : inv) saved_has_sacks = saved_has_sacks || it.item_record_id == kRockSack;
    }
    CHECK(!h.sink.inventories.empty() && !saved_has_sacks, "K11: the inventory handed to the saver when the session ended has no sacks");
    CHECK(api.log(kCyb).instances.size() == 3, "the saved log holds the three quests that are left");
}

void test_quest_gm() {
    std::printf("quests: the GM's commands, the quest items\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    const qs::QuestBook qb(data->quests);
    FakeQuestApi api;
    std::vector<svc::InventoryItem> items = kit();
    items.push_back(make_item(ItemContainer::Quest, 0, kRockSack, 300, 3));
    QuestHarness h(api, *data, log_after_start(), cyb_record(items), true);
    h.play();
    h.run(milliseconds(1500));
    CHECK(count_sacks(h) == 3, "the character carries three Goblin Rock Sacks in the quest bag");
    const content::QuestRow* rootways = qb.quest(kOfTheRootways);
    CHECK(rootways && !rootways->activation.empty(), "Of the Rootways has an activation rule (Welcome to Verra done and a player variable)");
    size_t from = h.got.size();
    api.queue(kCyb, "give", "RVR_FTUE_QST_1415_OfTheRootways");
    h.commands();
    CHECK(h.s->quest_log().find(kOfTheRootways) != nullptr, "questctl give: Of the Rootways is in the log although its rule does not hold");
    CHECK(quest_blocks_since(h, from).size() == 1, "... and the client got one block");
    api.queue(kCyb, "finish", "RVR_FTUE_QST_1415_OfTheRootways");
    h.commands();
    CHECK(h.s->quest_log().instances().empty() && h.s->quest_log().completions(kOfTheRootways) == 1, "questctl finish: out of the log, finished once");
    {
        dl::Facts facts;
        dl::Evaluator ev(*data->book, facts);
        CHECK(h.s->quest_log().can_start(rootways, ev, false) == qs::Create::QuestNotRepeatable && h.s->quest_log().can_start(rootways, ev, true) == qs::Create::QuestNotRepeatable,
              "it is not repeatable: refused by the rules, and a GM bypass skips only the activation rule");
    }
    api.queue(kCyb, "set-var", {}, "PickedSamias", 1);
    h.commands();
    CHECK(h.s->quest_log().variable("PickedSamias") == 1, "questctl set-var PickedSamias 1");
    api.queue(kCyb, "clear");
    h.commands();
    CHECK(h.s->quest_log().completions(kOfTheRootways) == 0 && h.s->quest_log().variable("PickedSamias") == 0, "questctl clear: the finished quests and the variables are forgotten");

    from = h.got.size();
    h.client_request(debug_start_rpc(kCorruptedMemories, false), h.pc_ch);
    CHECK(h.s->quest_log().find(kCorruptedMemories) != nullptr && quest_blocks_since(h, from).size() == 1, "ServerDebugStartQuest takes Corrupted Memories (GM commands on)");
    from = h.got.size();
    h.client_request(debug_start_rpc(kOfTheRootways, false), h.pc_ch);
    CHECK(h.s->quest_log().find(kOfTheRootways) == nullptr && quest_blocks_since(h, from).empty(),
          "ServerDebugStartQuest with the rules asked refuses Of the Rootways: its activation rule does not hold, nothing changes");
    from = h.got.size();
    h.client_request(debug_start_rpc(kOfTheRootways, true), h.pc_ch);
    CHECK(h.s->quest_log().find(kOfTheRootways) != nullptr && quest_blocks_since(h, from).size() == 1, "... and with the rules bypassed it takes it");
    verify_quest_gm_multi_and_items(api, h);
}

struct LogFacts : dl::Facts {
    const qs::QuestLog& log;
    explicit LogFacts(const qs::QuestLog& l) : log(l) {}
    int64_t player_variable(const std::string& n) const override { return log.variable(n); }
    std::optional<int64_t> quest_count(int64_t quest, const std::string& query) const override { return log.quest_count(quest, query); }
};

void test_quest_zone() {
    std::printf("quests: the starting area starts Welcome to Verra\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    CHECK(world::zoi_at(kStartingAreaX, kStartingAreaY) == static_cast<uint64_t>(kStartingAreaZoi), "the test spot is inside the zone that starts Welcome to Verra");
    CHECK(world::zoi_at(bootstrap::kDefaultSpawn.x, bootstrap::kDefaultSpawn.y) == static_cast<uint64_t>(kStartingAreaZoi), "so is the default spawn, where a new character starts");
    CHECK(world::zoi_at(kJoevaX, kJoevaY) != static_cast<uint64_t>(kStartingAreaZoi), "Joeva is not");
    const qs::QuestBook qb(data->quests);
    CHECK(qb.zone_quests(kStartingAreaZoi) == std::vector<int64_t>({kWelcomeToVerra}), "the data lists Welcome to Verra as the zone's quest");
    const content::QuestRow* welcome = qb.quest(kWelcomeToVerra);
    FakeQuestApi api;
    {
        svc::Character in_joeva = cyb_record();
        in_joeva.has_position = true;
        in_joeva.position = {kJoevaX, kJoevaY, bootstrap::kDefaultSpawn.z, 0.0f};
        QuestHarness h(api, *data, svc::SavedQuestLog{}, in_joeva);
        h.play();
        h.run(milliseconds(3500));
        CHECK(h.s->quest_log().instances().empty() && h.s->quests_taken() == 0, "a character outside the zone gets no quest");
    }
    {
        QuestHarness h(api, *data, log_after_start());
        h.play();
        h.run(milliseconds(3500));
        CHECK(h.s->quest_log().instances().empty() && h.s->quests_taken() == 0, "a character in the zone whose variable is set gets no quest: it starts once");
    }
    QuestHarness h(api, *data, svc::SavedQuestLog{});
    h.play();
    h.run(milliseconds(3500));
    const qs::QuestInstance* w = h.s->quest_log().find(kWelcomeToVerra);
    CHECK(w != nullptr && welcome != nullptr, "a new character standing in the zone got Welcome to Verra");
    if (!w || !welcome) return;
    CHECK(h.s->quest_log().variable("WelcomeToVerra") == 1 && w->objectives.size() == 9 && h.s->quests_taken() == 1, "its start action set WelcomeToVerra, and all nine objectives are in the instance");
    h.run(milliseconds(3000));
    CHECK(h.s->quest_log().instances().size() == 1 && h.s->quests_taken() == 1, "the zone starts it once, not every second");
    bool sent = false;
    for (const QuestBlock& b : quest_blocks_since(h, 0)) {
        for (const SeenQuest& q : quests_in(b)) {
            sent = sent || (q.record == kWelcomeToVerra && q.objectives.size() == 9 && q.variables.size() == 4 + 7 * 9);
        }
    }
    CHECK(sent, "the client was sent it, with its nine objectives and sixty-seven variables");
    h.saved();
    CHECK(api.log(kCyb).instances.size() == 1 && api.log(kCyb).variables.size() == 1, "the quest and the variable were saved");
    api.queue(kCyb, "abandon", "RVR_FTUE_QST_1400_WelcomeToVerra");
    h.commands();
    h.run(milliseconds(3000));
    CHECK(h.s->quest_log().instances().empty() && h.s->quests_taken() == 1, "a GM abandon takes it out, and the zone does not start it again");
    LogFacts facts(h.s->quest_log());
    dl::Evaluator ev(*data->book, facts);
    CHECK(h.s->quest_log().can_start(welcome, ev, false) == qs::Create::FailedActivationRules && h.s->quest_log().can_start(welcome, ev, true) == qs::Create::Ok,
          "the rule refuses it now (the variable is set); a GM start skips the rule");
}

struct HandInPickSpec {
    int64_t item = 0;
    qs::Guid128 stack;
    int32_t quantity = 1;
    uint8_t location = 3;
};

uint16_t delivery_elem_handle(const schema::RpcSchema& rpc, std::string_view path) {
    for (const schema::RepCommand& c : rpc.element_commands) {
        if (path == c.path) return c.handle;
    }
    std::printf("  [FAIL] ServerRequestItemDelivery has no element command %s\n", std::string(path).c_str());
    std::exit(2);
}

repl::ElementState make_delivery_item_row(const schema::RpcSchema& rpc, int64_t item_id, const std::vector<HandInPickSpec>& picks) {
    repl::ElementState row;
    row.values[delivery_elem_handle(rpc, "Items.ItemRecordId")] = repl::Value::i64(item_id);
    row.values[delivery_elem_handle(rpc, "Items.SelectedItems")] = repl::Value(repl::ArrayValue{});
    repl::ArrayValue stacks;
    for (const HandInPickSpec& q : picks) {
        if (q.item != item_id) continue;
        repl::ElementState s;
        s.values[delivery_elem_handle(rpc, "ItemQuantityAndLocations.FItemInstanceGuid.A")] = repl::Value::i64(static_cast<int32_t>(q.stack.a));
        s.values[delivery_elem_handle(rpc, "ItemQuantityAndLocations.FItemInstanceGuid.B")] = repl::Value::i64(static_cast<int32_t>(q.stack.b));
        s.values[delivery_elem_handle(rpc, "ItemQuantityAndLocations.FItemInstanceGuid.C")] = repl::Value::i64(static_cast<int32_t>(q.stack.c));
        s.values[delivery_elem_handle(rpc, "ItemQuantityAndLocations.FItemInstanceGuid.D")] = repl::Value::i64(static_cast<int32_t>(q.stack.d));
        s.values[delivery_elem_handle(rpc, "ItemQuantityAndLocations.Quantity")] = repl::Value::i64(q.quantity);
        s.values[delivery_elem_handle(rpc, "ItemQuantityAndLocations.InventoryLocated")] = repl::Value::u64(q.location);
        stacks.elements.push_back(std::move(s));
    }
    row.values[delivery_elem_handle(rpc, "Items.ItemQuantityAndLocations")] = repl::Value(std::move(stacks));
    return row;
}

BunchWriter delivery_rpc(int64_t quest, const qs::Guid128& context, const std::vector<std::pair<int64_t, std::vector<HandInPickSpec>>>& rows) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestItemDelivery");
    if (!rpc) return BunchWriter();
    repl::ArrayValue deliveries;
    for (const auto& [objective, picks] : rows) {
        repl::ElementState d;
        d.values[delivery_elem_handle(*rpc, "DeliveryItems.ContextGuid.A")] = repl::Value::i64(static_cast<int32_t>(context.a));
        d.values[delivery_elem_handle(*rpc, "DeliveryItems.ContextGuid.B")] = repl::Value::i64(static_cast<int32_t>(context.b));
        d.values[delivery_elem_handle(*rpc, "DeliveryItems.ContextGuid.C")] = repl::Value::i64(static_cast<int32_t>(context.c));
        d.values[delivery_elem_handle(*rpc, "DeliveryItems.ContextGuid.D")] = repl::Value::i64(static_cast<int32_t>(context.d));
        d.values[delivery_elem_handle(*rpc, "DeliveryItems.ObjectiveId")] = repl::Value::i64(objective);
        repl::ArrayValue items;
        std::vector<int64_t> seen;
        for (const HandInPickSpec& p : picks) {
            if (std::find(seen.begin(), seen.end(), p.item) != seen.end()) continue;
            seen.push_back(p.item);
            items.elements.push_back(make_delivery_item_row(*rpc, p.item, picks));
        }
        d.values[delivery_elem_handle(*rpc, "DeliveryItems.Items")] = repl::Value(std::move(items));
        deliveries.elements.push_back(std::move(d));
    }
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "QuestRecordId", repl::Value::i64(quest)) || !repl::set_rpc_param(*rpc, params, "DeliveryItems", repl::Value(std::move(deliveries)))) {
        return BunchWriter();
    }
    return actor_rpc(pc, "ServerRequestItemDelivery", params);
}

std::vector<int64_t> panel_objective_ids(const PcBlock& b) {
    std::vector<int64_t> out;
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RepCommand* array = pc.command("CurrentDialogueInstance.QueuedSubDialogues");
    const repl::Value* v = array ? b.state.get(array->handle) : nullptr;
    const auto* list = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!list || list->elements.size() != 1) return out;
    const Cmds elems = pc.elements(*array);
    const repl::Value* iv = value_at(elems, list->elements[0], "QueuedSubDialogues.ObjectiveIds");
    const auto* ids = iv ? iv->get_if<repl::ArrayValue>() : nullptr;
    if (!ids) return out;
    for (const repl::ElementState& it : ids->elements) {
        const auto g = it.values.find(1);
        if (g != it.values.end() && g->second.get_if<int64_t>()) out.push_back(*g->second.get_if<int64_t>());
    }
    return out;
}

void verify_hand_in_refusals(QuestHarness& h, const qs::Guid128& context, const qs::Guid128& stack, int64_t bread) {
    const auto refused_call = [&](const char* label, BunchWriter call) {
        const size_t refused = h.s->hand_ins_refused();
        const size_t blocks_before = h.got.size();
        h.client_request(std::move(call), h.pc_ch);
        CHECK(h.s->hand_ins_refused() == refused + 1 && h.s->hand_ins() == 0 && h.s->inventory().count_of(kBread) == bread &&
                  pc_blocks_since(h, blocks_before).empty() && quest_blocks_since(h, blocks_before).empty() && h.s->talk().sub().has_value(),
              label);
    };
    refused_call("a delivery for another quest is refused: nothing is taken, the panel stays", delivery_rpc(kRainOfRocks, context, {{kBreadObjective, {{kBread, stack, 1, 3}}}}));
    refused_call("a stack the character does not carry is refused", delivery_rpc(kHungryForMore, context, {{kBreadObjective, {{kBread, qs::Guid128{1, 2, 3, 4}, 1, 3}}}}));
    refused_call("an item the objective does not take is refused", delivery_rpc(kHungryForMore, context, {{kBreadObjective, {{kRockSack, stack, 1, 3}}}}));
    refused_call("more than the stack holds is refused", delivery_rpc(kHungryForMore, context, {{kBreadObjective, {{kBread, stack, 4, 3}}}}));
    refused_call("an objective the panel does not list is refused", delivery_rpc(kHungryForMore, context, {{kPilferedObjective, {{kBread, stack, 1, 3}}}}));
    refused_call("a stack in a bag the hand-in does not look in is refused", delivery_rpc(kHungryForMore, context, {{kBreadObjective, {{kBread, stack, 1, 4}}}}));
    refused_call("a delivery of no item is refused", delivery_rpc(kHungryForMore, context, {{kBreadObjective, {}}}));
    CHECK(h.s->quest_log().find(kHungryForMore)->objectives[0].count == 0, "the objective has counted nothing");
}

void verify_hand_in_delivery_and_save(
    FakeQuestApi& api,
    QuestHarness& h,
    ClientDialogue& client,
    const std::pair<qs::Guid128, qs::Guid128>& guids,
    std::pair<size_t, int64_t> counts) {
    const auto& [context, stack] = guids;
    const auto [pilfered, bread] = counts;
    const uint16_t kQueue = class_named("AoCPlayerControllerBP_C").command("CurrentDialogueInstance.QueuedSubDialogues")->handle;
    const size_t from = h.got.size();
    h.client_request(delivery_rpc(kHungryForMore, context, {{kBreadObjective, {{kBread, stack, 3, 3}}}}), h.pc_ch);
    CHECK(h.s->hand_ins() == 1 && h.s->hand_in_items() == 1 && h.s->inventory().count_of(kBread) == bread - 1, "one bread leaves the bag (the objective takes one; the two it does not need stay)");
    const qs::QuestInstance* after = h.s->quest_log().find(kHungryForMore);
    CHECK(after && after->objectives[0].completed && !after->objectives[0].active && after->objectives[0].count == 1, "the objective counted it and is done");
    CHECK(after && after->objectives[pilfered].active, "its success activated the next objective (the rations)");
    CHECK(quest_blocks_since(h, from).size() >= 1, "the client was told of the quest's change (the quest component's block)");
    const auto talk_sent = pc_blocks_since(h, from);
    CHECK(!h.s->talk().open() && has_handle(talk_sent, kQueue), "the talk ends (the hand-in node names none to go on at) and the queue is cleared");
    for (const PcBlock& b : talk_sent) client.apply(b.state);
    CHECK(client.lines.empty() && client.shown_node == 0, "K3: the talk is cleared on the client");

    const size_t refused = h.s->hand_ins_refused();
    const int64_t have = h.s->inventory().count_of(kBread);
    h.client_request(delivery_rpc(kHungryForMore, context, {{kBreadObjective, {{kBread, stack, 1, 3}}}}), h.pc_ch);
    CHECK(h.s->hand_ins_refused() == refused + 1 && h.s->inventory().count_of(kBread) == have && h.s->hand_ins() == 1, "a delivery while no panel is open is refused");
    h.saved();
    const svc::SavedQuestLog saved = api.log(kCyb);
    CHECK(saved.instances.size() == 1 && saved.instances[0].record_id == kHungryForMore, "the log was saved with the quest");
}

void test_quest_hand_in() {
    std::printf("quests: the hand-in to a person (Hungry For More: the Suspicious Bread to Cleric Beyam)\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    const qs::QuestBook qb(data->quests);
    const content::QuestRow* def = qb.quest(kHungryForMore);
    const content::QuestAcceptRow* turn = data->book->turn_in(kBeyamTurnIn);
    if (!def || !turn) {
        std::printf("  [skip] SKIP: this content.db has no hand-in nodes (contentc 37)\n");
        return;
    }
    const content::DialogueNodeRow* node = data->book->node(kBeyamTurnIn);
    CHECK(node && node->type == "TurnIn" && turn->quest == kHungryForMore && turn->on_success == 0 && turn->objectives == std::vector<int64_t>({kBreadObjective}),
          "the data: Meet04 is a TurnIn node of Hungry For More, lists the bread objective and names no node to go on at");
    const content::QuestObjectiveRow& first = def->objectives[0];
    CHECK(first.guid == kBreadObjective && first.type == "TurnIn" && !first.crate && first.count == 1 && first.items == std::vector<int64_t>({kBread}) &&
              first.sources.size() == 1 && first.sources[0].instance == kBeyamInstance && first.sources[0].node == kBeyamMeet01,
          "the first objective takes one Suspicious Bread to Beyam, his talk starting at Meet01");
    size_t pilfered = def->objectives.size();
    for (size_t i = 0; i < def->objectives.size(); ++i) {
        if (def->objectives[i].guid == kPilferedObjective) pilfered = i;
    }
    CHECK(pilfered < def->objectives.size(), "the quest has the objective the hand-in activates");
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RepCommand* queued_cmd = pc.command("CurrentDialogueInstance.QueuedSubDialogues");
    CHECK(queued_cmd && pc.rpc("ServerRequestItemDelivery"), "the build has the panel queue and the delivery call");
    if (pilfered >= def->objectives.size() || !queued_cmd || !pc.rpc("ServerRequestItemDelivery")) return;
    const uint16_t kQueue = queued_cmd->handle;

    FakeQuestApi api;
    std::vector<svc::InventoryItem> items = kit();
    items.push_back(make_item(Bag, 2, kBread, 300, 3));
    const qs::Guid128 stack{0xA0000000u + 300u, 0x1000u + 300u, 0x2000u + 300u, 300u};
    QuestHarness h(api, *data, log_after_start(), cyb_record(items));
    if (world::WorldActor* a = h.fx.world->registry().find(h.talker)) {
        a->npc.instance_guid = kBeyamInstance;
        a->npc.dialogue_root = kBeyamRoot;
        a->npc.name = "Cleric Beyam";
    }
    h.play();
    h.run(milliseconds(1500));
    api.queue(kCyb, "give", "RVR_SH_QST_1421_HungryForMore");
    h.commands();
    const qs::QuestInstance* qi = h.s->quest_log().find(kHungryForMore);
    CHECK(qi && qi->objectives.size() == def->objectives.size() && qi->objectives[0].active && !qi->objectives[0].completed, "a GM gave the quest: its first objective, the hand-in, is active");
    if (!qi) return;
    const qs::Guid128 context = qi->guid;
    const int64_t bread = h.s->inventory().count_of(kBread);
    CHECK(bread >= 3, "the character carries the bread");

    ClientDialogue client;
    size_t from = h.got.size();
    h.click();
    auto blocks = pc_blocks_since(h, from);
    CHECK(h.s->talk().open() && h.s->talk().state().shown_node == kBeyamMeet01 && h.s->talk().state().greeting, "the click opens his talk at Meet01, the node the hand-in objective names");
    for (const PcBlock& b : blocks) client.apply(b.state);
    std::vector<PcBlock> last;
    CHECK(walk_to_offer(h, client, last), "three lines lead to the hand-in node and the panel is queued");
    const dl::Talk::Sub sub = h.s->talk().sub().value_or(dl::Talk::Sub{});
    CHECK(sub.type == 3 && sub.quest == kHungryForMore && sub.node == kBeyamTurnIn && sub.pending_next == 0 && sub.objectives == std::vector<int64_t>({kBreadObjective}),
          "the panel is the hand-in panel (3) of Hungry For More at Meet04, listing the bread objective");
    if (!last.empty()) {
        CHECK(has_handle(last, kQueue), "the block that shows the panel carries QueuedSubDialogues");
        const SeenPanel panel = panel_of(last.back());
        CHECK(panel.count == 1 && panel.type == 3 && panel.quest == kHungryForMore && panel.node == kBeyamTurnIn && panel.pending_next == 0, "one ItemTurnIn (3) panel for the quest at the hand-in node");
        CHECK(panel_objective_ids(last.back()) == std::vector<int64_t>({kBreadObjective}), "K1: the panel's ObjectiveIds are the bread objective of the quest");
    }

    from = h.got.size();
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Close), h.pc_ch);
    CHECK(!h.s->talk().open() && h.s->inventory().count_of(kBread) == bread && has_handle(pc_blocks_since(h, from), kQueue), "a close at the panel ends the talk, takes nothing, and clears the queue");
    h.click();
    CHECK(h.s->talk().open() && walk_to_offer(h, client, last), "the talk and the panel come again");
    verify_hand_in_refusals(h, context, stack, bread);
    verify_hand_in_delivery_and_save(api, h, client, {context, stack}, {pilfered, bread});
}

void verify_quest_start_grants(const dl::Book& dlg_book, const qs::QuestBook& qb, const content::QuestRow& def) {
    constexpr int64_t kBreadTable = 6064634776945885185ll;
    constexpr int64_t kExtractTable = 6064634676809564264ll;
    dl::Facts facts;
    dl::Evaluator ev(dlg_book, facts);
    const auto granted_at_start = [&](const qs::QuestBook& book, const content::QuestRow& d) {
        qs::QuestLog l(11);
        std::vector<qs::Effect> fx;
        qs::Engine e(l, book, ev, facts, fx, 30.0);
        e.start(d, 0);
        e.settle();
        std::vector<int64_t> tables;
        for (const qs::Effect& x : fx) {
            if (x.kind == qs::Effect::Kind::GrantTable) tables.insert(tables.end(), x.tables.begin(), x.tables.end());
        }
        return tables;
    };
    const content::QuestRow* hfm = qb.quest(kHungryForMore);
    CHECK(hfm && hfm->start_actions.empty() && !hfm->objectives.empty() && hfm->objectives[0].awake && hfm->objectives[0].on_start.size() == 1 &&
              hfm->objectives[0].on_start[0].action == "GrantItem" && hfm->objectives[0].on_start[0].reward_table == kBreadTable,
          "the data: Hungry For More has no start action of its own; its first objective starts awake and its start action gives the bread's table");
    if (hfm) CHECK(granted_at_start(qb, *hfm) == std::vector<int64_t>({kBreadTable}), "starting Hungry For More gives the bread's table, once");
    const std::vector<int64_t> tables = granted_at_start(qb, def);
    CHECK(std::count(tables.begin(), tables.end(), kExtractTable) == 1, "starting The Root of the Problem gives its Heartwood Extract, once");

    content::QuestData synthetic;
    content::QuestRow row;
    row.guid = 9001;
    row.name = "synthetic";
    content::QuestObjectiveRow o;
    o.guid = 9101;
    o.type = "Interact";
    o.count = 1;
    o.awake = true;
    content::QuestAction give;
    give.action = "GrantItem";
    give.reward_table = 7;
    o.on_start.push_back(give);
    row.objectives.push_back(o);
    content::QuestAction activate_it;
    activate_it.action = "ActivateObjective";
    activate_it.quest = 9001;
    activate_it.objective = 9101;
    row.start_actions.push_back(activate_it);
    synthetic.quests[row.guid] = row;
    const qs::QuestBook book(synthetic);
    const content::QuestRow* r = book.quest(9001);
    CHECK(r && granted_at_start(book, *r) == std::vector<int64_t>({7}), "a start action that activates an objective that started active does not run its start actions twice");
}

void test_quest_hand_in_engine() {
    std::printf("quests: hand-in objectives in the engine (a talk that completes one, the node its talk starts at)\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    constexpr int64_t kRootOfTheProblem = 6064634676114292751ll;
    constexpr int64_t kCrestObjective = 6064634676179763221ll;
    constexpr int64_t kEudoxiaInstance = 6064630378330849304ll;
    constexpr int64_t kDeliverCrest1 = 6064634682362167750ll;
    const qs::QuestBook qb(data->quests);
    const content::QuestRow* def = qb.quest(kRootOfTheProblem);
    size_t crest = def ? def->objectives.size() : 0;
    for (size_t i = 0; def && i < def->objectives.size(); ++i) {
        if (def->objectives[i].guid == kCrestObjective) crest = i;
    }
    CHECK(def && crest < def->objectives.size() && def->objectives[crest].type == "TurnIn" && def->objectives[crest].optional && !def->objectives[crest].crate &&
              def->objectives[crest].sources.size() == 1 && def->objectives[crest].sources[0].instance == kEudoxiaInstance && def->objectives[crest].sources[0].node == kDeliverCrest1,
          "the data: the crest is an optional hand-in objective of The Root of the Problem whose person is Eudoxia, her talk starting at DeliverCrest_1");
    if (!def || crest >= def->objectives.size()) return;

    qs::QuestLog log(7);
    dl::Facts facts;
    dl::Evaluator ev(*data->book, facts);
    std::vector<qs::Effect> effects;
    qs::Engine eng(log, qb, ev, facts, effects, 10.0);
    eng.start(*def, 0);
    const qs::QuestInstance* q = log.find(kRootOfTheProblem);
    CHECK(q && q->objectives.size() == def->objectives.size() && !q->objectives[crest].active, "the quest is taken; the crest objective waits");
    CHECK(qs::dialogue_entry(log, qb, kEudoxiaInstance) == 0, "Eudoxia's talk starts at no node of the crest while it waits");
    content::DialogueActionRow activate;
    activate.action = "ActivateObjective";
    activate.quest = kRootOfTheProblem;
    activate.objective = kCrestObjective;
    eng.dialogue_action(activate);
    q = log.find(kRootOfTheProblem);
    CHECK(q && q->objectives[crest].active && !q->objectives[crest].completed, "the crest objective is active (the crest was found)");
    CHECK(qs::dialogue_entry(log, qb, kEudoxiaInstance) == kDeliverCrest1, "an active hand-in objective names the node Eudoxia's talk starts at");
    CHECK(qs::dialogue_entry(log, qb, kEudoxiaInstance + 1) == 0, "... for her and nobody else");

    content::DialogueActionRow other;
    other.action = "CompleteDialogueObjective";
    other.quest = kRootOfTheProblem;
    other.objective = def->objectives[0].guid;
    CHECK(def->objectives[0].type != "TurnIn" && !eng.dialogue_turn_in(other, true) && eng.turn_in(kRootOfTheProblem, def->objectives[0].guid, 1) == 0,
          "a talk and a delivery name an objective that is no hand-in: nothing counts");
    CHECK(eng.turn_in(kRootOfTheProblem, kCrestObjective, 0) == 0 && eng.turn_in(kRootOfTheProblem + 1, kCrestObjective, 1) == 0, "no count, or a quest the character does not have: nothing counts");

    content::DialogueActionRow done;
    done.action = "CompleteDialogueObjective";
    done.quest = kRootOfTheProblem;
    done.objective = kCrestObjective;
    CHECK(eng.dialogue_turn_in(done, true), "the node's CompleteDialogueObjective counts for the hand-in objective it names");
    q = log.find(kRootOfTheProblem);
    CHECK(q && q->objectives[crest].completed && !q->objectives[crest].active && q->objectives[crest].count == def->objectives[crest].count, "the crest objective is done");
    CHECK(qs::dialogue_entry(log, qb, kEudoxiaInstance) == 0, "and her talk starts at that node no more");
    CHECK(!eng.dialogue_turn_in(done, true) && eng.turn_in(kRootOfTheProblem, kCrestObjective, 1) == 0, "a done objective counts nothing more");
    verify_quest_start_grants(*data->book, qb, *def);
}

}  // namespace

void run_quest_session_suite_b() {
    test_quest_gm();
    test_quest_zone();
    test_quest_hand_in();
    test_quest_hand_in_engine();
}
