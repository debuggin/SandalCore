// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_foliage_common.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace fol = aoc::worldd::foliage;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using std::chrono::seconds;

static void check_map_synthetic_heights_and_damaged() {
    std::string heights = "# t\n";
    heights += tsv_line("A", "InstancedFoliageActor_25600_-31_16_0", "IntrepidGatherableFISMC_0", 0, {1, 2, 12024.0});
    heights += tsv_line("A", "InstancedFoliageActor_25600_-31_16_0", "IntrepidGatherableFISMC_0", 1, {1, 2, 13591.0});
    heights += tsv_line("A", "InstancedFoliageActor_25600_-30_16_0", "IntrepidGatherableFISMC_0", 0, {1, 2, 11064.6});
    heights += tsv_line("A", "InstancedFoliageActor_25600_-30_16_0", "IntrepidGatherableFISMC_0", 1, {1, 2, 14909.1});
    std::string e2;
    const auto hm = fol::FoliageMap::parse(heights, e2);
    const bool h_ok = hm && hm->owners().size() == 2 && hm->owners()[0].centre_z_tenths() == 128075 && hm->owners()[0].centre_z_cm() == 12807 && hm->owners()[1].centre_z_tenths() == 129868 && hm->owners()[1].centre_z_cm() == 12987;
    CHECK(h_ok, "retail's cells: 12807.5 cm is 128075 tenths and 12807 cm (a tie down), 12986.85 is 129868 tenths (cut at the tenth) and 12987 cm");
    CHECK(tsv_parse_fails("# c\nonly\tthree\tcolumns\n", "line 2: 3 columns"), "a line with the wrong column count stops the load naming it");
    CHECK(tsv_parse_fails(tsv_line("A", "InstancedFoliageActor_25600_1_1_0", "C", 0) + "1\tA\tCELL\tInstancedFoliageActor_25600_1_1_0\tC\tx\t1\t2\t3\t0\t0\t0\t1\t1\t1\tm\t\n", "line 2: the index is no number"), "an index that is no number");
    CHECK(tsv_parse_fails("1\tA\tCELL\tInstancedFoliageActor_25600_1_1_0\tC\t0\t1\tnan\t3\t0\t0\t0\t1\t1\t1\tm\t\n", "the position is no number"), "a position that is no number");
    CHECK(tsv_parse_fails(tsv_line("A", "NotAnOwner", "C", 0), "no InstancedFoliageActor_25600"), "an owner that is no cell actor");
    CHECK(tsv_parse_fails(tsv_line("A", "InstancedFoliageActor_25600_1_1_0", "C", 0) + tsv_line("B", "InstancedFoliageActor_25600_1_1_0", "C", 0), "listed twice"), "the same instance twice");
    CHECK(tsv_parse_fails("# nothing\n", "holds no instance"), "a file with no instance");
    std::string error;
    CHECK(!fol::FoliageMap::load("no/such/file.tsv", error) && error.find("cannot open") != std::string::npos, "a missing file says so");
}

static void test_map_synthetic() {
    std::printf("the map: a small file, and the damaged ones\n");
    std::string text = "# region_foliage v2\n# def_id\tdef_name\t...\n";
    text += tsv_line("Tree_Oak_Large_1", "InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_10", 0, {10, 20, 30});
    text += tsv_line("Tree_Oak_Large_1", "InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_2", 5, {11, 21, 31});
    text += tsv_line("Plant_Daffodil_2", "InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_1", 0, {12, 22, 32});
    text += tsv_line("Plant_Daffodil_2", "InstancedFoliageActor_25600_3_-4_1", "IntrepidGatherableFISMC_0", 7, {13, 23, 33});
    std::string error;
    const auto m = fol::FoliageMap::parse(text, error);
    CHECK(m.has_value(), "a file of four instances loads");
    if (!m) {
        return;
    }
    CHECK(m->rows().size() == 4 && m->def_names().size() == 2 && m->owners().size() == 2 && m->components() == 4, "4 instances, 2 defs, 2 owners, 4 components");
    const auto &cell = m->owners()[0];
    CHECK(cell.name == "InstancedFoliageActor_25600_-41_19_0" && cell.cell_x == -41 && cell.cell_y == 19, "the owner's cell comes from its name (negative numbers too)");
    CHECK(cell.components == std::vector<std::string>({"IntrepidGatherableFISMC_1", "IntrepidGatherableFISMC_10", "IntrepidGatherableFISMC_2"}), "an owner's components are sorted by name: _1, _10, _2 (byte order, as the retail opens list them)");
    CHECK(cell.z_lo_tenths == 300 && cell.z_hi_tenths == 320 && cell.centre_z_tenths() == 310 && cell.centre_z_cm() == 31, "the owner's height is the middle of its lowest and highest instance: in tenths of a cm, then the nearest cm");
    CHECK(m->find("InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_2", 5) == 1u && m->find("InstancedFoliageActor_25600_3_-4_1", "IntrepidGatherableFISMC_0", 7) == 3u, "an instance is found by its owner, component and index");
    const bool not_found = !m->find("InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_2", 4) && !m->find("InstancedFoliageActor_25600_-41_19_0", "IntrepidGatherableFISMC_3", 5) && !m->find("InstancedFoliageActor_25600_9_9_0", "IntrepidGatherableFISMC_2", 5) && !m->find("IFA_NodeLayoutReplicator_108646108518008_4_0", "IntrepidGatherableFISMC_2", 12);
    CHECK(not_found, "a wrong index, component, owner and a town's runtime foliage find nothing");
    double cx = 0;
    double cy = 0;
    m->cell_center(0, cx, cy);
    CHECK(cx == (-41 + 0.5) * 25600.0 && cy == (19 + 0.5) * 25600.0, "the cell's center is ((cx + 0.5) * 25,600, (cy + 0.5) * 25,600)");
    check_map_synthetic_heights_and_damaged();
}

