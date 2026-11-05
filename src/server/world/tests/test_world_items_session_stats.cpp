// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_items_session_common.h"
using namespace aoc::worldd;
static size_t changed_total(const Mirror::StatUpdate &u) {
    size_t n = 0;
    for (const auto &[name, set] : u.changed) n += set.size();
    return n;
}
static std::map<std::string, std::set<int64_t>> engine_diff(const Harness &h, const std::vector<sg::WornItem> &before, const std::vector<sg::WornItem> &after) {
    const bs::StatContext &ctx = world_stats::of(*h.fx.content);
    sg::CharacterStats cs(*ctx.bundle, *ctx.gear, h.s->character_stats()->class_enum(), h.s->character_stats()->level(), ctx.rounding);
    std::string err;
    std::vector<sg::StatValue> changed;
    std::map<std::string, std::set<int64_t>> out;
    if (!cs.start(before, err) || !cs.update(after, changed, err))
        return out;
    for (const sg::StatValue &v : changed) {
        if (!v.float_array() && (v.replication == 1 || v.replication == 5 || v.replication == 7))
            out[array_of(v.replication)].insert(v.guid);
    }
    return out;
}
static std::vector<sg::WornItem> without_slot(std::vector<sg::WornItem> worn, int slot) {
    worn.erase(std::remove_if(worn.begin(), worn.end(), [&](const sg::WornItem &w) { return w.slot == slot; }), worn.end());
    return worn;
}
static void test_stats_at_the_join() {
    std::printf("stats: the join gives the client the engine's stats\n");
    Harness h;
    const sg::CharacterStats *cs = h.s->character_stats();
    CHECK(cs && cs->started() && cs->class_enum() == 2 && cs->level() == 1 && cs->values().size() == 466, "the session has the Bard's 466 stats at level 1");
    std::string why;
    CHECK(stats_match(h, &why), "the client's arrays are those stats, every value's float bits");
    if (!why.empty())
        std::printf("    %s\n", why.c_str());
    CHECK(h.mirror.stat_items[kArrays[0]].size() == 335 && h.mirror.stat_items[kArrays[1]].size() == 96 && h.mirror.stat_items[kArrays[2]].size() == 11, "335 Owner, 96 OwnerProxy and 11 EveryoneProxy items");
    const auto value = [&](int64_t guid) { return cs && cs->find(guid) ? cs->find(guid)->value : -1.f; };
    constexpr int64_t kHealth = 108986352686712ll, kMaxHealth = 108986356618873ll;
    CHECK(value(kHealth) == value(kMaxHealth) && value(kMaxHealth) > 0, "Health starts at its maximum");
    CHECK(h.mirror.stat_updates.empty() || h.mirror.stat_updates.size() <= 2, "(the join is the arrays' first state: no later update has changed a stat)");
    CHECK(h.s->stat_updates() == 0 && h.s->stat_updates_failed() == 0, "no gear change yet, no failure");
}
static void verify_chest_off_stat_update(Harness &h, const std::vector<sg::WornItem> &before, const std::vector<sg::StatValue> &at_join) {
    const sg::CharacterStats *cs = h.s->character_stats();
    const Mirror::StatUpdate &u = h.mirror.stat_updates.back();
    const auto want = engine_diff(h, before, without_slot(before, 2));
    CHECK(!want.empty() && u.changed == want && u.deleted == 0, "exactly the stats the engine says change, by array, none removed");
    std::printf("    the starter chest changes %zu Owner, %zu OwnerProxy and %zu EveryoneProxy stats (retail's chest piece: 30, 14 and 2)\n", u.changed.count(kArrays[0]) ? u.changed.at(kArrays[0]).size() : 0,
                u.changed.count(kArrays[1]) ? u.changed.at(kArrays[1]).size() : 0, u.changed.count(kArrays[2]) ? u.changed.at(kArrays[2]).size() : 0);
    CHECK(changed_total(u) == 2 && u.changed.count(kArrays[0]) && u.changed.count(kArrays[1]),
          "the starter chest has one stat, so one Owner stat and the OwnerProxy stat derived from it change, and no other (retail's chest pieces have many)");
    const std::vector<int64_t> mitigation = {109183607068294ll};
    bool has = u.changed.count(kArrays[0]) && u.changed.at(kArrays[0]).count(mitigation[0]);
    CHECK(has, "the chest's own stat (the physical mitigation rating) is one of them");
    const sg::StatValue *now = cs->find(mitigation[0]);
    const sg::StatValue *was = nullptr;
    for (const auto &v : at_join) was = v.guid == mitigation[0] ? &v : was;
    CHECK(now && was && was->value - now->value == 20.0f, "it is 20 lower: the chest's value");
}
static void test_stats_follow_the_chest() {
    std::printf("stats: the chest piece off and on\n");
    Harness h;
    const std::vector<sg::WornItem> before = bs::worn_items_of(h.s->inventory());
    const sg::CharacterStats *cs = h.s->character_stats();
    const std::vector<sg::StatValue> at_join = cs->values();
    const size_t updates = h.mirror.stat_updates.size();
    h.request_both(item_used(4, 2, guid_n(102)), item_activate(4, 2, guid_n(102)));
    CHECK(h.mirror.stat_updates.size() == updates + 1 && h.s->stat_updates() == 1, "taking it off sends one stat update");
    std::string why;
    CHECK(stats_match(h, &why), "the client's arrays are the session's stats again");
    if (!why.empty())
        std::printf("    %s\n", why.c_str());
    if (h.mirror.stat_updates.size() == updates + 1)
        verify_chest_off_stat_update(h, before, at_join);
    const size_t updates2 = h.mirror.stat_updates.size();
    h.request_both(item_used(3, 4, guid_n(102)), item_activate(3, 4, guid_n(102)));
    CHECK(h.mirror.stat_updates.size() == updates2 + 1 && stats_match(h), "putting it on sends the stats back, and the client agrees");
    bool restored = cs->values().size() == at_join.size();
    for (size_t i = 0; restored && i < at_join.size(); ++i) restored = at_join[i].guid == cs->values()[i].guid && bits_of(at_join[i].value) == bits_of(cs->values()[i].value);
    CHECK(restored, "every stat has the value it had at the join, to the bit");
    CHECK(h.s->stat_updates() == 2 && h.s->stat_updates_failed() == 0 && h.mirror.unreadable == 0, "two stat updates, none failed, everything read");
}
static std::vector<svc::InventoryItem> bard_wearing_heavy_chest() {
    std::vector<svc::InventoryItem> items = bard_items();
    for (svc::InventoryItem &it : items) {
        if (it.container == Worn && it.slot == 2)
            it.item_record_id = fi::kHeavyChest;
    }
    return items;
}
static int64_t guid_named(const Harness &h, const char *name) {
    const sg::StatBundle &b = *world_stats::of(*h.fx.content).bundle;
    for (size_t i = 0; i < b.stats.size(); ++i) {
        if (b.stat_name(i) == name)
            return b.stats[i].guid;
    }
    return 0;
}
static void test_health_and_mana_clamp_re_equip(Harness &h, float max_health_with, float max_mana_with, float health_without, float mana_without) {
    const sg::CharacterStats *cs = h.s->character_stats();
    const int64_t kHealth = guid_named(h, "Health"), kMaxHealth = guid_named(h, "MaxHealth"), kMana = guid_named(h, "Mana"), kMaxMana = guid_named(h, "MaxMana");
    h.request(item_used(3, 4, guid_n(102)), h.pawn_ch);
    CHECK(cs->find(kMaxHealth)->value == max_health_with && cs->find(kHealth)->value == health_without && health_without < max_health_with, "putting it on: MaxHealth is back up and Health stays where it is (no healing by gear)");
    CHECK(cs->find(kMaxMana)->value == max_mana_with && cs->find(kMana)->value == mana_without, "...and Mana too");
    CHECK(stats_match(h), "...and the client agrees");
    const std::set<int64_t> &later = h.mirror.stat_updates.back().changed.at(kArrays[2]);
    CHECK(later.count(kMaxHealth) && later.count(kMaxMana) && !later.count(kHealth) && !later.count(kMana), "the maxima are deltas, the current values are not");
}
static void test_health_and_mana_clamp() {
    std::printf("stats: Health and Mana above a new maximum are clamped, below it they stay\n");
    Harness h(record_with(bard_wearing_heavy_chest()));
    const sg::CharacterStats *cs = h.s->character_stats();
    CHECK(stats_match(h), "(joined wearing the heavy chest: the client has the stats)");
    for (const char *name : {"Health", "Mana"}) {
        const int64_t cur = guid_named(h, name), max = guid_named(h, (std::string("Max") + name).c_str());
        CHECK(cur != 0 && max != 0 && cs->find(cur) && cs->find(max), (std::string(name) + " and Max" + name + " are stats of the Bard").c_str());
        CHECK(cs->find(cur)->value == cs->find(max)->value && cs->find(max)->value > 100, (std::string(name) + " starts at its maximum, which the heavy chest raised past 100").c_str());
    }
    const int64_t kHealth = guid_named(h, "Health"), kMaxHealth = guid_named(h, "MaxHealth"), kMana = guid_named(h, "Mana"), kMaxMana = guid_named(h, "MaxMana");
    const float max_health_with = cs->find(kMaxHealth)->value, max_mana_with = cs->find(kMaxMana)->value;
    h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
    const float health_without = cs->find(kHealth)->value, mana_without = cs->find(kMana)->value;
    CHECK(cs->find(kMaxHealth)->value < max_health_with && health_without == cs->find(kMaxHealth)->value, "taking it off: MaxHealth falls and Health is clamped to the new maximum");
    CHECK(cs->find(kMaxMana)->value < max_mana_with && mana_without == cs->find(kMaxMana)->value, "...and so are MaxMana and Mana");
    CHECK(stats_match(h), "the client's current values and maxima are the session's");
    const Mirror::StatUpdate &u = h.mirror.stat_updates.back();
    const std::set<int64_t> &everyone = u.changed.at(kArrays[2]);
    CHECK(everyone.count(kHealth) && everyone.count(kMaxHealth) && everyone.count(kMana) && everyone.count(kMaxMana), "Health, MaxHealth, Mana and MaxMana are all deltas of the EveryoneProxy array");
    test_health_and_mana_clamp_re_equip(h, max_health_with, max_mana_with, health_without, mana_without);
}
static void test_nothing_changes_nothing_sent() {
    std::printf("stats: a change of gear that moves no stat sends none\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Bag, 4, fi::kCostume, 204));
    Harness h(record_with(items));
    CHECK(stats_match(h), "(the join)");
    const size_t updates = h.mirror.stat_updates.size(), held = h.mirror.held_updates.size();
    h.request(item_used(3, 4, guid_n(204)), h.pawn_ch);
    CHECK(h.s->inventory().at(Worn, 71) && h.mirror.held_updates.size() == held + 1, "the costume is worn: HeldItems and AppearanceIDs are updated");
    CHECK(h.mirror.stat_updates.size() == updates && h.s->stat_updates() == 0 && h.s->stat_updates_failed() == 0, "and no stat update is sent: no stat moved");
    CHECK(stats_match(h) && gear_matches(h), "the client's stats and gear are the session's");
    h.request(bag_move(h, 1, 5, guid_n(201), 33), h.pc_ch);
    CHECK(h.mirror.stat_updates.size() == updates && h.s->stat_updates() == 0, "a move inside the bag does not touch the stats either");
}
static void test_stats_wait_for_the_window() {
    std::printf("stats: the deltas go out with the update when the pawn channel has room\n");
    Harness h;
    const size_t updates = h.mirror.stat_updates.size();
    const size_t room = Connection::kMaxUnackedReliable - h.conn.unacked_reliable(h.pawn_ch);
    clog_channel(h, h.pawn_ch, room);
    CHECK(h.conn.unacked_reliable(h.pawn_ch) == Connection::kMaxUnackedReliable, "(the pawn channel's window is full)");
    h.client_sends({h.client.actor(h.pawn_ch, item_used(4, 2, guid_n(102)), true)});
    CHECK(h.s->item_requests_applied() == 1 && h.s->stat_updates() == 1 && h.s->replies_pending() == 1 && h.mirror.stat_updates.size() == updates,
          "the chest comes off: the session's stats are changed at once, and the client has not heard yet: the update waits");
    CHECK(!stats_match(h), "(the client's arrays are still the old ones)");
    h.settle();
    CHECK(h.s->replies_pending() == 0 && !h.s->failed() && h.mirror.stat_updates.size() == updates + 1, "once the client acknowledges, one update goes out");
    CHECK(stats_match(h) && mirror_matches(h) && gear_matches(h) && h.mirror.unreadable == 0, "and the client's stats, storages and gear are the session's");
    Harness g;
    clog_channel(g, g.pawn_ch, Connection::kMaxUnackedReliable - g.conn.unacked_reliable(g.pawn_ch));
    const size_t u0 = g.mirror.stat_updates.size();
    g.client_sends({g.client.actor(g.pawn_ch, item_used(4, 2, guid_n(102)), true)});
    g.client_sends({g.client.actor(g.pawn_ch, item_used(3, 3, guid_n(203)), true)});
    g.settle();
    CHECK(g.s->stat_updates() == 2 && g.mirror.stat_updates.size() <= u0 + 2 && stats_match(g) && gear_matches(g) && mirror_matches(g), "two equips behind a full window: the client ends with the session's stats, gear and storages");
}
static float client_stat(const Harness &h, int64_t guid) {
    for (const auto &[name, items] : h.mirror.stat_items) {
        for (const auto &[id, item] : items) {
            if (item.record == guid)
                return std::bit_cast<float>(item.value);
        }
    }
    return std::numeric_limits<float>::quiet_NaN();
}
static void test_stats_runtime_dodge_and_ring_back(Harness &h, const emit::FIntrepidNetworkGUID &ability, const svc::InventoryItem &scepter) {
    constexpr int64_t kStamina = 6064629575339934476ll;
    h.request(item_used(4, 10, guid_n(202)), h.pawn_ch);
    CHECK(h.s->item_requests_applied() == 1 && h.s->abilities()->max_stamina() == 100 && h.s->abilities()->stamina() == 100, "the ring off: the runtime's MaxStamina is 100 and its Stamina is clamped to 100");
    CHECK(client_stat(h, content_fixture::kMaxStamina) == 100.0f && client_stat(h, kStamina) == 100.0f && stats_match(h) && h.s->stat_updates() == 1 && !h.s->abilities_failed(),
          "the client has both at 100, the session's stats agree, one stat update, the abilities still on");
    h.request(trigger_with_item(ability, content::kGuidDodge, 6, items::to_instance(scepter)), h.pawn_ch);
    const float after_dodge = client_stat(h, kStamina);
    CHECK(after_dodge <= 50.0f && h.s->abilities()->stamina() <= 50, "(a Dodge took 50 Stamina)");
    h.request(item_used(3, 2, guid_n(202)), h.pawn_ch);
    CHECK(h.s->abilities()->max_stamina() == 150 && client_stat(h, content_fixture::kMaxStamina) == 150.0f && h.s->abilities()->stamina() <= 50 && client_stat(h, kStamina) == static_cast<float>(h.s->abilities()->stamina()) &&
              h.mirror.unreadable == 0,
          "the ring back on: MaxStamina is 150 again and Stamina is not raised by it: the client's is the runtime's");
    CHECK(h.s->stat_updates_failed() == 0 && !h.s->failed(), "no stat update failed");
}
static void test_stats_through_the_runtime() {
    std::printf("stats: MaxStamina goes through the ability runtime (a Rest has started it) and clamps its Stamina\n");
    constexpr int64_t kStamina = 6064629575339934476ll;
    std::vector<svc::InventoryItem> items = bard_items();
    for (svc::InventoryItem &it : items) {
        if (it.container == Bag && it.item_record_id == fi::kRingA) {
            it.container = Worn;
            it.slot = 10;
        }
    }
    Harness h(record_with(items));
    CHECK(client_stat(h, kStamina) == 150.0f && client_stat(h, content_fixture::kMaxStamina) == 150.0f && stats_match(h), "joined wearing ring A: Stamina and MaxStamina are 150 on the client");
    const emit::FIntrepidNetworkGUID ability = h.s->bootstrap().setup().guids.ability;
    const svc::InventoryItem *scepter = h.s->inventory().at(Worn, 15);
    CHECK(h.s->abilities() && h.s->abilities()->stamina() == 150 && scepter != nullptr, "(the runtime exists from the first tick and started from the pawn values: Stamina 150)");
    if (!scepter)
        return;
    h.request(trigger_with_item(ability, content::kGuidRest, 5, items::to_instance(*scepter)), h.pawn_ch);
    CHECK(h.s->abilities() && h.s->abilities()->stamina() == 150 && h.s->abilities()->max_stamina() == 150 && h.s->abilities()->resting(), "the runtime holds 150 and 150 and the Rest is on");
    test_stats_runtime_dodge_and_ring_back(h, ability, *scepter);
}
static const char *const kRodStats[] = {"Artisan_Gathering_Fishing_Sport_Keep_Lure", "Artisan_Gathering_Fishing_Sport_Hook_Rate", "Artisan_Gathering_Fishing_Sport_Rare_Fish_Chance", "Artisan_Gathering_Fishing_Sport_Power"};
static void verify_rod_stat_update(const Harness &h, int64_t power) {
    const Mirror::StatUpdate &u = h.mirror.stat_updates.back();
    const auto it = u.changed.find(kArrays[0]);
    CHECK(it != u.changed.end() && it->second.count(power) == 1 && u.deleted == 0, "Power is a delta of the Owner array");
    const auto lure = it != u.changed.end() ? it->second.count(guid_named(h, kRodStats[0])) : 0;
    CHECK(lure == 0, "and a bucket stat the rod does not touch is not");
}
static void test_stats_follow_the_rod() {
    std::printf("stats: the fishing rod (a stat that replicates base and equipment) off and on\n");
    Harness h;
    const sg::CharacterStats *cs = h.s->character_stats();
    for (const char *name : kRodStats) {
        const sg::StatValue *v = cs->find(guid_named(h, name));
        CHECK(v && v->buckets() && v->replication == 1, (std::string(name) + ": an Owner stat that replicates base and equipment").c_str());
    }
    const int64_t power = guid_named(h, "Artisan_Gathering_Fishing_Sport_Power");
    const float base = cs->find(power)->base;
    CHECK(cs->find(power)->equip == 100.0f && cs->find(power)->value == base + 100.0f && stats_match(h), "at the join the rod's 100 is in Power's equipment, and the client's item has base, equipment and value");
    const size_t updates = h.mirror.stat_updates.size();
    h.request_both(item_used(4, 35, guid_n(135)), item_activate(4, 35, guid_n(135)));
    CHECK(h.mirror.stat_updates.size() == updates + 1, "the rod off: one stat update");
    std::string why;
    CHECK(stats_match(h, &why), "the client's arrays (value, base, equipment) are the session's after the rod is off");
    if (!why.empty())
        std::printf("    %s\n", why.c_str());
    if (h.mirror.stat_updates.size() == updates + 1)
        verify_rod_stat_update(h, power);
    CHECK(cs->find(power)->equip == 0 && cs->find(power)->value == base && client_stat(h, power) == base, "Power is its base again, on the client too");
    h.request_both(item_used(3, 4, guid_n(135)), item_activate(3, 4, guid_n(135)));
    CHECK(stats_match(h, &why), "and after it is back on");
    if (!why.empty())
        std::printf("    %s\n", why.c_str());
    CHECK(cs->find(power)->equip == 100.0f && cs->find(power)->base == base && client_stat(h, power) == base + 100.0f, "Power has the rod's 100 in its equipment again, the base untouched");
}
static void test_stats_of_replication_zero_back_on(Harness &h, const sg::CharacterStats *cs, int64_t power, int64_t size, size_t updates) {
    std::string why;
    if (h.mirror.stat_updates.size() == updates + 1) {
        const Mirror::StatUpdate &u = h.mirror.stat_updates.back();
        const auto it = u.changed.find(kArrays[0]);
        CHECK(it != u.changed.end() && it->second.count(power) == 1 && it->second.count(size) == 0 && u.deleted == 0, "the update carries Power and no item for Size_Small");
    }
    CHECK(std::isnan(client_stat(h, size)), "and still no item for Size_Small on the client");
    h.request_both(item_used(3, 4, guid_n(135)), item_activate(3, 4, guid_n(135)));
    CHECK(h.s->stat_updates_failed() == 0 && h.s->stat_updates() == 2 && cs->find(size)->value == 7.0f && cs->find(power)->equip == 100.0f && stats_match(h, &why),
          "the rod back on: no failure, Size_Small is 7 again, Power has its 100, the client agrees");
    CHECK(!h.s->failed() && !h.s->abilities_failed() && h.mirror.unreadable == 0, "the session carries on");
}
static void test_stats_of_replication_zero_are_not_sent() {
    std::printf("stats: a rod that moves a stat no array holds (Size_Small) next to Power\n");
    std::vector<svc::InventoryItem> items = bard_items();
    for (svc::InventoryItem &it : items) {
        if (it.container == Worn && it.slot == 35)
            it.item_record_id = fi::kSizeRod;
    }
    Harness h(record_with(items));
    const sg::CharacterStats *cs = h.s->character_stats();
    const int64_t power = content_fixture::kFishingPower, size = content_fixture::kSizeSmall;
    CHECK(cs->find(size) && cs->find(size)->replication == 0 && cs->find(size)->value == 7.0f && cs->find(power)->equip == 100.0f && stats_match(h),
          "joined wearing the size rod: Size_Small is 7 (replication 0), Power has its 100, and the client has what replicates");
    CHECK(std::isnan(client_stat(h, size)), "(the client's arrays hold no item for Size_Small)");
    const float base = cs->find(power)->base;
    const size_t updates = h.mirror.stat_updates.size();
    h.request_both(item_used(4, 35, guid_n(135)), item_activate(4, 35, guid_n(135)));
    CHECK(h.s->inventory().at(Worn, 35) == nullptr, "the rod is off");
    CHECK(h.s->stat_updates_failed() == 0 && h.s->stat_updates() == 1 && h.mirror.stat_updates.size() == updates + 1, "the rod off: no failed update, one update (Power moved)");
    std::string why;
    CHECK(cs->find(size)->value == 0.0f && cs->find(power)->equip == 0.0f && cs->find(power)->value == base && stats_match(h, &why), "Size_Small is 0 in the session, Power is its base, and the client has the session's stats");
    if (!why.empty())
        std::printf("    %s\n", why.c_str());
    test_stats_of_replication_zero_back_on(h, cs, power, size, updates);
}
static void test_gear_that_moves_only_unreplicated_stats() {
    std::printf("stats: ring B (it moves only Size_Small, a stat no array holds) on and off\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Bag, 4, fi::kRingB, 212));
    Harness h(record_with(items));
    const sg::CharacterStats *cs = h.s->character_stats();
    const int64_t size = content_fixture::kSizeSmall;
    CHECK(cs->find(size) && cs->find(size)->value == 0.0f && stats_match(h), "(joined: Size_Small is 0)");
    const size_t updates = h.mirror.stat_updates.size();
    h.request(item_used(3, 4, guid_n(212)), h.pawn_ch);
    CHECK(h.s->inventory().at(Worn, 10) != nullptr, "ring B is worn (RingRight)");
    CHECK(h.s->stat_updates_failed() == 0 && h.s->stat_updates() == 0 && h.mirror.stat_updates.size() == updates, "no failed update, no stat update sent");
    CHECK(cs->find(size)->value == 7.0f && cs->find(size)->equip == 7.0f && stats_match(h), "Size_Small is 7 in the session all the same (the next diff starts from it)");
    h.request(item_used(4, 10, guid_n(212)), h.pawn_ch);
    CHECK(h.s->stat_updates_failed() == 0 && h.s->stat_updates() == 0 && h.mirror.stat_updates.size() == updates && cs->find(size)->value == 0.0f && stats_match(h), "ring B off: nothing failed, nothing sent, Size_Small is 0 again");
    CHECK(!h.s->failed() && h.mirror.unreadable == 0, "the session carries on");
}
static void test_stats_equipment_changes_with_the_value_clamped() {
    std::printf("stats: a change of the equipment contribution alone (the value is clamped) is sent\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Worn, 14, fi::kEarLeftFirst, 210));
    items.push_back(make_item(Bag, 4, fi::kEarRightFirst, 211));
    Harness h(record_with(items));
    const sg::CharacterStats *cs = h.s->character_stats();
    const int64_t hook = content_fixture::kHookRate;
    CHECK(cs->find(hook) && cs->find(hook)->value == 100.0f && cs->find(hook)->equip == 150.0f && stats_match(h), "joined wearing the first earring: Hook_Rate is 100 (clamped), its equipment 150, and the client has them");
    const float base_before = cs->find(hook)->base;
    const size_t updates = h.mirror.stat_updates.size();
    h.request(item_used(3, 4, guid_n(211)), h.pawn_ch);
    CHECK(cs->find(hook)->value == 100.0f && cs->find(hook)->equip == 350.0f && cs->find(hook)->base == base_before, "the second earring: still 100, the equipment 350, the base untouched");
    CHECK(h.mirror.stat_updates.size() == updates + 1 && h.s->stat_updates() == 1, "one stat update goes out, though no value moved");
    std::string why;
    CHECK(stats_match(h, &why), "and the client's equipment is 350, as the session's");
    if (!why.empty())
        std::printf("    %s\n", why.c_str());
    CHECK(client_stat(h, hook) == 100.0f, "(the client's value is still 100)");
    h.request(item_used(4, 14, guid_n(210)), h.pawn_ch);
    CHECK(cs->find(hook)->value == 100.0f && cs->find(hook)->equip == 200.0f && stats_match(h, &why), "the first earring off: equipment 200, value 100, the client agrees");
}
static void test_stamina_low_after_dodges_is_not_refilled_by_gear() {
    std::printf("stats: a ring of MaxStamina off while the runtime's Stamina is low (two Dodges)\n");
    constexpr int64_t kStamina = 6064629575339934476ll;
    std::vector<svc::InventoryItem> items = bard_items();
    for (svc::InventoryItem &it : items) {
        if (it.container == Bag && it.item_record_id == fi::kRingA) {
            it.container = Worn;
            it.slot = 10;
        }
    }
    Harness h(record_with(items));
    const emit::FIntrepidNetworkGUID ability = h.s->bootstrap().setup().guids.ability;
    const svc::InventoryItem *scepter = h.s->inventory().at(Worn, 15);
    CHECK(h.s->abilities() && scepter && h.s->abilities()->stamina() == 150, "(joined wearing ring A: Stamina 150, the runtime is up)");
    if (!scepter)
        return;
    h.request(trigger_with_item(ability, content::kGuidDodge, 6, items::to_instance(*scepter)), h.pawn_ch);
    CHECK(h.s->abilities()->stamina() <= 100, "(the first Dodge took 50)");
    for (int i = 0; i < 40; ++i) h.step();
    h.request(trigger_with_item(ability, content::kGuidDodge, 7, items::to_instance(*scepter)), h.pawn_ch);
    const double low = h.s->abilities()->stamina();
    CHECK(low <= 55, "(the second Dodge landed too: Stamina is about 50)");
    CHECK(client_stat(h, kStamina) == static_cast<float>(low), "(the client has the runtime's Stamina)");
    const size_t updates = h.s->stat_updates();
    h.request(item_used(4, 10, guid_n(202)), h.pawn_ch);
    CHECK(h.s->stat_updates() == updates + 1 && h.s->abilities()->max_stamina() == 100, "the ring off: MaxStamina is 100 in the runtime");
    CHECK(h.s->abilities()->stamina() <= low, "the runtime's Stamina is not raised (the engine's own would be 100)");
    CHECK(client_stat(h, kStamina) <= static_cast<float>(low) && client_stat(h, kStamina) == static_cast<float>(h.s->abilities()->stamina()), "and neither is the client's: it is the runtime's");
    CHECK(client_stat(h, content_fixture::kMaxStamina) == 100.0f && !h.s->abilities_failed() && h.s->stat_updates_failed() == 0, "MaxStamina is 100 on the client; nothing failed");
}
static void test_stats_of_a_faulted_run_are_not_sent() {
    std::printf("stats: a change of gear whose stat run faults sends nothing and leaves the stats as the client has them\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Bag, 4, fi::kShirt, 205));
    Harness h(record_with(items));
    const sg::CharacterStats *cs = h.s->character_stats();
    const std::vector<sg::StatValue> at_join = cs->values();
    const size_t updates = h.mirror.stat_updates.size();
    CHECK(stats_match(h) && cs->faults() == 0, "(joined: the client has the stats, no fault)");
    h.request(item_used(3, 4, guid_n(205)), h.pawn_ch);
    CHECK(h.s->inventory().at(Worn, 19) != nullptr, "the shirt is worn (the item rules take it)");
    CHECK(h.s->stat_updates_failed() == 1 && h.s->stat_updates() == 0 && h.mirror.stat_updates.size() == updates, "its stat run faulted: counted as a failed update, nothing is sent");
    bool same = cs->values().size() == at_join.size();
    for (size_t i = 0; same && i < at_join.size(); ++i) same = bits_of(at_join[i].value) == bits_of(cs->values()[i].value);
    CHECK(same && cs->faults() == 0 && stats_match(h), "the session's stats are still the client's: the faulted values were not kept");
    CHECK(!h.s->failed() && h.mirror.unreadable == 0, "the session carries on");
    h.request(item_used(4, 19, guid_n(205)), h.pawn_ch);
    CHECK(h.s->stat_updates_failed() == 1 && h.s->stat_updates() == 0 && stats_match(h), "the shirt off: no failure, no change");
}
static bool stat_item_is_finite(const repl::StatInt32Inst &item) {
    bool finite = std::isfinite(std::bit_cast<float>(item.value));
    if (item.extended) {
        finite = finite && std::isfinite(std::bit_cast<float>(item.base)) && std::isfinite(std::bit_cast<float>(item.equipment));
    }
    return finite;
}
static bool all_stats_are_finite(const Harness &h, const sg::CharacterStats *cs) {
    bool finite = true;
    for (const sg::StatValue &v : cs->values()) {
        finite = finite && std::isfinite(v.value) && std::isfinite(v.base) && std::isfinite(v.equip);
    }
    for (const auto &[name, arr] : h.mirror.stat_items) {
        for (const auto &[id, item] : arr) {
            finite = finite && stat_item_is_finite(item);
        }
    }
    return finite;
}
static void test_equipment_overflow_behind_a_clamp_is_not_sent() {
    std::printf("stats: an equipment sum no float32 holds, behind a clamp (Hook_Rate), sends nothing\n");
    std::vector<svc::InventoryItem> items = bard_items();
    items.push_back(make_item(Bag, 4, fi::kCursedEarring, 206));
    Harness h(record_with(items));
    const sg::CharacterStats *cs = h.s->character_stats();
    const int64_t hook = content_fixture::kHookRate;
    const std::vector<sg::StatValue> at_join = cs->values();
    const size_t updates = h.mirror.stat_updates.size();
    CHECK(stats_match(h) && cs->faults() == 0 && cs->find(hook)->equip == 0.0f, "(joined: the client has the stats, no fault)");
    h.request(item_used(3, 4, guid_n(206)), h.pawn_ch);
    CHECK(h.s->inventory().at(Worn, 13) != nullptr, "the earring is worn (the item rules take it)");
    CHECK(h.s->stat_updates_failed() == 1 && h.s->stat_updates() == 0 && h.mirror.stat_updates.size() == updates, "its stat run faulted (the equipment's sum, though Hook_Rate's value is a clamped 100): a failed update, nothing is sent");
    bool same = cs->values().size() == at_join.size();
    for (size_t i = 0; same && i < at_join.size(); ++i) same = bits_of(at_join[i].value) == bits_of(cs->values()[i].value) && bits_of(at_join[i].equip) == bits_of(cs->values()[i].equip);
    CHECK(same && cs->faults() == 0 && stats_match(h), "the session's stats are still the client's");
    const bool finite = all_stats_are_finite(h, cs);
    CHECK(finite, "and nothing in them or in the client's arrays is an infinity or a NaN");
    CHECK(!h.s->failed() && h.mirror.unreadable == 0, "the session carries on");
}
static bool drop_owner_stat_item(const Harness &h, int64_t record) {
    repl::Subobject *stats = const_cast<repl::Subobject *>(h.s->bootstrap().pawn().find(h.mirror.guids.stats));
    repl::FastArrayState *owner = stats ? stats->obj.arrays.get(*stats->obj.schema, "StatRepInt32Owner") : nullptr;
    int32_t id = 0;
    if (owner) {
        for (const repl::FastArrayItem &it : owner->items()) {
            const auto found = it.values.values.find(1);
            const auto *t = found != it.values.values.end() ? found->second.get_if<repl::StatInt32Inst>() : nullptr;
            if (t && t->record == record)
                id = it.id;
        }
    }
    return owner && id != 0 && owner->remove(id);
}
static void test_stats_refused_mitigation() {
    constexpr int64_t kMitigation = 109183607068294ll;
    {
        Harness h;
        const sg::CharacterStats *cs = h.s->character_stats();
        const float before = cs->find(kMitigation)->value;
        const std::vector<sg::StatValue> at_join = cs->values();
        CHECK(drop_owner_stat_item(h, kMitigation), "(the pawn's Owner array lost the mitigation item)");
        const size_t updates = h.mirror.stat_updates.size();
        h.request_both(item_used(4, 2, guid_n(102)), item_activate(4, 2, guid_n(102)));
        CHECK(h.s->inventory().at(Worn, 2) == nullptr, "the chest is off (the item rules do not care)");
        CHECK(h.s->stat_updates_failed() == 1 && h.s->stat_updates() == 0, "the pawn refused the stats: a failed update");
        bool sent = false;
        for (size_t i = updates; i < h.mirror.stat_updates.size(); ++i) sent = sent || !h.mirror.stat_updates[i].changed.empty();
        CHECK(!sent, "...and no stat value is sent");
        bool same = cs->values().size() == at_join.size();
        for (size_t i = 0; same && i < at_join.size(); ++i) same = bits_of(at_join[i].value) == bits_of(cs->values()[i].value);
        CHECK(same && cs->find(kMitigation)->value == before, "the session's stats are as the client has them (the chest's 20 is still in): not committed");
        CHECK(!h.s->failed(), "the session carries on");
    }
}
static void test_stats_refused_max_stamina() {
    {
        std::vector<svc::InventoryItem> items = bard_items();
        for (svc::InventoryItem &it : items) {
            if (it.container == Bag && it.item_record_id == fi::kRingA) {
                it.container = Worn;
                it.slot = 10;
            }
        }
        Harness h(record_with(items));
        const sg::CharacterStats *cs = h.s->character_stats();
        CHECK(h.s->abilities() && h.s->abilities()->max_stamina() == 150 && drop_owner_stat_item(h, content_fixture::kMaxStamina), "(joined with ring A; the pawn lost MaxStamina's item)");
        h.request(item_used(4, 10, guid_n(202)), h.pawn_ch);
        CHECK(h.s->inventory().at(Worn, 10) == nullptr && h.s->stat_updates_failed() == 1 && h.s->stat_updates() == 0, "the ring is off, the stat update failed");
        CHECK(h.s->abilities()->max_stamina() == 150 && h.s->abilities()->stamina() == 150, "the runtime holds MaxStamina 150 and Stamina 150 still: it takes back what the client was not told");
        CHECK(cs->find(content_fixture::kMaxStamina)->value == 150.0f, "and so do the session's stats");
        CHECK(!h.s->failed() && !h.s->abilities_failed(), "the session and its abilities carry on");
    }
}
static void test_stats_refused_by_the_pawn_stay_the_clients() {
    std::printf("stats: a change the pawn's arrays cannot take is not committed to the session\n");
    test_stats_refused_mitigation();
    test_stats_refused_max_stamina();
}
static void test_level_up_refills() {
    std::printf("a level gained gives Health, Mana and Stamina back\n");
    Harness h(record_with_scrolls());
    const int64_t kHealth = guid_named(h, "Health"), kMaxHealth = guid_named(h, "MaxHealth"), kMana = guid_named(h, "Mana"), kMaxMana = guid_named(h, "MaxMana");
    const emit::FIntrepidNetworkGUID ability = h.s->bootstrap().setup().guids.ability;
    const svc::InventoryItem *scepter = h.s->inventory().at(Worn, 15);
    CHECK(scepter != nullptr, "(the Bard wears the Scepter)");
    if (!scepter) return;
    h.settle();
    CHECK(h.s->set_current_value(kHealth, 1.0f) && h.s->set_current_value(kMana, 1.0f), "(Health and Mana are spent)");
    h.request(trigger_with_item(ability, content::kGuidDodge, 6, items::to_instance(*scepter)), h.pawn_ch);
    CHECK(h.s->abilities() && h.s->abilities()->stamina() < h.s->abilities()->max_stamina(), "(a Dodge spends Stamina)");
    h.request_both(item_used(3, 4, guid_n(210)), item_activate(3, 4, guid_n(210)));
    CHECK(h.s->progress().level == 10, "(the scroll takes the character to level 10)");
    h.settle();
    const sg::CharacterStats *cs = h.s->character_stats();
    CHECK(cs->find(kHealth)->value == cs->find(kMaxHealth)->value && client_stat(h, kHealth) == client_stat(h, kMaxHealth) && client_stat(h, kMaxHealth) > 1.0f, "Health is at the new maximum, on the server and the client");
    CHECK(cs->find(kMana)->value == cs->find(kMaxMana)->value && client_stat(h, kMana) == client_stat(h, kMaxMana) && client_stat(h, kMaxMana) > 1.0f, "Mana is at the new maximum, on the server and the client");
    CHECK(h.s->abilities()->stamina() == h.s->abilities()->max_stamina() && client_stat(h, content::kGuidStamina) == client_stat(h, content::kGuidMaxStamina), "Stamina is at its maximum, in the runtime and on the client");
    for (int i = 0; i < 300; ++i) h.step(milliseconds(20));
    std::string why;
    const bool agree = stats_match(h, &why);
    if (!agree) std::printf("  [info] %s\n", why.c_str());
    CHECK(agree && !h.s->abilities_failed(), "the session's stats agree with the client's and the abilities stay on");
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    test_stats_at_the_join();
    test_stats_follow_the_chest();
    test_health_and_mana_clamp();
    test_nothing_changes_nothing_sent();
    test_stats_wait_for_the_window();
    test_stats_through_the_runtime();
    test_stats_follow_the_rod();
    test_stats_of_replication_zero_are_not_sent();
    test_gear_that_moves_only_unreplicated_stats();
    test_stats_equipment_changes_with_the_value_clamped();
    test_stamina_low_after_dodges_is_not_refilled_by_gear();
    test_stats_of_a_faulted_run_are_not_sent();
    test_equipment_overflow_behind_a_clamp_is_not_sent();
    test_stats_refused_by_the_pawn_stay_the_clients();
    test_level_up_refills();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
