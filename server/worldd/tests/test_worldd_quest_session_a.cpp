// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "quest_session_test_harness.h"

namespace {

bool quest_has_objective(const content::QuestRow* q, int64_t obj_guid) {
    if (!q) return false;
    for (const content::QuestObjectiveRow& x : q->objectives) {
        if (x.guid == obj_guid) return true;
    }
    return false;
}

void verify_quest_accept_nodes_and_links(const QuestTestData& data, const qs::QuestBook& qb, const dl::Book& book) {
    size_t accepts = 0, a_no_node = 0, a_wrong_type = 0, a_no_quest = 0, a_bad_next = 0, a_bad_objective = 0;
    for (const auto& [node, a] : data.quests.accepts) {
        ++accepts;
        const content::QuestRow* q = qb.quest(a.quest);
        const content::DialogueNodeRow* n = book.node(node);
        a_no_node += n ? 0 : 1;
        a_wrong_type += (n && n->type != "AcceptQuest") ? 1 : 0;
        a_no_quest += q ? 0 : 1;
        a_bad_next += (a.on_success != 0 && !book.node(a.on_success)) ? 1 : 0;
        for (const int64_t o : a.objectives) a_bad_objective += quest_has_objective(q, o) ? 0 : 1;
    }
    CHECK(accepts == 346 && a_no_node == 0 && a_wrong_type == 0 && a_no_quest == 2 && a_bad_next == 0 && a_bad_objective == 0,
          "K1: the 346 offer nodes are nodes of the data of the offer kind, lead on to nodes of the data and name objectives of their quests; two name a quest the design data lacks");

    size_t zone_links = 0, bad_zone = 0, item_links = 0, bad_items = 0;
    for (const auto& [zoi, list] : data.quests.zoi_quests) {
        for (const int64_t q : list) {
            ++zone_links;
            bad_zone += qb.quest(q) ? 0 : 1;
        }
    }
    for (const auto& [item, list] : data.quests.item_quests) {
        for (const content::QuestItemLink& l : list) {
            ++item_links;
            bad_items += (qb.quest(l.quest) && (l.kind == "delete_all" || l.kind == "delete_one" || l.kind == "abandon")) ? 0 : 1;
        }
    }
    CHECK(zone_links == 4 && bad_zone == 0 && item_links == 160 && bad_items == 0, "K1: the 4 zone links and the 160 item links name quests of the data");
}

void verify_quest_wire_items(const QuestTestData& data) {
    const schema::ClassSchema* cls = qs::component_schema();
    const auto lc = cls ? repl::fast_array_layout(*cls, "ReplicatedQuestCache") : std::nullopt;
    const auto lv = cls ? repl::fast_array_layout(*cls, "RepQuestVariables") : std::nullopt;
    CHECK(cls && lc && lv, "the build has the quest component with its two arrays");
    if (!lc || !lv) return;
    size_t not_fitting = 0, wrong_states = 0, wrong_counts = 0, quests_without_actions = 0;
    for (const auto& [guid, def] : data.quests.quests) {
        qs::QuestLog log(1);
        const qs::Started st = log.start(def, 0.0, 0);
        if (!st.instance) {
            ++not_fitting;
            continue;
        }
        const auto ci = qs::cache_item(*lc, *st.instance);
        const auto vi = qs::variables_item(*lv, *st.instance);
        if (!ci || !vi || !repl::item_matches(*lc, *ci) || !repl::item_matches(*lv, *vi)) {
            ++not_fitting;
            continue;
        }
        const auto vars = qs::instance_variables(*st.instance);
        wrong_counts += vars.size() == 4 + 7 * def.objectives.size() ? 0 : 1;
        if (!def.start_actions.empty()) continue;
        ++quests_without_actions;
        for (size_t i = 0; i < def.objectives.size(); ++i) {
            const qs::ObjectiveState& o = st.instance->objectives[i];
            wrong_states += (o.guid.zero() || o.record != def.objectives[i].guid || o.awake != def.objectives[i].awake || o.hidden != def.objectives[i].hidden || o.active != def.objectives[i].awake) ? 1 : 0;
        }
    }
    CHECK(not_fitting == 0 && wrong_counts == 0, "every one of the 3,026 quests starts, and its cache item and its variables (4 + 7 for each objective) fit the component's arrays");
    CHECK(wrong_states == 0 && quests_without_actions > 0, "K6: the objectives of a new quest are in the states their records give (the quests that have no start action)");
}

void test_quest_data() {
    std::printf("quests: every id the data names, and every quest on the wire\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    const qs::QuestBook qb(data->quests);
    const dl::Book& book = *data->book;
    size_t objectives = 0, shared_objectives = 0;
    std::set<int64_t> objective_ids;
    for (const auto& [guid, q] : data->quests.quests) {
        objectives += q.objectives.size();
        for (const content::QuestObjectiveRow& o : q.objectives) shared_objectives += objective_ids.insert(o.guid).second ? 0 : 1;
    }
    CHECK(qb.quests() == 3026 && objectives == 10516, "the book holds the data's 3,026 quests with 10,516 objectives");
    (void)shared_objectives;

    size_t offers = 0, no_quest = 0, no_node = 0;
    std::set<int64_t> missing;
    for (const auto* m : {&data->quests.instance_offers, &data->quests.asset_offers}) {
        for (const auto& [source, list] : *m) {
            for (const content::QuestOfferRow& o : list) {
                ++offers;
                no_quest += qb.quest(o.quest) ? 0 : 1;
                if (!qb.quest(o.quest)) missing.insert(o.quest);
                no_node += (o.node != 0 && !book.node(o.node)) ? 1 : 0;
            }
        }
    }
    CHECK(offers == 446 && no_node == 0, "K1: the 446 offers name a node of the data wherever they name one");
    CHECK(no_quest == 7 && missing == std::set<int64_t>({kMissingQuest}), "K1: all but 7 of the offers name quests of the data; those 7 name one quest the design data lacks");
    {
        dl::Facts facts;
        dl::Evaluator ev(book, facts);
        CHECK(qb.quest(kMissingQuest) == nullptr && qs::QuestLog(1).can_start(qb.quest(kMissingQuest), ev, false) == qs::Create::InvalidStaticData,
              "the server never offers it: the rules answer InvalidStaticData for a quest the data lacks");
    }
    verify_quest_accept_nodes_and_links(*data, qb, book);
    verify_quest_wire_items(*data);
}

std::string verify_quest_offer_and_decline(
    const QuestTestData& data,
    FakeQuestApi& api,
    QuestHarness& h,
    ClientDialogue& client,
    std::pair<uint16_t, uint16_t> handles) {
    const auto [kQueue, kExhaust] = handles;
    size_t from = h.got.size();
    h.click();
    auto blocks = pc_blocks_since(h, from);
    CHECK(blocks.size() == 1 && h.s->talk().open() && h.s->talk().state().shown_node == kSoshaOfferTop && h.s->talk().state().greeting, "the click opens the offer talk at the node the offer names");
    if (blocks.size() == 1) {
        CHECK(blocks[0].handles == std::vector<uint16_t>({154, 156, 166, 170}), "the greeting carries CurrentChoices, ObjectSource, the node shown and bIsInGreeting, as every greeting does (K4)");
        client.apply(blocks[0].state);
        CHECK(client.lines.size() == 1 && client.shown_node == kSoshaOfferTop && client.lines[0].node == kSoshaOfferTop && client.lines[0].index == 0, "K2: one line, naming the node shown");
    }

    std::vector<PcBlock> last;
    CHECK(walk_to_offer(h, client, last), "the lines lead to the node that offers the quest, and the quest panel is queued");
    const dl::Talk::Sub sub = h.s->talk().sub().value_or(dl::Talk::Sub{});
    const content::DialogueNodeRow* offer_node = data.book->node(sub.node);
    CHECK(sub.quest == kCorruptedMemories && sub.node == kAcceptNode && offer_node && offer_node->type == "AcceptQuest", "K1: the panel names the quest and the offer node of the data");
    if (!last.empty()) {
        CHECK(has_handle(last, kQueue), "the block that shows the offer carries QueuedSubDialogues");
        const SeenPanel panel = panel_of(last.back());
        CHECK(panel.count == 1 && panel.type == 2 && panel.quest == kCorruptedMemories && panel.preview_quest == kCorruptedMemories && panel.node == kAcceptNode,
              "one QuestAccept (2) panel whose quest and preview are Corrupted Memories, at the offer node");
        CHECK(panel.initial == std::vector<int64_t>({kCorruptedFirstObjective}), "K1, K6: the preview lists the one objective the quest starts with, an objective of the quest");
    }
    const size_t refused = h.s->dialogue_refused();
    from = h.got.size();
    h.client_request(dialogue_choice(choice_of(h.s->talk().state().shown_node, 0)), h.pc_ch);
    CHECK(h.s->dialogue_refused() == refused + 1 && pc_blocks_since(h, from).empty() && h.s->talk().sub().has_value(), "a line chosen while the panel is open is refused, nothing changes");

    from = h.got.size();
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Decline), h.pc_ch);
    blocks = pc_blocks_since(h, from);
    CHECK(h.s->quest_log().instances().empty() && h.s->quests_taken() == 0 && !h.s->talk().open(), "Decline takes nothing and ends the talk");
    CHECK(has_handle(blocks, kQueue) && has_handle(blocks, kExhaust) && quest_blocks_since(h, from).empty(), "the update carries the emptied queue and ExhaustBehavior, and no quest block");