static void test_map_real() {
    std::printf("the map: the Riverlands' region_foliage.tsv\n");
    const char *path = tsv_path();
    if (!path) {
        std::printf("  [skip] dist/content/region_foliage.tsv is not there\n");
        return;
    }
    std::string error;
    const auto m = fol::FoliageMap::load(path, error);
    CHECK(m.has_value(), "the file loads");
    if (!m) {
        return;
    }
    CHECK(m->rows().size() == 84218 && m->def_names().size() == 73 && m->owners().size() == 671 && m->components() == 10613, "84,218 instances, 73 defs, 671 owners and 10,613 components");
    size_t pillars = 0;
    size_t max_comps = 0;
    for (const auto &o : m->owners()) {
        max_comps = std::max(max_comps, o.components.size());
    }
    CHECK(max_comps == 38, "an owner has at most 38 components");
    for (const std::string &n : m->def_names()) {
        if (n.rfind("Riverlands_", 0) == 0) {
            ++pillars;
        }
    }
    CHECK(pillars == 4, "four defs are Riverlands_ scenery pillars");
    const WantStart wants[] = {{1428, "Tree_Oak_Large_1", -1032805.5, 495823.7, 12846.0}, {1451, "Plant_Daffodil_2", -992441.0, 502971.0, 14781.1}, {1519, "Tree_Oak_Large_3", -1040048.0, 473107.0, 15217.5}};
    size_t matched = 0;
    for (const auto &s : capture_foliage::kStarts) {
        for (const WantStart &w : wants) {
            if (match_want_start(*m, s, w)) {
                ++matched;
            }
        }
    }
    CHECK(matched == 3, "1428's Oak, 1451's Daffodil and 1519's Oak are the rows of the file the client named, at its positions");
    CHECK(check_real_cell_centre(*m), "the cell (-40, 19) is where the retail open puts its state actor and has the 28 components of that open");
}

static void check_store_plant_stone_reharvest(fol::FoliageStore &s) {
    s.harvest(1, 0.0);
    CHECK(s.advance(1799.0).empty(), "a plant: Empty for 1,800 s");
    auto up = s.advance(1800.0);
    CHECK(up.size() == 1 && up[0].state == fol::kInfancy, "... then Infancy");
    up = s.advance(2100.0);
    CHECK(up.size() == 1 && up[0].state == fol::kJuvenile && up[0].painted, "... for 300 s, then Juvenile, its painted state");
    s.harvest(2, 0.0);
    up = s.advance(840.0);
    CHECK(up.size() == 1 && up[0].state == fol::kAdult && up[0].painted, "a stone: Empty for 840 s, then whole at once");
    s.harvest(3, 10000.0);
    up = s.advance(1e9);
    CHECK(up.size() == 3 && up[0].state == fol::kInfancy && up[1].state == fol::kJuvenile && up[2].state == fol::kAdult && up[2].painted && s.deviations() == 0, "a pause past the whole regrowth lists every phase in order");
    s.harvest(0, 0.0);
    s.advance(2100.0);
    CHECK(s.state_of(0) == fol::kJuvenile, "(an Oak at Juvenile)");
    const auto again = s.harvest(0, 2150.0);
    CHECK(again && again->state == fol::kEmpty && s.state_of(0) == fol::kEmpty, "a tree cut as a sapling is Empty again");
    CHECK(s.advance(2449.0).empty(), "the old timer (Adult at 3,900) is void: nothing happens before the new Empty has run");
    up = s.advance(2450.0);
    CHECK(up.size() == 1 && up[0].state == fol::kInfancy, "the new regrowth begins after its own 300 s");
    CHECK(s.next_due() && *s.next_due() > 2450.0, "the next transition is known");
    fol::FoliageStore t({0}, {oak()}, {});
    t.harvest(0, 0.0);
    const double left = t.seconds_until_whole(0, 100.0);
    t.advance(5000.0);
    CHECK(left == 3800.0 && t.seconds_until_whole(0, 5000.0) == 0.0, "the seconds left of a growing instance (200 s of Empty and the two growing phases), 0 for a whole one");
}

