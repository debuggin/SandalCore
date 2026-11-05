// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "capture_leveling.h"
#include "protocol/repl/hit_rpc.h"
#include "test_world_items_session_common.h"
using namespace aoc::worldd;
static repl::DesignDataId adventuring_id() {
    return repl::DesignDataId::named(content::kGuidAdventuring, content::kTypeLevelingTypeDef, "Adventuring");
}
static repl::DesignDataId universal_id() {
    return tree_id(content::kGuidUniversalTree);
}
static size_t count_calls(const std::vector<std::string> &calls, const char *name) {
    size_t n = 0;
    for (const std::string &c : calls) n += c == name ? 1 : 0;
    return n;
}
static void test_d1_join_has_the_tracker() {
    std::printf("plan D1: the join carries the XPTracker\n");
    Harness h(record_with_scrolls());
    CHECK(h.mirror.unreadable == 0 && h.mirror.tracker_updates == 1, "the join's XPTracker chain reads: one tracker update");
    const auto xp = h.mirror.tracker.experience(adventuring_id());
    const auto debt = h.mirror.tracker.experience_debt(adventuring_id());
    CHECK(xp && *xp == 0 && debt && *debt == 0 && !h.mirror.tracker.purchased_ranks(universal_id()), "a new character: Adventuring 0 XP, debt 0, no Universal rank");
    CHECK(h.s->progress() == (leveling::Progress{1, 0, 0}) && h.s->class_id() == 2 && h.s->universal_ranks().empty(), "the session plays level 1, 0 XP, a Bard");
    CHECK(!h.got.empty() && h.got.back().channel == h.pc_ch && h.got.size() == 18, "the XPTracker is the join's last chain, on the PlayerController's channel (18 with this record's two Add chains)");
    CHECK(h.sink.progress.empty() && !h.s->progress_changed(), "nothing is saved at the join");
}
static void count_effect_durations(const Harness &h, size_t &infinite, size_t &markers) {
    infinite = 0;
    markers = 0;
    for (const auto &[id, e] : h.mirror.effect_items) (e.duration > 1e30f ? infinite : markers) += e.duration == 1.0f || e.duration > 1e30f ? 1 : 0;
}
static void test_d1_level_scroll_10_stats_and_effects(Harness &h, const sg::CharacterStats *cs, size_t stat_updates, float max_stamina_before) {
    std::string why;
    CHECK(stats_match(h, &why) && h.mirror.stat_updates.size() == stat_updates + 1, "the stats of level 10 went out in one update and the client's arrays are the session's");
    if (!why.empty())
        std::printf("    %s\n", why.c_str());
    const float max_stamina_after = cs->find(content::kGuidMaxStamina)->value;
    CHECK(max_stamina_after > max_stamina_before && cs->level() == 10 && cs->passives().size() == 1 && cs->passives()[0].mods.size() == 1 && cs->passives()[0].mods[0].value == 10,
          "MaxStamina rose (the level, and Endurance Training I's +10 is among the engine's inputs, as an effect)");
    CHECK(h.mirror.effects_added == 3 && h.mirror.effects_removed == 0, "three CurrentEffects items: the granted passives (Sprint Training's 1 s marker, Endurance Training, Resonant Weapon)");
    size_t infinite = 0, markers = 0;
    count_effect_durations(h, infinite, markers);
    CHECK(infinite == 2 && markers == 1, "two of them infinite (FLT_MAX: the record has no duration), one a 1 s marker (Sprint Training's effect lasts 1 s)");
    CHECK(h.sink.progress.size() == 1 && h.sink.progress[0].second.level == 10 && h.sink.progress[0].second.class_id == 2 && h.sink.progress[0].second.experience.size() == 1 && h.sink.progress[0].second.experience[0].xp == 418410 &&
              h.sink.progress[0].second.experience[0].leveling_type_guid == content::kGuidAdventuring,
          "the progress is handed to the sink at once: level 10, Bard, Adventuring 418,410");
}
static void test_d1_level_scroll_25(Harness &h) {
    const size_t chains2 = h.got.size();
    h.request_both(item_used(3, 5, guid_n(211)), item_activate(3, 5, guid_n(211)));
    CHECK(h.s->progress() == (leveling::Progress{25, 418410 + 14295400 - 418400 + 10, 0}), "level 25, the total raised by curve(25) - curve(10) + 10");
    const auto ranks2 = h.mirror.tracker.purchased_ranks(universal_id());
    CHECK(ranks2 && ranks2->size() == 4 && h.mirror.level == 25 && h.mirror.level_ups == (std::vector<int>{10, 25}),
          "four Universal ranks (Guard Training I at 15 and Sprint Training II at 20 are the new ones), Level 25, a second broadcast");
    CHECK(count_calls(pc_calls_since(h, chains2), "ClientUpdateSkillOnActionbar") == 2 && stats_match(h) && h.sink.progress.size() == 2, "two more skill calls, the stats agree, saved again");
    h.s->finish("end");
    CHECK(h.sink.progress.size() == 3 && h.sink.inventories.size() == 1, "the end hands the progress over once more, and the inventory");
}
static void test_d1_level_scroll() {
    std::printf("plan D1: the level-10 scroll, then the level-25 scroll\n");
    Harness h(record_with_scrolls());
    const sg::CharacterStats *cs = h.s->character_stats();
    const float max_stamina_before = cs->find(content::kGuidMaxStamina)->value;
    const size_t chains = h.got.size();
    const size_t stat_updates = h.mirror.stat_updates.size();
    h.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
    CHECK(h.s->uses_applied() == 1 && h.s->uses_refused() == 0 && h.mirror.unreadable == 0, "the scroll is used: one use applied, everything the server sent reads");
    CHECK(h.s->progress() == (leveling::Progress{10, 418410, 0}) && h.s->levels_gained() == 9, "level 10, 418,410 XP (curve(10) + 10), no debt");
    CHECK(h.s->inventory().at(Bag, 4) == nullptr && mirror_matches(h), "the scroll is gone from bag slot 4, and the client's storages agree");
    const auto xp = h.mirror.tracker.experience(adventuring_id());
    const auto ranks = h.mirror.tracker.purchased_ranks(universal_id());
    CHECK(xp && *xp == 418410 && ranks && ranks->size() == 2 && (*ranks)[0].guid == gm::kSprint1Rank && (*ranks)[1].guid == gm::kStamBoost1Rank && (*ranks)[0].type_id == content::kTypeSkillRank,
          "the client's XPTracker: Adventuring 418,410 and the Universal list with Sprint Training I and Endurance Training I");
    CHECK(h.mirror.level == 10 && h.mirror.level_ups == (std::vector<int>{10}), "the pawn's Level is 10 and BroadcastPlayLevelUpEffects(Adventuring, 10) was sent");
    CHECK(h.s->universal_ranks() == (std::vector<int64_t>{gm::kSprint1Rank, gm::kStamBoost1Rank}), "the session holds the two ranks");
    const auto calls = pc_calls_since(h, chains);
    CHECK(count_calls(calls, "ClientUpdateSkillOnActionbar") == 5 && count_calls(calls, "ClientReceiveStorageEvent") == 1,
          "five ClientUpdateSkillOnActionbar calls (Sprint Training I and Endurance Training I of the Universal tree, Flourish, Resonant Weapon and Fortissimo of the Bard's) and the one storage event (the Remove of the scroll)");
    CHECK(h.mirror.events.size() == 25 + 1 && h.mirror.events.back().type == static_cast<uint8_t>(repl::StorageEventType::Remove), "the event is a Remove");
    test_d1_level_scroll_10_stats_and_effects(h, cs, stat_updates, max_stamina_before);
    test_d1_level_scroll_25(h);
}
static void test_d1_class_scroll_tank_and_back(Harness &h, float strength_bard) {
    const sg::CharacterStats *cs = h.s->character_stats();
    const size_t stat_updates = h.mirror.stat_updates.size();
    h.request_both(item_used(3, 6, guid_n(212)), item_activate(3, 6, guid_n(212)));
    CHECK(h.s->uses_applied() == 1 && h.s->class_id() == 7 && cs->class_enum() == 7 && h.s->inventory().at(Bag, 6) == nullptr, "the Tank scroll: the class is Tank (7), the scroll consumed");
    CHECK(h.mirror.stat_updates.size() == stat_updates + 1 && stats_match(h, nullptr, true) && cs->find(109183598614138ll)->value != strength_bard, "the stats are a Tank's now (Strength differs), and the client has them");
    CHECK(h.s->bootstrap().setup().class_record_guid == 108640201692798ll, "CombatInfo.PrimaryClassId is the Tank's record");
    CHECK(h.s->progress() == (leveling::Progress{1, 0, 0}) && h.mirror.level_ups.empty() && h.mirror.level == 1, "no XP, no level-up");
    CHECK(h.sink.progress.size() == 1 && h.sink.progress[0].second.class_id == 7 && h.sink.progress[0].second.level == 1, "the progress is saved with class 7");
    h.request_both(item_used(3, 7, guid_n(213)), item_activate(3, 7, guid_n(213)));
    CHECK(h.s->class_id() == 2 && cs->find(109183598614138ll)->value == strength_bard && stats_match(h, nullptr, true), "the Bard scroll now works: a Bard again, the stats back (the Tank's own resource keeps its item on the client)");
}
static void test_d1_class_scroll() {
    std::printf("plan D1: the class scrolls\n");
    Harness h(record_with_scrolls());
    const sg::CharacterStats *cs = h.s->character_stats();
    const float strength_bard = cs->find(109183598614138ll)->value;
    h.request_both(item_used(3, 7, guid_n(213)), item_activate(3, 7, guid_n(213)));
    CHECK(h.s->uses_refused() == 1 && h.s->uses_applied() == 0 && h.s->inventory().at(Bag, 7) && h.s->class_id() == 2 && mirror_matches(h), "the Bard scroll for a Bard is refused, the scroll stays");
    test_d1_class_scroll_tank_and_back(h, strength_bard);
}
static void test_d1_consumables_and_keep_scrolls(Harness &h) {
    h.request_both(item_used(3, 9, guid_n(215)), item_activate(3, 9, guid_n(215)));
    const svc::InventoryItem *potions = h.s->inventory().at(Bag, 9);
    CHECK(h.s->uses_applied() == 2 && potions && potions->item_record_id == fi::kPotion && potions->quantity == 20 && mirror_matches(h), "the consumables scroll: a stack of 20 potions where it was");
    h.request_both(item_used(3, 11, guid_n(217)), item_activate(3, 11, guid_n(217)));
    const svc::InventoryItem *keep = h.s->inventory().at(Bag, 11);
    CHECK(h.s->uses_applied() == 3 && keep && keep->item_record_id == gm::kKeep && keep->instance_guid == guid_n(217) && h.s->inventory().count(Bag) == 14, "the keep scroll stays, its potions arrive");
    h.s->finish("end");
    CHECK(h.sink.inventories.size() == 1 && h.sink.inventories[0].items.size() == h.s->inventory().size(), "the end saves the inventory with the new items");
}
static void test_d1_gear_and_consumables() {
    std::printf("plan D1: the gear and consumables scrolls\n");
    Harness h(record_with_scrolls());
    const size_t chains = h.got.size();
    const size_t stat_updates = h.mirror.stat_updates.size();
    h.request_both(item_used(3, 8, guid_n(214)), item_activate(3, 8, guid_n(214)));
    const svc::InventoryItem *chest = h.s->inventory().at(Bag, 8);
    const svc::InventoryItem *legs = h.s->inventory().at(Bag, 12);
    CHECK(h.s->uses_applied() == 1 && chest && chest->item_record_id != gm::kGear01 && h.s->inventory().count(Bag) == 13, "the gear scroll is consumed: 12 bag items became 13");
    CHECK(chest && chest->item_record_id == fi::kChest && chest->durability == 11 && legs && legs->item_record_id == fi::kLegs,
          "the chest took the scroll's slot (the lowest free) and the legs the next free slot, with the record's durability");
    CHECK(mirror_matches(h) && count_calls(pc_calls_since(h, chains), "ClientReceiveStorageEvent") == 3, "a Remove and two Adds; the client's bag agrees");
    CHECK(chest->instance_guid != legs->instance_guid && chest->instance_guid.size() == 32, "fresh instances with their own guids");
    CHECK(h.s->progress() == (leveling::Progress{1, 0, 0}) && h.sink.progress.empty() && h.mirror.stat_updates.size() == stat_updates, "no XP, no save of progress, no stats");
    test_d1_consumables_and_keep_scrolls(h);
}
static items::Inventory test_d1_refusals_basic() {
    Harness h(record_with_scrolls());
    const items::Inventory before = h.s->inventory();
    const auto progress = h.s->progress();
    size_t refused = 0;
    const auto refusal = [&](const char *what, const BunchWriter &req) {
        const size_t chains = h.got.size();
        h.request_both(req, req);
        ++refused;
        CHECK(h.s->uses_refused() + h.s->item_requests_refused() >= refused && h.s->uses_applied() == 0 && h.s->inventory() == before && h.s->progress() == progress && h.sink.progress.empty() && h.mirror.level_ups.empty(), what);
        (void)chains;
    };
    refusal("an action of another type (Callout): refused", item_used(3, 10, guid_n(216)));
    refusal("a stale instance (another guid in the slot): refused", item_used(3, 4, guid_n(999)));
    refusal("an empty slot: refused", item_used(3, 20, guid_n(210)));
    refusal("a slot past the bag: refused", item_used(3, 70, guid_n(210)));
    CHECK(h.mirror.tracker_updates == 1, "no XPTracker update went out");
    return before;
}
static void test_d1_refusals_edge_cases(const items::Inventory &before) {
    {
        svc::Character r = record_with_scrolls();
        r.level = 12;
        Harness at12(r);
        at12.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
        CHECK(at12.s->uses_refused() == 1 && at12.s->inventory().at(Bag, 4) && at12.s->progress().level == 12, "the level-10 scroll at level 12 is refused and stays");
        CHECK(g_log.str().find("below level 10") != std::string::npos, "the log says why");
    }
    {
        Harness g(record_with_scrolls());
        g.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
        const auto p = g.s->progress();
        g.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
        CHECK(g.s->uses_applied() == 1 && g.s->progress() == p && g.s->item_requests_refused() == 1, "the scroll used up: the slot is empty, the second use is refused");
    }
    {
        std::vector<svc::InventoryItem> items = bard_items_with_scrolls();
        for (int slot = 12; slot < 70; ++slot) items.push_back(make_item(Bag, slot, fi::kPotion, 300 + static_cast<uint32_t>(slot)));
        Harness full(record_with(items));
        full.request_both(item_used(3, 8, guid_n(214)), item_activate(3, 8, guid_n(214)));
        CHECK(full.s->uses_refused() == 1 && full.s->inventory().at(Bag, 8) && full.s->inventory().at(Bag, 8)->item_record_id == gm::kGear01, "a full bag: the gear scroll is refused and stays");
        full.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
        CHECK(full.s->uses_applied() == 1 && full.s->progress().level == 10, "a level scroll needs no room: used");
    }
    {
        Harness spam(record_with_scrolls());
        std::vector<fake::Bunch> bunches;
        for (int i = 0; i < 12; ++i) bunches.push_back(spam.client.actor(spam.pawn_ch, item_used(3, 10, guid_n(216)), true));
        spam.client_sends(std::move(bunches));
        spam.settle();
        CHECK(spam.s->requests_dropped() >= 4 && spam.s->uses_refused() <= PlayerSession::kItemBurst && !spam.s->failed() && spam.s->inventory() == before, "12 uses at once: at most the burst are judged, the rest dropped, nothing changes");
    }
}
static void test_d1_refusals() {
    std::printf("plan D1: the refusals change nothing and send no XPTracker or level-up\n");
    const items::Inventory before = test_d1_refusals_basic();
    test_d1_refusals_edge_cases(before);
}
static void test_d1_relog() {
    std::printf("plan D1: a record with a level, XP and a class carries them to the join\n");
    svc::Character r = record_with_scrolls();
    r.level = 10;
    r.experience.push_back({content::kGuidAdventuring, 500000, 7});
    Harness h(r);
    CHECK(h.s->progress() == (leveling::Progress{10, 500000, 7}) && h.s->universal_ranks().size() == 2, "level 10 with its XP row: 500,000 XP, debt 7, two ranks granted");
    const auto xp = h.mirror.tracker.experience(adventuring_id());
    const auto debt = h.mirror.tracker.experience_debt(adventuring_id());
    const auto ranks = h.mirror.tracker.purchased_ranks(universal_id());
    CHECK(xp && *xp == 500000 && debt && *debt == 7 && ranks && ranks->size() == 2, "the join's XPTracker says so, the Universal list included");
    const repl::Subobject *base_info = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.base_character_info);
    const repl::Value *level_value = base_info ? base_info->obj.state.get(*base_info->obj.schema, "Level") : nullptr;
    const int64_t *pawn_level = level_value ? level_value->get_if<int64_t>() : nullptr;
    CHECK(pawn_level && *pawn_level == 10 && h.s->character_stats()->level() == 10 && h.s->character_stats()->passives().size() == 1, "the pawn's Level is 10 and the stats were computed with Endurance Training I");
    svc::Character t = record_with_scrolls();
    t.level = 5;
    t.experience.push_back({content::kGuidAdventuring, 100, 0});
    Harness h2(t);
    CHECK(h2.s->progress() == (leveling::Progress{5, 29900, 0}) && h2.s->universal_ranks() == (std::vector<int64_t>{gm::kSprint1Rank}), "a record with 100 XP at level 5 joins at level 5's threshold, Sprint Training I granted");
    svc::Character tk = record_with_scrolls();
    tk.class_id = 7;
    tk.class_name = "Tank";
    tk.archetype_id = 17754;
    Harness h3(tk);
    CHECK(h3.s->class_id() == 7 && h3.s->character_stats()->class_enum() == 7, "a Tank record plays as a Tank");
}
static void test_d1_saver_progress() {
    std::printf("plan D1: the progress reaches the character service through the saver\n");
    FakeCharacters api;
    {
        CharacterSaver saver(api, quick());
        Fixture fx;
        fx.world->set_position_sink(&saver);
        Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
        std::string error;
        auto s = PlayerSession::join(conn, *fx.world, record_with_scrolls(), identity(), 0, error);
        CHECK(s != nullptr, "(joined)");
        if (!s)
            return;
        svc::Progress p;
        p.level = 10;
        p.class_id = 2;
        p.experience.push_back({content::kGuidAdventuring, 418410, 0});
        saver.save_progress(kCyb, p);
        CHECK(wait_until([&] { return api.progress_count() == 1; }) && api.progress[0].first == kCyb && api.progress[0].second == p, "a progress handed to the saver reaches SaveProgress whole");
        CHECK(saver.progress_saved() == 1, "counted as saved");
    }
}
static const char *professions_db_path() {
    const char *env = std::getenv("AOC_CONTENT_DB");
    if (env && *env)
        return env;
#ifdef PROFESSIONS_REAL_DB
    if (fs::exists(PROFESSIONS_REAL_DB))
        return PROFESSIONS_REAL_DB;
#endif
    return nullptr;
}
namespace g2 {
inline constexpr int64_t kMining25 = 6064632989080814323ll, kMining9 = 6064634615541072001ll, kAlchemy9 = 6064634615657267369ll, kAll25 = 6064632988513402885ll;
inline constexpr int64_t kT3Pickaxe = 6064629376467927053ll;
inline constexpr int64_t kMining = 108752769996425ll, kAlchemy = 108752757020295ll;
inline constexpr int64_t kLtMining = 109460719621753ll, kLtAlchemy = 109460747540106ll;
inline constexpr int64_t kNovice = 108906339653240ll, kApprentice = 108906340767353ll, kJourneyman = 108906342012538ll;
} // namespace g2
static repl::DesignDataId leveling_id(int64_t leveling_type) {
    repl::DesignDataId d;
    d.guid = leveling_type;
    d.type_id = content::kTypeLevelingTypeDef;
    return d;
}
static svc::Character record_with_artisan_scrolls(const std::vector<int64_t> &scrolls) {
    svc::Character c = cyb_record();
    std::vector<svc::InventoryItem> items;
    for (const auto &it : c.items)
        if (it.container == Worn)
            items.push_back(it);
    for (size_t i = 0; i < scrolls.size(); ++i) items.push_back(make_item(Bag, 4 + static_cast<int>(i), scrolls[i], 300 + static_cast<uint32_t>(i)));
    c.items = std::move(items);
    return c;
}
static std::string profession_name(const professions::Book &book, int64_t leveling_type) {
    for (const professions::Profession &p : book.all())
        if (p.leveling_type == leveling_type)
            return p.name;
    return "?";
}
static void verify_g2_join_certifications(const Harness &h) {
    const auto certs = h.mirror.tracker.certifications();
    bool order = certs.size() == std::size(capture_leveling::kJoinCertifications), novice = true;
    for (size_t i = 0; order && i < certs.size(); ++i) {
        order = certs[i].key.guid == capture_leveling::kJoinCertifications[i].key && certs[i].key.type_id == content::kTypeLevelingTypeDef;
        novice = novice && certs[i].value == g2::kNovice;
    }
    CHECK(order && novice && certs.size() == 22, "the client's LevelingCertificationsFastMap has the 22 professions' leveling types in the retail join's order, each Novice");
}
static void test_g2_join_new_character(const char *path) {
    Harness h(record_with_artisan_scrolls({}), true, true, path);
    const professions::Book *book = h.fx.world->professions();
    CHECK(book && book->size() == 22 && h.s->professions().valid(), "the world and the session have the 22 professions");
    verify_g2_join_certifications(h);
    const auto xp = h.mirror.tracker.experience();
    CHECK(xp.size() == 1 && xp[0].key.guid == content::kGuidAdventuring, "the ExperienceFastMap has Adventuring only: a profession with no XP is not listed");
    bool all_novice = true;
    for (const professions::Profession &p : book->all()) all_novice = all_novice && h.s->professions().state(p.guid) == professions::State{};
    CHECK(all_novice, "every profession is Novice, level 1, no XP");
    CHECK(h.sink.progress.empty() && !h.s->progress_changed(), "nothing is saved at the join");
    CHECK(h.mirror.unreadable == 0 && h.mirror.tracker_updates == 1, "the join's XPTracker is still one chain");
}
static svc::ProfessionState make_retail_profession_state(const professions::Book &book, const professions::Profession &p) {
    int64_t cert = g2::kNovice, xp = 0;
    for (const auto &e : capture_leveling::kJoinCertifications) {
        if (e.key == p.leveling_type) {
            cert = e.value;
        }
    }
    for (const auto &e : capture_leveling::kJoinExperience) {
        if (e.key == p.leveling_type) {
            xp = e.value;
        }
    }
    const int tier = book.tier_of_guid(cert);
    return {p.guid, tier, book.level_of(p, xp, tier), xp};
}
static void populate_retail_professions(const professions::Book &book, svc::Character &c) {
    for (const professions::Profession &p : book.all()) {
        c.professions.push_back(make_retail_profession_state(book, p));
    }
}
static void test_g2_join_retail_character(const char *path) {
    Harness probe(record_with_artisan_scrolls({}), true, true, path);
    const professions::Book &book = *probe.fx.world->professions();
    svc::Character c = record_with_artisan_scrolls({});
    populate_retail_professions(book, c);
    Harness h(c, true, true, path);
    const auto certs = h.mirror.tracker.certifications();
    bool same = certs.size() == std::size(capture_leveling::kJoinCertifications);
    for (size_t i = 0; same && i < certs.size(); ++i) same = certs[i].key.guid == capture_leveling::kJoinCertifications[i].key && certs[i].value == capture_leveling::kJoinCertifications[i].value;
    CHECK(same, "the join's LevelingCertificationsFastMap equals the retail join's: the same 22 keys in the same order, the same certification levels");
    std::map<int64_t, int64_t> want, got;
    for (const auto &e : capture_leveling::kJoinExperience)
        if (profession_name(book, e.key) != "?")
            want[e.key] = e.value;
    for (const auto &e : h.mirror.tracker.experience())
        if (profession_name(book, e.key.guid) != "?")
            got[e.key.guid] = e.value;
    CHECK(want.size() == 19 && got == want, "the ExperienceFastMap lists the same 19 professions with the same XP (the 3 with none are not listed)");
    const professions::Professions &p = h.s->professions();
    CHECK(p.level(108752769996425ll) == 19 && p.tier(108752769996425ll) == 1 && p.level(108752769996426ll) == 21 && p.tier(108752769996426ll) == 2 && p.level(108752770061964ll) == 18,
          "Mining is level 19 Apprentice, Lumberjacking 21 Journeyman, Fishing 18 Apprentice (the curve and the tiers fit the retail state)");
}
static void test_g2_join() {
    const char *path = professions_db_path();
    if (!path) {
        std::printf("Gathering stage 2: the join's professions: skipped (no real content.db)\n");
        return;
    }
    std::printf("Gathering stage 2: the join lists the professions\n");
    test_g2_join_new_character(path);
    test_g2_join_retail_character(path);
}
static void verify_g2_alchemy_saved_rows(const Harness &h) {
    CHECK(h.sink.progress.size() == 1 && h.sink.progress[0].second.professions.size() == 22, "the progress is saved with all 22 profession rows");
    {
        const auto &rows = h.sink.progress[0].second.professions;
        const auto it = std::find_if(rows.begin(), rows.end(), [](const svc::ProfessionState &r) { return r.profession_guid == g2::kAlchemy; });
        CHECK(it != rows.end() && it->certification_tier == 0 && it->level == 9 && it->xp == 32401 && rows[0].profession_guid < rows[21].profession_guid, "Alchemy's row is Novice, level 9, 32,401 XP; the rows are in guid order");
    }
}
static void test_g2_scrolls_alchemy(Harness &h) {
    size_t chains = h.got.size();
    const size_t stat_updates0 = h.mirror.stat_updates.size();
    h.request_both(item_used(3, 6, guid_n(302)), item_activate(3, 6, guid_n(302)));
    CHECK(h.s->uses_applied() == 1 && h.s->uses_refused() == 0 && h.mirror.unreadable == 0 && !h.s->failed(), "the Alchemy Level 9 scroll is used");
    CHECK(h.s->professions().state(g2::kAlchemy) == (professions::State{0, 9, 32401}), "Alchemy is Novice level 9 with 32,401 XP (the XP from the start of level 1 to level 9, plus 1)");
    CHECK(h.s->inventory().at(Bag, 6) == nullptr && mirror_matches(h), "the scroll is consumed");
    const auto alchemy_xp = h.mirror.tracker.experience(leveling_id(g2::kLtAlchemy));
    const auto alchemy_cert = h.mirror.tracker.certification(leveling_id(g2::kLtAlchemy));
    CHECK(alchemy_xp && *alchemy_xp == 32401 && alchemy_cert && *alchemy_cert == g2::kNovice, "the client's XPTracker: Alchemy 32,401 XP, Novice");
    const auto adv = h.mirror.tracker.experience(adventuring_id());
    CHECK(adv && *adv == 0 && h.s->progress() == (leveling::Progress{1, 0, 0}) && h.mirror.level == 1, "Adventuring is untouched, the pawn's Level is still 1");
    CHECK(h.mirror.level_ups == (std::vector<int>{9}) && h.mirror.level_up_categories == (std::vector<int64_t>{g2::kLtAlchemy}), "BroadcastPlayLevelUpEffects(Alchemy's leveling type, 9) was sent");
    CHECK(count_calls(pc_calls_since(h, chains), "ClientUpdateSkillOnActionbar") == 0 && h.mirror.stat_updates.size() == stat_updates0, "no skill, no stat changed");
    verify_g2_alchemy_saved_rows(h);
}
static void test_g2_scrolls_tool_and_refusal(Harness &h, int pickaxe_slot, const std::string &pickaxe_guid) {
    if (pickaxe_slot >= 0) {
        h.step(milliseconds(150));
        h.request_both(item_used(3, pickaxe_slot, pickaxe_guid), item_activate(3, pickaxe_slot, pickaxe_guid));
        CHECK(h.s->inventory().at(Worn, 26) && h.s->inventory().at(Worn, 26)->item_record_id == g2::kT3Pickaxe && mirror_matches(h), "the T3 pickaxe is worn in the Pickaxe slot 26, the client's equipment agrees");
    }
    const size_t refused = h.s->uses_refused(), applied = h.s->uses_applied();
    h.request_both(item_used(3, 5, guid_n(301)), item_activate(3, 5, guid_n(301)));
    CHECK(h.s->uses_refused() == refused + 1 && h.s->uses_applied() == applied && h.s->inventory().at(Bag, 5) && h.s->inventory().at(Bag, 5)->item_record_id == g2::kMining9, "a Level 9 scroll for a level 25 miner is refused and stays");
}
static void test_g2_scrolls_mining_and_tool(Harness &h) {
    size_t chains = h.got.size();
    h.request_both(item_used(3, 4, guid_n(300)), item_activate(3, 4, guid_n(300)));
    CHECK(h.s->uses_applied() == 2 && h.s->professions().state(g2::kMining) == (professions::State{2, 25, 266400}), "the Mining Level 25 scroll: Journeyman, level 25, 266,400 XP");
    const auto mining_cert = h.mirror.tracker.certification(leveling_id(g2::kLtMining));
    const auto mining_xp = h.mirror.tracker.experience(leveling_id(g2::kLtMining));
    CHECK(mining_cert && *mining_cert == g2::kJourneyman && mining_xp && *mining_xp == 266400, "the client's XPTracker: Mining Journeyman, 266,400 XP");
    CHECK(h.mirror.level_up_categories == (std::vector<int64_t>{g2::kLtAlchemy, g2::kLtMining}) && h.mirror.level_ups == (std::vector<int>{9, 25}), "a second broadcast: Mining's leveling type, level 25");
    int pickaxe_slot = -1;
    std::string pickaxe_guid;
    for (const svc::InventoryItem &it : h.s->inventory().items()) {
        if (it.container == Bag && it.item_record_id == g2::kT3Pickaxe) {
            pickaxe_slot = it.slot;
            pickaxe_guid = it.instance_guid;
        }
    }
    CHECK(pickaxe_slot >= 0 && h.s->inventory().at(Bag, 4) == nullptr && mirror_matches(h), "the T3 pickaxe arrived in the bag, the scroll is gone, and the client's bag agrees");
    CHECK(count_calls(pc_calls_since(h, chains), "ClientReceiveStorageEvent") == 2, "two storage events: the scroll's Remove and the pickaxe's Add");
    CHECK(h.s->character_stats()->profession_levels().count("Mining") == 1 && h.s->character_stats()->profession_levels().at("Mining") == 25 && h.s->character_stats()->profession_levels().at("Alchemy") == 9,
          "the stat engine answers GetPlayerProfessionLevel with the real levels");
    CHECK(h.sink.progress.size() == 2, "saved again");
    test_g2_scrolls_tool_and_refusal(h, pickaxe_slot, pickaxe_guid);
}
static void test_g2_scrolls_relog(Harness &h, const professions::Book &book, const char *path) {
    h.s->finish("end");
    CHECK(h.sink.progress.size() >= 3, "(saved)");
    svc::Character relog = record_with_artisan_scrolls({});
    relog.professions = h.sink.progress.back().second.professions;
    relog.experience = h.sink.progress.back().second.experience;
    Harness again(relog, true, true, path);
    bool same = again.s->professions().valid();
    for (const professions::Profession &p : book.all()) same = same && again.s->professions().state(p.guid) == h.s->professions().state(p.guid);
    CHECK(same, "a relog from the saved rows plays the same 22 professions");
    const auto recerts = again.mirror.tracker.certifications();
    bool rejourney = recerts.size() == 22;
    for (const auto &c : recerts) rejourney = rejourney && c.value == g2::kJourneyman;
    const auto re_xp = again.mirror.tracker.experience(leveling_id(g2::kLtMining));
    CHECK(rejourney && re_xp && *re_xp == 266400 && again.mirror.tracker.experience().size() == 23, "and the join lists them: Journeyman everywhere, Mining 266,400 XP");
}
static void test_g2_scrolls_all_and_relog(Harness &h, const professions::Book &book, const char *path) {
    const size_t broadcasts = h.mirror.level_ups.size();
    h.request_both(item_used(3, 7, guid_n(303)), item_activate(3, 7, guid_n(303)));
    bool every = true;
    for (const professions::Profession &p : book.all()) {
        const professions::State s = h.s->professions().state(p.guid);
        every = every && s.level == 25 && s.tier == 2;
    }
    CHECK(every && h.s->professions().state(g2::kMining) == (professions::State{2, 25, 266400}), "_All: all 22 professions are level 25 and Journeyman (Mining, already there, is left as it was)");
    const auto certs = h.mirror.tracker.certifications();
    bool journeymen = certs.size() == 22;
    for (const auto &c : certs) journeymen = journeymen && c.value == g2::kJourneyman;
    CHECK(journeymen && h.mirror.tracker.experience().size() == 23, "the client's maps: 22 Journeyman certifications, 22 professions with XP (and Adventuring)");
    CHECK(h.mirror.level_ups.size() == broadcasts + 21, "21 level-up broadcasts (every profession but Mining gained levels)");
    CHECK(h.mirror.unreadable == 0 && !h.s->failed() && h.s->inventory().at(Bag, 7) == nullptr, "everything the server sent reads; the scroll is consumed");
    test_g2_scrolls_relog(h, book, path);
}
static void test_g2_scrolls() {
    const char *path = professions_db_path();
    if (!path) {
        std::printf("Gathering stage 2: the scrolls: skipped (no real content.db)\n");
        return;
    }
    std::printf("Gathering stage 2: the GM artisan scrolls, the XPTracker, the level-ups, the saves, the tool and a relog\n");
    Harness h(record_with_artisan_scrolls({g2::kMining25, g2::kMining9, g2::kAlchemy9, g2::kAll25}), true, true, path);
    const professions::Book &book = *h.fx.world->professions();
    CHECK(h.mirror.unreadable == 0 && h.s->professions().valid(), "(joined)");
    test_g2_scrolls_alchemy(h);
    test_g2_scrolls_mining_and_tool(h);
    test_g2_scrolls_all_and_relog(h, book, path);
}
static void test_g2_saver_round_trip() {
    const char *path = professions_db_path();
    if (!path) {
        std::printf("Gathering stage 2: the saver round trip: skipped (no real content.db)\n");
        return;
    }
    std::printf("Gathering stage 2: the professions reach the character service through the saver and come back\n");
    Harness h(record_with_artisan_scrolls({g2::kMining25}), true, true, path);
    h.request_both(item_used(3, 4, guid_n(300)), item_activate(3, 4, guid_n(300)));
    CHECK(h.s->professions().state(g2::kMining) == (professions::State{2, 25, 266400}) && !h.sink.progress.empty(), "the scroll is used and the progress is handed over");
    FakeCharacters api;
    svc::Progress saved;
    {
        CharacterSaver saver(api, quick());
        saver.save_progress(kCyb, h.sink.progress.back().second);
        CHECK(wait_until([&] { return api.progress_count() == 1; }), "the saver gives it to SaveProgress");
        CHECK(saver.progress_saved() == 1, "counted as saved");
    }
    {
        std::lock_guard<std::mutex> l(api.mu);
        if (!api.progress.empty())
            saved = api.progress.back().second;
    }
    CHECK(saved.professions.size() == 22 && saved == h.sink.progress.back().second, "the character service got all 22 profession rows, the progress whole");
    svc::Character relog = record_with_artisan_scrolls({});
    relog.professions = saved.professions;
    Harness again(relog, true, true, path);
    CHECK(again.s->professions().state(g2::kMining) == (professions::State{2, 25, 266400}), "a relog from the character service's rows: Mining Journeyman, level 25, 266,400 XP");
    const auto cert = again.mirror.tracker.certification(leveling_id(g2::kLtMining));
    CHECK(cert && *cert == g2::kJourneyman, "the join's XPTracker says so");
}
namespace gc {
inline constexpr int64_t kProcessingXpScroll = 6064634041989399767ll;
inline constexpr int64_t kProcessingXpAbility = 6064634041996149977ll, kProcessingXpHit = 6064634041997853914ll, kProcessingXpEffect = 6064634041998836955ll;
inline constexpr int64_t kConstitutionScroll = 6064630417664311553ll;
inline constexpr int64_t kConstitutionAbility = 6064630560632275139ll, kConstitutionHit = 6064630560632275140ll;
inline constexpr int64_t kHerbalismElixir = 6064632105782280214ll;
inline constexpr int64_t kHerbalismAbility = 6064632320007733313ll, kHerbalismHit = 6064632320007733314ll, kHerbalismEffect = 6064632320007733315ll;
inline constexpr int64_t kProcessingXpRating = 6064629302956655088ll, kHerbalismQuantityRating = 6064629302956589530ll, kConstitution = 109183598614140ll;
inline constexpr int64_t kSelfTargeting = 6064628968549646338ll;
inline constexpr int64_t kRoastedChicken = 6064630417660903452ll, kRoastedChickenAbility = 6064632320235340026ll;
}  // namespace gc
static BunchWriter trigger_bag_item(const emit::FIntrepidNetworkGUID &ability, int64_t guid, int32_t id, int slot, const repl::ItemInstance &item) {
    const schema::ClassSchema &cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema *rpc = cls.rpc("ServerTriggerAbility");
    BunchWriter out, field;
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0})) && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(slot));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(item)) && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(3));
    bool failed = false;
    if (!ok || repl::write_rpc_field(cls, *rpc, p, field, client_options(), &failed) == 0 || failed) return out;
    repl::write_content_block(out, repl::BlockHeader{false, false, ability}, field, false);
    return out;
}
static BunchWriter hit_request_for(const bs::PlayerSetup &setup, int64_t ability, int64_t targeting, int64_t hit) {
    const schema::ClassSchema &cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema *rpc = cls.rpc("RequestHitVerification_Server");
    BunchWriter out, field;
    repl::HitRequest req;
    repl::HitComponentRef c;
    c.object = setup.guids.collision_cylinder;
    c.server_timestamp = 1.0f;
    req.components.push_back(c);
    req.ability_guid = ability;
    req.targeting_guid = targeting;
    req.hit_guid = hit;
    const auto params = rpc ? repl::hit_request_params(*rpc, req) : std::nullopt;
    bool failed = false;
    if (!params || repl::write_rpc_field(cls, *rpc, *params, field, client_options(), &failed) == 0 || failed) return out;
    repl::write_content_block(out, repl::BlockHeader{false, false, setup.guids.ability}, field, false);
    return out;
}
static const Mirror::EffectSeen *effect_of(const Harness &h, int64_t guid) {
    for (const auto &[id, e] : h.mirror.effect_items) {
        if (e.guid == guid) return &e;
    }
    return nullptr;
}
struct UseConsumableSpec {
    int slot;
    int64_t ability;
    int64_t hit;
    uint32_t guid;
    int32_t id;
    int hit_after_steps;
};
static bool use_consumable(Harness &h, const UseConsumableSpec &u) {
    const svc::InventoryItem *item = h.s->inventory().at(Bag, u.slot);
    if (!item) return false;
    const bs::PlayerSetup &setup = h.s->bootstrap().setup();
    std::vector<fake::Bunch> sent;
    sent.push_back(h.client.actor(h.pawn_ch, item_used(3, u.slot, guid_n(u.guid)), true));
    sent.push_back(h.client.actor(h.pawn_ch, trigger_bag_item(setup.guids.ability, u.ability, u.id, u.slot, items::to_instance(*item)), true));
    if (u.hit_after_steps == 0) sent.push_back(h.client.actor(h.pawn_ch, hit_request_for(setup, u.ability, gc::kSelfTargeting, u.hit), true));
    h.client_sends(std::move(sent));
    for (int i = 0; i < 10; ++i) h.step();
    if (u.hit_after_steps > 0) h.client_sends({h.client.actor(h.pawn_ch, hit_request_for(setup, u.ability, gc::kSelfTargeting, u.hit), true)});
    for (int i = 0; i < 15; ++i) h.step();
    return true;
}
static void check_scroll_and_elixir_buffs(Harness &h) {
    const sg::CharacterStats *cs = h.s->character_stats();
    const float xp_before = cs->find(gc::kProcessingXpRating)->value;
    const size_t chains = h.got.size();
    CHECK(use_consumable(h, {4, gc::kProcessingXpAbility, gc::kProcessingXpHit, 310, 21, 0}) && h.mirror.unreadable == 0 && !h.s->failed(), "the Processing XP scroll is used (ServerItemUsed, the trigger and the hit request in one datagram)");
    CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->quantity == 4 && h.s->uses_applied() == 1 && h.s->uses_refused() == 0, "one scroll of the stack of five is taken, no refusal");
    const size_t storage_calls = count_calls(pc_calls_since(h, chains), "ClientReceiveStorageEvent");
    CHECK(storage_calls == 1, "the client is told with one storage event");
    CHECK(!h.mirror.events.empty() && h.mirror.events.back().type == static_cast<uint8_t>(repl::StorageEventType::Remove), "... a Remove");
    CHECK(mirror_matches(h), "... and the client's bag agrees with the session's");
    if (storage_calls != 1) std::printf("    %zu storage event call(s) since the use\n", storage_calls);
    const Mirror::EffectSeen *e1 = effect_of(h, gc::kProcessingXpEffect);
    CHECK(e1 && e1->duration == 300.0f, "the buff is on the pawn for 300 s");
    CHECK(cs->find(gc::kProcessingXpRating)->value == xp_before + 370.0f, "Artisan_Processing_XP_Rating rose by the scroll's own 370 (the item's stat block at its quality)");
    CHECK(stats_match(h), "the client's stat arrays are the session's");
    const float con_before = cs->find(gc::kConstitution)->value;
    CHECK(use_consumable(h, {5, gc::kConstitutionAbility, gc::kConstitutionHit, 311, 22, 0}), "the Constitution scroll is used");
    CHECK(h.s->inventory().at(Bag, 5) == nullptr && mirror_matches(h), "the scroll is gone from its slot");
    CHECK(effect_of(h, gc::kProcessingXpEffect) == nullptr, "the first scroll's buff is gone: one scroll at a time");
    CHECK(cs->find(gc::kProcessingXpRating)->value == xp_before && cs->find(gc::kConstitution)->value == con_before + 26.0f, "the XP rating is back, Constitution rose by the scroll's 26");
    bool long_buff = false;
    for (const auto &[id, e] : h.mirror.effect_items) long_buff = long_buff || e.duration == 10800.0f;
    CHECK(long_buff, "the Constitution scroll's buff lasts 10,800 s");
    const float herb_before = cs->find(gc::kHerbalismQuantityRating)->value;
    CHECK(use_consumable(h, {6, gc::kHerbalismAbility, gc::kHerbalismHit, 312, 23, 7}), "the Herbalism elixir is used");
    const Mirror::EffectSeen *e3 = effect_of(h, gc::kHerbalismEffect);
    CHECK(e3 && e3->duration == 3600.0f && long_buff, "the elixir's buff lasts 3,600 s and the scroll's is still on");
    CHECK(cs->find(gc::kHerbalismQuantityRating)->value == herb_before + 458.0f, "the Herbalism Quantity rating rose by the Uncommon 458");
    CHECK(h.s->inventory().at(Bag, 6) && h.s->inventory().at(Bag, 6)->quantity == 3 && mirror_matches(h) && stats_match(h), "three elixirs are left and the client agrees");
}
static void check_consumable_refusals(Harness &h) {
    const bs::PlayerSetup &setup = h.s->bootstrap().setup();
    const uint64_t used = h.s->uses_applied();
    const svc::InventoryItem *other = h.s->inventory().at(Bag, 6);
    h.client_sends({h.client.actor(h.pawn_ch, trigger_bag_item(setup.guids.ability, gc::kProcessingXpAbility, 24, 6, items::to_instance(*other)), true)});
    for (int i = 0; i < 15; ++i) h.step();
    CHECK(h.s->uses_applied() == used && h.s->inventory().at(Bag, 6)->quantity == 3 && h.s->ability_ends_sent() >= 1, "the ability of another item in that slot is refused (ClientEndAbility) and nothing is taken");
    for (int i = 0; i < 60; ++i) h.step();
    const svc::InventoryItem *xp = h.s->inventory().at(Bag, 4);
    const int32_t xp_left = xp ? xp->quantity : 0;
    const repl::ItemInstance xp_item = xp ? items::to_instance(*xp) : repl::ItemInstance{};
    const uint64_t used_before_pair = h.s->uses_applied();
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(310)), true),
                    h.client.actor(h.pawn_ch, trigger_bag_item(setup.guids.ability, gc::kProcessingXpAbility, 30, 4, xp_item), true),
                    h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(310)), true),
                    h.client.actor(h.pawn_ch, trigger_bag_item(setup.guids.ability, gc::kProcessingXpAbility, 31, 4, xp_item), true)});
    for (int i = 0; i < 25; ++i) h.step();
    CHECK(xp_left == 4 && h.s->uses_applied() == used_before_pair + 1 && h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->quantity == xp_left - 1 && mirror_matches(h),
          "two presses of one scroll within 65 ms: one is taken, the second is refused at its cost step and takes nothing");
    const size_t refused = h.s->uses_refused();
    h.request(item_used(3, 7, guid_n(313)), h.pawn_ch);
    CHECK(h.s->uses_refused() == refused + 1 && h.s->inventory().at(Bag, 7) && h.s->inventory().at(Bag, 7)->quantity == 1 && h.s->uses_applied() == used_before_pair + 1, "a meal is not served yet: the use is refused and the meal stays");
}
static void test_consumables_scroll_and_elixir() {
    const char *path = professions_db_path();
    if (!path) {
        std::printf("Consumables: a scroll and an elixir: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: a scroll and an elixir are used through their ability\n");
    svc::Character rec = cyb_record();
    std::vector<svc::InventoryItem> items;
    for (const auto &it : rec.items)
        if (it.container == Worn) items.push_back(it);
    items.push_back(make_item(Bag, 4, gc::kProcessingXpScroll, 310, 5));
    items.push_back(make_item(Bag, 5, gc::kConstitutionScroll, 311, 1));
    svc::InventoryItem elixir = make_item(Bag, 6, gc::kHerbalismElixir, 312, 4);
    elixir.quality = 6;
    items.push_back(elixir);
    items.push_back(make_item(Bag, 7, gc::kRoastedChicken, 313, 1));
    rec.items = items;
    Harness h(rec, true, true, path);
    CHECK(h.mirror.unreadable == 0 && !h.s->failed(), "(joined)");
    const auto listed = [&](int64_t a) { return std::find(h.mirror.records.begin(), h.mirror.records.end(), a) != h.mirror.records.end(); };
    CHECK(listed(gc::kProcessingXpAbility) && listed(gc::kConstitutionAbility) && listed(gc::kHerbalismAbility),
          "the join lists the abilities of the carried scrolls and the elixir in ReplicatedRecordsToLoad (the client sends no trigger for an ability it has not loaded)");
    CHECK(!listed(gc::kRoastedChickenAbility), "a meal's ability is not listed: meals are not served yet");
    check_scroll_and_elixir_buffs(h);
    check_consumable_refusals(h);
}
static void check_consumable_destroyed_before_cost(Harness &h, float con_before) {
    const sg::CharacterStats *cs = h.s->character_stats();
    const bs::PlayerSetup &setup = h.s->bootstrap().setup();
    const auto *pipe = h.s->pipeline();
    MoveSpec destroy;
    destroy.type = static_cast<uint8_t>(repl::StorageEventType::Remove);
    destroy.to_object = setup.guids.backpack;
    destroy.to_slot = 4;
    destroy.to_count = 1;
    destroy.has_source = false;
    destroy.source_guid = destroy.event_guid = destroy.target_guid = guid_n(330);
    const size_t effects_before = pipe->effect_count(), ends_before = h.s->ability_ends_sent(), chains = h.got.size();
    const uint64_t used_before = h.s->uses_applied();
    const svc::InventoryItem *first = h.s->inventory().at(Bag, 4);
    const repl::ItemInstance first_item = first ? items::to_instance(*first) : repl::ItemInstance{};
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 4, guid_n(330)), true),
                    h.client.actor(h.pawn_ch, trigger_bag_item(setup.guids.ability, gc::kConstitutionAbility, 41, 4, first_item), true),
                    h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kConstitutionAbility, gc::kSelfTargeting, gc::kConstitutionHit), true),
                    h.client.actor(h.pc_ch, storage_move(destroy), true)});
    CHECK(h.s->item_reservations() == 1 && h.s->inventory().at(Bag, 4) == nullptr, "the trigger was accepted (the scroll is reserved) and the stack is destroyed in the same datagram");
    for (int i = 0; i < 25; ++i) h.step();
    CHECK(h.s->uses_applied() == used_before && h.s->items_destroyed() == 1, "the cast took nothing: its item was gone at the cost step");
    CHECK(cs->find(gc::kConstitution)->value == con_before && pipe->effect_count() == effects_before, "no buff: Constitution did not rise and no effect is on the pawn (the hit the client asked for went with the cast)");
    CHECK(pipe->charges_left(gc::kConstitutionAbility) == -1, "no cooldown: the cast that could not be paid started none");
    CHECK(h.s->ability_ends_sent() == ends_before + 1 && h.s->item_reservations() == 0, "the cast was ended (ClientEndAbility) and nothing stays reserved");
    CHECK(count_calls(pc_calls_since(h, chains), "ClientReceiveStorageEvent") == 1 && mirror_matches(h), "the client was told once, by the destroy, and its bag agrees");
    CHECK(h.mirror.unreadable == 0 && !h.s->failed(), "(nothing unreadable)");
}
static void check_consumable_next_use_and_death(Harness &h, float con_before) {
    const sg::CharacterStats *cs = h.s->character_stats();
    const bs::PlayerSetup &setup = h.s->bootstrap().setup();
    const auto *pipe = h.s->pipeline();
    const uint64_t used_before = h.s->uses_applied();
    const svc::InventoryItem *second = h.s->inventory().at(Bag, 5);
    const repl::ItemInstance second_item = second ? items::to_instance(*second) : repl::ItemInstance{};
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 5, guid_n(331)), true),
                    h.client.actor(h.pawn_ch, trigger_bag_item(setup.guids.ability, gc::kConstitutionAbility, 42, 5, second_item), true),
                    h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kConstitutionAbility, gc::kSelfTargeting, gc::kConstitutionHit), true)});
    for (int i = 0; i < 25; ++i) h.step();
    CHECK(h.s->uses_applied() == used_before + 1 && h.s->inventory().at(Bag, 5) && h.s->inventory().at(Bag, 5)->quantity == 1 && mirror_matches(h),
          "the next use is paid: one scroll of the stack of two is taken and the client agrees");
    CHECK(cs->find(gc::kConstitution)->value == con_before + 26.0f && h.s->item_reservations() == 0, "... and its buff is on (Constitution rose by the scroll's 26)");
    for (int i = 0; i < 60; ++i) h.step();
    const float xp_before = cs->find(gc::kProcessingXpRating)->value;
    const uint64_t used_before_death = h.s->uses_applied();
    const svc::InventoryItem *last = h.s->inventory().at(Bag, 6);
    const repl::ItemInstance last_item = last ? items::to_instance(*last) : repl::ItemInstance{};
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, 6, guid_n(332)), true),
                    h.client.actor(h.pawn_ch, trigger_bag_item(setup.guids.ability, gc::kProcessingXpAbility, 43, 6, last_item), true),
                    h.client.actor(h.pawn_ch, hit_request_for(setup, gc::kProcessingXpAbility, gc::kSelfTargeting, gc::kProcessingXpHit), true)});
    CHECK(h.s->item_reservations() == 1, "the trigger was accepted: the scroll is reserved for the cast");
    h.s->take_damage(h.conn, 1.0e9, h.t);
    for (int i = 0; i < 25; ++i) h.step();
    CHECK(h.s->dead() && h.s->deaths() == 1, "the character died before the cost step");
    CHECK(h.s->item_reservations() == 0, "the death let the reservation go (a stack of one can be used again)");
    CHECK(h.s->inventory().at(Bag, 6) && h.s->inventory().at(Bag, 6)->quantity == 1 && h.s->uses_applied() == used_before_death, "the scroll is still in its slot and nothing was taken");
    CHECK(cs->find(gc::kProcessingXpRating)->value == xp_before && pipe->stacks_of(gc::kProcessingXpEffect) == 0, "the hit the client asked for before the death gave no buff");
}
static void test_consumables_item_gone_and_death() {
    const char *path = professions_db_path();
    if (!path) {
        std::printf("Consumables: the stack gone before the cost step, a death before it: skipped (no real content.db)\n");
        return;
    }
    std::printf("Consumables: the stack leaves the bag between the trigger and the cost step; the character dies in that window\n");
    svc::Character rec = cyb_record();
    std::vector<svc::InventoryItem> carried;
    for (const auto &it : rec.items)
        if (it.container == Worn) carried.push_back(it);
    carried.push_back(make_item(Bag, 4, gc::kConstitutionScroll, 330, 1));
    carried.push_back(make_item(Bag, 5, gc::kConstitutionScroll, 331, 2));
    carried.push_back(make_item(Bag, 6, gc::kProcessingXpScroll, 332, 1));
    rec.items = carried;
    Harness h(rec, true, true, path);
    const sg::CharacterStats *cs = h.s->character_stats();
    const auto *pipe = h.s->pipeline();
    CHECK(h.mirror.unreadable == 0 && !h.s->failed() && pipe, "(joined)");
    if (!pipe) return;
    const float con_before = cs->find(gc::kConstitution)->value;
    check_consumable_destroyed_before_cost(h, con_before);
    check_consumable_next_use_and_death(h, con_before);
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    test_d1_join_has_the_tracker();
    test_d1_level_scroll();
    test_d1_class_scroll();
    test_d1_gear_and_consumables();
    test_d1_refusals();
    test_d1_relog();
    test_d1_saver_progress();
    test_g2_join();
    test_g2_scrolls();
    test_g2_saver_round_trip();
    test_consumables_scroll_and_elixir();
    test_consumables_item_gone_and_death();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
