// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_talents_common.h"

namespace content = aoc::worldd::content;
namespace talents = aoc::worldd::talents;
namespace repl = aoc::protocol::repl;

static void test_catalog_synthetic() {
    std::printf("the catalog\n");
    const auto cat = synthetic_catalog();
    if (!cat) return;
    CHECK(cat->tree_count() == 6 && cat->universal() && cat->universal()->guid == syn::kUniversalTree, "six trees, the Universal tree found by its kind");
    CHECK(cat->class_tree(syn::kClass) && cat->class_tree(syn::kClass)->guid == syn::kClassTree && !cat->class_tree(12345) && cat->class_tree(syn::kOtherClass)->guid == syn::kOtherClassTree,
          "class_tree: a class's tree by the class record, none for a guid that is no class");
    CHECK(cat->professions().size() == 1 && cat->professions()[0]->guid == syn::kProfessionTree, "the profession trees with a rank from the start: the one with an auto-granted node");
    CHECK(cat->find(syn::kClassTree, syn::rB).ok() && cat->find(syn::kClassTree, syn::rB).node->name == std::string("B_needs_A") && !cat->find(syn::kClassTree, syn::rW).ok() &&
              !cat->find(999, syn::rB).ok(),
          "find: a rank in the tree it is in; not in another tree, none in a tree that is none");
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

int main() {
    test_catalog_synthetic();
    test_points_and_costs();
    test_gates();
    test_requests();
    run_talents_part2();
    run_talents_part3();
    run_talents_part4();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