static void test_store() {
    std::printf("the store: regrowth with a fake clock\n");
    fol::FoliageStore s({0, 1, 2, 0}, {oak(), plant(), stone()}, {});
    CHECK(s.state_of(0) == fol::kAdult && s.state_of(1) == fol::kJuvenile && s.state_of(2) == fol::kAdult && s.deviations() == 0, "an instance starts at the state its def paints it in; nothing deviates");
    CHECK(fol::FoliageStore::chain(oak()) == std::vector<int>({0, 1, 2}) && fol::FoliageStore::chain(plant()) == std::vector<int>({0, 1}) && fol::FoliageStore::chain(stone()) == std::vector<int>({2}), "the chains: an Oak passes Infancy, Juvenile and Adult, a plant Infancy then Juvenile, a stone goes straight back");
    const auto c0 = s.harvest(0, 100.0);
    const bool c0_ok = c0 && c0->row == 0 && c0->state == fol::kEmpty && !c0->painted && s.state_of(0) == fol::kEmpty && s.deviations() == 1 && s.deviates(0);
    CHECK(c0_ok, "a harvest makes the instance Empty");
    CHECK(!s.harvest(0, 101.0) && !s.harvest(99, 101.0), "an Empty instance and an unknown one cannot be harvested");
    CHECK(s.advance(399.9).empty() && s.state_of(0) == fol::kEmpty, "Empty for 300 s");
    auto up = s.advance(400.0);
    CHECK(up.size() == 1 && up[0].state == fol::kInfancy && !up[0].painted, "at 300 s: Infancy");
    CHECK(s.advance(2199.9).empty(), "Infancy for 1800 s");
    up = s.advance(2200.0);
    CHECK(up.size() == 1 && up[0].state == fol::kJuvenile && !up[0].painted && s.state_of(0) == fol::kJuvenile, "at 2,100 s: Juvenile");
    CHECK(s.advance(3999.9).empty(), "Juvenile for 1800 s");
    up = s.advance(4000.0);
    const bool adult_ok = up.size() == 1 && up[0].state == fol::kAdult && up[0].painted && s.state_of(0) == fol::kAdult && !s.deviates(0) && s.deviations() == 0;
    CHECK(adult_ok, "at 3,900 s: Adult, its painted state, whole again and out of the list");
    CHECK(s.regrow_seconds(0) == 3900.0 && s.regrow_seconds(1) == 2100.0 && s.regrow_seconds(2) == 840.0, "an Oak takes 3,900 s from harvest to whole, a plant 2,100 s, a stone 840 s");
    check_store_plant_stone_reharvest(s);
}

static void test_store_mult() {
    std::printf("the store: the multiplier scales every phase\n");
    fol::FoliageStore s({0}, {oak()}, fol::FoliageStore::Config{0.01});
    s.harvest(0, 0.0);
    CHECK(s.advance(2.99).empty() && s.advance(3.0).size() == 1 && s.state_of(0) == fol::kInfancy, "x0.01: an Oak is Empty for 3 s");
    CHECK(s.advance(20.9).empty() && s.advance(21.0).size() == 1 && s.state_of(0) == fol::kJuvenile, "... a sapling for 18 s");
    const auto up = s.advance(39.0);
    CHECK(up.size() == 1 && up[0].painted, "... medium for 18 s, then whole (the launcher's 0.01)");
    CHECK(s.regrow_seconds(0) == 39.0, "(39 s in all)");
    fol::FoliageStore bad({0}, {oak()}, fol::FoliageStore::Config{-3.0});
    CHECK(bad.config().regrow_mult == 1.0, "a multiplier that is none is 1");
}

static void test_locks() {
    std::printf("the store: one player cuts an instance at a time\n");
    fol::FoliageStore s({0, 0}, {oak()}, {});
    CHECK(s.lock(0, 7) && s.locked_by(0) == 7 && s.locks() == 1, "the first player gets the lock");
    CHECK(s.lock(0, 7), "and gets it again (a repeated start)");
    CHECK(!s.lock(0, 8) && s.locked_by(0) == 7, "a second player is refused");
    s.unlock(0, 8);
    CHECK(s.locked_by(0) == 7, "another player cannot release it");
    CHECK(s.lock(1, 8) && s.locks() == 2, "another instance is free");
    s.unlock(0, 7);
    CHECK(s.locked_by(0) == 0 && s.lock(0, 8), "released, it is free for the next player");
    fol::DefTimes d = oak();
    d.life[0] = 0;
    fol::FoliageStore z({0}, {d}, {});
    z.harvest(0, 0.0);
    const auto up = z.advance(10000.0);
    CHECK(up.size() == 3 && up.back().painted, "a state of no time is passed at once, not waited in for ever");
}