    h.click();
    CHECK(h.s->talk().open() && walk_to_offer(h, client, last), "the quest is on offer again after a decline");
    from = h.got.size();
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Close), h.pc_ch);
    CHECK(h.s->quest_log().instances().empty() && !h.s->talk().open() && quest_blocks_since(h, from).empty(), "Close at the panel takes nothing and ends the talk");

    h.click();
    CHECK(h.s->talk().open() && walk_to_offer(h, client, last), "the panel opens once more");
    api.queue(kCyb, "give", "RVR_HAR_QST_1000_CorruptedMemories");
    h.commands();
    CHECK(h.s->quest_log().instances().size() == 1 && h.s->talk().sub().has_value(), "a GM gave the quest meanwhile: it is in the log and the panel is still open");
    const std::string given_guid = h.s->quest_log().instances().empty() ? std::string() : h.s->quest_log().instances()[0].guid.hex();
    const size_t q_refused = h.s->quests_refused();
    from = h.got.size();
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Accept), h.pc_ch);
    CHECK(h.s->quests_refused() == q_refused + 1 && h.s->quests_taken() == 1 && h.s->quest_log().instances().size() == 1 && quest_blocks_since(h, from).empty(),
          "the Accept is refused (the quest is active already): still one instance, no quest block");
    CHECK(!h.s->talk().open() && has_handle(pc_blocks_since(h, from), kQueue), "... and the panel is closed and its queue cleared");
    api.queue(kCyb, "abandon", "RVR_HAR_QST_1000_CorruptedMemories");
    h.commands();
    CHECK(h.s->quest_log().instances().empty(), "a GM takes it out again");
    return given_guid;
}

