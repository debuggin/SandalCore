// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_talents_common.h"

namespace content = aoc::worldd::content;
namespace talents = aoc::worldd::talents;
namespace stats = aoc::worldd::stats;

static void test_lists_and_mods() {
    std::printf("the lists, the abilities and the mods\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    talents::Book b(*cat, syn::kClass, 2);
    const auto trees = b.listed_trees();
    CHECK(trees.size() == 3 && trees[0]->guid == syn::kClassTree && trees[1]->guid == syn::kProfessionTree && trees[2]->guid == syn::kUniversalTree,
          "the trees the XPTracker lists: the class tree, the professions with a rank, the Universal tree");
    talents::Book l1(*cat, syn::kClass, 1);
    CHECK(l1.listed_trees().size() == 2 && l1.list(*cat->universal()).empty(), "at level 1 the Universal tree has no rank to list");
    b.set_level(25);
    run(b, {{syn::rW, syn::kWeaponTree, true}, {syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}});
    CHECK(b.listed_trees().size() == 4 && b.listed_trees().back()->guid == syn::kWeaponTree && b.list(*cat->tree(syn::kWeaponTree)) == std::vector<int64_t>{syn::rW},
          "a weapon tree is listed once it has a bought rank, and its list is only what was bought (no auto ranks)");
    CHECK(b.list(*cat->tree(syn::kOtherClassTree)).empty(), "another class's tree has no list");
    CHECK(b.abilities(*cat->class_tree(syn::kClass)) == (std::vector<int64_t>{syn::kAbilityA, syn::kAbilityB, syn::kAbilityC}), "the class tree's abilities in list order: the auto-granted, then the bought");
    run(b, {{syn::rE, syn::kClassTree, true}, {syn::rWeaponMod, syn::kWeaponTree, true}});
    const auto flat = [](const std::vector<stats::ActiveEffect>& effects) {
        std::vector<stats::EffectMod> out;
        for (const auto& e : effects) out.insert(out.end(), e.mods.begin(), e.mods.end());
        return out;
    };
    const auto none = flat(b.passives(0));
    const auto with_line = flat(b.passives(syn::kLine));
    CHECK(none.size() == 1 && none[0].stat == syn::kStatStamina && none[0].value == 7.5 && with_line.size() == 2 && with_line[1].value == 3,
          "mods: the class passive's constant always, a weapon rank's while its line is in hand");
    CHECK(flat(b.passives(syn::kOtherLine)).size() == 1, "...not with another weapon line in hand");
    CHECK(b.passives(syn::kLine).size() == 2 && b.passives(syn::kLine)[0].stacks == 1 && b.passives(syn::kLine)[0].effect.type_id == stats::kRefTypeEffect,
          "each is an infinite effect of one stack, referenced as an Effect record");
    talents::Book l(*cat, syn::kClass, 25);
    size_t dropped = 0;
    l.load(syn::kClassTree, {syn::rB, 424242, syn::rA, syn::rB, syn::rC}, &dropped);
    CHECK(l.purchased(syn::kClassTree) == (std::vector<int64_t>{syn::rB, syn::rC}) && dropped == 3, "load keeps the known ranks once each in order; an unknown one, an auto-granted one and a duplicate are dropped");
    dropped = 0;
    l.load(424242, {syn::rB}, &dropped);
    CHECK(dropped == 1 && l.purchased(424242).empty(), "load of a tree that is none drops its ranks");
    l.load(syn::kClassTree, {});
    CHECK(l.purchased().empty(), "load of an empty list removes the tree");
    talents::Book c(*cat, syn::kClass, 25);
    run(c, {{syn::rB, syn::kClassTree, true}});
    c.set_class(syn::kOtherClass);
    CHECK(c.list(*cat->class_tree(syn::kClass)).empty() && c.purchased(syn::kClassTree) == std::vector<int64_t>{syn::rB} && c.class_tree()->guid == syn::kOtherClassTree,
          "after a class change the old class tree is not on the character, its bought ranks are kept");
    c.set_class(syn::kClass);
    CHECK(c.list(*cat->class_tree(syn::kClass)) == (std::vector<int64_t>{syn::rA, syn::rB}), "...and come back with the class");
}

namespace {
constexpr int64_t rS1 = 3501, rS2a = 3502, rS2b = 3503, rS3 = 3504, rNeedSlot = 3510, rNeedOpt = 3511, rNeedStranger = 3512;
constexpr int64_t kSlotNode = 400, kSlotGuid = 777;

std::optional<talents::Catalog> build_slot_catalog(std::string& error) {
    content::TalentData d = syn::data();
    const auto option = [](int64_t skill, int opt, std::vector<content::SkillRankRow> ranks) {
        content::SkillNodeRow n = syn::node(kSlotNode, "Slot", std::move(ranks), true, 0);
        n.skill = skill;
        n.skill_name = "option" + std::to_string(skill);
        n.opt = opt;
        n.slot = kSlotGuid;
        return n;
    };
    content::SkillTreeRow& cls = d.trees[0];
    const int ord = static_cast<int>(cls.nodes.size());
    for (auto n : {option(5001, 0, {syn::ability_rank(rS1, 0, syn::kAbilityA + 50, syn::one)}),
                   option(5002, 1, {syn::ability_rank(rS2a, 0, 0, syn::one), syn::ability_rank(rS2b, 1, 0, syn::one)}),
                   option(5003, 2, {syn::ability_rank(rS3, 0, 0, syn::one)})}) {
        n.ord = ord;
        cls.nodes.push_back(std::move(n));
    }
    content::SkillNodeRow need_slot = syn::node(rNeedSlot - 100, "NeedsTheSlot", {syn::ability_rank(rNeedSlot, 0, 0, syn::one)}, false, 0);
    need_slot.requires_nodes = {kSlotNode};
    need_slot.ord = ord + 1;
    content::SkillNodeRow need_opt = syn::req_node(rNeedOpt - 100, "NeedsOption2", {syn::ability_rank(rNeedOpt, 0, 0, syn::one)}, {5002}, false);
    need_opt.ord = ord + 2;
    content::SkillNodeRow need_stranger = syn::node(rNeedStranger - 100, "NeedsAnAbsentSlot", {syn::ability_rank(rNeedStranger, 0, 0, syn::one)}, false, 0);
    need_stranger.requires_nodes = {123456789};
    need_stranger.ord = ord + 3;
    cls.nodes.push_back(std::move(need_slot));
    cls.nodes.push_back(std::move(need_opt));
    cls.nodes.push_back(std::move(need_stranger));
    return talents::Catalog::from(std::move(d), error);
}
}  // namespace

static void test_slots_synthetic() {
    std::printf("talent slots, synthetic\n");
    std::string error;
    auto cat = build_slot_catalog(error);
    if (!cat) {
        std::printf("  [FAIL] the slot catalog does not build: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    CHECK(cat->find(syn::kClassTree, rS1).ok() && cat->find(syn::kClassTree, rS2b).ok() && cat->find(syn::kClassTree, rS2b).node->skill == 5002 &&
              cat->find(syn::kClassTree, rS2b).rank->ord == 1 && cat->find(syn::kClassTree, rS3).node->guid == kSlotNode,
          "every option of a slot is a rank of the tree, found with its option's node");
    talents::Book b(*cat, syn::kClass, 25);
    auto p = run(b, {{rS1, syn::kClassTree, true}});
    CHECK(p.ok && b.purchased(syn::kClassTree) == std::vector<int64_t>{rS1}, "an option of a slot is bought like any rank");
    p = run(b, {{rS2a, syn::kClassTree, true}});
    CHECK(refused_with(p, "its slot already holds") && b.purchased(syn::kClassTree).size() == 1, "another option of the same slot is refused while the first is owned");
    p = run(b, {{rS1, syn::kClassTree, false}, {rS3, syn::kClassTree, true}});
    CHECK(p.ok && b.purchased(syn::kClassTree) == std::vector<int64_t>{rS3}, "a swap (refund of the owned option, purchase of another) in one request is accepted");
    p = run(b, {{rS3, syn::kClassTree, false}, {rS2a, syn::kClassTree, true}, {rS2b, syn::kClassTree, true}});
    CHECK(p.ok && b.purchased(syn::kClassTree) == (std::vector<int64_t>{rS2a, rS2b}), "both ranks of one option (the second rank follows the first) are bought together");
    talents::Book n(*cat, syn::kClass, 25);
    p = run(n, {{rNeedSlot, syn::kClassTree, true}});
    CHECK(refused_with(p, "prerequisite"), "a requirement that names a slot node is not met while no option of it is owned");
    run(n, {{rS3, syn::kClassTree, true}});
    CHECK(run(n, {{rNeedSlot, syn::kClassTree, true}}).ok, "...and is met by any option of it");
    p = run(n, {{rS3, syn::kClassTree, false}});
    CHECK(refused_with(p, "without its prerequisite") && n.purchased(syn::kClassTree).size() == 2, "refunding the only option owned while a rank needs the slot is refused");
    (void)rNeedOpt;
    CHECK(run(n, {{rNeedStranger, syn::kClassTree, true}}).ok, "a requirement that names a node the tree does not hold is met (the data cannot be bought toward)");
    CHECK(n.list(*cat->class_tree(syn::kClass)).back() == rNeedStranger && n.abilities(*cat->class_tree(syn::kClass)).size() == 1,
          "a picked option is in the tree's list; an option without an ability adds none");
    talents::Book a(*cat, syn::kClass, 25);
    run(a, {{rS1, syn::kClassTree, true}});
    CHECK(a.abilities(*cat->class_tree(syn::kClass)) == (std::vector<int64_t>{syn::kAbilityA, syn::kAbilityA + 50}), "...and the option's ability joins the tree's abilities");
    talents::Book g(*cat, syn::kClass, 25);
    p = g.plan(build_of({{987654321, syn::kClassTree, true}}));
    CHECK(refused_with(p, "entry 1 (rank Guid 987654321, tree Guid 1001, LevelingType Guid 2001) names a rank that is not in tree ClassTree"),
          "the refusal of a rank the tree does not hold names the entry's rank, tree and LevelingType Guids");
}

static void test_known_abilities_synthetic() {
    std::printf("KnownAbilities and the records, synthetic\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    talents::Book b(*cat, syn::kClass, 25);
    run(b, {{syn::rB, syn::kClassTree, true}, {syn::rD, syn::kClassTree, true}});
    const std::vector<int64_t> tail = {501, 502, 503};
    const std::vector<int64_t> carried = {syn::kAbilityC + 1000, 7777};
    const talents::AbilityLists l = talents::join_abilities(b, 100, 4242, carried, tail);
    CHECK(l.known == (std::vector<int64_t>{100, syn::kAbilityA, syn::kAbilityB, syn::kAbilityProf, 4242, 501, 502, 503}),
          "known: Unstuck, the class tree's abilities, the professions', the combo of the weapon in hand, the static tail");
    CHECK(l.records == (std::vector<int64_t>{100, syn::kAbilityA, syn::kAbilityB, syn::kAbilityProf, 4242, carried[0], 7777, 501, 502, 503}),
          "records: the same with the combos of the other weapons carried after the combo, before the tail");
    const talents::AbilityLists none = talents::join_abilities(b, 100, 0, {}, tail);
    CHECK(none.known.size() == 7 && none.records == none.known, "no weapon in hand and none carried: no combo, no extra");
    const talents::AbilityLists dup = talents::join_abilities(b, 100, syn::kAbilityA, std::vector<int64_t>{syn::kAbilityA, 501}, tail);
    CHECK(std::count(dup.known.begin(), dup.known.end(), syn::kAbilityA) == 1 && dup.records.size() == dup.known.size(), "an ability twice is listed once, a carried combo already known adds nothing");
    const std::vector<int64_t> item_abilities = {9100, 7777, 9100};
    const talents::AbilityLists with_items = talents::join_abilities(b, 100, 4242, carried, tail, item_abilities);
    CHECK(with_items.known == l.known, "an item's ability is in no KnownAbilities (retail's login: Summon_Mount is in the records only)");
    CHECK(with_items.records == (std::vector<int64_t>{100, syn::kAbilityA, syn::kAbilityB, syn::kAbilityProf, 4242, carried[0], 7777, 9100, 501, 502, 503}),
          "the records: the combos carried, then the items' abilities (once each, one already listed adds nothing), then the tail");
    CHECK(talents::join_abilities(b, 100, 4242, carried, tail, {}).records == l.records, "no item ability: the lists are as before");
    b.set_class(syn::kOtherClass);
    const talents::AbilityLists other = talents::join_abilities(b, 100, 0, {}, tail);
    CHECK(other.known == (std::vector<int64_t>{100, 9900, syn::kAbilityProf, 501, 502, 503}), "after a class change: the new class's tree, the old class's abilities gone");
}

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

void run_talents_part2() {
    test_lists_and_mods();
    test_slots_synthetic();
    test_known_abilities_synthetic();
    test_real_catalog();
    test_real_join_lists();
}