static void check_data_reach(World &w, uint32_t oak_row) {
    struct Start {
        const char *owner;
        const char *comp;
        uint32_t index;
        int state;
        double px;
        double py;
    };
    const Start starts[] = {{kCell41_19, "IntrepidGatherableFISMC_0", 0, 2, -1032839.01, 495968.38}, {"InstancedFoliageActor_25600_-39_19_0", "IntrepidGatherableFISMC_16", 9, 1, -992420.92, 503043.62}, {"InstancedFoliageActor_25600_-41_18_0", "IntrepidGatherableFISMC_12", 1, 2, -1040165.38, 472990.0}};
    bool covered = true;
    double least = 1e9;
    for (const Start &s : starts) {
        const uint32_t row = w.row_of(s.owner, s.comp, s.index);
        const auto &r = w.f->map().rows()[row];
        const double margin = w.f->reach_cm(row, s.state) - std::hypot(r.x - s.px, r.y - s.py);
        least = std::min(least, margin);
        if (margin < 50.0) {
            covered = false;
        }
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "the reach covers 1428's, 1451's and 1519's starts (148, 75 and 166 cm away) by at least 50 cm (the least margin %.0f cm)", least);
    CHECK(covered, msg);
    CHECK(w.f->reach_cm(oak_row, 2) == 1055.0, "an Adult Oak reaches 1,055 cm: max(125, 250) + 500 + 205 + 100");
    CHECK(w.f->reach_cm(oak_row, 0) == 0.0 && w.f->reach_cm(oak_row, 3) == 0.0, "a sapling and an Empty instance have no reach");
}

static void test_data(World &w) {
    std::printf("the data: the map's defs against content.db's foliage tables\n");
    CHECK(w.f->defs() == 73 && w.f->unresolved_defs() == 0, "all 73 defs of the map resolve to a foliage_def");
    CHECK(w.f->harvestable_defs() == 69, "69 can be harvested (the four Riverlands_ pillars have no interaction)");
    CHECK(w.f->duplicated_names() == 0, "no def of the map has two records in the design data (the one repeated name, Stone_Limestone_Large_2, is no def of the Riverlands)");
    CHECK(w.g->interactions() == 236, "Gathering holds all 236 interactions");
    const uint32_t oak_row = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 0);
    const fol::DefInfo &oak_def = w.f->def_of(oak_row);
    CHECK(oak_def.name == "Tree_Oak_Large_1" && oak_def.interactions[2] && oak_def.interactions[1] && !oak_def.interactions[0], "1428's Oak: Adult and Juvenile have a harvest, a sapling none");
    const bool oak_it_ok = oak_def.interactions[2] && oak_def.interactions[2]->name == "Tree_Oak_Large" && oak_def.interactions[2]->base_duration == 15.0 && oak_def.interactions[2]->tool_slot == 29 && oak_def.interactions[1]->name == "Tree_Oak_Medium" && oak_def.interactions[1]->base_duration == 7.5;
    CHECK(oak_it_ok, "Adult: Tree_Oak_Large, 15 s, the Axe (slot 29); Juvenile: Tree_Oak_Medium, 7.5 s");
    CHECK(w.f->store().state_of(oak_row) == fol::kAdult && w.f->store().regrow_seconds(oak_row) == 3900.0, "painted Adult; 300 + 1800 + 1800 s back");
    const uint32_t daffodil_row = w.row_of("InstancedFoliageActor_25600_-39_19_0", "IntrepidGatherableFISMC_16", 9);
    const fol::DefInfo &daffodil_def = w.f->def_of(daffodil_row);
    const bool daf_ok = daffodil_def.name == "Plant_Daffodil_2" && w.f->store().state_of(daffodil_row) == fol::kJuvenile && daffodil_def.interactions[1] && daffodil_def.interactions[1]->base_duration == 11.25 && daffodil_def.interactions[1]->tool_slot == 33 && w.f->store().regrow_seconds(daffodil_row) == 2100.0;
    CHECK(daf_ok, "1451's Daffodil: painted Juvenile, 11.25 s with the Sickle (slot 33), 1800 + 300 s back");
    check_data_reach(w, oak_row);
}

static void check_reward_case(World &w, stats::CharacterStats::Evaluation &run, const std::map<int, SeenItem> &by_capture, const RewardCase &c) {
    const fol::DefInfo *def = find_def_by_prefix(w, c.def, true);
    const content::GatherDefRow *it = def ? def->interactions[c.state] : nullptr;
    if (!it) {
        return;
    }
    std::set<std::tuple<uint64_t, int, int>> outcomes;
    aoc::worldd::stats::SplitMix rng{77};
    const aoc::worldd::world::NodeSite site{};
    std::string why;
    for (int i = 0; i < 6000; ++i) {
        const auto h = w.g->harvest(*it, run, 25, site, nullptr, rng, why);
        if (h && !h->items.empty()) {
            outcomes.emplace(static_cast<uint64_t>(h->items[0].item), static_cast<int>(h->items[0].quantity), h->items[0].quality);
        }
    }
    bool all = true;
    bool one_record = true;
    for (const int cap : c.captures) {
        const auto s = by_capture.find(cap);
        if (s == by_capture.end() || outcomes.count({s->second.record, s->second.quantity, s->second.quality}) != 1) {
            all = false;
        }
    }
    const uint64_t record = outcomes.empty() ? 0 : std::get<0>(*outcomes.begin());
    for (const auto &o : outcomes) {
        if (std::get<0>(o) != record) {
            one_record = false;
        }
    }
    char msg[200];
    std::snprintf(msg, sizeof msg, "%s rolls the captured items (%zu distinct outcomes in 6,000 harvests, all of one record)", c.what, outcomes.size());
    CHECK(all && one_record, msg);
}

