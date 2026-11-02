// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_items_session_helpers.h"
using namespace aoc::worldd;
static int64_t menacing_ability_of(const Harness &h, int64_t rank) {
    const auto ref = catalog_of(h).find(tree_guid(h, "Bard"), rank);
    return ref.ok() ? ref.rank->ability : 0;
}
static BunchWriter ranged_rpc(bool ranged) {
    const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema *rpc = pawn.rpc("ServerSetUseRangedWeapon");
    const auto params = repl::set_use_ranged_weapon_params(*rpc, ranged);
    return params ? actor_rpc(pawn, "ServerSetUseRangedWeapon", *params) : BunchWriter();
}
static bool has_no_bard_tree_ability(const Harness &h, int64_t bard, int64_t menacing, const std::vector<int64_t> &bard_known) {
    bool no_bard = true;
    for (const int64_t a : bard_known) {
        const bool bard_tree_ability = a == catalog_of(h).auto_ranks(*catalog_of(h).tree(bard), 3)[0].rank->ability || a == menacing_ability_of(h, menacing);
        if (bard_tree_ability)
            no_bard = no_bard && std::find(h.s->known_abilities().begin(), h.s->known_abilities().end(), a) == h.s->known_abilities().end();
    }
    return no_bard;
}
static void test_d1s2_class_change_back_to_bard(Harness &h, int64_t bard, int64_t tank, int64_t menacing, const std::vector<int64_t> &bard_known) {
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 1, "the Bard's tree cannot be bought by a Tank");
    h.request_both(item_used(3, 7, guid_n(213)), item_activate(3, 7, guid_n(213)));
    std::vector<int64_t> back_sorted = h.s->known_abilities(), before_sorted = bard_known;
    std::sort(back_sorted.begin(), back_sorted.end());
    std::sort(before_sorted.begin(), before_sorted.end());
    CHECK(h.s->class_id() == 2 && back_sorted == before_sorted && h.mirror.known == h.s->known_abilities(), "back to Bard: KnownAbilities are the Bard's again, with Menacing (the join's order, not the live append's)");
    CHECK(client_list(h, bard).size() == 4 && client_list(h, tank).empty() && h.s->action_bar().has_skill(menacing) && client_bar_is(h, h.s->action_bar()),
          "the Bard tree's list is back with the bought rank, the Tank tree's is gone, Menacing is on the bar again");
}
static void test_d1s2_class_change_verify_tank(Harness &h, int64_t bard, int64_t tank, int64_t quickwit_rank, const std::vector<int64_t> &bard_known) {
    const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
    bool cleared = false;
    for (const repl::ActionbarSlot &s : h.mirror.bar_payloads.back().slots) cleared = cleared || (s.use.use_type == repl::ActionbarUseType::None);
    CHECK(cleared && !h.s->action_bar().has_skill(menacing) && !h.s->action_bar().has_skill(quickwit_rank), "the payload holds cleared slots (UseType None) for the Bard's skills, which are off the bar");
    for (const auto &[key, use] : h.mirror.bar) {
        if (use.use_type == repl::ActionbarUseType::Skill) {
            const auto ref = catalog_of(h).find(tank, use.skill_rank_id);
            CHECK(ref.ok(), "every skill on the client's bar is a Tank skill");
        }
    }
    CHECK(h.s->bootstrap().setup().class_record_guid == 108640201692798ll && h.s->character_stats()->class_enum() == 7 && stats_match(h, nullptr, true), "CombatInfo's class, the stats and the client's arrays are the Tank's");
    CHECK(h.s->talents()->purchased(bard) == (std::vector<int64_t>{kInstrumentFlute, menacing}), "the Bard's bought rank (and the Flute) is kept under the Bard tree");
    CHECK(h.sink.progress.back().second.class_id == 7 && h.sink.progress.back().second.skill_ranks.size() == 1 && h.sink.progress.back().second.skill_ranks[0].tree_guid == bard, "the saved progress: class 7, the Bard tree's ranks kept");
    test_d1s2_class_change_back_to_bard(h, bard, tank, menacing, bard_known);
}
static void test_d1s2_class_change() {
    std::printf("plan D1, stage 2: a class scroll switches the class live, and each class keeps its ranks\n");
    Harness h(bard_at(3));
    const int64_t bard = tree_guid(h, "Bard"), tank = tree_guid(h, "Tank");
    const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
    h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
    const std::vector<int64_t> bard_known = h.s->known_abilities();
    CHECK(h.s->talent_requests_applied() == 1 && h.s->action_bar().has_skill(menacing), "(a Bard with Menacing bought)");
    const auto quickwit = h.s->action_bar().at(0, 0);
    CHECK(quickwit && quickwit->use_type == repl::ActionbarUseType::Skill, "(and QuickWit on bank 0 slot 0)");
    const int64_t quickwit_rank = quickwit ? quickwit->skill_rank_id : 0;
    const size_t from = h.mirror.sequence.size();
    const size_t payloads = h.mirror.bar_payloads.size();
    h.request_both(item_used(3, 6, guid_n(212)), item_activate(3, 6, guid_n(212)));
    CHECK(h.s->class_id() == 7 && h.s->uses_applied() == 1 && h.mirror.unreadable == 0, "the Tank scroll is used and everything the server sent reads");
    const tl::Tree *ttree = catalog_of(h).tree(tank);
    const std::vector<int64_t> tank_abilities = h.s->talents()->abilities(*ttree);
    CHECK(!tank_abilities.empty(), "(the Tank tree lists abilities at level 3)");
    const bool no_bard = has_no_bard_tree_ability(h, bard, menacing, bard_known);
    CHECK(no_bard && h.mirror.known == h.s->known_abilities() && h.mirror.records.size() == h.s->record_abilities().size(), "the Bard tree's abilities left KnownAbilities and the records, on the client too");
    CHECK(std::find(h.s->known_abilities().begin(), h.s->known_abilities().end(), tank_abilities[0]) != h.s->known_abilities().end() && h.s->known_abilities().front() == bs::general_unstuck() &&
              std::equal(h.s->known_abilities().end() - 45, h.s->known_abilities().end(), bs::static_known_tail().begin()),
          "the Tank tree's abilities are in, Unstuck is first and the static tail is last");
    CHECK(client_list(h, bard).empty() && client_list(h, tank).size() == h.s->talents()->list(*ttree).size() && !client_list(h, tank).empty(), "the XPTracker: the Bard tree's item is gone, the Tank tree's list is there");
    CHECK(h.mirror.bar_payloads.size() == payloads + 1 && count_tag(sequence_since(h, from), "P") == 1 && client_bar_is(h, h.s->action_bar()), "the bar goes out in one payload and the client's bar is the session's");
    test_d1s2_class_change_verify_tank(h, bard, tank, quickwit_rank, bard_known);
}
static void test_d1s2_relog() {
    std::printf("plan D1, stage 2: a relog keeps the ranks and the bar\n");
    svc::Character record = bard_at(3);
    std::vector<int64_t> known_before, records_before;
    actionbar::Bar bar_before;
    std::map<int64_t, std::vector<int64_t>> purchased_before;
    svc::Progress saved;
    {
        Harness h(record);
        const int64_t bard = tree_guid(h, "Bard");
        h.request(purchase_rpc(build_of({{rank_named(h, "Bard", "Bard_Music_Menacing"), bard, true}, {rank_named(h, "Bard", "Bard_Music_Leech"), bard, true}})), h.pawn_ch);
        repl::ActionbarSlot s;
        s.bank_index = 1;
        s.slot_index = 7;
        s.use.use_type = repl::ActionbarUseType::Ability;
        s.use.ability_id = content::kGuidSprint;
        s.use.combo_button = 3;
        h.step(milliseconds(150));
        h.request(set_slot_rpc(s), h.pc_ch);
        const tl::Node *un = node_where(h, "Universal", [](const tl::Node &n) { return n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity == 2; });
        if (un)
            h.request(purchase_rpc(build_of({{un->ranks.front().guid, content::kGuidUniversalTree, true}})), h.pawn_ch);
        known_before = h.s->known_abilities();
        records_before = h.s->record_abilities();
        bar_before = h.s->action_bar();
        purchased_before = h.s->talents()->purchased();
        h.s->finish("end");
        saved = h.sink.progress.back().second;
        record.level = saved.level;
        record.class_id = saved.class_id;
        record.experience = saved.experience;
        for (const svc::TreeRanks &t : saved.skill_ranks) record.skill_ranks.push_back(t);
        for (const svc::ActionbarSlot &a : saved.action_bar) record.action_bar.push_back(a);
    }
    CHECK(saved.replace_skill_ranks && !saved.skill_ranks.empty() && saved.replace_action_bar && !saved.action_bar.empty(), "(the session saved ranks and a bar)");
    Harness again(record);
    CHECK(again.s->talents() && again.s->talents()->purchased() == purchased_before, "the second login has the same bought ranks, tree by tree, in order");
    CHECK(again.s->action_bar() == bar_before, "...the same bar, slot for slot (the item guid and the combo button included)");
    std::vector<int64_t> a = known_before, b = again.s->known_abilities();
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    CHECK(a == b && again.mirror.known == again.s->known_abilities(), "...the same KnownAbilities as a set (the join lists them in tree order, a live purchase appended) and the client has them");
    const int64_t bard = tree_guid(again, "Bard");
    CHECK(client_list(again, bard) == again.s->talents()->list(*catalog_of(again).tree(bard)) && client_list(again, bard).size() == 5, "the client's Bard tree list: the two granted ranks, the Flute and the two bought");
    CHECK(client_list(again, content::kGuidUniversalTree).size() == 1 && again.mirror.unreadable == 0, "...and the Universal tree's: the one bought rank (the level grants none before 5)");
    const size_t payloads = again.mirror.bar_payloads.size();
    again.request(client_load_rpc(), again.pc_ch);
    CHECK(again.mirror.bar_payloads.size() == payloads + 1 && client_bar_is(again, bar_before), "the load sends the stored bar");
}
static std::optional<bool> pawn_ranged_flag(const Harness &h) {
    const repl::Subobject *combat = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.combat_info);
    const repl::Value *v = combat ? combat->obj.state.get(*combat->obj.schema, "bIsUsingRangedWeapon") : nullptr;
    if (!v || !v->get_if<bool>())
        return std::nullopt;
    return *v->get_if<bool>();
}
static void test_d1s2_weapon_swap_back(Harness &h) {
    CHECK(pawn_ranged_flag(h) == std::optional<bool>(true), "CombatInfo.bIsUsingRangedWeapon is true on the pawn (retail 403: the CombatInfo block of the swap's reply)");
    const size_t again_from = h.mirror.sequence.size();
    h.step(milliseconds(120));
    h.request(ranged_rpc(true), h.pawn_ch);
    CHECK(h.s->weapon_swaps() == 1 && sequence_since(h, again_from).empty(), "the same set again: nothing changes, nothing is sent");
    h.step(milliseconds(120));
    h.request(ranged_rpc(false), h.pawn_ch);
    CHECK(h.s->weapon_swaps() == 2 && h.s->known_abilities().back() == 6064631845843960261ll && h.s->known_abilities().size() == 57, "back to the melee set: the Scepter's combo is the last again");
    CHECK(pawn_ranged_flag(h) == std::optional<bool>(false), "...and CombatInfo.bIsUsingRangedWeapon is false again");
}
static void test_d1s2_weapon_swap() {
    std::printf("plan D1, stage 2: swapping weapons moves the combo to the end\n");
    Harness h(bard_at(1));
    const auto before = h.s->known_abilities();
    const size_t before_records = h.s->record_abilities().size();
    CHECK(before.size() == 57 && before[11] == 6064631845843960261ll, "(the Scepter's combo is KnownAbilities' element 11)");
    const size_t from = h.mirror.sequence.size();
    h.request(ranged_rpc(true), h.pawn_ch);
    CHECK(h.s->weapon_swaps() == 1 && h.s->using_ranged_weapon() && count_tag(sequence_since(h, from), "A") == 1, "ServerSetUseRangedWeapon(true): one abilities update");
    const auto after = h.s->known_abilities();
    CHECK(after.size() == 57 && after.back() == 6064631012536680449ll && std::find(after.begin(), after.end(), 6064631845843960261ll) == after.end() && h.mirror.known == after,
          "KnownAbilities keep their count: the Scepter's combo left, the Book's is the last element, on the client too");
    CHECK(h.s->record_abilities().back() == 6064631012536680449ll && h.s->record_abilities().size() == before_records && h.mirror.records == h.s->record_abilities() &&
              std::find(h.s->record_abilities().begin(), h.s->record_abilities().end(), 6064631845843960261ll) != h.s->record_abilities().end(),
          "the records: the Book's combo is the last element, the Scepter's stays carried, the count is the same");
    const repl::Subobject *owner = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.owner_info);
    const repl::Value *line = owner ? owner->obj.state.get(*owner->obj.schema, "WeaponSkillLineId.Guid") : nullptr;
    CHECK(line && line->get_if<int64_t>() && *line->get_if<int64_t>() == 6064631670991093890ll, "OwnerInfo.WeaponSkillLineId follows: Weapon_Ranged_Book");
    test_d1s2_weapon_swap_back(h);
}
static std::vector<std::string> non_stat_seq_since(const Harness &h, size_t from) {
    std::vector<std::string> out;
    for (const std::string &t : sequence_since(h, from)) {
        if (t != "S")
            out.push_back(t);
    }
    return out;
}
static void test_d1s2_talent_slots_instrument_swap(Harness &h, int64_t bard, int64_t menacing, int64_t flute, int64_t fiddle) {
    using V = std::vector<std::string>;
    const auto seq = [&](size_t from) { return non_stat_seq_since(h, from); };
    size_t from = 0;
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({{fiddle, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 2 && h.s->talents()->purchased(bard) == std::vector<int64_t>{flute}, "the Fiddle while the Flute is picked: refused (one option of a slot at a time)");
    advance(h, 1200);
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{flute, bard, false}, {fiddle, bard, true}})), h.pawn_ch);
    const std::vector<int64_t> swapped = client_list(h, bard);
    CHECK(h.s->talent_requests_applied() == 2 && seq(from) == (V{"E+", "L", "U"}) && swapped.back() == fiddle && std::count(swapped.begin(), swapped.end(), flute) == 0,
          "a swap Flute -> Fiddle is one call: one reply (E+ L U), the client's list has the Fiddle and not the Flute");
    advance(h, 1200);
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 3 && seq(from) == (V{"A", "L", "U"}), "Menacing, once an Instrument is picked: A L U");
}
static void test_d1s2_talent_slots_instrument(Harness &h, int64_t bard, const std::vector<const tl::Node *> &instrument) {
    using V = std::vector<std::string>;
    const auto seq = [&](size_t from) { return non_stat_seq_since(h, from); };
    const int64_t menacing = rank_named(h, "Bard", "Bard_Music_Menacing");
    const int64_t flute = instrument[0]->ranks.front().guid, fiddle = instrument[1]->ranks.front().guid;
    const std::vector<int64_t> list0 = client_list(h, bard);
    size_t from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{menacing, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 1 && h.s->talent_requests_applied() == 0 && seq(from) == V{"L"} && client_list(h, bard) == list0,
          "a song that needs an Instrument, before one is picked: refused, and the client's unchanged list is sent again");
    h.step(milliseconds(2100));
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{flute, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 1 && seq(from) == (V{"E+", "L", "U"}) && client_list(h, bard).back() == flute && h.mirror.unreadable == 0, "picking the Flute (a talent of a slot): E+ L U, the rank is in the client's list");
    CHECK(h.mirror.skill_updates.size() == 1 && h.mirror.skill_updates[0].skill_id == instrument[0]->skill && h.mirror.skill_updates[0].skill_tree_id == bard && h.mirror.skill_updates[0].rank_id == 0 &&
              !h.mirror.skill_updates[0].is_refunding && h.mirror.skill_updates[0].use_hint,
          "ClientUpdateSkillOnActionbar: the option's own Skill (not the slot's), the tree, the default rank id, the hint");
    test_d1s2_talent_slots_instrument_swap(h, bard, menacing, flute, fiddle);
}
static void test_d1s2_talent_slots_hymn_swap_and_bar(Harness &h, int64_t bard, const std::vector<const tl::Node *> &hymn, int64_t hymn_rank, int64_t hymn_aoe_rank) {
    using V = std::vector<std::string>;
    const auto seq = [&](size_t from) { return non_stat_seq_since(h, from); };
    size_t from = 0;
    h.step(milliseconds(2100));
    h.request(purchase_rpc(build_of({{hymn_aoe_rank, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 3, "the AOE Hymn beside the Hymn: refused");
    h.step(milliseconds(2100));
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{hymn_rank, bard, false}, {hymn_aoe_rank, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 5 && seq(from) == (V{"A", "L", "U"}) && std::count(h.mirror.known.begin(), h.mirror.known.end(), hymn[0]->ranks.front().ability) == 0 &&
              std::count(h.mirror.known.begin(), h.mirror.known.end(), hymn[1]->ranks.front().ability) == 1 && !h.s->action_bar().has_skill(hymn_rank) && h.s->action_bar().has_skill(hymn_aoe_rank),
          "a swap Hymn -> Hymn (AOE): one call, one reply (A L U); the old ability leaves both lists and the bar, the new one is in them");
    repl::ActionbarSlot slot;
    slot.bank_index = 1;
    slot.slot_index = 5;
    slot.use.use_type = repl::ActionbarUseType::Skill;
    slot.use.ability_id = hymn[1]->ranks.front().ability;
    slot.use.skill_rank_id = hymn_aoe_rank;
    h.step(milliseconds(150));
    h.request(set_slot_rpc(slot), h.pc_ch);
    const actionbar::Bar &bar = h.s->action_bar();
    int copies = 0;
    for (const repl::ActionbarSlot &x : bar.slots()) copies += x.use.skill_rank_id == hymn_aoe_rank ? 1 : 0;
    CHECK(bar.at(1, 5) && bar.at(1, 5)->skill_rank_id == hymn_aoe_rank && copies == 1, "the client's own placement of the skill is stored, and the skill is on the bar once");
}
static void test_d1s2_talent_slots_hymn(Harness &h, int64_t bard, const std::vector<const tl::Node *> &hymn, int64_t &hymn_aoe_rank) {
    using V = std::vector<std::string>;
    const auto seq = [&](size_t from) { return non_stat_seq_since(h, from); };
    const int64_t hymn_rank = hymn[0]->ranks.front().guid;
    hymn_aoe_rank = hymn[1]->ranks.front().guid;
    size_t from = 0;
    h.step(milliseconds(150));
    from = h.mirror.sequence.size();
    const size_t updates = h.mirror.skill_updates.size();
    h.request(purchase_rpc(build_of({{hymn_rank, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 4 && seq(from) == (V{"A", "L", "U"}), "the Hymn talent (a node of a slot with an ability, a point spent in the tree first): A L U");
    CHECK(h.mirror.skill_updates.size() == updates + 1 && h.mirror.skill_updates.back().skill_id == hymn[0]->skill && h.mirror.skill_updates.back().skill_tree_id == bard && h.mirror.skill_updates.back().use_hint,
          "...its skill update names the Hymn option's Skill");
    CHECK(h.mirror.known == h.s->known_abilities() && std::count(h.mirror.known.begin(), h.mirror.known.end(), hymn[0]->ranks.front().ability) == 1 && h.mirror.known.back() == hymn[0]->ranks.front().ability &&
              h.mirror.records.back() == hymn[0]->ranks.front().ability,
          "the ability is appended to the client's KnownAbilities and records");
    CHECK(h.s->action_bar().has_skill(hymn_rank) && client_list(h, bard).back() == hymn_rank, "the Hymn is on the stored bar (a design choice) and in the tree's list");
    test_d1s2_talent_slots_hymn_swap_and_bar(h, bard, hymn, hymn_rank, hymn_aoe_rank);
}
static void test_d1s2_talent_slots_relog(const svc::Character &record, const std::map<int64_t, std::vector<int64_t>> &purchased_before, const actionbar::Bar &bar_before, const std::vector<int64_t> &known_before, int64_t hymn_aoe_rank) {
    Harness again(record);
    const int64_t bard = tree_guid(again, "Bard");
    CHECK(again.s->talents() && again.s->talents()->purchased() == purchased_before, "the second login has the same talents (the Fiddle, Menacing and the AOE Hymn), in order");
    std::vector<int64_t> a = known_before, b = again.s->known_abilities();
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    CHECK(a == b && again.mirror.known == again.s->known_abilities() && std::count(b.begin(), b.end(), 0) == 0, "...the same KnownAbilities as a set, and the client has them");
    CHECK(again.s->action_bar() == bar_before && again.s->action_bar().at(1, 5) && again.s->action_bar().at(1, 5)->skill_rank_id == hymn_aoe_rank, "...the same bar, with the Hymn where the client put it");
    const size_t payloads = again.mirror.bar_payloads.size();
    again.request(client_load_rpc(), again.pc_ch);
    bool in_payload = false;
    if (again.mirror.bar_payloads.size() == payloads + 1) {
        for (const repl::ActionbarSlot &x : again.mirror.bar_payloads.back().slots) in_payload = in_payload || (x.bank_index == 1 && x.slot_index == 5 && x.use.skill_rank_id == hymn_aoe_rank);
    }
    CHECK(in_payload && client_bar_is(again, again.s->action_bar()), "ServerRequestClientLoad sends a bar with the talent's skill in the slot the client chose, and the client holds exactly the stored bar");
    CHECK(client_list(again, bard) == again.s->talents()->list(*catalog_of(again).tree(bard)), "the client's Bard list at the join is the character's, talents included");
}
static void test_d1s2_talent_slots() {
    std::printf("plan D1, stage 2: talents (nodes that offer a choice of skills) are bought like any rank\n");
    svc::Character record = record_with_scrolls();
    record.level = 25;
    svc::Progress saved;
    std::map<int64_t, std::vector<int64_t>> purchased_before;
    actionbar::Bar bar_before;
    std::vector<int64_t> known_before;
    int64_t hymn_aoe_rank = 0;
    {
        Harness h(record);
        const int64_t bard = tree_guid(h, "Bard");
        std::vector<const tl::Node *> instrument, hymn;
        for (const tl::Node &n : catalog_of(h).tree(bard)->nodes) {
            if (n.name == "Bard_Passive_Instrument")
                instrument.push_back(&n);
            if (n.name == "Bard_Song_Hymn")
                hymn.push_back(&n);
        }
        CHECK(instrument.size() == 3 && hymn.size() == 2 && instrument[0]->slot != 0 && hymn[0]->slot != 0, "(the Bard's Instrument slot offers three skills and the Hymn slot two)");
        if (instrument.size() != 3 || hymn.size() != 2)
            return;
        test_d1s2_talent_slots_instrument(h, bard, instrument);
        test_d1s2_talent_slots_hymn(h, bard, hymn, hymn_aoe_rank);
        purchased_before = h.s->talents()->purchased();
        bar_before = h.s->action_bar();
        known_before = h.s->known_abilities();
        h.s->finish("end");
        saved = h.sink.progress.back().second;
        record.level = saved.level;
        record.class_id = saved.class_id;
        record.experience = saved.experience;
        for (const svc::TreeRanks &t : saved.skill_ranks) record.skill_ranks.push_back(t);
        for (const svc::ActionbarSlot &a : saved.action_bar) record.action_bar.push_back(a);
    }
    test_d1s2_talent_slots_relog(record, purchased_before, bar_before, known_before, hymn_aoe_rank);
}
static void test_d1s2_regeneration() {
    std::printf("plan D1, stage 2: Health and Mana regenerate every second\n");
    constexpr int64_t kHealth = content::kGuidHealth, kMaxHealth = content::kGuidMaxHealth, kHealthRegen = content::kGuidHealthRegeneration;
    std::vector<svc::InventoryItem> items = bard_items();
    Harness h(record_with(items));
    CHECK(h.s->character_stats()->find(kHealthRegen)->value > 0, "(the Bard's HealthRegeneration is positive)");
    h.request(item_used(3, 3, guid_n(203)), h.pawn_ch);
    const float regen = h.s->character_stats()->find(kHealthRegen)->value;
    const float max = h.s->character_stats()->find(kMaxHealth)->value;
    const float hp = h.s->character_stats()->find(kHealth)->value;
    CHECK(max > hp, "the chest raised MaxHealth above Health");
    const size_t before = h.mirror.stat_updates.size();
    const size_t ticks = h.s->regen_ticks();
    advance(h, 1100);
    const size_t made = h.s->regen_ticks() - ticks;
    CHECK(made >= 1 && h.mirror.stat_updates.size() > before, "a second later: a regeneration tick, sent as a stat delta");
    const float now = h.s->character_stats()->find(kHealth)->value;
    if (std::fabs(now - std::min(max, hp + regen * static_cast<float>(made))) >= 1e-2f) {
        std::printf("    Health %f -> %f, regeneration %f, %zu tick(s), maximum %f\n", hp, now, regen, made, max);
    }
    CHECK(std::fabs(now - std::min(max, hp + regen * static_cast<float>(made))) < 1e-2f, "Health gained one HealthRegeneration for each tick, clamped at the maximum");
    std::string why;
    CHECK(stats_match(h, &why), "the client's arrays are the session's stats");
    if (!why.empty())
        std::printf("    %s\n", why.c_str());
    for (int i = 0; i < 2000 && h.s->character_stats()->find(kHealth)->value < max; ++i) h.step(milliseconds(5000));
    CHECK(h.s->character_stats()->find(kHealth)->value == max && stats_match(h), "Health reaches its maximum, exactly, and the client has it");
    const size_t at_full = h.s->regen_ticks();
    for (int i = 0; i < 4; ++i) h.step(milliseconds(5000));
    CHECK(h.s->regen_ticks() == at_full && !h.s->failed(), "at the maximum nothing more is sent");
    CHECK(h.s->abilities() && h.s->abilities()->stamina() == h.s->abilities()->max_stamina(), "Stamina is the ability runtime's: untouched");
}
static void test_d1s2_shapes_bard(Harness &h, int64_t bard) {
    using V = std::vector<std::string>;
    const auto seq = [](const Harness &hr, size_t from) { return non_stat_seq_since(hr, from); };
    const auto rank = [&](const char *name) { return rank_named(h, "Bard", name); };
    size_t from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{rank("Bard_Music_Menacing"), bard, true}, {rank("Bard_Music_Leech"), bard, true}, {rank("Bard_Music_Hopeful"), bard, true}})), h.pawn_ch);
    CHECK(seq(h, from) == (V{"A", "L", "U", "U", "U"}), "S5 three ranks in one call: A L U U U (the abilities of all three in one update)");
    CHECK(h.mirror.skill_updates.size() >= 3 && h.mirror.skill_updates[h.mirror.skill_updates.size() - 3].skill_id == node_named(h, "Bard", "Bard_Music_Menacing")->skill &&
              h.mirror.skill_updates.back().skill_id == node_named(h, "Bard", "Bard_Music_Hopeful")->skill,
          "...the skill updates in the order of the request");
    h.step(milliseconds(150));
    h.request(purchase_rpc(build_of({{rank("Bard_Music_Pensive"), bard, true}})), h.pawn_ch);
    h.step(milliseconds(150));
    from = h.mirror.sequence.size();
    const int64_t passive = rank("Bard_Passive_QuickWit_Burning");
    h.request(purchase_rpc(build_of({{passive, bard, true}})), h.pawn_ch);
    CHECK(h.s->talent_requests_applied() == 3 && seq(h, from) == (V{"E+", "L", "U"}), "S2 a class passive: E+ L U");
    CHECK(h.mirror.effect_items.rbegin()->second.duration == 1.0f && h.s->effect_markers_pending() == 1, "...whose item lasts the record's own duration (this Bard passive's effect is a 1 s marker: the server ends it)");
    h.step(milliseconds(150));
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{passive, bard, false}})), h.pawn_ch);
    CHECK(seq(h, from) == (V{"E-", "L"}) && h.mirror.effect_items.empty(), "S6b a refund of a passive: E- L, and its item is gone");
    h.step(milliseconds(150));
    h.request(purchase_rpc(build_of({{passive, bard, true}})), h.pawn_ch);
    h.step(milliseconds(150));
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{rank("Bard_Music_Menacing"), bard, false}, {rank("Bard_Music_Leech"), bard, false}, {rank("Bard_Music_Hopeful"), bard, false}, {rank("Bard_Music_Pensive"), bard, false}, {passive, bard, false}})),
              h.pawn_ch);
    const std::map<int64_t, std::vector<int64_t>> only_flute = {{bard, {kInstrumentFlute}}};
    CHECK(seq(h, from) == (V{"A", "E-", "L"}) && h.s->talents()->purchased() == only_flute && h.s->known_abilities().size() == 57 + 2 && h.mirror.known == h.s->known_abilities(),
          "S8b a respec with a passive: A E- L, no skill update; KnownAbilities are the level's again (the captured 57, Flourish and Fortissimo)");
}
static void test_d1s2_shapes_universal_and_weapon(Harness &h) {
    using V = std::vector<std::string>;
    const auto seq = [](const Harness &hr, size_t from) { return non_stat_seq_since(hr, from); };
    size_t from = 0;
    const tl::Node *un = node_where(h, "Universal", [](const tl::Node &n) { return n.ranks.front().effect != 0 && !n.ranks.front().effect_infinite && n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity == 2; });
    if (!un) {
        CHECK(false, "(a Universal marker rank to buy)");
        return;
    }
    const int64_t uni_rank = un->ranks.front().guid;
    h.step(milliseconds(150));
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{uni_rank, content::kGuidUniversalTree, true}})), h.pawn_ch);
    CHECK(seq(h, from).front() == "E+" && count_tag(sequence_since(h, from), "L") == 1, "S3 a Universal rank: E+ L (U with the flag)");
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{uni_rank, content::kGuidUniversalTree, false}, {uni_rank, content::kGuidUniversalTree, false}})), h.pawn_ch);
    CHECK(h.s->talent_requests_refused() == 1 && h.s->effect_markers_pending() == 1, "(the same rank twice in a call is refused whole: the marker is still on)");
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{uni_rank, content::kGuidUniversalTree, false}})), h.pawn_ch);
    CHECK(seq(h, from) == (V{"E-", "L"}), "a refund inside the marker's second removes its item: E- L");
    h.step(milliseconds(150));
    h.request(purchase_rpc(build_of({{uni_rank, content::kGuidUniversalTree, true}})), h.pawn_ch);
    advance(h, 1200);
    from = h.mirror.sequence.size();
    h.request(purchase_rpc(build_of({{uni_rank, content::kGuidUniversalTree, false}})), h.pawn_ch);
    CHECK(seq(h, from) == V{"L"}, "S8d / S6c a refund in a Universal tree after its marker ended: L alone (no item to remove, no skill update)");
    const tl::Node *w1 = node_where(h, "Weapon_1H_Scepter", [](const tl::Node &n) { return n.ranks.front().effect != 0 && n.ranks.front().costs.size() == 1 && n.ranks.front().costs[0].quantity == 1; });
    if (w1) {
        h.step(milliseconds(150));
        h.request(purchase_rpc(build_of({{w1->ranks.front().guid, tree_guid(h, "Weapon_1H_Scepter"), true}})), h.pawn_ch);
        from = h.mirror.sequence.size();
        h.request(purchase_rpc(build_of({{w1->ranks.front().guid, tree_guid(h, "Weapon_1H_Scepter"), false}})), h.pawn_ch);
        const V s = seq(h, from);
        CHECK(s == (V{"L"}) || s == (V{"E-", "L"}), "a weapon rank refunded: L, and E- first when its item is still on the pawn");
    }
}
static void test_d1s2_shapes() {
    std::printf("plan D1, stage 2: the reply shapes of the retail captures\n");
    svc::Character level25 = bard_at(25);
    level25.experience.push_back({kScepterLine, kLineXpLevel2, 0});
    Harness h(level25);
    const int64_t bard = tree_guid(h, "Bard");
    test_d1s2_shapes_bard(h, bard);
    test_d1s2_shapes_universal_and_weapon(h);
}
static void test_d1s2_stored_garbage() {
    std::printf("plan D1, stage 2: what the record stores is judged by the catalog at the join\n");
    Harness probe(bard_at(3));
    const int64_t bard = tree_guid(probe, "Bard");
    const int64_t menacing = rank_named(probe, "Bard", "Bard_Music_Menacing");
    const int64_t rhetoric = rank_named(probe, "Bard", "Bard_Rhetoric");
    svc::Character r = bard_at(3);
    r.skill_ranks.push_back({bard, {999, menacing, 999, rhetoric, menacing}});
    r.skill_ranks.push_back({424242, {1, 2}});
    svc::ActionbarSlot ok;
    ok.bank = 0;
    ok.slot = 4;
    ok.use_type = 1;
    ok.ability_id = content::kGuidSprint;
    svc::ActionbarSlot far_bank = ok;
    far_bank.bank = 99;
    svc::ActionbarSlot empty = ok;
    empty.slot = 5;
    empty.use_type = 0;
    svc::ActionbarSlot guid = ok;
    guid.slot = 6;
    guid.use_type = 3;
    guid.item_guid = "not 32 digits";
    svc::ActionbarSlot rarity = ok;
    rarity.slot = 7;
    rarity.use_type = 3;
    rarity.item_rarity = 200;
    svc::ActionbarSlot wrapped = rarity;
    wrapped.slot = 8;
    wrapped.item_rarity = 256;
    svc::ActionbarSlot combo = ok;
    combo.slot = 9;
    combo.combo_button = 16;
    r.action_bar = {ok, far_bank, empty, guid, rarity, wrapped, combo};
    Harness h(r);
    CHECK(h.s->talents()->purchased(bard) == std::vector<int64_t>{menacing} && h.s->talents()->purchased().size() == 1, "the join keeps the ranks the catalog knows, once each, in order (Menacing alone)");
    bool noted = false;
    for (const std::string &n : h.s->setup_notes()) noted = noted || n.find("stored rank(s)") != std::string::npos;
    CHECK(noted, "...and says what it left out");
    CHECK(h.s->action_bar().at(0, 4) && h.s->action_bar().size() == 2 && h.s->action_bar().at(0, 6) && !h.s->action_bar().at(0, 5),
          "the bar keeps the slots that are slots: a bank out of range and an empty slot are left out, a bad item guid is a zero guid");
    CHECK(!h.s->action_bar().at(0, 7) && !h.s->action_bar().at(0, 8) && !h.s->action_bar().at(0, 9), "...a rarity of 200, a rarity of 256 (a byte's 0) and a combo button of 16 are left out too (the service would refuse the whole save)");
    CHECK(h.mirror.unreadable == 0 && !h.s->failed() && client_list(h, bard).size() == 3, "the session joins and the client's list of the tree is the granted ranks and Menacing");
}
static const ItemContainer R0 = ItemContainer::Resource0;
static std::vector<svc::InventoryItem> resource_kit() {
    std::vector<svc::InventoryItem> out = bard_items();
    out.push_back(make_item(R0, 0, fr::kOak, 300, 7));
    out.push_back(make_item(R0, 1, fr::kDaffodil, 301, 3));
    out.push_back(make_item(R0, 2, fr::kCoal, 302, 20));
    return out;
}
static void test_resource_join() {
    std::printf("Gathering stage 3: the join opens the resource container with what it holds\n");
    Harness h(record_with(resource_kit()));
    const items::Inventory &inv = h.s->inventory();
    CHECK(inv.grid(R0).cells() == 24 && inv.grid(ItemContainer::Resource1).empty() && inv.count(R0) == 3, "Resource0 is the Sanctus bag's 6 x 4 and holds the three stacks; no other container");
    CHECK(mirror_matches(h), "the client was told every item of every container it holds, the resource ones included");
    const auto oak = h.mirror.storage.find({3, 0});
    CHECK(oak != h.mirror.storage.end() && oak->second.record_id == static_cast<uint64_t>(fr::kOak) && oak->second.quantity == 7, "the oak (x7) sits in the client's Resource0 slot 0");
    const bs::SessionGuids &g = h.s->bootstrap().setup().guids;
    const repl::Subobject *owner = h.s->bootstrap().pawn().find(g.owner_info);
    const repl::Value *rc = owner ? owner->obj.state.get(*owner->obj.schema, "ResourceContainers") : nullptr;
    const auto *arr = rc ? rc->get_if<repl::ArrayValue>() : nullptr;
    bool named = arr && arr->elements.size() == 3;
    if (named) {
        const auto first = arr->elements[0].values.find(1);
        const auto *o = first == arr->elements[0].values.end() ? nullptr : first->second.get_if<emit::FIntrepidNetworkGUID>();
        named = o && *o == g.resource_storage[0] && arr->elements[1].values.empty() && arr->elements[2].values.empty();
    }
    CHECK(named, "OwnerInfo.ResourceContainers names Resource0 and leaves the other two null");
    CHECK(h.mirror.payloads >= 4, "the join sent the Backpack's, the Equipment's, the Quest storage's and Resource0's payloads");
}
static void test_resource_requests_refusals(Harness &h, const bs::SessionGuids &g, const MoveSpec &m, const std::string &coal) {
    const items::Inventory before = h.s->inventory();
    const size_t refused = h.s->item_requests_refused(), payloads = h.mirror.payloads;
    const auto expect_refused = [&](MoveSpec spec, const char *what) {
        const size_t r0 = h.s->item_requests_refused();
        h.request(storage_move(spec), h.pc_ch);
        const bool ok = h.s->item_requests_refused() == r0 + 1 && h.s->inventory() == before;
        if (!ok)
            std::printf("    (%s: refused %zu -> %zu)\n", what, r0, h.s->item_requests_refused());
        return ok;
    };
    MoveSpec onto = m;
    onto.from_slot = 9;
    onto.to_slot = 0;
    CHECK(expect_refused(onto, "onto the oak"), "a Move onto another stack is refused (a swap or a fill)");
    MoveSpec swap = onto;
    swap.type = static_cast<uint8_t>(repl::StorageEventType::Swap);
    swap.target_guid = guid_n(300);
    swap.to_count = 7;
    CHECK(expect_refused(swap, "swap"), "a Swap in the container is refused");
    MoveSpec fill = onto;
    fill.type = static_cast<uint8_t>(repl::StorageEventType::Fill);
    CHECK(expect_refused(fill, "fill"), "a Fill is refused");
    MoveSpec destroy = onto;
    destroy.type = static_cast<uint8_t>(repl::StorageEventType::Remove);
    CHECK(expect_refused(destroy, "destroy"), "a Remove whose slot does not hold the event's item (a destroy of the wrong stack) is refused");
    MoveSpec to_bag = m;
    to_bag.from_slot = 2;
    to_bag.from_count = 20;
    to_bag.source_guid = to_bag.event_guid = coal;
    to_bag.to_object = g.backpack;
    to_bag.to_slot = 30;
    CHECK(expect_refused(to_bag, "to the Backpack"), "a resource stack dragged into the Backpack is refused");
    MoveSpec from_bag = m;
    from_bag.from_object = g.backpack;
    from_bag.from_slot = 1;
    from_bag.from_count = 5;
    from_bag.source_guid = from_bag.event_guid = guid_n(201);
    from_bag.to_object = g.resource_storage[0];
    from_bag.to_slot = 20;
    CHECK(expect_refused(from_bag, "from the Backpack"), "a potion dragged into the resource container is refused");
    MoveSpec third = m;
    third.from_slot = 9;
    third.to_object = g.resource_storage[2];
    third.to_slot = 0;
    CHECK(expect_refused(third, "third container"), "a drag to Resource2, which the character has no bag for, is refused");
    CHECK(h.s->item_requests_refused() == refused + 7, "seven refusals counted");
    CHECK(h.mirror.payloads > payloads && mirror_matches(h), "the client was sent its containers again after the refusals, and agrees");
}
static void test_resource_requests() {
    std::printf("Gathering stage 3: the client's drags inside the resource container\n");
    Harness h(record_with(resource_kit()));
    const bs::SessionGuids g = h.s->bootstrap().setup().guids;
    const std::string daffodil = guid_n(301), coal = guid_n(302);
    MoveSpec m;
    m.from_object = g.resource_storage[0];
    m.to_object = g.resource_storage[0];
    m.from_slot = 1;
    m.from_count = 3;
    m.to_slot = 9;
    m.source_guid = m.event_guid = daffodil;
    const size_t events = h.mirror.events.size();
    h.request(storage_move(m), h.pc_ch);
    CHECK(h.s->inventory().at(R0, 9) && !h.s->inventory().at(R0, 1) && h.s->inventory().at(R0, 9)->quantity == 3 && h.mirror.events.size() == events + 2 && mirror_matches(h),
          "a Move inside Resource0 is made: a Remove and an Add on the container's guid, and the client agrees");
    const bool on_container = h.mirror.events.size() == events + 2 && h.mirror.events[events + 1].targets[0].object == g.resource_storage[0] && h.mirror.events[events + 1].targets[0].slots[0].slot == 9;
    CHECK(on_container, "...the Add names Resource0's guid and slot 9");
    test_resource_requests_refusals(h, g, m, coal);
}
static bool saved_inventory_has_repaired_resources(const RecordingSink::Saved &s) {
    bool oak = false, daffodil = false, none_in_bag = true;
    for (const svc::InventoryItem &it : s.items) {
        oak = oak || (it.container == R0 && it.slot == 0 && it.item_record_id == fr::kOak);
        daffodil = daffodil || (it.container == R0 && it.slot == 1 && it.item_record_id == fr::kDaffodil);
        none_in_bag = none_in_bag && !(it.container == Bag && (it.item_record_id == fr::kOak || it.item_record_id == fr::kDaffodil));
    }
    return oak && daffodil && none_in_bag;
}
static void test_resource_repair() {
    std::printf("Gathering stage 3: the login moves the Backpack's resource items to the container and saves that\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Bag, 4, fr::kOak, 310, 5));
    items.push_back(make_item(Bag, 5, fr::kDaffodil, 311, 3));
    items.push_back(make_item(Bag, 6, fi::kPotion, 312, 2));
    Harness h(record_with(items));
    const items::Inventory &inv = h.s->inventory();
    CHECK(inv.at(R0, 0) && inv.at(R0, 0)->item_record_id == fr::kOak && inv.at(R0, 1) && inv.at(R0, 1)->item_record_id == fr::kDaffodil && !inv.at(Bag, 4) && !inv.at(Bag, 5) && inv.at(Bag, 6),
          "the oak and the daffodils moved from the Backpack to Resource0 (slots 0 and 1); the potion stayed");
    CHECK(mirror_matches(h) && h.mirror.storage.count({3, 0}) == 1 && h.mirror.storage.count({0, 4}) == 0, "the client was told the repaired layout at the join (no resource item in its Backpack)");
    CHECK(h.s->inventory_changed() && h.s->inventory_dirty(), "the session knows the layout differs from the record's and saves it");
    h.s->finish("the test");
    bool saved = false;
    for (const auto &s : h.sink.inventories) saved = saved || saved_inventory_has_repaired_resources(s);
    CHECK(saved, "the inventory handed to the saver has the resource items in Resource0");
}
static void test_resource_no_room_at_login() {
    std::printf("Gathering stage 3: a resource item that finds no room at the login stays in the Backpack\n");
    std::vector<svc::InventoryItem> items = bard_items();
    for (uint32_t i = 0; i < 7; ++i) {
        svc::InventoryItem oak = make_item(Bag, 4 + static_cast<int>(i), fr::kOak, 320 + i, 20);
        oak.quality = static_cast<int32_t>(i + 1);
        items.push_back(oak);
    }
    Harness h(record_with(items));
    const items::Inventory &inv = h.s->inventory();
    CHECK(inv.count(R0) == 6 && inv.at(Bag, 10) && inv.at(Bag, 10)->item_record_id == fr::kOak, "six oaks moved, the seventh stays in the Backpack (nothing is deleted)");
    CHECK(inv.size() == bard_items().size() + 7, "every item is still there");
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    test_d1s2_class_change();
    test_d1s2_relog();
    test_d1s2_weapon_swap();
    test_d1s2_talent_slots();
    test_d1s2_regeneration();
    test_d1s2_shapes();
    test_d1s2_stored_garbage();
    test_resource_join();
    test_resource_requests();
    test_resource_repair();
    test_resource_no_room_at_login();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
