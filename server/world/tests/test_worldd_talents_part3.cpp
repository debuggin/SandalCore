// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_talents_common.h"
#include "retail_purchases.h"

namespace content = aoc::worldd::content;
namespace talents = aoc::worldd::talents;
namespace repl = aoc::protocol::repl;

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

void run_talents_part3() {
    test_real_purchases();
    test_real_slots();
    test_retail_requests();
}