static void test_rewards(World &w) {
    std::printf("the rewards: the captured items from the data's tables\n");
    std::map<int, SeenItem> by_capture;
    for (const auto &it : capture_foliage::kItems) {
        by_capture[it.capture] = {it.record, static_cast<int>(it.quantity), static_cast<int>(it.quality)};
    }
    auto stats_owner = w.make_stats();
    std::string why;
    auto run = stats_owner->evaluation({}, why);
    if (!run) {
        return;
    }
    std::map<int64_t, double> ratings;
    const aoc::worldd::stats::StatBundle &bundle = *w.world->stat_bundle();
    for (size_t i = 0; i < bundle.stats.size(); ++i) {
        const std::string &n = bundle.stat_name(i);
        if (n.rfind("Artisan", 0) == 0 && n.size() > 7 && n.compare(n.size() - 7, 7, "_Rating") == 0) {
            ratings[bundle.stats[i].guid] = 8409.0;
        }
    }
    w.g->set_ratings(ratings);
    const RewardCase cases[] = {{"an Adult Oak", "Tree_Oak_Large_1", 2, {1428, 1519}}, {"a Juvenile Oak", "Tree_Oak_Large_1", 1, {1465, 1468, 1515}}, {"a Daffodil", "Plant_Daffodil_2", 1, {1429, 1451}}};
    for (const RewardCase &c : cases) {
        check_reward_case(w, *run, by_capture, c);
    }
    w.g->set_ratings({});
    const fol::DefInfo *ash = find_def_by_prefix(w, "Tree_Ash_Large", false);
    CHECK(ash && ash->interactions[2] && ash->interactions[2]->name.find("Ash") != std::string::npos, "the Ash trees have a harvest of their own (Tree_Ash_*)");
    bool lengths = true;
    for (const auto &f : capture_foliage::kFoliageData) {
        if (f.duration > 0.0f && (f.duration < 5.0f || f.duration > 15.0f)) {
            lengths = false;
        }
    }
    CHECK(lengths, "(the captured channels are 5 to 15 s: base x the speed curve)");
}

static void record_tsv_row(const std::string &raw, const std::map<ClientName, WireCell> &client, TsvRowStats &st) {
    const std::vector<std::string> col = parse_tsv_row_cols(raw);
    if (col.size() < 6) {
        return;
    }
    ++st.rows;
    bump_tsv_category(col[1], st.stones, st.trees, st.plants);
    if (col[4].rfind("IntrepidGatherableFISMC_", 0) != 0) {
        ++st.foreign;
    }
    const auto cell = client.find(client_fname(col[3]));
    if (cell == client.end()) {
        ++st.no_actor;
        return;
    }
    const auto comp = cell->second.components.find(client_fname(col[4]));
    if (comp == cell->second.components.end()) {
        ++st.no_component;
        return;
    }
    if (!comp->second) {
        ++st.uninitialized;
        return;
    }
    ++st.resolved;
    bump_tsv_category(col[1], st.stones_ok, st.trees_ok, st.plants_ok);
}

static void test_every_instance_has_its_state(World &w) {
    std::printf("the cells: every instance of the map resolves the way the client looks it up\n");
    std::map<ClientName, WireCell> client;
    size_t actors = 0;
    size_t unreadable = 0;
    size_t doubled = 0;
    size_t components = 0;
    size_t not_init = 0;
    w.world->registry().for_each([&](const world::WorldActor &a) {
        if (a.kind != world::ActorKind::Foliage) {
            return;
        }
        ++actors;
        WireCell cell;
        if (!read_cell_from_wire(a.actor, cell)) {
            ++unreadable;
            return;
        }
        components += cell.components.size();
        for (const auto &[n, init] : cell.components) {
            if (!init) {
                ++not_init;
            }
        }
        if (!client.emplace(cell.name, cell).second) {
            ++doubled;
        }
    });
    char msg[240];
    std::snprintf(msg, sizeof msg, "%zu state actors in the world, %zu read back from their bits, %zu FoliageName twice, %zu components, %zu not initialized", actors, actors - unreadable, doubled, components, not_init);
    CHECK(actors == 671 && unreadable == 0 && doubled == 0 && components == 10613 && not_init == 0 && client.size() == 671, msg);
    TsvRowStats st;
    FILE *f = std::fopen(tsv_path(), "rb");
    char buf[2048];
    while (f && std::fgets(buf, sizeof buf, f)) {
        record_tsv_row(buf, client, st);
    }
    if (f) {
        std::fclose(f);
    }
    std::snprintf(msg, sizeof msg, "%zu of the %zu rows resolve to an initialized component (%zu without a state actor, %zu without a component, %zu not initialized)", st.resolved, st.rows, st.no_actor, st.no_component, st.uninitialized);
    CHECK(st.rows == 84218 && st.resolved == st.rows, msg);
    std::snprintf(msg, sizeof msg, "... %zu of %zu trees, %zu of %zu plants, %zu of %zu stones (and the four rock pillars)", st.trees_ok, st.trees, st.plants_ok, st.plants, st.stones_ok, st.stones);
    CHECK(st.trees == 42250 && st.trees_ok == st.trees && st.plants == 21046 && st.plants_ok == st.plants && st.stones == 20922 && st.stones_ok == st.stones, msg);
    CHECK(st.foreign == 0, "every row, the 20,922 stones among them, is in an IntrepidGatherableFISMC_<n> component: the class the client's target scan accepts");
}