void verify_seen_quest_accept(const qs::QuestBook& qb, const content::QuestRow& def, const SeenQuest& q, const std::string& given_guid) {
    CHECK(q.record == kCorruptedMemories && qb.quest(q.record) != nullptr, "K1: the item is the quest of the data");
    CHECK(q.instance.size() == 32 && q.instance != std::string(32, '0') && q.instance != given_guid && q.parent == std::string(32, '0'),
          "K5: the instance has a guid of its own (not the earlier instance's) and no parent");
    CHECK(q.locality == 1, "the quest is Local (1)");
    bool same = q.objectives.size() == def.objectives.size();
    std::set<std::string> guids;
    for (size_t i = 0; same && i < q.objectives.size(); ++i) {
        same = same && q.objectives[i].second == def.objectives[i].guid;
        guids.insert(q.objectives[i].first);
    }
    CHECK(same && guids.size() == def.objectives.size() && guids.count(q.instance) == 0 && guids.count("missing") == 0,
          "K6: five objectives, the quest's own in its order, each with a guid of its own");
    std::vector<uint64_t> keys;
    std::map<uint64_t, int32_t> by_key;
    for (const auto& [k, v] : q.variables) {
        keys.push_back(k);
        by_key[k] = v;
    }
    CHECK(keys == expected_variable_keys(def), "K9: the variables are keyed by the hash of their names, the quest's four and each objective's seven in the game's order");
    CHECK(q.vars_id >= 0 && q.variables_key == q.instance, "the variables item is keyed by the instance's guid");
    const auto on = [&](const std::string& name) {
        const auto it = by_key.find(qs::city_hash64(name));
        return it == by_key.end() ? -1 : it->second;
    };
    const auto& o = def.objectives;
    CHECK(on(qs::quest_variable_name(kCorruptedMemories, "Active")) == 1 && on(qs::quest_variable_name(kCorruptedMemories, "Completed")) == 0, "the quest is Active, not Completed");
    CHECK(on(qs::objective_variable_name(kCorruptedMemories, o[0].guid, "Active")) == 1 && on(qs::objective_variable_name(kCorruptedMemories, o[0].guid, "Awake")) == 1 &&
              on(qs::objective_variable_name(kCorruptedMemories, o[0].guid, "Hidden")) == 0,
          "K6: the first objective is Active, Awake and not Hidden (the quest's start action activates it)");
    bool rest_hidden = true;
    for (size_t i = 1; i < o.size(); ++i) {
        rest_hidden = rest_hidden && on(qs::objective_variable_name(kCorruptedMemories, o[i].guid, "Hidden")) == 1 &&
                      on(qs::objective_variable_name(kCorruptedMemories, o[i].guid, "Active")) == 0 &&
                      on(qs::objective_variable_name(kCorruptedMemories, o[i].guid, "Awake")) == 0;
    }
    CHECK(rest_hidden, "K6: the other four are Hidden, not Awake and not Active");
}

