// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_talents_common.h"
#include "retail_purchases.h"

namespace fs = std::filesystem;
namespace content = aoc::worldd::content;
namespace talents = aoc::worldd::talents;
namespace stats = aoc::worldd::stats;
namespace actionbar = aoc::worldd::actionbar;
namespace repl = aoc::protocol::repl;
namespace cf = content_fixture;

struct RealWorld {
    std::filesystem::path                        dir;
    std::optional<aoc::worldd::content::ContentDb> db;
    std::optional<aoc::worldd::talents::Catalog>   catalog;
    RealWorld() {
        dir = content_fixture::make_temp_dir("world_talents_");
        const std::filesystem::path path = dir / "content.db";
        if (!content_fixture::make_db(path)) std::exit(2);
        std::string error;
        db = aoc::worldd::content::ContentDb::open(path.string(), error);
        if (!db) {
            std::printf("  [FAIL] the fixture does not open: %s\n", error.c_str());
            std::exit(2);
        }
        auto data = db->talent_data(error);
        if (!data) {
            std::printf("  [FAIL] talent_data: %s\n", error.c_str());
            std::exit(2);
        }
        catalog = aoc::worldd::talents::Catalog::from(std::move(*data), error);
        if (!catalog) {
            std::printf("  [FAIL] the catalog: %s\n", error.c_str());
            std::exit(2);
        }
    }
    ~RealWorld() {
        db.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

static constexpr int64_t kBardClass = 108640158570105ll;
static constexpr int64_t kTankClass = 108640201692798ll;
static constexpr int64_t kScepterCombo = 6064631845843960261ll;
static constexpr int64_t kBookCombo = 6064631012536680449ll;

static void test_real_catalog() {
    std::printf("the real trees of the fixture\n");
    RealWorld w;
    const talents::Catalog& cat = *w.catalog;
    CHECK(cat.tree_count() == 28, "28 trees: the eight class trees, the Universal tree, the 14 weapon trees and the five gathering trees");
    const talents::Tree* bard = cat.class_tree(kBardClass);
    const talents::Tree* tank = cat.class_tree(kTankClass);
    CHECK(bard && bard->name == "Bard" && bard->guid == 6064629885900499588ll && tank && tank->name == "Tank" && cat.universal() && cat.universal()->guid == content::kGuidUniversalTree,
          "the Bard's, the Tank's and the Universal tree");
    CHECK(cat.professions().size() == 5, "five profession trees have a rank a character has from the start");
    std::vector<std::string> names;
    for (const talents::Tree* t : cat.professions()) names.push_back(t->name);
    CHECK(names == (std::vector<std::string>{"Mining", "Herbalism", "Lumberjacking", "Hunting", "Fishing"}), "...Mining, Herbalism, Lumberjacking, Hunting, Fishing, in that order (the captured Bard's)");
    CHECK(cat.earned(content::kGuidAdventuring, 25, cat.point_type(talents::Point::Archetype)) == 24 && cat.earned(content::kGuidAdventuring, 25, cat.point_type(talents::Point::Stamina)) == 24 &&
              cat.earned(content::kGuidAdventuring, 1, cat.point_type(talents::Point::Archetype)) == 0 && cat.earned(content::kGuidAdventuring, 50, cat.point_type(talents::Point::Archetype)) == 49,
          "the points by level: 24 Archetype and 24 Stamina by level 25 (49 by level 50, which the cap never lets a character reach)");
    const std::vector<talents::RankRef> autos = cat.auto_ranks(*bard, 25);
    CHECK(autos.size() == 4 && autos[0].node->name == "Bard_Rhetoric" && autos[1].node->name == "Bard_Flourish" && autos[2].node->name == "Bard_Passive_ResonantWeapon" &&
              autos[3].node->name == "Bard_Fortissimo",
          "the Bard's auto-granted ranks by level: Rhetoric (1), Flourish (3), Resonant Weapon (5), Fortissimo (10)");
    CHECK(cat.auto_ranks(*bard, 1).size() == 1 && cat.auto_ranks(*bard, 4).size() == 2 && cat.auto_ranks(*bard, 10).size() == 4, "...at levels 1, 4 and 10: one, two and four");
    const talents::Tree* ranger = cat.class_tree(108640156341880ll);
    std::vector<std::string> order;
    if (ranger) {
        for (const talents::RankRef& r : cat.auto_ranks(*ranger, 20)) order.push_back(r.node->name);
    }
    CHECK(order == (std::vector<std::string>{"Ranger_Salvo", "Ranger_HuntOfTheRaven", "Ranger_MarkPrey", "Ranger_Passive_WeaponFocus_Bows", "Ranger_Camouflage", "Ranger_Thorns",
                                             "Ranger_Strider", "Ranger_Regeneration"}),
          "the Ranger's auto-granted ranks by level, then by the tree's order: the retail Ranger's list order (Salvo, Hunt of the Raven, Mark Prey, Camouflage ...)");
    const talents::Tree* summoner = cat.class_tree(108640185046653ll);
    CHECK(summoner && cat.auto_ranks(*summoner, 1).size() == 21 && cat.auto_ranks(*summoner, 0).size() == 19, "the Summoner has 21 auto-granted ranks at level 1, 19 of them at level 0 (the pet commands)");
}

static void report_known_diff(const std::vector<int64_t>& derived, const std::vector<int64_t>& captured) {
    if (derived == captured) return;
    std::printf("    derived %zu, captured %zu\n", derived.size(), captured.size());
    for (size_t i = 0; i < std::min(derived.size(), captured.size()); ++i) {
        if (derived[i] != captured[i]) {
            std::printf("    first difference at %zu: %lld against %lld\n", i, static_cast<long long>(derived[i]), static_cast<long long>(captured[i]));
            break;
        }
    }
}

static void test_real_join_lists() {
    std::printf("KnownAbilities of the real Bard: the captured Bard's, element for element\n");
    RealWorld w;
    const talents::Catalog& cat = *w.catalog;
    talents::Book bard(cat, kBardClass, 1);
    const talents::AbilityLists l = talents::join_abilities(bard, aoc::worldd::bootstrap::general_unstuck(), kScepterCombo, std::vector<int64_t>{kScepterCombo, kBookCombo},
                                                            aoc::worldd::bootstrap::static_known_tail());
    const std::vector<int64_t> captured_known = aoc::worldd::bootstrap::captured_bard_known_abilities();
    const std::vector<aoc::worldd::bootstrap::AbilityRecord> captured_records = aoc::worldd::bootstrap::captured_bard_records_to_load();
    report_known_diff(l.known, captured_known);
    CHECK(captured_known.size() == 57 && l.known == captured_known, "a level-1 Bard's KnownAbilities are the captured 57, in the captured order");
    bool same_records = l.records.size() == captured_records.size();
    for (size_t i = 0; same_records && i < l.records.size(); ++i) same_records = l.records[i] == captured_records[i].guid && captured_records[i].type_id == content::kTypeAbilityRecord;
    CHECK(captured_records.size() == 58 && same_records, "...and the records the captured 58 (the Book's combo after the Scepter's), every type the ability record type");
    talents::Book b10(cat, kBardClass, 10);
    const talents::AbilityLists l10 = talents::join_abilities(b10, aoc::worldd::bootstrap::general_unstuck(), kScepterCombo, std::vector<int64_t>{kScepterCombo, kBookCombo},
                                                              aoc::worldd::bootstrap::static_known_tail());
    CHECK(l10.known.size() == 57 + 2 && l10.known[1] == captured_known[1] && l10.known[2] != captured_known[2] && l10.known[3] != captured_known[2],
          "a level-10 Bard knows two more (Flourish and Fortissimo; Resonant Weapon is a passive), after QuickWit");
    talents::Book tank(cat, kTankClass, 1);
    const talents::AbilityLists lt = talents::join_abilities(tank, aoc::worldd::bootstrap::general_unstuck(), 0, {}, aoc::worldd::bootstrap::static_known_tail());
    CHECK(lt.known[0] == captured_known[0] && lt.known.size() == 1 + 2 + 9 + 45 && std::equal(lt.known.end() - 45, lt.known.end(), captured_known.end() - 45),
          "a level-1 Tank: Unstuck, Vengeance and Grit, the nine professions' abilities, no combo, the same tail");
    CHECK(std::find(lt.known.begin(), lt.known.end(), captured_known[1]) == lt.known.end(), "...and not the Bard's QuickWit");
}

static void test_real_purchases() {
    std::printf("real Bard ranks\n");
    RealWorld w;
    const talents::Catalog& cat = *w.catalog;
    const int64_t bard_tree = cat.class_tree(kBardClass)->guid;
    const auto rank_named = [&](const char* name) -> int64_t {
        for (const talents::Node& n : cat.tree(bard_tree)->nodes) {
            if (n.name == name) return n.ranks.front().guid;
        }
        return 0;
    };
    const int64_t menacing = rank_named("Bard_Music_Menacing"), shielding = rank_named("Bard_Dance_Shielding"), mighty = rank_named("Bard_Passive_Dance_Shielding_Mighty"),
                  conflict = rank_named("Bard_Story_Conflict"), saga = rank_named("Bard_Saga"), instrument = rank_named("Bard_Passive_Instrument");
    CHECK(menacing && shielding && mighty && conflict && saga && instrument, "the ranks are in the fixture");
    talents::Book b(cat, kBardClass, 1);
    auto p = run(b, {{menacing, bard_tree, true}});
    CHECK(refused_with(p, "prerequisite"), "the instrument songs need an Instrument talent picked (a slot of Flute, Fiddle and Drum), which none is yet");
    p = run(b, {{instrument, bard_tree, true}});
    CHECK(p.ok && b.points_left(*cat.tree(bard_tree), cat.point_type(talents::Point::Archetype)) == 0, "the Instrument talent costs no point: a level-1 Bard picks it");
    p = run(b, {{menacing, bard_tree, true}});
    CHECK(refused_with(p, "0 left"), "a level-1 Bard has no points");
    b.set_level(2);
    p = run(b, {{shielding, bard_tree, true}});
    CHECK(refused_with(p, "needs 1 point(s) spent in the tree (0 are)"), "Dance of Shielding needs a point spent in the tree first");
    p = run(b, {{menacing, bard_tree, true}});
    CHECK(p.ok && b.points_left(*cat.tree(bard_tree), cat.point_type(talents::Point::Archetype)) == 0, "Menacing costs the one point of level 2");
    p = run(b, {{shielding, bard_tree, true}});
    CHECK(refused_with(p, "0 left"), "...Shielding then has the point spent and none to pay");
    b.set_level(3);
    p = run(b, {{shielding, bard_tree, true}});
    CHECK(p.ok && b.list(*cat.tree(bard_tree)).size() == 5, "at level 3 Shielding is bought; the list is Rhetoric, Flourish (granted at 3) and the three bought: Instrument, Menacing, Shielding");
    CHECK(b.list(*cat.tree(bard_tree)) == (std::vector<int64_t>{cat.auto_ranks(*cat.tree(bard_tree), 3)[0].rank->guid, cat.auto_ranks(*cat.tree(bard_tree), 3)[1].rank->guid, instrument, menacing,
                                                                  shielding}),
          "(the list is the two auto ranks of level 3 and then the bought, in purchase order)");
    b.set_level(25);
    p = run(b, {{mighty, bard_tree, true}});
    CHECK(refused_with(p, "point(s) spent"), "Mighty needs 4 points spent in the tree: 2 are");
    p = run(b, {{conflict, bard_tree, true}});
    CHECK(refused_with(p, "10 point(s) spent"), "a story needs 10 points spent in the tree");
    p = run(b, {{saga, bard_tree, true}});
    CHECK(refused_with(p, "prerequisite"), "the Saga needs one of the six stories first");
    for (const char* name : {"Bard_Music_Leech", "Bard_Music_Hopeful", "Bard_Music_Pensive", "Bard_Music_Epic", "Bard_Misc_Discordance", "Bard_Dance_Maddening", "Bard_GetOffTheStage",
                              "Bard_Dance_Mesmerizing"}) {
        p = run(b, {{rank_named(name), bard_tree, true}});
        if (!p.ok) std::printf("    %s: %s\n", name, p.refusal.c_str());
    }
    p = run(b, {{mighty, bard_tree, true}});
    CHECK(p.ok, "with ten ranks bought (10 points spent in the tree), Mighty (4 points spent) is bought");
    p = run(b, {{conflict, bard_tree, true}});
    CHECK(p.ok, "...and a story, which needs 10 points spent");
    p = run(b, {{saga, bard_tree, true}});
    CHECK(p.ok, "...and then the Saga, which needs one of the stories");
    p = run(b, {{conflict, bard_tree, false}});
    CHECK(refused_with(p, "without its prerequisite") && b.purchased(bard_tree).size() == 14, "refunding the story while the Saga stays is refused");
    p = run(b, {{saga, bard_tree, false}, {conflict, bard_tree, false}});
    CHECK(p.ok && b.purchased(bard_tree).size() == 12, "...refunding the Saga with it is fine");
}

static void test_real_slots() {
    std::printf("the real Bard's talent slots\n");
    RealWorld w;
    const talents::Catalog& cat = *w.catalog;
    const talents::Tree* bard = cat.class_tree(kBardClass);
    size_t slot_options = 0;
    std::set<int64_t> slot_nodes;
    for (const talents::Node& n : bard->nodes) {
        if (n.slot == 0) continue;
        ++slot_options;
        slot_nodes.insert(n.guid);
    }
    CHECK(slot_nodes.size() == 9 && slot_options > slot_nodes.size(), "the Bard's tree has nine talent nodes (slots), each with more than one option");
    const auto node_named = [&](const char* name) {
        std::vector<const talents::Node*> out;
        for (const talents::Node& n : bard->nodes) {
            if (n.name == name) out.push_back(&n);
        }
        return out;
    };
    const auto hymn = node_named("Bard_Song_Hymn");
    CHECK(hymn.size() == 2 && hymn[0]->slot != 0 && hymn[0]->slot == hymn[1]->slot && hymn[0]->skill != hymn[1]->skill && hymn[0]->ord == hymn[1]->ord &&
              hymn[0]->point_requirement == 1 && !hymn[0]->ranks.empty() && !hymn[1]->ranks.empty(),
          "the Hymn slot offers two skills, each with its ranks, at the node's place and with its gates");
    if (hymn.size() != 2) return;
    talents::Book b(cat, kBardClass, 25);
    const int64_t first = hymn[0]->ranks.front().guid, second = hymn[1]->ranks.front().guid;
    auto p = run(b, {{first, bard->guid, true}});
    CHECK(refused_with(p, "needs 1 point(s) spent in the tree (0 are)"), "a talent has the node's gates: the Hymn slot needs a point spent in the tree");
    const auto instrument = node_named("Bard_Passive_Instrument"), menacing = node_named("Bard_Music_Menacing");
    CHECK(instrument.size() == 3 && menacing.size() == 1, "the Instrument slot offers three skills (Flute, Fiddle, Drum); Menacing is a plain node");
    if (instrument.empty() || menacing.empty()) return;
    p = run(b, {{menacing[0]->ranks.front().guid, bard->guid, true}});
    CHECK(refused_with(p, "prerequisite"), "Menacing needs an Instrument picked: a requirement that names a slot node");
    p = run(b, {{menacing[0]->ranks.front().guid, bard->guid, true}, {instrument[1]->ranks.front().guid, bard->guid, true}});
    CHECK(p.ok && b.list(*bard).back() == instrument[1]->ranks.front().guid,
          "...and any option of the slot meets it, also when the build lists Menacing before the Fiddle (the client lists a tree's nodes in the tree's order)");
    p = run(b, {{first, bard->guid, true}});
    CHECK(p.ok && b.list(*bard).back() == first, "with a point spent, the first option is bought and is in the tree's list");
    p = run(b, {{second, bard->guid, true}});
    CHECK(refused_with(p, "its slot already holds"), "the second option of the same slot is refused");
    p = run(b, {{first, bard->guid, false}, {second, bard->guid, true}});
    CHECK(p.ok && b.list(*bard).back() == second, "a swap (refund, purchase) in one request moves the pick");
}

namespace {
struct RetailStats {
    size_t accepted = 0;
    size_t refused = 0;
    size_t entries = 0;
    size_t differing_lists = 0;
};

void verify_retail_call(const talents::Catalog& cat, const retail_purchases::Call& call, RetailStats& st) {
    const int64_t tree_guid = retail_purchases::kEntries[call.first].tree;
    const talents::Tree* tree = cat.tree(tree_guid);
    if (!tree) {
        std::printf("    capture %d line %d: the tree %lld is not in the catalog\n", call.capture, call.line, static_cast<long long>(tree_guid));
        ++st.refused;
        return;
    }
    talents::Book book(cat, tree->kind == talents::Kind::Class ? tree->class_guid : kBardClass, 25);
    repl::SkillBuild build;
    std::vector<int64_t> before(retail_purchases::kAfter + call.after_first, retail_purchases::kAfter + call.after_first + call.after_count);
    for (size_t i = 0; i < call.count; ++i) {
        const retail_purchases::Entry& e = retail_purchases::kEntries[call.first + i];
        repl::SkillRankChange c;
        c.rank = id_of(e.rank, content::kTypeSkillRank);
        c.tree = id_of(e.tree, content::kTypeSkillTree);
        c.leveling_type = id_of(e.leveling_type, content::kTypeLevelingTypeDef);
        c.purchase = e.purchase;
        build.changes.push_back(c);
        ++st.entries;
        if (e.purchase) before.erase(std::remove(before.begin(), before.end(), e.rank), before.end());
    }
    for (size_t i = 0; i < call.count; ++i) {
        const retail_purchases::Entry& e = retail_purchases::kEntries[call.first + i];
        if (!e.purchase && std::find(before.begin(), before.end(), e.rank) == before.end()) before.push_back(e.rank);
    }
    book.load(tree_guid, before);
    const talents::Plan plan = book.plan(build);
    if (!plan.ok) {
        ++st.refused;
        std::printf("    capture %d line %d (%zu entries, tree %s): %s\n", call.capture, call.line, call.count, tree->name.c_str(), plan.refusal.c_str());
        return;
    }
    ++st.accepted;
    book.apply(plan);
    std::vector<int64_t> expected;
    for (size_t i = 0; i < call.after_count; ++i) {
        const talents::RankRef r = cat.find(tree_guid, retail_purchases::kAfter[call.after_first + i]);
        if (r.ok() && !r.is_auto()) expected.push_back(r.rank->guid);
    }
    if (book.purchased(tree_guid) != expected) {
        ++st.differing_lists;
        std::printf("    capture %d line %d: our bought ranks (%zu) are not the retail list's (%zu)\n", call.capture, call.line, book.purchased(tree_guid).size(), expected.size());
    }
}
}  // namespace

static void test_retail_requests() {
    std::printf("the retail requests\n");
    RealWorld w;
    const talents::Catalog& cat = *w.catalog;
    RetailStats st;
    for (const retail_purchases::Call& call : retail_purchases::kCalls) {
        verify_retail_call(cat, call, st);
    }
    CHECK(std::size(retail_purchases::kCalls) == 128 && st.entries == 411, "the corpus is 128 requests and 411 entries");
    CHECK(st.refused == 0 && st.accepted == 128, "every one of the 128 retail requests is accepted");
    CHECK(st.differing_lists == 0, "...and leaves the tree's bought ranks as the server's own list had them (the order of the rest kept, the purchases after)");
}

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
    CHECK(!db && contains(error, "tables this world reads") && contains(error, "contentc"),
          "a content.db built before the talents is refused at open, with how to rebuild it");
}

static void test_readers() {
    std::printf("the readers\n");
    const fs::path dir = cf::make_temp_dir("world_talents_readers_");
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
              c && c->slot_index == 1, "place_skill: the first empty slots of bank 0, a Skill slot with the ability and the rank");
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

void run_talents_retail_tests() {
    test_real_catalog();
    test_real_join_lists();
    test_real_purchases();
    test_real_slots();
    test_retail_requests();
    test_readers();
    test_action_bar();
}