static void test_cells_equal_retail(World &w) {
    std::printf("the cells: an untouched cell's open against the retail opens (1198's login, 1428)\n");
    fol::FoliageCells &cells = w.f->cells();
    size_t painted = 0;
    size_t found = 0;
    size_t actor_ok = 0;
    size_t spawn_ok = 0;
    RetailCompStats cst;
    std::set<uint64_t> server_ids;
    for (const auto &open : capture_foliage::kIfaOpens) {
        const auto owner = w.f->map().owner_of(open.name);
        if (!owner) {
            continue;
        }
        ++painted;
        const world::WorldActor *a = w.world->registry().find(cells.actor_of(*owner));
        if (!a) {
            continue;
        }
        ++found;
        repl::ActorShadow shadow;
        std::vector<repl::ContentBlock> blocks;
        if (!repl::collect_open_blocks(a->actor, shadow, blocks) || blocks.size() != 1 + open.component_count) {
            continue;
        }
        if (check_retail_actor_block(*a, open, server_ids)) {
            ++actor_ok;
        }
        const auto &q = a->actor.open.spawn.location;
        if (q && q->mode == repl::QuantVec::Mode::Scaled && open.spawn_scaled && q->x == open.spawn[0] && q->y == open.spawn[1] && q->z == open.spawn[2]) {
            ++spawn_ok;
        }
        check_retail_components(*a, open, blocks, cst);
    }
    CHECK(painted == 10, "ten of the twelve retail opens are painted cells of the map (the other two are a town's runtime foliage)");
    CHECK(found == painted, "... and each has a state actor in the world from the start");
    char actor_msg[300];
    std::snprintf(actor_msg, sizeof actor_msg, "the actor block (520 bits: AuthServerIDReplicated, ReplicatedMovement, RemoteRole, Role, FoliageName) is the retail block to the last bit in each, but for that one server id (%zu values in the ten)", server_ids.size());
    CHECK(actor_ok == painted && server_ids == std::set<uint64_t>({0, 37, 48}), actor_msg);
    CHECK(spawn_ok == painted, "SerializeNewActor's location (x and y the cell's centre, z the middle of its instances' lowest and highest) is the retail location in each");
    CHECK(cst.order_ok == painted, "the components are the same, in the same byte order (_0, _1, _10 ... _19, _2 ...), in each");
    char msg[200];
    std::snprintf(msg, sizeof msg, "all %zu component blocks are the retail blocks (%zu of them with no IndexedStates: equal to the last bit; the rest up to the IndexedStates field)", cst.comps_total, cst.whole_total);
    CHECK(cst.comps_total == 166 && cst.comps_ok == cst.comps_total && cst.whole_ok == cst.whole_total && cst.whole_total > 0, msg);
}

static void check_cell_actor_meta(const world::WorldActor &a, const fol::FoliageMap &map, uint16_t owner) {
    CHECK(a.actor.subobjects.size() == map.owners()[owner].components.size() && a.actor.subobjects.size() == 28, "it has a component for each FISMC of the owner: 28, as the retail open of that cell");
    double cx = 0;
    double cy = 0;
    map.cell_center(owner, cx, cy);
    CHECK(a.x == cx && a.y == cy && a.x == -1011200.0 && a.y == 499200.0, "it stands at the cell's centre (-1,011,200, 499,200), where the retail open puts it");
    const bool arch_ok = a.actor.open.archetype.size() == 2 && a.actor.open.archetype[0].guid.ObjectId == 18292086725290287385ull && a.actor.subobjects[0].dynamic_class && a.actor.subobjects[0].dynamic_class->front().guid.ObjectId == 8959184318458680037ull;
    CHECK(arch_ok, "its archetype is Default__IntrepidStateIFA and its components are IntrepidFoliageStateComponent (the ids of the retail capture's export)");
    CHECK(name_text(a.actor.obj, "FoliageName") == "InstancedFoliageActor_25600_-40_19_0", "FoliageName is the owner");
    const bool comp_ok = name_text(a.actor.subobjects[0].obj, "FoliageComponentUUID") == "IntrepidGatherableFISMC_0" && name_text(a.actor.subobjects[1].obj, "FoliageComponentUUID") == "IntrepidGatherableFISMC_1" && name_text(a.actor.subobjects[2].obj, "FoliageComponentUUID") == "IntrepidGatherableFISMC_10";
    CHECK(comp_ok, "the components are in byte order: _0, _1, _10 ... (the retail open's)");
}