SeenQuest verify_quest_accept_wire(
    const qs::QuestBook& qb,
    const content::QuestRow& def,
    QuestHarness& h,
    ClientDialogue& client,
    const std::string& given_guid) {
    const schema::ClassSchema* cls = qs::component_schema();
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const uint16_t kQueue = pc.command("CurrentDialogueInstance.QueuedSubDialogues")->handle;
    const uint16_t kExhaust = pc.command("CurrentDialogueInstance.ExhaustBehavior")->handle;
    std::vector<PcBlock> last;
    h.click();
    CHECK(h.s->talk().open() && walk_to_offer(h, client, last), "the panel opens a last time");
    const size_t seen = h.got.size();
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Accept), h.pc_ch);
    const auto q1 = quest_blocks_since(h, seen);
    const auto dlg = pc_blocks_since(h, seen);
    CHECK(h.s->quest_log().find(kCorruptedMemories) != nullptr && h.s->quests_taken() == 2, "Accept takes the quest");
    CHECK(q1.size() == 1 && q1[0].fields.size() == 3, "one quest component block with three arrays: the cache, the map pin of the first objective (a place to go to) and the variables");
    CHECK(!h.s->talk().open() && has_handle(dlg, kQueue) && !has_handle(dlg, kExhaust),
          "the talk ends, the update carries the emptied queue (ExhaustBehavior is not sent again: its value did not change since the first answer)");
    for (const auto& b : dlg) client.apply(b.state);
    CHECK(client.lines.empty() && client.shown_node == 0 && client.source == emit::FIntrepidNetworkGUID{}, "K3: the talk is cleared on the client");
    SeenQuest accepted;
    if (q1.size() == 1) {
        const auto quests = quests_in(q1[0]);
        CHECK(quests.size() == 1, "one quest in the block");
        if (quests.size() == 1) {
            accepted = quests[0];
            verify_seen_quest_accept(qb, def, accepted, given_guid);
        }
        const repl::FastArrayField* cache = field_named(q1[0].fields, *cls, "ReplicatedQuestCache");
        const repl::FastArrayField* vars = field_named(q1[0].fields, *cls, "RepQuestVariables");
        CHECK(cache && vars && cache->delta.changed.size() == 1 && vars->delta.changed.size() == 1 && cache->delta.deleted.empty() && vars->delta.deleted.empty(), "one item in each array, nothing deleted");
        CHECK(cache && vars && cache->delta.base_key > 0 && cache->delta.array_key > cache->delta.base_key && vars->delta.array_key > vars->delta.base_key,
              "K7: each array's key rises against the key the client has (the earlier quests of this session raised it)");
    }
    return accepted;
}

