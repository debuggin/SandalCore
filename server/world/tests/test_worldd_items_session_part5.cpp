// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_items_session_helpers.h"
using namespace aoc::worldd;
static std::vector<repl::ActionbarSlot> sorted_slots(std::vector<repl::ActionbarSlot> slots) {
    std::sort(slots.begin(), slots.end(), [](const repl::ActionbarSlot &a, const repl::ActionbarSlot &b) { return a.bank_index != b.bank_index ? a.bank_index < b.bank_index : a.slot_index < b.slot_index; });
    return slots;
}
static std::vector<repl::ActionbarSlot> distinct_slots(std::vector<repl::ActionbarSlot> slots) {
    slots = sorted_slots(std::move(slots));
    slots.erase(std::unique(slots.begin(), slots.end(), [](const repl::ActionbarSlot &a, const repl::ActionbarSlot &b) { return a.bank_index == b.bank_index && a.slot_index == b.slot_index && a.use == b.use; }), slots.end());
    return slots;
}
static std::optional<repl::ActionbarPayload> captured_default_payload() {
    const auto bytes = bs::captured::bytes_of(bs::captured::kActionBarBlock);
    const auto views = repl::read_content_blocks(bytes.data(), bs::captured::kActionBarBlock.bits, 0);
    if (!views || views->empty())
        return std::nullopt;
    const repl::BlockView &v = (*views)[0];
    size_t p = v.payload_pos;
    const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), bytes.data(), v.payload_pos + v.payload_bits, p);
    if (!calls || calls->empty())
        return std::nullopt;
    return repl::parse_actionbar_payload((*calls)[0]);
}
static void log_record_ability_diff(const Harness &h, const std::vector<int64_t> &expected_records) {
    if (h.s->record_abilities() != expected_records || h.mirror.records != expected_records) {
        std::printf("    session %zu, client %zu, expected %zu\n", h.s->record_abilities().size(), h.mirror.records.size(), expected_records.size());
        for (size_t i = 0; i < expected_records.size(); ++i) {
            const int64_t a = i < h.s->record_abilities().size() ? h.s->record_abilities()[i] : 0, b = i < h.mirror.records.size() ? h.mirror.records[i] : 0;
            if (a != expected_records[i] || b != expected_records[i]) {
                std::printf("    first difference at %zu: session %lld, client %lld, expected %lld\n", i, static_cast<long long>(a), static_cast<long long>(b), static_cast<long long>(expected_records[i]));
                break;
            }
        }
    }
}
static void test_d1s2_join_abilities_and_trees(Harness &h) {
    CHECK(h.mirror.unreadable == 0, "everything the server sent reads");
    const std::vector<int64_t> captured = bs::captured_bard_known_abilities();
    const auto records = bs::captured_bard_records_to_load();
    CHECK(h.s->known_abilities() == captured && h.mirror.known == captured, "a level-1 Bard's KnownAbilities are the captured Bard's, in the session and on the client");
    std::vector<int64_t> expected_records;
    for (size_t i = 0; i < records.size(); ++i) {
        expected_records.push_back(records[i].guid);
        if (i == 12)
            expected_records.push_back(6064630480735371270ll);
    }
    log_record_ability_diff(h, expected_records);
    CHECK(h.s->record_abilities() == expected_records && h.mirror.records == expected_records && records.size() == 58,
          "...and the records the captured 58 with the bag's two-hander's combo after the Book's (as the retail character's carried weapons: after the combo, before the tail)");
    const int64_t bard = tree_guid(h, "Bard");
    CHECK(client_list(h, bard) == std::vector<int64_t>{rank_named(h, "Bard", "Bard_Rhetoric")}, "the XPTracker lists the Bard tree with its one auto-granted rank (Rhetoric)");
    size_t professions = 0;
    for (const char *name : {"Mining", "Herbalism", "Lumberjacking", "Hunting", "Fishing"}) professions += client_list(h, tree_guid(h, name)).size();
    CHECK(professions == 9 && client_list(h, content::kGuidUniversalTree).empty(), "...and the gathering trees' nine ranks (1, 1, 1, 3 and 3), no Universal rank at level 1");
    CHECK(h.s->talents() && h.s->talents()->purchased().empty(), "nothing is bought");
}
static void log_actionbar_slots(const std::vector<repl::ActionbarSlot> &slots, const char *who) {
    for (const repl::ActionbarSlot &x : slots) {
        std::printf("    %s: bank %d slot %d type %u instigator %u ability %lld rank %lld pet %lld item %lld combo %u\n", who, x.bank_index, x.slot_index, static_cast<unsigned>(x.use.use_type), static_cast<unsigned>(x.use.instigator),
                    static_cast<long long>(x.use.ability_id), static_cast<long long>(x.use.skill_rank_id), static_cast<long long>(x.use.pet_command_id), static_cast<long long>(x.use.item_record_id),
                    static_cast<unsigned>(x.use.combo_button));
    }
}
static void test_d1s2_join_action_bar(Harness &h) {
    const auto cap = captured_default_payload();
    CHECK(cap && !cap->auto_granted && cap->slots.size() == 5, "(the captured default bar decodes: five slots)");
    if (cap && h.s->action_bar().slots() != sorted_slots(cap->slots)) {
        log_actionbar_slots(h.s->action_bar().slots(), "session ");
        log_actionbar_slots(cap->slots, "captured");
    }
    CHECK(cap && h.s->action_bar().slots() == sorted_slots(cap->slots), "the session's default bar for a level-1 Bard is the captured one (as a set of slots): QuickWit on bank 0 slot 0, the weapon bank's Guard, Sprint, swap and Dodge");
    const size_t payloads = h.mirror.bar_payloads.size();
    const size_t from = h.mirror.sequence.size();
    h.request(client_load_rpc(), h.pc_ch);
    CHECK(h.mirror.bar_payloads.size() == payloads + 1 && count_tag(sequence_since(h, from), "P") == 1 && client_bar_is(h, h.s->action_bar()), "ServerRequestClientLoad: the bar goes out once, as one payload, and the client has exactly it");
    CHECK(cap && distinct_slots(h.mirror.bar_payloads.back().slots) == sorted_slots(cap->slots), "...and it is the captured payload, slot for slot (retail lists bank 4's slot 4 before slot 3; the set is the same)");
    CHECK(h.sink.progress.empty() && !h.s->progress_changed(), "nothing is saved at the join");
}
static void test_d1s2_join() {
    std::printf("plan D1, stage 2: the join lists the character's trees, abilities and bar\n");
    Harness h(record_with_scrolls());
    test_d1s2_join_abilities_and_trees(h);
    test_d1s2_join_action_bar(h);
}
static void test_d1s2_purchase_buy_second_rank(Harness &h, int64_t bard, int64_t menacing, int64_t shielding, const tl::Node *menacing_node) {
    size_t from = 0;
    CHECK(h.s->action_bar().at(0, 2) && h.s->action_bar().at(0, 2)->skill_rank_id == menacing && h.s->action_bar().at(0, 2)->ability_id == menacing_node->ranks[0].ability,
          "the skill is on the first empty slot of the class pages (a design choice, not retail's)");
    CHECK(h.sink.progress.size() == 1 && h.sink.progress[0].second.replace_skill_ranks && h.sink.progress[0].second.skill_ranks.size() == 1 && h.sink.progress[0].second.skill_ranks[0].tree_guid == bard &&
              h.sink.progress[0].second.skill_ranks[0].rank_guids == (std::vector<int64_t>{kInstrumentFlute, menacing}) && h.sink.progress[0].second.replace_action_bar && h.sink.progress[0].second.action_bar.size() == 7,
          "the ranks and the bar are handed to the sink at once (the weapon bank's four slots, QuickWit, Flourish and Menacing)");
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{shielding, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 2 && sequence_since(h, from) == (std::vector<std::string>{"A", "L", "U"}) && h.s->talents()->points_left(*catalog_of(h).tree(bard), catalog_of(h).point_type(tl::Point::Archetype)) == 0,
          "Dance of Shielding (a point spent in the tree first): bought with the second point");
}
static void test_d1s2_purchase_buy(Harness &h, int64_t bard, int64_t menacing, int64_t shielding, const tl::Node *menacing_node) {
    CHECK(menacing_node->name == "Bard_Music_Menacing" && h.s->known_abilities().size() == 58, "(a level-3 Bard knows 58 abilities: Flourish joined the captured 57)");
    CHECK(client_list(h, bard).size() == 3 && h.s->action_bar().at(0, 1) && h.s->action_bar().at(0, 1)->use_type == repl::ActionbarUseType::Skill, "(Rhetoric, Flourish and the Flute are listed, and the two skills are on the bar)");
    const size_t known_before = h.mirror.known.size();
    size_t from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 1 && h.mirror.unreadable == 0, "the purchase is applied and everything the server sent reads");
    CHECK(sequence_since(h, from) == (std::vector<std::string>{"A", "L", "U"}), "the reply is the abilities' update, the tree's list and the skill update, in that order");
    CHECK(h.mirror.known.size() == known_before + 1 && h.mirror.known.back() == menacing_node->ranks[0].ability && h.mirror.known == h.s->known_abilities() && h.mirror.records.size() == h.s->record_abilities().size() &&
              h.mirror.records.back() == menacing_node->ranks[0].ability,
          "KnownAbilities and the records grow by the rank's ability, appended at the end (the client's lists are the session's)");
    CHECK(client_list(h, bard) == (std::vector<int64_t>{rank_named(h, "Bard", "Bard_Rhetoric"), catalog_of(h).auto_ranks(*catalog_of(h).tree(bard), 3)[1].rank->guid, kInstrumentFlute, menacing}),
          "the tree's whole list: the two granted ranks, the Flute the character picked, then the bought one");
    CHECK(h.mirror.skill_updates.size() == 1 && h.mirror.skill_updates[0].skill_id == menacing_node->skill && h.mirror.skill_updates[0].skill_tree_id == bard && h.mirror.skill_updates[0].rank_id == 0 &&
              !h.mirror.skill_updates[0].is_refunding && h.mirror.skill_updates[0].use_hint,
          "ClientUpdateSkillOnActionbar: the Skill, the tree, the default rank id, not a refund, with the hint");
    test_d1s2_purchase_buy_second_rank(h, bard, menacing, shielding, menacing_node);
}
static void test_d1s2_purchase_respec_and_end(Harness &h, int64_t bard, int64_t leech) {
    size_t from = 0;
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{leech, bard, false}})), h.pawn_ch);
    CHECK(h.s->talents()->purchased(bard) == std::vector<int64_t>{kInstrumentFlute} && client_list(h, bard).size() == 3 && count_tag(sequence_since(h, from), "U") == 0 && h.s->known_abilities().size() == 58 &&
              h.mirror.known == h.s->known_abilities(),
          "a respec leaves the two granted ranks and the Flute (a free pick, not in the call) listed, and the abilities of the level only: no skill update");
    h.s->finish("end");
    CHECK(!h.sink.progress.empty() && h.sink.progress.back().second.replace_skill_ranks && h.sink.progress.back().second.skill_ranks.size() == 1 &&
              h.sink.progress.back().second.skill_ranks[0].rank_guids == std::vector<int64_t>{kInstrumentFlute},
          "the end hands the ranks over once more: the Flute alone");
}
static void test_d1s2_purchase_refund_and_swap(Harness &h, int64_t bard, int64_t menacing, int64_t shielding, const tl::Node *menacing_node) {
    size_t from = 0;
    from = h.mirror.sequence.size();
    const size_t updates = h.mirror.skill_updates.size();
    h.request(purchase_rpc(build_of({{shielding, bard, false}})), h.pawn_ch);
    CHECK(sequence_since(h, from) == (std::vector<std::string>{"A", "L"}) && h.mirror.skill_updates.size() == updates, "a refund: the abilities' update and the list, no skill update");
    CHECK(h.mirror.known == h.s->known_abilities() && h.mirror.known.back() == menacing_node->ranks[0].ability && !h.s->action_bar().has_skill(shielding) &&
              h.s->talents()->points_left(*catalog_of(h).tree(bard), catalog_of(h).point_type(tl::Point::Archetype)) == 1,
          "the ability left the lists (a removal at the tail: the count shrinks), its slot is free, the point is back");
    from = h.mirror.sequence.size();
    const int64_t leech = rank_named(h, "Bard", "Bard_Music_Leech");
    h.request(purchase_rpc(build_of({{leech, bard, true}, {menacing, bard, false}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 4 && count_tag(sequence_since(h, from), "A") == 1 && count_tag(sequence_since(h, from), "L") == 1 && count_tag(sequence_since(h, from), "U") == 1,
          "a swap (refund Menacing, buy Leech) is one reply: one abilities update, one list, one skill update");
    CHECK(h.mirror.known == h.s->known_abilities() && std::find(h.mirror.known.begin(), h.mirror.known.end(), menacing_node->ranks[0].ability) == h.mirror.known.end() && client_list(h, bard).back() == leech,
          "the client's lists have Leech's ability and not Menacing's; its tree list ends with Leech");
    test_d1s2_purchase_respec_and_end(h, bard, leech);
}
static void test_d1s2_purchase() {
    std::printf("plan D1, stage 2: a purchase is answered as the retail server answers\n");
    Harness h(bard_at(3));
    const int64_t bard = tree_guid(h, "Bard");
    const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing"), shielding = rank_named(h, "Bard", "Bard_Dance_Shielding");
    const tl::Node *menacing_node = node_named(h, "Bard", "Bard_Music_Menacing");
    test_d1s2_purchase_buy(h, bard, menacing, shielding, menacing_node);
    test_d1s2_purchase_refund_and_swap(h, bard, menacing, shielding, menacing_node);
}
static void test_d1s2_passives_universal(Harness &h, const tl::Node &un) {
    const int64_t universal = content::kGuidUniversalTree;
    size_t from = h.mirror.sequence.size();
    const size_t items_before = h.mirror.effect_items.size();
    h.request(purchase_rpc(build_of({{un.ranks.front().guid, universal, true}})), h.pawn_ch);
    const auto seq = sequence_since(h, from);
    CHECK(count_tag(seq, "E+") == 1 && count_tag(seq, "L") == 1 && h.mirror.effect_items.size() == items_before + 1 && seq.back() == (un.auto_hotbar ? "U" : "L"),
          "a Universal rank: its effect item, the list and (the node has the flag) the skill update");
    const auto item = h.mirror.effect_items.rbegin()->second;
    CHECK(item.guid == un.ranks.front().effect && std::fabs(item.duration - static_cast<float>(un.ranks.front().effect_duration)) < 1e-6f && item.duration == 1.0f, "the item is the rank's effect with the record's duration: a 1 s marker");
    CHECK(h.s->effect_markers_pending() == 1, "the session has the marker's end to send");
    advance(h, 1100);
    CHECK(h.mirror.effects_removed >= 1 && h.mirror.effect_items.size() == items_before && h.s->effect_markers_pending() == 0, "a second later the server removes the marker's item itself");
}
static void test_d1s2_passives_weapon_and_refund(Harness &h, const tl::Node &un, const tl::Node &wp) {
    const int64_t universal = content::kGuidUniversalTree;
    size_t from = 0;
    from = h.mirror.sequence.size();
    const size_t before_w = h.mirror.effect_items.size();
    h.request(purchase_rpc(build_of({{wp.ranks.front().guid, tree_guid(h, "Weapon_1H_Scepter"), true}})), h.pawn_ch);
    CHECK(count_tag(sequence_since(h, from), "E+") == 1 && h.mirror.effect_items.size() == before_w + 1, "a rank of the weapon line in hand (the Scepter): its effect item");
    if (wp.ranks.front().effect_infinite) {
        const auto w = h.mirror.effect_items.rbegin()->second;
        CHECK(w.duration > 1e30f && h.s->effect_markers_pending() == 0, "an infinite effect: FLT_MAX and no end");
    } else {
        advance(h, 400);
        CHECK(h.mirror.effect_items.size() == before_w, "a marker: its item is gone 0.1 s later");
    }
    const tl::Node *other = node_where(h, "Weapon_2H_Axe", [](const tl::Node &n) { return n.ranks.front().effect != 0 && n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity <= 2; });
    if (other) {
        from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{other->ranks.front().guid, tree_guid(h, "Weapon_2H_Axe"), true}})), h.pawn_ch);
        const auto s2 = sequence_since(h, from);
        CHECK(count_tag(s2, "E+") == 0 && count_tag(s2, "L") == 1 && h.s->talent_requests_applied() == 3, "a rank of an axe tree while a scepter is in hand: the list, no effect item");
    }
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{un.ranks.front().guid, universal, false}})), h.pawn_ch);
    CHECK(count_tag(sequence_since(h, from), "E-") == 0 && count_tag(sequence_since(h, from), "L") == 1, "a refund of a marker that has ended: the list alone (L, as retail's weapon and Universal refunds)");
}
static void test_d1s2_passives() {
    std::printf("plan D1, stage 2: a passive's effect item, a marker's end\n");
    svc::Character bard = bard_at(3);
    bard.experience.push_back({kScepterLine, kLineXpLevel2, 0});
    bard.experience.push_back({kAxe2HLine, kLineXpLevel2, 0});
    Harness h(bard);
    const int64_t universal = content::kGuidUniversalTree;
    const tl::Node *un = node_where(h, "Universal", [](const tl::Node &n) { return n.ranks.front().effect != 0 && !n.ranks.front().effect_infinite && n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity == 2; });
    const tl::Node *wp = node_where(h, "Weapon_1H_Scepter", [](const tl::Node &n) { return n.ranks.front().effect != 0 && n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity <= 2; });
    CHECK(un && wp, un ? "(the fixture has a Scepter weapon rank to buy)" : "(the fixture has a Universal marker rank to buy)");
    if (!un || !wp)
        return;
    test_d1s2_passives_universal(h, *un);
    test_d1s2_passives_weapon_and_refund(h, *un, *wp);
}
static auto d1s2_state(const Harness &h) {
    return std::make_tuple(h.s->known_abilities(), h.s->record_abilities(), h.s->talents()->purchased(), h.s->action_bar().slots().size(), h.s->progress());
}
static void test_d1s2_refusals_level1() {
    {
        Harness h(bard_at(1));
        const int64_t bard = tree_guid(h, "Bard");
        const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
        const auto before = d1s2_state(h);
        const auto list = client_list(h, bard);
        size_t from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
        CHECK(h.s->talent_requests_refused() == 1 && h.s->talent_requests_applied() == 0 && d1s2_state(h) == before, "no points at level 1: refused, nothing changes");
        CHECK(sequence_since(h, from) == std::vector<std::string>{"L"} && client_list(h, bard) == list, "the client is corrected: the tree's unchanged list is sent again, nothing else");
        CHECK(h.sink.progress.empty() && !h.s->failed(), "nothing is saved, the session carries on");
        const int64_t rhetoric = rank_named(h, "Bard", "Bard_Rhetoric");
        const int64_t shielding = rank_named(h, "Bard", "Bard_Dance_Shielding");
        struct Case {
            const char *what;
            repl::SkillBuild build;
        };
        std::vector<Case> cases;
        cases.push_back({"a refund of an auto-granted rank", build_of({{rhetoric, bard, false}})});
        cases.push_back({"a rank twice in one request", build_of({{menacing, bard, true}, {menacing, bard, true}})});
        cases.push_back({"an unknown rank", build_of({{424242, bard, true}})});
        cases.push_back({"an unknown tree", build_of({{menacing, 424242, true}})});
        cases.push_back({"a refund of a rank that is not bought", build_of({{shielding, bard, false}})});
        cases.push_back({"a profession tree", build_of({{rank_named(h, "Mining", "Mining_SurveyPylon"), tree_guid(h, "Mining"), true}})});
        cases.push_back({"another class's tree", build_of({{rank_named(h, "Tank", "Tank_Grit"), tree_guid(h, "Tank"), false}})});
        cases.push_back({"the auto-granted rank bought", build_of({{rhetoric, bard, true}})});
        size_t refused = 1;
        for (const Case &c : cases) {
            h.step(milliseconds(2100));
            h.request(purchase_rpc(c.build), h.pawn_ch);
            ++refused;
            const std::string msg = std::string(c.what) + ": refused, nothing changes";
            CHECK(h.s->talent_requests_refused() == refused && d1s2_state(h) == before && h.s->talent_requests_applied() == 0, msg.c_str());
        }
    }
}
static void test_d1s2_refusals_level25_trees_and_order(Harness &h, int64_t bard, int64_t menacing, int64_t shielding, int64_t conflict) {
    const auto before = d1s2_state(h);
    h.request(purchase_rpc(build_of({{menacing, bard, false}, {shielding, bard, false}, {conflict, bard, false}, {menacing, bard, false}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 6 && d1s2_state(h) == before && !h.s->failed(), "a refund of ranks that are not bought, one of them twice: refused");
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({{rank_named(h, "Mining", "Mining_SurveyPylon"), tree_guid(h, "Mining"), true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 7 && d1s2_state(h) == before, "a profession tree: refused");
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({{rank_named(h, "Tank", "Tank_Grit"), tree_guid(h, "Tank"), true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 8 && d1s2_state(h) == before, "another class's tree: refused");
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 9 && d1s2_state(h) == before, "an empty request: refused");
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({{menacing, bard, true}, {shielding, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 1 && h.s->talents()->purchased(bard).size() == 3, "(Menacing and Shielding bought in one request, beside the Flute: the second counts the first's point)");
}
static void test_d1s2_refusals_level25_gates() {
    Harness h(bard_at(25));
    const int64_t bard = tree_guid(h, "Bard");
    const int64_t shielding = rank_named(h, "Bard", "Bard_Dance_Shielding"), conflict = rank_named(h, "Bard", "Bard_Story_Conflict");
    const auto before = d1s2_state(h);
    h.request(purchase_rpc(build_of({{shielding, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 1 && d1s2_state(h) == before, "a missing prerequisite (a point spent in the tree) is refused");
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({{conflict, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 2 && d1s2_state(h) == before, "a story needs ten points spent in the tree: refused");
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({{424242, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 3 && d1s2_state(h) == before, "an unknown Guid: refused");
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({{rank_named(h, "Bard", "Bard_Rhetoric"), bard, false}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 4 && d1s2_state(h) == before, "a refund of an auto-granted rank: refused");
    h.step(milliseconds(2100));
    const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
    h.request(purchase_rpc(build_of({{menacing, bard, true}, {menacing, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 5 && d1s2_state(h) == before, "a rank twice in one request: refused whole");
    h.step(milliseconds(2100));
    test_d1s2_refusals_level25_trees_and_order(h, bard, menacing, shielding, conflict);
}
static void test_d1s2_refusals_burst() {
    Harness h(bard_at(25));
    const int64_t bard = tree_guid(h, "Bard");
    const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
    std::vector<fake::Bunch> burst;
    for (int i = 0; i < 12; ++i) burst.push_back(h.client.actor(h.pawn_ch, purchase_rpc(build_of({{menacing, bard, i % 2 == 0}})), true));
    h.client_sends(std::move(burst));
    h.settle();
    CHECK(h.s->requests_dropped() >= 4 && h.s->talent_requests_applied() + h.s->talent_requests_refused() <= PlayerSession::kTalentBurst && !h.s->failed(),
          "twelve purchases at once: at most the burst are judged, the rest dropped and counted");
    CHECK(h.s->talents()->purchased(bard).size() <= 1, "(and the state is one the rules allow)");
}
static void test_d1s2_refusals() {
    std::printf("plan D1, stage 2: every refusal leaves the trees, the abilities, the stats, the bar and the effects untouched\n");
    test_d1s2_refusals_level1();
    test_d1s2_refusals_level25_gates();
    test_d1s2_refusals_burst();
}
static repl::ActionbarSlot make_bar_slot(int bank, int s, uint8_t type, int64_t ability = 0, int64_t rank = 0) {
    repl::ActionbarSlot out;
    out.bank_index = bank;
    out.slot_index = s;
    out.use.use_type = type;
    out.use.ability_id = ability;
    out.use.skill_rank_id = rank;
    return out;
}
static void test_d1s2_bar_set_and_refusals(Harness &h) {
    const size_t sent = h.got.size();
    h.request(set_slot_rpc(make_bar_slot(0, 3, repl::ActionbarUseType::Ability, content::kGuidSprint)), h.pc_ch);
    CHECK(h.s->bar_sets_applied() == 1 && h.s->action_bar().at(0, 3) && h.s->action_bar().at(0, 3)->ability_id == content::kGuidSprint, "ServerSetActionbarSlotUseData: the slot is stored");
    CHECK(pc_calls_since(h, sent).empty() && h.sink.progress.size() == 1 && h.sink.progress[0].second.replace_action_bar && h.sink.progress[0].second.action_bar.size() == 6, "no reply to it; the bar (six slots now) is handed to the sink");
    h.request(set_slot_rpc(make_bar_slot(0, 0, repl::ActionbarUseType::None)), h.pc_ch);
    CHECK(!h.s->action_bar().at(0, 0) && h.s->action_bar().size() == 5, "a UseType of None clears a slot");
    const size_t refused = h.s->bar_sets_refused();
    for (const repl::ActionbarSlot &bad : {make_bar_slot(20, 0, 1, 5), make_bar_slot(0, 16, 1, 5), make_bar_slot(-1, 0, 1, 5), make_bar_slot(0, -3, 1, 5), make_bar_slot(30000, 30000, 1, 5)}) {
        h.step(milliseconds(120));
        h.request(set_slot_rpc(bad), h.pc_ch);
    }
    CHECK(h.s->bar_sets_refused() == refused + 5 && h.s->action_bar().size() == 5 && !h.s->failed(), "a bank or slot out of range (five of them): refused and counted, the bar is as it was");
    repl::ActionbarSlot odd = make_bar_slot(1, 1, 1, 5);
    odd.use.instigator = 7;
    h.step(milliseconds(120));
    h.request(set_slot_rpc(odd), h.pc_ch);
    CHECK(h.s->bar_sets_refused() == refused + 6 && !h.s->action_bar().at(1, 1), "an instigator the client does not have: refused");
    const int64_t flourish_rank = rank_named(h, "Bard", "Bard_Rhetoric");
    h.step(milliseconds(120));
    h.request(set_slot_rpc(make_bar_slot(1, 4, repl::ActionbarUseType::Skill, 6064630736949673984ll, flourish_rank)), h.pc_ch);
    CHECK(h.s->action_bar().at(1, 4) && !h.s->action_bar().at(0, 0), "a skill the client sets elsewhere is on the new slot");
}
static void test_d1s2_bar_burst_and_load(Harness &h) {
    std::vector<fake::Bunch> burst;
    for (int i = 0; i < 30; ++i) burst.push_back(h.client.actor(h.pc_ch, set_slot_rpc(make_bar_slot(2, i % 12, 1, 5)), true));
    h.client_sends(std::move(burst));
    h.settle();
    CHECK(h.s->requests_dropped() >= 14 && !h.s->failed(), "thirty slot sets at once: the burst is judged, the rest dropped and counted");
    const size_t payloads = h.mirror.bar_payloads.size();
    h.step(milliseconds(2100));
    h.request(client_load_rpc(), h.pc_ch);
    CHECK(h.mirror.bar_payloads.size() == payloads + 1 && client_bar_is(h, h.s->action_bar()) && h.mirror.bar_payloads.back().slots == h.s->action_bar().wire_slots() &&
              distinct_slots(h.mirror.bar_payloads.back().slots) == h.s->action_bar().slots(),
          "ServerRequestClientLoad sends the stored bar, whole, once (the weapon bank as retail lists it, three times)");
    h.s->finish("end");
    CHECK(h.sink.progress.back().second.action_bar.size() == h.s->action_bar().size(), "the end hands the bar over once more");
}
static void test_d1s2_bar() {
    std::printf("plan D1, stage 2: ServerSetActionbarSlotUseData and ServerRequestClientLoad\n");
    Harness h(bard_at(2));
    test_d1s2_bar_set_and_refusals(h);
    test_d1s2_bar_burst_and_load(h);
}
static void test_d1s2_join_bar_lists_the_weapon_bank_three_times() {
    std::printf("plan D1, bar animation probe 2: the join's bar payload lists the weapon bank as retail does\n");
    Harness h(record_with_scrolls());
    h.request(client_load_rpc(), h.pc_ch);
    const std::vector<repl::ActionbarSlot> &slots = h.mirror.bar_payloads.back().slots;
    std::vector<int> bank4;
    size_t others = 0;
    for (const repl::ActionbarSlot &x : slots) {
        if (x.bank_index == repl::ActionbarBank::Weapon)
            bank4.push_back(x.slot_index);
        else
            ++others;
    }
    CHECK(bank4 == (std::vector<int>{1, 2, 4, 3, 1, 2, 4, 3, 1, 2, 4, 3}), "bank 4's slots 1, 2, 4, 3 are listed three times, line 131's order");
    CHECK(others == h.s->action_bar().size() - 4 && slots.size() == others + 12,
          "...and every other slot once (a level-1 Bard: QuickWit, so 13 slots in all; retail's 24 of the retail capture also hold slots of a higher-level bar and banks 2 and 18)");
    CHECK(client_bar_is(h, h.s->action_bar()) && h.s->action_bar().size() == 5, "the client's bar is the stored one (five slots): the copies are the same slots");
}
static void test_d1s2_purchase_reply_waits_84_ms() {
    std::printf("plan D1, bar animation probe 1: a purchase is answered 84 ms after the request, in one tick\n");
    Harness h(bard_at(3));
    const int64_t bard = tree_guid(h, "Bard");
    const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
    CHECK(h.s->purchase_reply_delay() == milliseconds(84) && PlayerSession::kPurchaseReplyDelay == milliseconds(84), "the default delay is retail's median, 84 ms");
    const size_t from = h.mirror.sequence.size();
    const size_t chains = h.got.size();
    h.client_sends({h.client.actor(h.pawn_ch, purchase_rpc(build_of({{menacing, bard, true}})), true)});
    h.flush();
    CHECK(h.s->talent_requests_applied() == 1 && h.got.size() == chains && h.s->replies_pending() == 3, "the request is applied in its own tick and nothing is sent: the pawn's update, the XPTracker and the skill update wait");
    h.step(milliseconds(10));
    CHECK(h.got.size() == chains && h.s->replies_pending() == 3, "10 ms after the request: still nothing on the wire (neither on the PlayerController's channel nor the pawn's)");
    h.step(milliseconds(73));
    CHECK(h.got.size() == chains && h.s->replies_pending() == 3, "83 ms after it: still nothing");
    h.step(milliseconds(1));
    CHECK(sequence_since(h, from) == (std::vector<std::string>{"A", "L", "U"}) && h.s->replies_pending() == 0, "84 ms after it, in one tick: the pawn's update, then the XPTracker, then the skill update");
    bool one_packet = h.got.size() == chains + 3;
    for (size_t i = chains; one_packet && i < h.got.size(); ++i) one_packet = h.packet_of[i] == h.packet_of[chains];
    CHECK(one_packet, "...and the three chains go in one packet (retail: 116 of 123 replies)");
    const int64_t leech = rank_named(h, "Bard", "Bard_Music_Leech");
    const size_t from2 = h.mirror.sequence.size();
    h.client_sends({h.client.actor(h.pawn_ch, purchase_rpc(build_of({{leech, bard, true}})), true)});
    h.step(milliseconds(20));
    h.client_sends({h.client.actor(h.pawn_ch, purchase_rpc(build_of({{leech, bard, false}})), true)});
    h.settle();
    CHECK(h.s->talent_requests_applied() == 3 && count_tag(sequence_since(h, from2), "U") == 1 && h.mirror.known == h.s->known_abilities() && !h.s->action_bar().has_skill(leech),
          "a purchase and its refund 20 ms apart: both answered after their delay, in order, and the client ends with the session's lists");
    Harness z(bard_at(3));
    z.s->set_purchase_reply_delay(milliseconds(0));
    const size_t zfrom = z.mirror.sequence.size();
    z.client_sends({z.client.actor(z.pawn_ch, purchase_rpc(build_of({{menacing, tree_guid(z, "Bard"), true}})), true)});
    z.flush();
    CHECK(sequence_since(z, zfrom) == (std::vector<std::string>{"A", "L", "U"}), "a delay of 0 sends the reply in the tick of the request, as before");
}
static std::vector<std::pair<int64_t, int64_t>> buy_all_auto_hotbar_actives(Harness &h, int64_t bard) {
    std::vector<std::pair<int64_t, int64_t>> bought;
    const tl::Tree *tree = catalog_of(h).tree(bard);
    if (!tree) {
        return bought;
    }
    for (const tl::Node &n : tree->nodes) {
        if (n.ranks.empty() || n.ranks.front().ability == 0 || !n.auto_hotbar) {
            continue;
        }
        const size_t before = h.s->talent_requests_applied();
        h.step(milliseconds(2100));
        h.request(purchase_rpc(build_of({{n.ranks.front().guid, bard, true}})), h.pawn_ch);
        if (h.s->talent_requests_applied() > before) {
            bought.emplace_back(n.ranks.front().ability, n.ranks.front().guid);
        }
    }
    return bought;
}
static void build_allowance_burst(Harness &h, const std::vector<std::pair<int64_t, int64_t>> &bought, size_t N, std::vector<repl::ActionbarSlot> &sent, std::vector<fake::Bunch> &burst) {
    const size_t K = bought.size();
    for (size_t i = 0; i < N; ++i) {
        repl::ActionbarSlot x;
        x.bank_index = 1 + static_cast<int>(i / 12);
        x.slot_index = static_cast<int>(i % 12);
        if (i < K) {
            x.use.use_type = repl::ActionbarUseType::Skill;
            x.use.ability_id = bought[i].first;
            x.use.skill_rank_id = bought[i].second;
        } else {
            x.use.use_type = repl::ActionbarUseType::Ability;
            x.use.ability_id = 5000 + static_cast<int64_t>(i);
        }
        sent.push_back(x);
        burst.push_back(h.client.actor(h.pc_ch, set_slot_rpc(x), true));
    }
}
static void test_d1s2_allowance_exhaust_after_burst(Harness &h, size_t K) {
    size_t copies = 0;
    for (const repl::ActionbarSlot &x : h.s->action_bar().slots()) {
        if (x.bank_index < actionbar::kClassBanks && x.use.use_type == repl::ActionbarUseType::Skill)
            ++copies;
    }
    CHECK(copies <= K, "...and a bought skill is on the class pages once (the server's own placement moved with the client's Set)");
    std::vector<fake::Bunch> more;
    for (int i = 0; i < 24; ++i) {
        repl::ActionbarSlot x;
        x.bank_index = 3;
        x.slot_index = i % 12;
        x.use.use_type = repl::ActionbarUseType::Ability;
        x.use.ability_id = 9000 + i;
        more.push_back(h.client.actor(h.pc_ch, set_slot_rpc(x), true));
    }
    h.step(milliseconds(2100));
    const size_t dropped2 = h.s->requests_dropped();
    h.client_sends(std::move(more));
    h.settle();
    CHECK(h.s->requests_dropped() > dropped2 && !h.s->failed(), "(a burst with no purchase before it is still held to the budget)");
}
static void test_d1s2_purchase_sets_have_their_own_allowance() {
    std::printf("plan D1, stage 2: the client's burst of ServerSetActionbarSlotUseData after a purchase is not dropped by the ten-a-second budget\n");
    Harness h(bard_at(25));
    const int64_t bard = tree_guid(h, "Bard");
    const std::vector<std::pair<int64_t, int64_t>> bought = buy_all_auto_hotbar_actives(h, bard);
    const size_t K = bought.size();
    std::printf("    (%zu active ranks bought)\n", K);
    CHECK(K >= 4, "(a level-25 Bard buys at least four actives one request at a time)");
    const size_t N = 20;
    std::vector<fake::Bunch> burst;
    std::vector<repl::ActionbarSlot> sent;
    build_allowance_burst(h, bought, N, sent, burst);
    const size_t dropped = h.s->requests_dropped();
    h.client_sends(std::move(burst));
    h.settle();
    size_t held = 0;
    for (const repl::ActionbarSlot &x : sent) {
        const repl::ActionbarUseData *at = h.s->action_bar().at(x.bank_index, x.slot_index);
        held += (at && *at == x.use) ? 1 : 0;
    }
    CHECK(held == N && h.s->requests_dropped() == dropped && h.s->bar_sets_applied() == N, "all 20 Sets of the client are stored where it put them (the purchases' allowance, then the bar budget's 16): none dropped");
    test_d1s2_allowance_exhaust_after_burst(h, K);
}
static void test_d1s2_level_grants() {
    std::printf("plan D1, stage 2: a level grants the class tree's ranks as well as the Universal tree's\n");
    Harness h(record_with_scrolls());
    const int64_t bard = tree_guid(h, "Bard");
    const size_t from = h.mirror.sequence.size();
    h.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
    const auto seq = sequence_since(h, from);
    CHECK(h.s->progress().level == 10 && count_tag(seq, "A") == 1 && count_tag(seq, "U") == 5 && count_tag(seq, "E+") == 3,
          "level 10: one abilities update, five skill updates (Flourish, Resonant Weapon, Fortissimo of the Bard; Sprint Training I and Endurance Training I) and three effect items");
    CHECK(h.s->known_abilities().size() == 57 + 2 && h.mirror.known == h.s->known_abilities() && h.s->known_abilities()[57] == catalog_of(h).auto_ranks(*catalog_of(h).tree(bard), 10)[1].rank->ability,
          "Flourish and Fortissimo join KnownAbilities, appended (Resonant Weapon is a passive)");
    CHECK(client_list(h, bard).size() == 4 && client_list(h, content::kGuidUniversalTree).size() == 2, "the Bard tree's list has the four ranks of level 10, the Universal tree's two");
    CHECK(h.s->action_bar().at(0, 1) && h.s->action_bar().at(0, 2) && !h.s->action_bar().at(0, 3), "the granted skills with an ability (Flourish, Fortissimo) are on the bar after QuickWit: slots 1 and 2");
    CHECK(!h.sink.progress.empty() && h.sink.progress.back().second.replace_action_bar && h.sink.progress.back().second.action_bar.size() == 5 + 2, "the progress carries the bar");
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    test_d1s2_join();
    test_d1s2_purchase();
    test_d1s2_passives();
    test_d1s2_refusals();
    test_d1s2_bar();
    test_d1s2_join_bar_lists_the_weapon_bank_three_times();
    test_d1s2_purchase_reply_waits_84_ms();
    test_d1s2_purchase_sets_have_their_own_allowance();
    test_d1s2_level_grants();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
