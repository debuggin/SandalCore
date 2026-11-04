// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#ifndef TALENT_FIXTURE_PATH
#define TALENT_FIXTURE_PATH "build/gen/world/tests/talent_fixture.sql"
#endif

#include "test_worldd_talents_common.h"
#include "retail_purchases.h"

namespace fs = std::filesystem;
namespace content = aoc::worldd::content;
namespace talents = aoc::worldd::talents;
namespace stats = aoc::worldd::stats;
namespace actionbar = aoc::worldd::actionbar;
namespace repl = aoc::protocol::repl;
namespace cf = content_fixture;

static void test_catalog_synthetic() {
    std::printf("the catalog\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    CHECK(cat->tree_count() == 6 && cat->universal() && cat->universal()->guid == syn::kUniversalTree, "six trees, the Universal tree found by its kind");
    CHECK(cat->class_tree(syn::kClass) && cat->class_tree(syn::kClass)->guid == syn::kClassTree && !cat->class_tree(12345) && cat->class_tree(syn::kOtherClass)->guid == syn::kOtherClassTree,
          "class_tree: a class's tree by the class record, none for a guid that is no class");
    CHECK(cat->professions().size() == 1 && cat->professions()[0]->guid == syn::kProfessionTree, "the profession trees with a rank from the start: the one with an auto-granted node");
    CHECK(cat->find(syn::kClassTree, syn::rB).ok() && cat->find(syn::kClassTree, syn::rB).node->name == std::string("B_needs_A") && !cat->find(syn::kClassTree, syn::rW).ok() &&
              !cat->find(999, syn::rB).ok(), "find: a rank in the tree it is in; not in another tree, none in a tree that is none");
    CHECK(cat->find(syn::kClassTree, syn::rG2).rank->ord == 1 && cat->find(syn::kClassTree, syn::rG2).node->name == std::string("G_two_ranks"), "the second rank of a skill is found with its node");
    CHECK(cat->point_type(talents::Point::Archetype) == syn::kArchetype && cat->point_type(talents::Point::Stamina) == syn::kStamina && cat->point_type(talents::Point::Weapon) == syn::kWeapon,
          "the three point types by name");
    CHECK(cat->earned(syn::kAdventuring, 1, syn::kArchetype) == 0 && cat->earned(syn::kAdventuring, 2, syn::kArchetype) == 1 && cat->earned(syn::kAdventuring, 25, syn::kStamina) == 24 &&
              cat->earned(syn::kAdventuring, 99, syn::kStamina) == 24 && cat->earned(syn::kWeaponType, 25, syn::kWeapon) == 24 && cat->earned(777, 25, syn::kWeapon) == 0,
          "earned: nothing at level 1, one of each Adventuring type per level from 2, 24 by level 25, nothing for a type that is none");
    const auto a1 = cat->auto_ranks(*cat->class_tree(syn::kClass), 1);
    const auto a0 = cat->auto_ranks(*cat->class_tree(syn::kClass), 0);
    CHECK(a1.size() == 1 && a1[0].rank->guid == syn::rA && a0.empty(), "auto_ranks: the class tree's one auto skill at level 1 (listed twice in the data, granted once), none at level 0");
    CHECK(cat->auto_ranks(*cat->universal(), 1).empty() && cat->auto_ranks(*cat->universal(), 2).size() == 1, "the Universal tree's auto node at level 2");

    std::string error;
    content::TalentData bad = syn::data();
    bad.point_types.erase(content::kPointWeapon);
    CHECK(!talents::Catalog::from(bad, error) && contains(error, content::kPointWeapon), "no Combat_Weapon point type: refused, naming it");
    bad = syn::data();
    bad.trees[1].kind = "other";
    CHECK(!talents::Catalog::from(bad, error) && contains(error, "Universal"), "no Universal tree: refused");
}

static void test_points_and_costs() {
    std::printf("points and costs\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    talents::Book b(*cat, syn::kClass, 1);
    CHECK(b.points_left(*cat->class_tree(syn::kClass), syn::kArchetype) == 0, "a level-1 character has no points");
    auto p = run(b, {{syn::rB, syn::kClassTree, true}});
    CHECK(refused_with(p, "costs 1 point(s) and the character has 0 left"), "buying a rank with no points is refused, and says what is left");
    b.set_level(2);
    p = run(b, {{syn::rB, syn::kClassTree, true}});
    CHECK(p.ok && p.changes.size() == 1 && p.after.at(syn::kClassTree) == std::vector<int64_t>{syn::rB} && b.points_left(*cat->class_tree(syn::kClass), syn::kArchetype) == 0,
          "at level 2 one point buys one rank and none is left");
    p = run(b, {{syn::rC, syn::kClassTree, true}});
    CHECK(refused_with(p, "0 left") && b.purchased(syn::kClassTree).size() == 1, "a second rank is refused: nothing left, nothing changed");
    p = run(b, {{syn::rB, syn::kClassTree, false}});
    CHECK(p.ok && b.purchased(syn::kClassTree).empty() && b.points_left(*cat->class_tree(syn::kClass), syn::kArchetype) == 1, "a refund gives the point back");
    b.set_level(3);
    const auto before = b.purchased();
    p = run(b, {{syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}, {syn::rF + 0, syn::kClassTree, true}});
    CHECK(!p.ok && b.purchased() == before, "three ranks for two points: the whole request is refused and nothing is bought");
    p = run(b, {{syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}});
    CHECK(p.ok && b.purchased(syn::kClassTree) == (std::vector<int64_t>{syn::rB, syn::rC}), "two ranks for two points, kept in the order of the request");
    talents::Book u(*cat, syn::kClass, 2);
    p = run(u, {{syn::rUbuy, syn::kUniversalTree, true}});
    CHECK(refused_with(p, "costs 2 point(s)"), "a Universal rank costs 2 Stamina points: one level's worth is not enough");
    u.set_level(3);
    p = run(u, {{syn::rUbuy, syn::kUniversalTree, true}});
    CHECK(p.ok && u.purchased(syn::kUniversalTree) == std::vector<int64_t>{syn::rUbuy} && u.points_left(*cat->universal(), syn::kStamina) == 0 &&
              u.points_left(*cat->class_tree(syn::kClass), syn::kArchetype) == 2,
          "at level 3 two Stamina points buy it; the Archetype points are another pool");
    talents::Book w(*cat, syn::kClass, 2);
    p = run(w, {{syn::rW, syn::kWeaponTree, true}});
    CHECK(refused_with(p, "costs 2 point(s)"), "a weapon rank costs 2 Weapon points");
    w.set_level(25);
    p = run(w, {{syn::rW, syn::kWeaponTree, true}, {syn::rWeaponMod, syn::kWeaponTree, true}});
    CHECK(p.ok && w.points_left(*cat->tree(syn::kWeaponTree), syn::kWeapon) == 24 - 3, "at level 25 both weapon ranks are bought: 3 of the 24 Weapon points are spent");
}

static void test_gates_level_and_prereqs(const talents::Catalog& cat) {
    talents::Book b(cat, syn::kClass, 25);
    talents::Book low(cat, syn::kClass, 4);
    auto p = run(low, {{syn::rF, syn::kClassTree, true}});
    CHECK(refused_with(p, "needs level 5 (the character is level 4)"), "LevelRequirement: a level-5 rank at level 4 is refused");
    low.set_level(5);
    CHECK(run(low, {{syn::rF, syn::kClassTree, true}}).ok, "...and bought at 5");
    p = run(b, {{syn::rE, syn::kClassTree, true}});
    CHECK(refused_with(p, "needs 2 point(s) spent in the tree (0 are)"), "PointRequirement: 2 points must be spent in the tree first");
    p = run(b, {{syn::rC, syn::kClassTree, true}});
    p = run(b, {{syn::rE, syn::kClassTree, true}});
    CHECK(refused_with(p, "(1 are)"), "...one spent is not enough");
    CHECK(run(b, {{syn::rB, syn::kClassTree, true}, {syn::rE, syn::kClassTree, true}}).ok, "...the second purchase of one request counts what the first spent");
    talents::Book fresh(cat, syn::kClass, 25);
    CHECK(run(fresh, {{syn::rB, syn::kClassTree, true}}).ok, "SkillsRequired: B needs the auto-granted A, which a level-25 character has");
    talents::Book l1(cat, syn::kClass, 1);
    CHECK(refused_with(l1.plan(build_of({{syn::rB, syn::kClassTree, true}})), "0 left"), "...at level 1 the prerequisite is met and the points are the problem");
    talents::Book h(cat, syn::kClass, 25);
    p = run(h, {{syn::rH, syn::kClassTree, true}});
    CHECK(refused_with(p, "prerequisite"), "bAllAreRequired: H needs B and C, owning neither is refused");
    run(h, {{syn::rB, syn::kClassTree, true}});
    p = run(h, {{syn::rH, syn::kClassTree, true}});
    CHECK(refused_with(p, "prerequisite"), "...owning only B is refused");
    run(h, {{syn::rC, syn::kClassTree, true}});
    CHECK(run(h, {{syn::rH, syn::kClassTree, true}}).ok, "...owning B and C buys it");
    talents::Book set(cat, syn::kClass, 25);
    p = run(set, {{syn::rH, syn::kClassTree, true}, {syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}});
    CHECK(p.ok && set.purchased(syn::kClassTree) == (std::vector<int64_t>{syn::rH, syn::rB, syn::rC}), "a request that lists H before the B and C it needs is accepted; the list keeps the request's order");
    p = run(set, {{syn::rE, syn::kClassTree, true}, {syn::rD, syn::kClassTree, true}, {syn::rF, syn::kClassTree, true}});
    CHECK(refused_with(p, "excludes") && set.purchased(syn::kClassTree).size() == 3, "...but a rank that is never met is still refused: D excludes the owned C, and nothing of the request is bought");
    talents::Book any(cat, syn::kClass, 25);
    p = run(any, {{syn::rI, syn::kClassTree, true}});
    CHECK(refused_with(p, "prerequisite"), "any of B or E: owning neither is refused");
    run(any, {{syn::rB, syn::kClassTree, true}});
    CHECK(run(any, {{syn::rI, syn::kClassTree, true}}).ok, "...owning one of them buys it");
    CHECK(run(fresh, {{syn::rJ, syn::kClassTree, true}}).ok, "a requirement that names nothing is met");
}

static void test_gates_exclusives_and_ranks(const talents::Catalog& cat) {
    const talents::Tree& ct = *cat.class_tree(syn::kClass);
    talents::Book x(cat, syn::kClass, 25);
    run(x, {{syn::rC, syn::kClassTree, true}});
    auto p = run(x, {{syn::rD, syn::kClassTree, true}});
    CHECK(refused_with(p, "excludes a skill the character owns"), "MutuallyExclusiveSkills: D excludes C, which is owned");
    talents::Book y(cat, syn::kClass, 25);
    run(y, {{syn::rD, syn::kClassTree, true}});
    p = run(y, {{syn::rC, syn::kClassTree, true}});
    CHECK(refused_with(p, "excludes"), "...and the other way round");
    p = run(y, {{syn::rC, syn::kClassTree, true}, {syn::rD, syn::kClassTree, false}});
    CHECK(p.ok && y.purchased(syn::kClassTree) == std::vector<int64_t>{syn::rC}, "a swap in one request (the refund of D first) buys C");
    talents::Book both(cat, syn::kClass, 25);
    p = run(both, {{syn::rC, syn::kClassTree, true}, {syn::rD, syn::kClassTree, true}});
    CHECK(!p.ok && both.purchased().empty(), "both exclusive ranks in one request: refused whole");
    talents::Book g(cat, syn::kClass, 25);
    p = run(g, {{syn::rG2, syn::kClassTree, true}});
    CHECK(refused_with(p, "earlier rank is not bought"), "the second rank before the first is refused");
    CHECK(run(g, {{syn::rG1, syn::kClassTree, true}, {syn::rG2, syn::kClassTree, true}}).ok && g.points_left(ct, syn::kArchetype) == 24 - 3, "the two ranks in order, in one request: 1 + 2 points");
    p = run(g, {{syn::rG1, syn::kClassTree, false}});
    CHECK(refused_with(p, "without its earlier rank") && g.purchased(syn::kClassTree).size() == 2, "refunding the first rank of a skill whose second stays is refused");
    CHECK(run(g, {{syn::rG2, syn::kClassTree, false}, {syn::rG1, syn::kClassTree, false}}).ok && g.purchased(syn::kClassTree).empty(), "refunding both ranks of the skill is fine");
}

static void test_gates() {
    std::printf("the gates\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    test_gates_level_and_prereqs(*cat);
    test_gates_exclusives_and_ranks(*cat);
}

static void test_requests_validation(const talents::Catalog& cat) {
    talents::Book b(cat, syn::kClass, 25);
    auto p = b.plan(repl::SkillBuild{});
    CHECK(refused_with(p, "no rank"), "an empty request is refused");
    p = run(b, {{syn::rB, syn::kClassTree, true}, {syn::rB, syn::kClassTree, true}});
    CHECK(refused_with(p, "repeats") && b.purchased().empty(), "a rank twice in one request is refused whole");
    run(b, {{syn::rB, syn::kClassTree, true}});
    p = run(b, {{syn::rB, syn::kClassTree, true}});
    CHECK(refused_with(p, "bought already"), "a rank that is bought already");
    p = run(b, {{syn::rA, syn::kClassTree, true}});
    CHECK(refused_with(p, "granted by the level"), "the auto-granted rank cannot be bought");
    p = run(b, {{syn::rA, syn::kClassTree, false}});
    CHECK(refused_with(p, "granted by the level") && refused_with(p, "refunded"), "...nor refunded: no refund of an auto-granted rank");
    p = run(b, {{syn::rC, syn::kClassTree, false}});
    CHECK(refused_with(p, "not bought"), "a refund of a rank that is not bought");
    repl::SkillBuild bad = build_of({{syn::rB, syn::kClassTree, true}});
    bad.changes[0].rank.type_id = 12345;
    CHECK(refused_with(b.plan(bad), "no skill rank"), "a rank whose TypeId is none of a skill rank's");
    bad = build_of({{syn::rC, syn::kClassTree, true}});
    bad.changes[0].tree.type_id = content::kTypeSkillRank;
    CHECK(refused_with(b.plan(bad), "no skill tree"), "a tree whose TypeId is none of a skill tree's");
    CHECK(refused_with(b.plan(build_of({{syn::rC, 999999, true}})), "does not know"), "an unknown tree");
    CHECK(refused_with(b.plan(build_of({{999999, syn::kClassTree, true}})), "not in tree"), "an unknown rank");
    CHECK(refused_with(b.plan(build_of({{syn::rW, syn::kClassTree, true}})), "not in tree"), "a rank of another tree, named in this one");
    bad = build_of({{syn::rC, syn::kClassTree, true}});
    bad.changes[0].tree.name = repl::NameValue::plain("Prototype_Assassin_SkillTable");
    bad.changes[0].rank.name = repl::NameValue::plain("anything");
    CHECK(b.plan(bad).ok, "the Name of the ids is never read: a tree is its Guid");
    CHECK(refused_with(b.plan(build_of({{syn::rOther, syn::kOtherClassTree, true}})), "cannot be bought here"), "another class's tree");
    CHECK(refused_with(b.plan(build_of({{syn::rProf, syn::kProfessionTree, true}})), "cannot be bought here"), "a profession tree");
    CHECK(refused_with(b.plan(build_of({{syn::rOther + 51, syn::kStageTree, true}})), "cannot be bought here"), "a staging tree");
    talents::Book other(cat, syn::kOtherClass, 25);
    CHECK(other.plan(build_of({{syn::rOther, syn::kOtherClassTree, true}})).ok, "...but it is bought by a character of that class");
    repl::SkillBuild huge;
    for (size_t i = 0; i < talents::kMaxBuildChanges + 1; ++i) huge.changes.push_back(build_of({{syn::rB, syn::kClassTree, false}}).changes[0]);
    CHECK(refused_with(b.plan(huge), "at most"), "a request over the cap is refused before anything else");
}

static void test_requests_respec_and_swaps(const talents::Catalog& cat) {
    talents::Book r(cat, syn::kClass, 25);
    run(r, {{syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}, {syn::rE, syn::kClassTree, true}});
    const std::vector<int64_t> list_before = r.list(*cat.class_tree(syn::kClass));
    CHECK(list_before == (std::vector<int64_t>{syn::rA, syn::rB, syn::rC, syn::rE}), "the tree's list: the auto-granted rank then the bought ones in purchase order");
    auto p = run(r, {{syn::rE, syn::kClassTree, false}, {syn::rB, syn::kClassTree, false}, {syn::rC, syn::kClassTree, false}});
    CHECK(p.ok && r.purchased().empty() && r.list(*cat.class_tree(syn::kClass)) == std::vector<int64_t>{syn::rA} && build_of({{syn::rB, syn::kClassTree, false}}).refund_only(),
          "a respec: every bought rank refunded, the auto-granted one stays in the list");
    talents::Book d(cat, syn::kClass, 25);
    run(d, {{syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}, {syn::rH, syn::kClassTree, true}});
    p = run(d, {{syn::rB, syn::kClassTree, false}});
    CHECK(refused_with(p, "without its prerequisite") && d.purchased(syn::kClassTree).size() == 3, "refunding B while H, which needs it, stays: refused, nothing changes");
    CHECK(run(d, {{syn::rH, syn::kClassTree, false}, {syn::rB, syn::kClassTree, false}}).ok, "...refunding H with it is fine");
    talents::Book s(cat, syn::kClass, 3);
    run(s, {{syn::rB, syn::kClassTree, true}, {syn::rC, syn::kClassTree, true}});
    p = run(s, {{syn::rF + 0, syn::kClassTree, true}, {syn::rB, syn::kClassTree, false}});
    CHECK(refused_with(p, "needs level 5"), "(a swap names the purchase first: the level is judged all the same)");
    p = run(s, {{syn::rG1, syn::kClassTree, true}, {syn::rB, syn::kClassTree, false}});
    CHECK(p.ok && p.changes.size() == 2 && !p.changes[0].purchase && p.changes[1].purchase && s.purchased(syn::kClassTree) == (std::vector<int64_t>{syn::rC, syn::rG1}),
          "a swap: the refund is applied first, so its point pays for the purchase; the changes come refunds first");
    talents::Book m(cat, syn::kClass, 25);
    p = run(m, {{syn::rB, syn::kClassTree, true}, {syn::rUbuy, syn::kUniversalTree, true}, {syn::rW, syn::kWeaponTree, true}});
    CHECK(p.ok && p.trees == (std::vector<int64_t>{syn::kClassTree, syn::kUniversalTree, syn::kWeaponTree}) && p.after.size() == 3, "a request over three trees: all judged, all applied");
    talents::Book n(cat, syn::kClass, 25);
    const auto pl = n.plan(build_of({{syn::rB, syn::kClassTree, true}}));
    CHECK(pl.ok && n.purchased().empty(), "plan() changes nothing; apply() does");
    n.apply(pl);
    CHECK(n.purchased(syn::kClassTree) == std::vector<int64_t>{syn::rB}, "apply() stores the ranks");
    talents::Plan refused;
    n.apply(refused);
    CHECK(n.purchased(syn::kClassTree) == std::vector<int64_t>{syn::rB}, "apply() of a refused plan does nothing");
}

static void test_requests() {
    std::printf("what a request may do\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    test_requests_validation(*cat);
    test_requests_respec_and_swaps(*cat);
}

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

int main() {
    test_catalog_synthetic();
    test_points_and_costs();
    test_gates();
    test_requests();
    test_lists_and_mods();
    test_slots_synthetic();
    test_known_abilities_synthetic();
    run_talents_retail_tests();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