void verify_quest_abandon_and_relog(
    const QuestTestData& data,
    const content::QuestRow& def,
    FakeQuestApi& api,
    QuestHarness& h,
    const std::pair<SeenQuest, std::string>& state) {
    const auto& [accepted, given_guid] = state;
    const schema::ClassSchema* cls = qs::component_schema();
    size_t others = 0;
    quest_blocks_since(h, 0, &others);
    CHECK(others == 0, "K8: the quest component's blocks went out on the PlayerController's channel only");
    h.saved();
    svc::SavedQuestLog saved = api.log(kCyb);
    CHECK(saved.instances.size() == 1 && saved.instances[0].record_id == kCorruptedMemories && saved.instances[0].instance_guid == accepted.instance && saved.instances[0].objectives.size() == 5,
          "the log was saved: the quest with the guid the client knows it by and its five objectives");

    size_t from = h.got.size();
    h.click();
    CHECK(pc_blocks_since(h, from).empty() && !h.s->talk().open(), "Sosha has nothing more to say to a character who has the quest: no window");

    const std::optional<qs::Guid128> g = qs::Guid128::from_hex(accepted.instance);
    from = h.got.size();
    h.client_request(blacklist_rpc(g.value_or(qs::Guid128{}), true), h.pc_ch);
    CHECK(quest_blocks_since(h, from).empty() && h.s->quest_log().instances().size() == 1, "ServerSetQuestBlacklisted is accepted and changes nothing");

    const size_t abandoned_before = h.s->quests_abandoned();
    from = h.got.size();
    h.client_request(abandon_rpc({g.value_or(qs::Guid128{})}), h.pc_ch);
    const auto q2 = quest_blocks_since(h, from);
    CHECK(h.s->quest_log().instances().empty() && h.s->quests_abandoned() == abandoned_before + 1, "Abandon takes the quest out of the log");
    CHECK(q2.size() == 1 && q2[0].fields.size() == 2, "one quest component block with two arrays");
    if (q2.size() == 1) {
        const repl::FastArrayField* cache = field_named(q2[0].fields, *cls, "ReplicatedQuestCache");
        const repl::FastArrayField* vars = field_named(q2[0].fields, *cls, "RepQuestVariables");
        CHECK(cache && vars && cache->delta.deleted == std::vector<int32_t>({accepted.cache_id}) && cache->delta.changed.empty() &&
                  vars->delta.deleted == std::vector<int32_t>({accepted.vars_id}) && vars->delta.changed.empty(),
              "K7: both arrays delete the id the item was added with, and change nothing");
        CHECK(cache && cache->delta.base_key > 0 && cache->delta.array_key == cache->delta.base_key + 1, "K7: the array key rises by one, against the key the client has");
    }
    h.saved();
    CHECK(api.log(kCyb).instances.empty(), "the saved log is empty again");
    from = h.got.size();
    const size_t refused_before = h.s->quests_refused();
    h.client_request(abandon_rpc({qs::Guid128{1, 2, 3, 4}}), h.pc_ch);
    CHECK(quest_blocks_since(h, from).empty() && h.s->quests_refused() == refused_before + 1, "an abandon of a guid nobody has is refused: no block");

    h.click();
    CHECK(h.s->talk().open() && h.s->talk().state().shown_node == kSoshaOfferTop, "the quest is on offer again after the abandon");
    ClientDialogue again;
    std::vector<PcBlock> last;
    CHECK(walk_to_offer(h, again, last), "the lines lead to the offer again");
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Accept), h.pc_ch);
    const qs::QuestInstance* second = h.s->quest_log().find(kCorruptedMemories);
    CHECK(second && second->guid.hex() != accepted.instance && second->guid.hex() != given_guid, "K5: the third instance has a guid of its own");
    const std::string second_guid = second ? second->guid.hex() : std::string();
    h.saved();
    h.end();
    saved = api.log(kCyb);
    CHECK(saved.instances.size() == 1 && saved.instances[0].instance_guid == second_guid, "the log the session ended with holds the quest");
    QuestHarness r(api, data, saved);
    r.play();
    r.run(milliseconds(1500));
    CHECK(r.s->quests_on() && r.s->quest_log().instances().size() == 1 && r.s->quest_log().instances()[0].guid.hex() == second_guid, "K5: after a relog the quest has the same instance guid");
    bool in_join = false, objectives_same = false;
    for (const QuestBlock& b : quest_blocks_since(r, 0)) {
        for (const SeenQuest& q : quests_in(b)) {
            in_join = in_join || (q.instance == second_guid && q.record == kCorruptedMemories);
            objectives_same = objectives_same || (q.instance == second_guid && q.objectives.size() == 5 && q.objectives[0].second == def.objectives[0].guid);
        }
    }
    CHECK(in_join && objectives_same, "the join's quest block carries that quest with the same guid and its objectives");
    r.click();
    CHECK(!r.s->talk().open(), "and Sosha offers nothing to the relogged character either");
}