static void test_cell_actor(World &w) {
    std::printf("the cells: the state actor of the cell the retail capture opened\n");
    fol::FoliageCells &cells = w.f->cells();
    const auto &map = w.f->map();
    const uint32_t stone_row = w.row_of("InstancedFoliageActor_25600_-40_19_0", "IntrepidGatherableFISMC_18", 0);
    const uint16_t owner = map.rows()[stone_row].owner;
    CHECK(w.f->name_of(stone_row) == "Stone_Granite_Large_3", "(a Granite stone)");
    const uint64_t actor_id = cells.actor_of(owner);
    CHECK(actor_id != 0 && cells.actors() == 671 && cells.items_of(owner) == 0, "every cell has its state actor before anything in it is cut: 671, this one with no item");
    const size_t in_registry = w.world->registry().size();
    const Clock::time_point t0{seconds(1000)};
    const auto change = w.f->harvested(stone_row, t0);
    CHECK(change && change->state == fol::kEmpty, "the harvest is Empty");
    CHECK(cells.actor_of(owner) == actor_id && cells.actors() == 671 && w.world->registry().size() == in_registry, "the first cut of the cell changed its actor and made no other");
    const world::WorldActor *a = w.world->registry().find(actor_id);
    const bool flags_ok = a && a->kind == world::ActorKind::Foliage && a->snapshot_only && a->keep_fast_shadow && a->dormancy_delay == fol::kStateActorOpen && !a->never_dormant;
    CHECK(flags_ok, "it is a snapshot-only foliage actor with a 265 ms open, whose fast arrays the view keeps");
    if (!a) {
        return;
    }
    check_cell_actor_meta(*a, map, owner);
    CHECK(cells.shown_state(stone_row) == 3u && cells.items_of(owner) == 1, "one item: the instance, Empty (3)");
    const double back = w.f->store().regrow_seconds(stone_row);
    w.f->tick(after(t0, back - 1.0));
    CHECK(cells.shown_state(stone_row) == 3u, "(a stone is still Empty a second before its time)");
    const uint32_t version = w.world->registry().find(actor_id)->version;
    w.f->tick(after(t0, back + 1.0));
    CHECK(!cells.shown_state(stone_row) && cells.items_of(owner) == 0 && w.world->registry().find(actor_id)->version > version, "then it is whole: the item leaves the list and the actor's version moved");
}

static void check_cell_first_open_form(World &w, uint64_t actor_id, uint32_t oak_row) {
    const uint32_t other = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 1);
    const Clock::time_point t1{seconds(30000)};
    w.f->harvested(oak_row, t1);
    w.f->harvested(other, t1);
    const world::WorldActor *b = w.world->registry().find(actor_id);
    repl::Actor fresh = b->actor;
    b->first_open_form(fresh);
    CHECK(states_listed(fresh, 3) == 2 && states_listed(fresh, 11) == 0, "two instances cut a moment ago are a live harvest to a first look: still 3 (the client plays the fall)");
    w.f->tick(after(t1, 60));
    const uint32_t third = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 2);
    w.f->harvested(third, after(t1, 60));
    repl::Actor copy = b->actor;
    b->first_open_form(copy);
    CHECK(states_listed(copy, 11) == 2 && states_listed(copy, 3) == 1 && states_listed(b->actor, 3) == 3 && states_listed(b->actor, 11) == 0, "a minute later a first look lists the two older Empty instances as 11 and the new one as 3; the actor itself holds 3 for all");
    const repl::FastArrayState *live = nullptr;
    const repl::FastArrayState *shown = nullptr;
    for (size_t i = 0; i < b->actor.subobjects.size() && !live; ++i) {
        const auto layout = repl::fast_array_layout(*b->actor.subobjects[i].obj.schema, "IndexedStates");
        const repl::FastArrayState *l = b->actor.subobjects[i].obj.arrays.find(layout->field);
        if (l && !l->items().empty()) {
            live = l;
            shown = copy.subobjects[i].obj.arrays.find(layout->field);
        }
    }
    const bool keys_ok = live && shown && live->array_key() == shown->array_key() && live->items()[0].key == shown->items()[0].key && live->items()[0].id == shown->items()[0].id;
    CHECK(keys_ok, "and the copy keeps the array's key and the items' keys and IDs (only the value changed: what a connection has is the server's count)");
}

static void test_cell_regrowth(World &w) {
    std::printf("the cells: an Oak's items through its regrowth\n");
    fol::FoliageCells &cells = w.f->cells();
    const uint32_t oak_row = w.row_of(kCell41_19, "IntrepidGatherableFISMC_0", 0);
    const uint16_t owner = w.f->map().rows()[oak_row].owner;
    const Clock::time_point t0{seconds(20000)};
    w.f->harvested(oak_row, t0);
    const uint64_t actor_id = cells.actor_of(owner);
    CHECK(actor_id != 0 && cells.shown_state(oak_row) == 3u, "a harvest: the instance is Empty (3) in its cell's list");
    const world::WorldActor *a = w.world->registry().find(actor_id);
    CHECK(a && a->actor.subobjects.size() == 8, "the cell (-41, 19) has 8 components");
    const uint32_t v0 = a ? a->version : 0;
    w.f->tick(after(t0, 10));
    CHECK(cells.shown_state(oak_row) == 3u, "(nothing is due after 10 s)");
    w.f->tick(after(t0, 301));
    CHECK(cells.shown_state(oak_row) == 0u && w.world->registry().find(actor_id)->version > v0, "after 300 s: Infancy (0), and the actor's version moved: the clients are sent it");
    w.f->tick(after(t0, 301 + 1800));
    CHECK(cells.shown_state(oak_row) == 1u, "after 1,800 s more: Juvenile (1)");
    w.f->tick(after(t0, 301 + 3600 + 5));
    CHECK(!cells.shown_state(oak_row) && cells.items_of(owner) == 0 && w.f->store().deviations() == 0, "after 1,800 s more: whole, the item is deleted from the list");
    check_cell_first_open_form(w, actor_id, oak_row);
}

