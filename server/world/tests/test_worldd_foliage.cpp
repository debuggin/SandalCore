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

struct TsvPos {
    double x = 100;
    double y = 200;
    double z = 300;
};

static std::string tsv_line(const char *def, const char *owner, const char *comp, int index, TsvPos p = {}) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "1\t%s\tCELL\t%s\t%s\t%d\t%.1f\t%.1f\t%.1f\t0\t0\t0\t1\t1\t1\tmesh\t\n", def, owner, comp, index, p.x, p.y, p.z);
    return buf;
}

static bool tsv_parse_fails(const std::string &body, const char *want) {
    std::string e;
    const auto bad = aoc::worldd::foliage::FoliageMap::parse(body, e);
    return !bad && e.find(want) != std::string::npos;
}

struct WantStart {
    int capture;
    const char *def;
    double x;
    double y;
    double z;
};

static bool match_want_start(const aoc::worldd::foliage::FoliageMap &m, const capture_foliage::Start &s, const WantStart &w) {
    if (s.capture != w.capture) {
        return false;
    }
    const auto row = m.find(s.owner, s.component, s.has_instance ? s.instance : 0u);
    if (!row) {
        return false;
    }
    const auto &r = m.rows()[*row];
    return m.def_names()[r.def] == w.def && std::fabs(r.x - w.x) < 0.2 && std::fabs(r.y - w.y) < 0.2 && std::fabs(r.z - w.z) < 0.2;
}

static bool check_real_cell_centre(const aoc::worldd::foliage::FoliageMap &m) {
    for (const auto &o : capture_foliage::kIfaOpens) {
        if (std::string(o.name) != "InstancedFoliageActor_25600_-40_19_0") {
            continue;
        }
        const auto owner = m.owner_of(o.name);
        if (!owner) {
            return false;
        }
        double cx = 0;
        double cy = 0;
        m.cell_center(*owner, cx, cy);
        return static_cast<int64_t>(cx) == o.location[0] && static_cast<int64_t>(cy) == o.location[1] && m.owners()[*owner].components.size() == o.component_count;
    }
    return false;
}

static aoc::worldd::foliage::DefTimes oak() {
    aoc::worldd::foliage::DefTimes d;
    d.paint = aoc::worldd::foliage::kAdult;
    d.empty_seconds = 300;
    d.has[0] = d.has[1] = d.has[2] = true;
    d.life[0] = 1800;
    d.life[1] = 1800;
    d.life[2] = 0;
    return d;
}

static aoc::worldd::foliage::DefTimes plant() {
    aoc::worldd::foliage::DefTimes d;
    d.paint = aoc::worldd::foliage::kJuvenile;
    d.empty_seconds = 1800;
    d.has[0] = d.has[1] = true;
    d.life[0] = 300;
    return d;
}

static aoc::worldd::foliage::DefTimes stone() {
    aoc::worldd::foliage::DefTimes d;
    d.paint = aoc::worldd::foliage::kAdult;
    d.empty_seconds = 840;
    d.has[2] = true;
    return d;
}

struct SeenItem {
    uint64_t record;
    int quantity;
    int quality;
};

struct RewardCase {
    const char *what;
    const char *def;
    int state;
    std::set<int> captures;
};

static const aoc::worldd::foliage::DefInfo *find_def_by_prefix(World &w, const char *prefix, bool exact) {
    for (uint32_t r = 0; r < w.f->map().rows().size(); ++r) {
        const std::string &n = w.f->name_of(r);
        if (exact ? (n == prefix) : (n.rfind(prefix, 0) == 0)) {
            return &w.f->def_of(r);
        }
    }
    return nullptr;
}

static std::string temp_dup_tsv_path() {
    const char *d = std::getenv("TEMP");
    if (!d || !*d) {
        d = std::getenv("TMP");
    }
    return (d && *d ? std::string(d) + "/" : std::string()) + "aoc_foliage_dup.tsv";
}

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
            test_duplicate_name(w);
            run_foliage_cells_tests(w, db, tsv);
        }
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