void test_quest_offer_accept_abandon() {
    std::printf("a quest offered by an NPC: decline, close, refuse, accept, abandon, take it again, relog\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    const qs::QuestBook qb(data->quests);
    const content::QuestRow* def = qb.quest(kCorruptedMemories);
    CHECK(def && def->objectives.size() == 5 && def->can_abandon && !def->repeatable && def->name == "RVR_HAR_QST_1000_CorruptedMemories",
          "the data has Corrupted Memories: 5 objectives, it can be abandoned and is not repeatable");
    const auto& offers = qb.offers(kSoshaInstance, 0);
    CHECK(offers.size() == 1 && offers[0].quest == kCorruptedMemories && offers[0].node == kSoshaOfferTop, "Sosha Terim offers it, her talk starting at the offer's node");
    const schema::ClassSchema* cls = qs::component_schema();
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RepCommand* queued_cmd = pc.command("CurrentDialogueInstance.QueuedSubDialogues");
    const schema::RepCommand* exhaust_cmd = pc.command("CurrentDialogueInstance.ExhaustBehavior");
    CHECK(def && cls && queued_cmd && exhaust_cmd, "the build has the quest component, the panel queue and ExhaustBehavior");
    if (!def || !cls || !queued_cmd || !exhaust_cmd) return;

    FakeQuestApi api;
    QuestHarness h(api, *data, log_after_start());
    h.play();
    h.run(milliseconds(1500));
    CHECK(h.s->quests_on() && h.s->quest_log().instances().empty(), "the character's log loaded, no quest in it");
    size_t others = 0;
    CHECK(quest_blocks_since(h, 0, &others).size() >= 1 && others == 0, "the join sent the quest component's block (no quests: empty arrays), on the PlayerController's channel");
    ClientDialogue client;
    const std::string given_guid = verify_quest_offer_and_decline(*data, api, h, client, {queued_cmd->handle, exhaust_cmd->handle});
    const SeenQuest accepted = verify_quest_accept_wire(qb, *def, h, client, given_guid);
    verify_quest_abandon_and_relog(*data, *def, api, h, {accepted, given_guid});
}

void test_quest_refusals() {
    std::printf("quests: refusals change nothing\n");
    std::string why;
    const auto data = quest_test_data(why);
    if (!data) {
        std::printf("  [skip] SKIP: %s\n", why.c_str());
        return;
    }
    const qs::QuestBook qb(data->quests);
    FakeQuestApi api;
    svc::SavedQuestLog have;
    {
        qs::QuestLog log(7);
        log.start(*qb.quest(kCorruptedMemories), 0.5, 0);
        log.set_variable("WelcomeToVerra", 1);
        have = log.to_saved();
    }
    QuestHarness h(api, *data, have);
    h.play();
    h.run(milliseconds(1500));
    CHECK(h.s->quests_on() && h.s->quest_log().instances().size() == 1, "a character who has the quest joins with it");
    size_t from = h.got.size();
    h.client_request(sub_dialogue_response(repl::SubDialogueResponse::Accept), h.pc_ch);
    CHECK(quest_blocks_since(h, from).empty() && h.s->quest_log().instances().size() == 1 && h.s->quests_taken() == 0, "an Accept with no panel open changes nothing");
    from = h.got.size();
    h.click();
    CHECK(!h.s->talk().open() && pc_blocks_since(h, from).empty(), "the click on Sosha shows no window: the quest is in the log and her own talk has nothing to say");

    const content::QuestRow* welcome = qb.quest(kWelcomeToVerra);
    CHECK(welcome && !welcome->can_abandon && welcome->objectives.size() == 9, "K11: the data says Welcome to Verra cannot be abandoned (9 objectives)");
    api.queue(kCyb, "give", "RVR_FTUE_QST_1400_WelcomeToVerra");
    h.commands();
    const qs::QuestInstance* w = h.s->quest_log().find(kWelcomeToVerra);
    CHECK(w != nullptr, "a GM give puts it in the log whatever its rules say");
    if (w) {
        const qs::Guid128 g = w->guid;
        from = h.got.size();
        const size_t refused = h.s->quests_refused();
        h.client_request(abandon_rpc({g}), h.pc_ch);
        CHECK(h.s->quest_log().find(kWelcomeToVerra) != nullptr && quest_blocks_since(h, from).empty() && h.s->quests_refused() == refused + 1,
              "K11: an abandon of it is refused by the server, and nothing changes");
        CHECK(h.s->quest_log().variable("WelcomeToVerra") == 1, "its start action set the player variable WelcomeToVerra");
        const qs::QuestInstance* cm = h.s->quest_log().find(kCorruptedMemories);
        from = h.got.size();
        if (cm) h.client_request(abandon_rpc({g, cm->guid}), h.pc_ch);
        CHECK(h.s->quest_log().find(kWelcomeToVerra) != nullptr && h.s->quest_log().find(kCorruptedMemories) == nullptr && quest_blocks_since(h, from).size() == 1,
              "a call naming both drops only the one that may be abandoned, in one block");
    }
    api.queue(kCyb, "give", "RVR_FTUE_QST_1400_WelcomeToVerra");
    from = h.got.size();
    h.commands();
    CHECK(h.s->quest_log().instances().size() == 1 && quest_blocks_since(h, from).empty(), "a second give of a quest in the log changes nothing");
    api.queue(kCyb, "give", "NoSuchQuest");
    api.queue(kCyb, "abandon", "RVR_HAR_QST_1000_CorruptedMemories");
    api.queue(kCyb, "frobnicate", "RVR_HAR_QST_1000_CorruptedMemories");
    from = h.got.size();
    h.commands();
    CHECK(h.s->quest_log().instances().size() == 1 && quest_blocks_since(h, from).empty(), "a quest that is not in the data, a quest that is not in the log and an unknown command change nothing");
    from = h.got.size();
    h.client_request(debug_start_rpc(kCorruptedMemories, true), h.pc_ch);
    CHECK(h.s->quest_log().find(kCorruptedMemories) == nullptr && quest_blocks_since(h, from).empty(), "the client's own debug start is refused when GM commands are off");
}

}  // namespace

void run_quest_session_suite_a() {
    test_quest_data();
    test_quest_offer_accept_abandon();
    test_quest_refusals();
}