static void test_duplicate_name(World &w) {
    std::printf("the data: a def name with two records\n");
    const std::string path = temp_dup_tsv_path();
    {
        FILE *fp = std::fopen(path.c_str(), "wb");
        const std::string text = "# test\n" + tsv_line("Stone_Limestone_Large_2", "InstancedFoliageActor_25600_1_1_0", "IntrepidGatherableFISMC_0", 0) + tsv_line("Tree_Oak_Large_1", "InstancedFoliageActor_25600_1_1_0", "IntrepidGatherableFISMC_1", 0);
        if (fp) {
            std::fwrite(text.data(), 1, text.size(), fp);
            std::fclose(fp);
        }
    }
    fol::Config cfg;
    cfg.map_path = path;
    std::string error;
    world::ActorRegistry registry;
    auto f = fol::Foliage::make(*w.db, *w.g, registry, [&] {
        return w.world->mint_guid();
    },
                                cfg, error);
    CHECK(f != nullptr, "a map with the repeated name loads");
    if (f) {
        const auto range = w.g->foliage_data()->defs_by_name.equal_range("Stone_Limestone_Large_2");
        int64_t lowest = INT64_MAX;
        size_t records = 0;
        for (auto it = range.first; it != range.second; ++it, ++records) {
            lowest = std::min(lowest, it->second);
        }
        const bool dup_ok = records == 2 && f->duplicated_names() == 1 && f->def_of(0).row && f->def_of(0).row->guid == lowest && f->unresolved_defs() == 0;
        CHECK(dup_ok, "the design data has two records of it; the first by guid is the def, counted once");
        CHECK(f->def_of(0).row->empty_duration == 5700.0 && f->def_of(0).row->paint_state == fol::kAdult, "(5,700 s empty, painted Adult)");
    }
    std::remove(path.c_str());
    fol::Config missing;
    missing.map_path = "no/such/map.tsv";
    const bool miss_ok = !fol::Foliage::make(*w.db, *w.g, registry, [&] {
        return w.world->mint_guid();
    },
                                             missing, error) &&
                         error.find("cannot open") != std::string::npos && error.find("mapc extract") != std::string::npos;
    CHECK(miss_ok, "a missing map is an error that names the file and `mapc extract`");
}

static void test_load(const char *db, const char *tsv) {
    std::printf("the load: the 671 state actors made at start\n");
    World w;
    if (!w.open(db, tsv, false)) {
        return;
    }
    const size_t registry_before = w.world->registry().size();
    CHECK(w.f->cells().actors() == 0, "(the first draft's way, an actor at a cell's first cut: none yet, for the measurement)");
    const size_t ws0 = working_set_bytes();
    const auto t0 = std::chrono::steady_clock::now();
    const size_t made = w.f->cells().make_all();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const size_t ws1 = working_set_bytes();
    size_t subobjects = 0;
    size_t total_bits = 0;
    size_t max_bits = 0;
    size_t opens = 0;
    w.world->registry().for_each([&](const world::WorldActor &a) {
        if (a.kind != world::ActorKind::Foliage) {
            return;
        }
        subobjects += a.actor.subobjects.size();
        repl::ActorShadow shadow;
        std::vector<repl::ContentBlock> blocks;
        if (!repl::collect_open_blocks(a.actor, shadow, blocks)) {
            return;
        }
        size_t bits = 0;
        for (const repl::ContentBlock &b : blocks) {
            bits += b.payload.bit_pos();
        }
        total_bits += bits;
        max_bits = std::max(max_bits, bits);
        ++opens;
    });
    const double grown_mb = ws1 > ws0 ? static_cast<double>(ws1 - ws0) / (1024.0 * 1024.0) : 0.0;
    CHECK(made == 671 && subobjects == 10613 && w.world->registry().size() == registry_before + 671 && opens == 671, "671 actors with 10,613 components were made and each builds its open");
    CHECK(ms < 2000.0, "they are made in under two seconds, once, at start");
    CHECK(ws0 == 0 || grown_mb < 100.0, "and they hold under 100 MB of the process (the measurement: see above)");
}

int main() {
    spdlog::set_level(spdlog::level::warn);
    test_map_synthetic();
    test_map_real();
    test_store();
    test_store_mult();
    test_locks();
    const char *db = db_path();
    const char *tsv = tsv_path();
    if (!db || !tsv) {
        std::printf("\n[skip] the world checks need dist/content/content.db (version 15) and dist/content/region_foliage.tsv\n");
    } else {
        World w;
        if (w.open(db, tsv)) {
            test_data(w);
            test_rewards(w);
            test_every_instance_has_its_state(w);
            test_cells_equal_retail(w);
            test_cell_actor(w);
            test_cell_regrowth(w);
            test_duplicate_name(w);
        }
        test_load(db, tsv);
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
