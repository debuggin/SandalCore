// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_talents_common.h"

namespace fs = std::filesystem;
namespace content = aoc::worldd::content;
namespace actionbar = aoc::worldd::actionbar;
namespace repl = aoc::protocol::repl;
namespace cf = content_fixture;

static void test_readers_valid_fixture(const fs::path& dir) {
    std::string error;
    const fs::path path = dir / "good.db";
    cf::make_db(path);
    auto db = content::ContentDb::open(path.string(), error);
    CHECK(db.has_value(), "the fixture opens");
    if (!db) return;
    const auto data = db->talent_data(error);
    CHECK(data && data->trees.size() == 28 && data->point_types.size() == 11 && data->point_types.at("Combat_Archetype") == 6064631448935923712ll,
          "talent_data: 28 trees, 11 point types, Combat_Archetype by name");
    size_t nodes = 0, ranks = 0;
    if (data) {
        for (const auto& t : data->trees) {
            nodes += t.nodes.size();
            for (const auto& n : t.nodes) ranks += n.ranks.size();
        }
    }
    CHECK(nodes > 600 && ranks >= nodes - 5, "...with their nodes and the ranks of each node's skill");
    const auto sw = db->item_weapon(6064631131688402948ll, error);
    CHECK(sw && sw->attack_ability == kScepterCombo && sw->skill_line == 6064632329144307267ll && sw->skill_line_name == "Weapon_1H_Scepter",
          "item_weapon: the starter scepter's combo and weapon line");
    error.clear();
    CHECK(!db->item_weapon(6064629382556287782ll, error) && error.empty(), "item_weapon: a bag is no weapon (nullopt, no error)");
}

static void test_readers_bad_storage_and_schema(const fs::path& dir) {
    std::string error;
    struct Case {
        const char* name;
        const char* sql;
        const char* part;
    };
    for (const Case& c : {Case{"a node's level as text", "UPDATE skill_node SET level_requirement = 'five' WHERE node_guid = (SELECT min(node_guid) FROM skill_node)", "skill_node"},
                          Case{"a node's level as a fraction", "UPDATE skill_node SET point_requirement = 1.5 WHERE node_guid = (SELECT min(node_guid) FROM skill_node)", "skill_node"},
                          Case{"a rank's ability as text", "UPDATE skill_rank SET ability_guid = 'x' WHERE rank_guid = (SELECT min(rank_guid) FROM skill_rank)", "skill_rank"},
                          Case{"a rank's duration as text", "UPDATE skill_rank SET effect_duration = 'long' WHERE rank_guid = (SELECT min(rank_guid) FROM skill_rank)", "skill_rank"},
                          Case{"a cost of text", "UPDATE skill_rank_cost SET quantity = 'two' WHERE rank_guid = (SELECT min(rank_guid) FROM skill_rank_cost)", "skill_rank_cost"},
                          Case{"a cost out of range", "UPDATE skill_rank_cost SET quantity = 5000 WHERE rank_guid = (SELECT min(rank_guid) FROM skill_rank_cost)", "skill_rank_cost"},
                          Case{"a point as text", "UPDATE leveling_point SET quantity = 'one' WHERE type_guid = (SELECT min(type_guid) FROM leveling_point)", "leveling_point"},
                          Case{"a tree's leveling type as text", "UPDATE skill_tree SET leveling_type_guid = 'x' WHERE guid = (SELECT min(guid) FROM skill_tree)", "skill_tree"},
                          Case{"a requirement as text", "UPDATE skill_node_require SET skill_guid = 'x' WHERE node_guid = (SELECT min(node_guid) FROM skill_node_require)", "skill_node_require"},
                          Case{"a node of a tree that is none", "UPDATE skill_node SET tree_guid = 42 WHERE node_guid = (SELECT min(node_guid) FROM skill_node)", "tree"}}) {
        const fs::path path = dir / "bad.db";
        std::error_code ec;
        fs::remove(path, ec);
        cf::make_db(path);
        sqlite3* raw = nullptr;
        sqlite3_open(path.string().c_str(), &raw);
        cf::exec(raw, c.sql);
        sqlite3_close(raw);
        auto db = content::ContentDb::open(path.string(), error);
        if (!db) {
            std::printf("  [FAIL] %s: the file does not open: %s\n", c.name, error.c_str());
            ++g_fail;
            continue;
        }
        error.clear();
        const auto data = db->talent_data(error);
        const std::string msg = std::string(c.name) + ": refused, naming the table";
        CHECK(!data && contains(error, c.part), msg.c_str());
    }
    const fs::path path = dir / "old.db";
    cf::DbOptions o;
    o.talent_tables = false;
    cf::make_db(path, o);
    const auto db = content::ContentDb::open(path.string(), error);
    CHECK(!db && contains(error, "tables this worldd reads") && contains(error, "contentc"),
          "a content.db built before the talents is refused at open, with how to rebuild it");
}

static void test_readers() {
    std::printf("the readers\n");
    const fs::path dir = cf::make_temp_dir("worldd_talents_readers_");
    test_readers_valid_fixture(dir);
    test_readers_bad_storage_and_schema(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static repl::ActionbarSlot make_slot(int bank, int s, uint8_t type, int64_t ability = 0, int64_t rank = 0) {
    repl::ActionbarSlot x;
    x.bank_index = bank;
    x.slot_index = s;
    x.use.use_type = type;
    x.use.ability_id = ability;
    x.use.skill_rank_id = rank;
    return x;
}

static void test_action_bar_set_and_bounds() {
    using namespace actionbar;
    Bar b;
    std::string why;
    CHECK(b.empty() && b.slots().empty() && b.first_free() == std::make_pair(0, 0), "an empty bar: the first free slot is bank 0 slot 0");
    CHECK(b.set(make_slot(0, 0, repl::ActionbarUseType::Skill, 11, 111)) && b.set(make_slot(4, 3, repl::ActionbarUseType::SwapWeapons, 5)) && b.size() == 2 && b.at(0, 0)->skill_rank_id == 111,
          "set stores a slot");
    CHECK(b.first_free() == std::make_pair(0, 1), "the first free slot of the class pages is after it");
    CHECK(b.set(make_slot(0, 0, repl::ActionbarUseType::None)) && b.size() == 1 && !b.at(0, 0), "a UseType of None clears the slot");
    CHECK(b.set(make_slot(0, 0, repl::ActionbarUseType::None)), "...a clear of a slot that is empty is fine");
    for (const auto& s : {make_slot(-1, 0, 1), make_slot(20, 0, 1), make_slot(0, -1, 1), make_slot(0, 12, 1), make_slot(19, 12, 1), make_slot(0, 16, 1), make_slot(0, 0, 10), make_slot(2147483647, 0, 1)}) {
        why.clear();
        CHECK(!b.set(s, &why) && !why.empty(), "a slot outside the banks 0-19 and slots 0-11, or a use type past 9, is refused with a reason");
    }
    repl::ActionbarSlot bad_instigator = make_slot(0, 2, 1);
    bad_instigator.use.instigator = 4;
    repl::ActionbarSlot bad_quality = make_slot(0, 2, 3);
    bad_quality.use.item_quality = -1;
    CHECK(!b.set(bad_instigator) && !b.set(bad_quality) && b.size() == 1, "an instigator past 3 and a negative item quality are refused");
    repl::ActionbarSlot bad_rarity = make_slot(0, 2, 3), bad_combo = make_slot(0, 2, 2), top_rarity = make_slot(0, 3, 3), top_combo = make_slot(0, 4, 2);
    bad_rarity.use.item_rarity = 200;
    bad_combo.use.combo_button = 16;
    top_rarity.use.item_rarity = 15;
    top_combo.use.combo_button = 15;
    why.clear();
    CHECK(!b.set(bad_rarity, &why) && !why.empty() && !b.at(0, 2), "an item rarity of 200 is refused with a reason, nothing stored");
    why.clear();
    CHECK(!b.set(bad_combo, &why) && !why.empty() && !b.at(0, 2) && b.size() == 1, "a combo button of 16 is refused with a reason, nothing stored");
    CHECK(b.set(top_rarity) && b.set(top_combo) && b.size() == 3 && b.set(make_slot(0, 3, repl::ActionbarUseType::None)) && b.set(make_slot(0, 4, repl::ActionbarUseType::None)) && b.size() == 1,
          "15 is the highest rarity and combo button: stored (and cleared again)");
    Bar full;
    size_t placed = 0;
    for (int bank = 0; bank <= kMaxBank && placed < kMaxSlots; ++bank) {
        for (int s = 0; s <= kMaxSlot && placed < kMaxSlots; ++s) placed += full.set(make_slot(bank, s, 1, 5)) ? 1 : 0;
    }
    CHECK(full.size() == static_cast<size_t>((kMaxBank + 1) * (kMaxSlot + 1)) && !full.set(make_slot(19, 12, 1, 5)) && full.set(make_slot(0, 0, 1, 6)),
          "every slot of the 20 banks of 12 can be set: one past the last slot is refused, one that replaces a slot is not");
}

static void test_action_bar_skills_and_wire() {
    using namespace actionbar;
    Bar p;
    const auto a = p.place_skill(900, 9001);
    const auto c = p.place_skill(901, 9002);
    CHECK(a && a->bank_index == 0 && a->slot_index == 0 && a->use.use_type == repl::ActionbarUseType::Skill && a->use.ability_id == 900 && a->use.skill_rank_id == 9001 &&
              c && c->slot_index == 1,
          "place_skill: the first empty slots of bank 0, a Skill slot with the ability and the rank");
    CHECK(!p.place_skill(900, 9001) && p.has_skill(9001) && !p.has_skill(1), "a rank on the bar is not placed twice");
    for (int i = 0; i < kClassBanks * kClassSlots; ++i) p.place_skill(1000 + i, 20000 + i);
    CHECK(!p.place_skill(5, 5) && p.size() == static_cast<size_t>(kClassBanks * kClassSlots), "a full set of class pages: nothing is placed");
    const auto freed = p.remove_skills({9001, 20003, 777});
    CHECK(freed.size() == 2 && freed[0] == std::make_pair(0, 0) && p.first_free() == std::make_pair(0, 0), "remove_skills frees the slots of the ranks it is given, in order");
    Bar m;
    m.place_skill(500, 5000);
    m.set(make_slot(1, 5, repl::ActionbarUseType::Skill, 500, 5000));
    const std::vector<std::pair<int, int>> others = m.remove_other_copies(5000, 1, 5);
    CHECK(others.size() == 1 && others[0] == std::make_pair(0, 0) && m.size() == 1 && m.at(1, 5), "remove_other_copies leaves the skill on the slot it was just put on");
    const Bar d = Bar::defaults({107887054050936ll, content::kGuidSprint, content::kGuidDodge});
    CHECK(d.size() == 4 && d.at(4, 1)->ability_id == 107887054050936ll && d.at(4, 2)->ability_id == content::kGuidSprint && d.at(4, 3)->use_type == repl::ActionbarUseType::SwapWeapons &&
              d.at(4, 3)->ability_id == content::kGuidDodge && d.at(4, 4)->use_type == repl::ActionbarUseType::Ability && d.at(4, 4)->ability_id == content::kGuidDodge,
          "the default weapon bank: Guard, Sprint, the weapon swap (Dodge's id as captured), Dodge");
    const auto sl = d.slots();
    CHECK(sl.size() == 4 && sl[0].slot_index == 1 && sl[3].slot_index == 4, "the slots come by bank then slot");
    Bar w = d;
    w.set(make_slot(0, 0, repl::ActionbarUseType::Skill, 11, 111));
    w.set(make_slot(2, 5, repl::ActionbarUseType::Ability, 22));
    const auto wire = w.wire_slots();
    CHECK(wire.size() == 2 + 12 && wire[0].bank_index == 0 && wire[1].bank_index == 2, "wire_slots: the class slots once, by bank then slot, ahead of the weapon bank here");
    bool order = true;
    const int expect[] = {1, 2, 4, 3};
    for (size_t i = 0; i < 12; ++i) {
        const repl::ActionbarSlot& x = wire[2 + i];
        order = order && x.bank_index == 4 && x.slot_index == expect[i % 4] && x.use == *d.at(4, x.slot_index);
    }
    CHECK(order, "...then the weapon bank's slots 1, 2, 4, 3 and again twice, each with its stored use");
    Bar after;
    after.set(make_slot(4, 2, repl::ActionbarUseType::Ability, 7));
    after.set(make_slot(4, 6, repl::ActionbarUseType::Ability, 8));
    after.set(make_slot(16, 0, repl::ActionbarUseType::Ability, 9));
    const auto wire2 = after.wire_slots();
    CHECK(wire2.size() == 3 + 1 + 1 && wire2[0].slot_index == 2 && wire2[1].slot_index == 2 && wire2[2].slot_index == 2 && wire2[3].slot_index == 6 && wire2[4].bank_index == 16,
          "a weapon bank with only slot 2 lists it three times; slot 6, not one of the four, once; the pet bank after it");
    CHECK(Bar().wire_slots().empty() && w.slots().size() == 6, "an empty bar has no wire slots, and the stored slots are not tripled");
}

static void test_action_bar() {
    std::printf("the action bar\n");
    test_action_bar_set_and_bounds();
    test_action_bar_skills_and_wire();
}

void run_talents_part4() {
    test_readers();
    test_action_bar();
}
