// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_gather_common.h"

using namespace aoc::worldd;
namespace cg = capture_gather;

static gather::ToolInfo make_tool(World &w, int64_t item, float durability = 100.0f) {
    gather::ToolInfo t;
    t.worn = true;
    t.tool = w.g->tool(item);
    t.durability = durability;
    return t;
}

struct Rig {
    World &w;
    std::unique_ptr<stats::CharacterStats> cs;
    std::unique_ptr<stats::CharacterStats::Evaluation> run;
    stats::SplitMix rng;

    explicit Rig(World &world, uint64_t seed = 1)
        : w(world), cs(world.make_stats()) {
        std::string err;
        run = cs->evaluation({}, err);
        if (!run) {
            std::printf("  [FAIL] the stat run does not start: %s\n", err.c_str());
            std::exit(2);
        }
        rng.state = seed;
    }

    std::optional<gather::Harvest> harvest(int64_t def_guid, std::string *why = nullptr) {
        std::string e;
        const content::GatherDefRow *d = w.g->def(def_guid);
        auto h = w.g->harvest(*d, *run, 25, world::NodeSite{}, nullptr, rng, e);
        if (why)
            *why = e;
        return h;
    }
};

static bool valid_common_harvest(const std::optional<gather::Harvest> &h, int64_t quantity, int xp) {
    if (!h || h->items.size() != 1)
        return false;
    if (h->items[0].quantity != quantity || h->items[0].quality != 1)
        return false;
    return h->profession_xp == xp && h->primary_rarity == 0;
}

static void check_rules_promotions_and_combat(World &w, const professions::Book &book, const professions::Professions &novice, const content::GatherDefRow &basalt, const content::GatherDefRow &halcyonite) {
    const content::GatherDefRow &rividium = w.g->data().defs.at(id::kRividium);
    professions::Professions apprentice = novice;
    apprentice.raise_xp_to(id::kMining, book.max_xp(*book.find(id::kMining), 0));
    apprentice.promote(id::kMining);
    CHECK(apprentice.tier(id::kMining) == 1 && apprentice.level(id::kMining) == 10, "an Apprentice Miner at level 10 (a Promote at the Novice cap)");
    CHECK(gather::check_requirements(halcyonite, apprentice, make_tool(w, id::kT1Pickaxe), false).why == gather::Refusal::ToolTierTooLow, "an Apprentice with a T1 pickaxe at an Apprentice node: the tool's tier is too low");
    CHECK(gather::check_requirements(halcyonite, apprentice, make_tool(w, id::kT2Pickaxe), false).ok(), "with a T2 pickaxe it harvests");
    CHECK(gather::check_requirements(halcyonite, apprentice, make_tool(w, id::kT3Pickaxe), false).why == gather::Refusal::ToolNotUsable, "a T3 pickaxe needs Journeyman: the Apprentice cannot use it (world's equip rules let it be worn)");
    CHECK(gather::check_requirements(rividium, apprentice, make_tool(w, id::kT2Pickaxe), false).why == gather::Refusal::NotCertified, "an Apprentice at a Journeyman node: not certified");
    professions::Professions journeyman = apprentice;
    journeyman.raise_xp_to(id::kMining, book.max_xp(*book.find(id::kMining), 1));
    journeyman.promote(id::kMining);
    CHECK(journeyman.tier(id::kMining) == 2 && gather::check_requirements(rividium, journeyman, make_tool(w, id::kT3Pickaxe), false).ok(), "a Journeyman with the T3 pickaxe harvests a Journeyman node");
    CHECK(gather::check_requirements(halcyonite, journeyman, make_tool(w, id::kT3Pickaxe), false).ok(), "...and an Apprentice node (a higher tool reaches lower nodes)");
    CHECK(gather::check_requirements(rividium, journeyman, make_tool(w, id::kT2Pickaxe), false).why == gather::Refusal::ToolTierTooLow, "...but not a Journeyman node with a T2 pickaxe");
    content::GatherDefRow no_combat = basalt;
    no_combat.allow_in_combat = false;
    const bool combat_ok = gather::check_requirements(no_combat, novice, make_tool(w, id::kT1Pickaxe), true).why == gather::Refusal::InCombat && gather::check_requirements(basalt, novice, make_tool(w, id::kT1Pickaxe), true).ok();
    CHECK(combat_ok, "in combat: refused where the interaction does not allow it, harvested where it does");
    CHECK(gather::check_requirements(basalt, professions::Professions(), make_tool(w, id::kT1Pickaxe), false).why == gather::Refusal::NoProfessions, "a world with no profession tables refuses");
    CHECK(gather::reach_cm(basalt) == basalt.interact_radius + basalt.interact_tolerance + 100.0, "the reach is the radius, the tolerance and 100 cm");
}

static void test_rules(World &w) {
    std::printf("the requirements of a harvest\n");
    const professions::Book &book = *w.world->professions();
    professions::Professions novice(book);
    const content::GatherDefRow *basalt = w.def_named("Stone_Basalt_Large_3");
    const content::GatherDefRow *halcyonite = &w.g->data().defs.at(id::kHalcyonite);
    const content::GatherDefRow *rividium = &w.g->data().defs.at(id::kRividium);
    CHECK(basalt && basalt->cert_tier == 0 && halcyonite->cert_tier == 1 && rividium->cert_tier == 2, "Novice, Apprentice and Journeyman Mining nodes");
    CHECK(gather::check_requirements(*basalt, novice, make_tool(w, id::kT1Pickaxe), false).ok(), "a Novice with a T1 pickaxe harvests a Novice node");
    CHECK(gather::check_requirements(*basalt, novice, gather::ToolInfo{}, false).why == gather::Refusal::NoTool, "no tool in the slot: refused");
    gather::ToolInfo junk;
    junk.worn = true;
    junk.durability = 10;
    CHECK(gather::check_requirements(*basalt, novice, junk, false).why == gather::Refusal::NotATool, "a worn item that is no gathering tool: refused");
    CHECK(gather::check_requirements(*basalt, novice, make_tool(w, id::kT1Axe), false).why == gather::Refusal::WrongToolProfession, "an axe in the pickaxe's place: refused (the tool is another profession's)");
    CHECK(gather::check_requirements(*basalt, novice, make_tool(w, id::kT1Pickaxe, 0.0f), false).why == gather::Refusal::ToolBroken, "a tool with no durability: refused");
    CHECK(gather::check_requirements(*halcyonite, novice, make_tool(w, id::kT1Pickaxe), false).why == gather::Refusal::NotCertified, "a Novice at an Apprentice node: not certified");
    check_rules_promotions_and_combat(w, book, novice, *basalt, *halcyonite);
}

static void expect_dist(const char *name, const int counts[7], int n, const double want[6]) {
    bool ok = true;
    char line[512];
    int len = std::snprintf(line, sizeof line, "%s:", name);
    for (int i = 0; i < 6; ++i) {
        const double p = want[i] / 100.0;
        const double got = static_cast<double>(counts[i]) / n;
        const double tol = 3.0 * std::sqrt(p * (1 - p) / n) + 0.0005;
        if (std::fabs(got - p) > tol)
            ok = false;
        len += std::snprintf(line + len, sizeof line - static_cast<size_t>(len), " %.2f", got * 100.0);
    }
    std::snprintf(line + len, sizeof line - static_cast<size_t>(len), " (want %.2f %.2f %.2f %.2f %.3f %.3f)", want[0], want[1], want[2], want[3], want[4], want[5]);
    CHECK(ok && counts[6] == 0, line);
}

static void test_rarity(World &w) {
    std::printf("the rarity of a harvest: 100,000 harvests per case against the analytic tables\n");
    constexpr int N = 100000;
    struct Case {
        const char *name;
        int64_t def;
        double rating;
        double want[6];
    };
    const Case cases[] = {
        {"standard, rating 0", id::kMoonbell, 0, {81.9, 11.056, 3.437, 2.096, 1.01, 0.501}},
        {"standard, rating 8409", id::kMoonbell, 8409, {45.5, 43.278, 7.186, 2.481, 1.049, 0.505}},
        {"_T1_Only, rating 0", id::kDaffodil, 0, {85.365, 10.805, 2.221, 1.098, 0.51, 0.001}},
        {"_T1_Only, rating 8409", id::kDaffodil, 8409, {47.4, 44.4, 6.1, 1.5, 0.55, 0.005}},
    };
    Rig rig(w, 77);
    for (const Case &c : cases) {
        w.rate_all(c.rating);
        int counts[7] = {0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < N; ++i) {
            const auto h = rig.harvest(c.def);
            const int band = (!h || h->items.empty()) ? 6 : std::min(h->primary_rarity, 6);
            ++counts[band];
        }
        expect_dist(c.name, counts, N, c.want);
    }
    w.rate_all(0);
    w.g->set_ratings({});
}

static void test_fresh_seed() {
    std::printf("the seed of the harvest rolls: a new number on every start of the server\n");
    std::set<uint64_t> seen;
    bool never_zero = true;
    for (int i = 0; i < 64; ++i) {
        const uint64_t s = gather::fresh_seed();
        if (s == 0)
            never_zero = false;
        seen.insert(s);
    }
    CHECK(never_zero, "a fresh seed is never 0 (0 is how --gather-seed asks for a new one)");
    CHECK(seen.size() == 64, "64 fresh seeds in a row are all different");
    CHECK(gather::Config{}.seed == 0x6A7E5EEDull, "a Config made by hand keeps the fixed seed, so the tests' rolls repeat");
    stats::SplitMix a;
    stats::SplitMix b;
    a.state = gather::fresh_seed();
    b.state = gather::fresh_seed();
    bool differ = false;
    for (int i = 0; i < 8; ++i) {
        if (a.rand_double() != b.rand_double())
            differ = true;
    }
    CHECK(differ, "two fresh seeds start two different streams of rolls");
}

static void check_tier_rolls(Rig &rig, const std::vector<double> &common_roll) {
    const struct {
        int64_t def;
        const char *name;
        int xp;
        int64_t quantity;
    } tiers[] = {
        {id::kDaffodil, "Daffodil (Novice)", 80, 3},
        {id::kMoonbell, "Moonbell (Apprentice)", 90, 3},
        {id::kHalcyonite, "Halcyonite (Apprentice)", 90, 5},
        {id::kRividium, "Rividium (Journeyman)", 100, 5},
    };
    for (const auto &t : tiers) {
        rig.rng.script = &common_roll;
        rig.rng.scripted = 0;
        std::string why;
        const auto h = rig.harvest(t.def, &why);
        const bool ok = valid_common_harvest(h, t.quantity, t.xp);
        const long long got_q = (h && !h->items.empty()) ? static_cast<long long>(h->items[0].quantity) : -1ll;
        const long long got_xp = h ? static_cast<long long>(h->profession_xp) : -1ll;
        char msg[200];
        std::snprintf(msg, sizeof msg, "%s: x%lld q1, +%lld profession XP (want x%lld, +%d)", t.name, got_q, got_xp, static_cast<long long>(t.quantity), t.xp);
        CHECK(ok, msg);
        CHECK(h && h->adventuring_xp > 0, (std::string(t.name) + ": and Adventuring experience").c_str());
        CHECK(rig.rng.scripted == common_roll.size(), (std::string(t.name) + ": the three scripted draws are all it used (the quality formula's two, the quantity's one)").c_str());
    }
}

static void check_quality_readings(World &w) {
    for (const bool literal : {false, true}) {
        gather::Config cfg;
        cfg.quality_literal = literal;
        std::string error;
        auto g2 = gather::Gathering::make(*w.db, w.registry(), *w.world->ability_catalog(), cfg, error);
        CHECK(g2 != nullptr, literal ? "a Gathering with the literal reading" : "a Gathering with the band reading");
        if (!g2)
            continue;
        std::map<int64_t, double> r;
        for (const int64_t guid : w.ratings)
            r[guid] = 8409;
        g2->set_ratings(r);
        const std::vector<double> bonus = {0.99, 0.5, 0.0, 0.0};
        stats::SplitMix r2;
        r2.script = &bonus;
        const content::GatherDefRow *oak = w.def_named("Tree_Oak_Large_2");
        std::string why;
        auto run = w.make_stats();
        auto ev = run->evaluation({}, why);
        const auto x = oak ? g2->harvest(*oak, *ev, 25, world::NodeSite{}, nullptr, r2, why) : std::nullopt;
        const long long got_q = (x && !x->items.empty()) ? static_cast<long long>(x->items[0].quantity) : -1ll;
        char msg[160];
        std::snprintf(msg, sizeof msg, "an Oak at rating 8409 with a lucky quantity roll gives %lld (5 + 1), %s reading", got_q, literal ? "literal" : "band");
        const bool ok = x && x->items.size() == 1 && x->items[0].quantity == 6 && x->items[0].quality == 1;
        CHECK(ok, msg);
    }
}

static void test_rules_of_rolls(World &w) {
    std::printf("what a harvest gives: the quantity, the experience, the wear\n");
    w.g->set_ratings({});
    Rig rig(w, 5);
    const std::vector<double> common_roll = {0.99, 0.5, 0.99};
    check_tier_rolls(rig, common_roll);
    const std::vector<double> uncommon = {0.99, 0.925, 0.99};
    rig.rng.script = &uncommon;
    rig.rng.scripted = 0;
    auto h = rig.harvest(id::kMoonbell);
    const bool unc_ok = h && h->items.size() == 1 && h->items[0].quality == 6 && h->primary_rarity == 1 && h->profession_xp == 135;
    CHECK(unc_ok, "an Uncommon (q6) Moonbell gives +135 (90 x 1.5)");
    check_quality_readings(w);
    rig.rng.script = &common_roll;
    rig.rng.scripted = 0;
    h = rig.harvest(id::kDaffodil);
    CHECK(h && h->durability_loss == 1.0, "a plant wears the tool by 1");
    size_t twos = 0;
    size_t ones = 0;
    size_t others = 0;
    for (const auto &[guid, d] : w.g->data().defs) {
        rig.rng.script = nullptr;
        const auto x = rig.harvest(guid);
        if (!x)
            continue;
        if (x->durability_loss == 2.0) {
            ++twos;
        } else if (x->durability_loss == 1.0) {
            ++ones;
        } else {
            ++others;
        }
    }
    CHECK(ones > 300 && others == 0, "every node wears the tool by 1 or 2");
    std::printf("      (%zu nodes wear the tool by 2, %zu by 1)\n", twos, ones);
}

static void check_bonus_1470_odds(World &w) {
    Rig stat(w, 31);
    int coal = 0;
    int sapphire = 0;
    int iron = 0;
    constexpr int N = 60000;
    for (int i = 0; i < N; ++i) {
        const auto x = stat.harvest(id::kLimestone);
        if (!x)
            continue;
        for (const combat::RolledItem &it : x->items) {
            coal += it.item == 109162169652923ll;
            sapphire += it.item == 6064629252182507530ll;
            iron += it.item == 109162169652928ll;
        }
    }
    char msg[200];
    std::snprintf(msg, sizeof msg, "over %d harvests: Coal %.2f %% (3 %%), Iron %.2f %% and Sapphire %.2f %% (about 50 %% each: two of three picks)", N, coal * 100.0 / N, iron * 100.0 / N, sapphire * 100.0 / N);
    const bool coal_ok = coal > N * 0.025 && coal < N * 0.036;
    const bool gem_ok = iron > N * 0.40 && iron < N * 0.60 && sapphire > N * 0.40 && sapphire < N * 0.60;
    CHECK(coal_ok && gem_ok, msg);
}

static void test_bonus_1470(World &w) {
    std::printf("the retail capture: the bonus drops of a Limestone node, with forced draws\n");
    w.g->set_ratings({});
    Rig rig(w, 9);
    const std::vector<double> draws = {0.99, 0.925, 0.99, 0.01, 0.1, 0.99, 0.3, 0.9, 0.99, 0.975, 0.99};
    rig.rng.script = &draws;
    std::string why;
    const auto h = rig.harvest(id::kLimestone, &why);
    CHECK(h.has_value(), "the harvest rolls");
    if (!h)
        return;
    CHECK(h->items.size() == 3, "three stacks");
    if (h->items.size() == 3) {
        std::vector<std::tuple<uint64_t, int64_t, int>> got;
        std::vector<std::tuple<uint64_t, int64_t, int>> want;
        for (const combat::RolledItem &it : h->items)
            got.emplace_back(static_cast<uint64_t>(it.item), it.quantity, it.quality);
        for (const cg::FrameEvent &e : cg::kFrameEvents) {
            if (e.capture == 1470)
                want.emplace_back(e.event.record, e.event.quantity, e.event.quality);
        }
        CHECK(got == want, "Limestone x5 q6, Coal x3 q1 and Sapphire x5 q31: the captured records, quantities and qualities, in the captured order");
    }
    CHECK(rig.rng.scripted == draws.size(), "and every scripted draw was used, none more drawn");
    const bool xp_ok = h->primary_rarity == 1 && h->primary_quality == 6 && h->profession_xp == 150;
    CHECK(xp_ok, "the experience is the first item's: Uncommon at a Journeyman node, 100 x 1.5 = 150 (the Heroic bonus gem adds nothing)");
    check_bonus_1470_odds(w);
}

static void check_duration_curve(World &w, Rig &rig) {
    std::string why;
    double last = 100;
    bool monotone = true;
    for (const double rating : {0.0, 50.0, 107.5, 500.0, 2000.0, 4204.5, 10000.0}) {
        w.rate_all(rating);
        const content::GatherDefRow *d = w.g->def(id::kHalcyonite);
        const auto s = w.g->channel_seconds(*d, *rig.run, why);
        if (!s || *s > last + 1e-9 || *s < 7.5 - 1e-9)
            monotone = false;
        if (s)
            last = *s;
        if (rating == 107.5)
            CHECK(s && std::fabs(*s / 15.0 - 0.927) < 0.003, "ratings of 107.5 (the equation sums two: 215) give the 0.927 of the retail data (13.9 s of 15)");
    }
    CHECK(monotone, "a higher rating never lengthens the channel, and it never goes under half the base");
    w.g->set_ratings({});
}

static void test_durations(World &w) {
    std::printf("the channel's length against the captured harvests\n");
    Rig rig(w, 3);
    w.g->set_ratings({});
    std::string why;
    const struct {
        int64_t def;
        double base;
    } bases[] = {
        {id::kDaffodil, 11.25},
        {id::kMoonbell, 11.25},
        {id::kHalcyonite, 15.0},
        {id::kRividium, 15.0},
        {id::kLimestone, 15.0},
    };
    bool bases_ok = true;
    for (const auto &b : bases) {
        const content::GatherDefRow *d = w.g->def(b.def);
        const auto s = d ? w.g->channel_seconds(*d, *rig.run, why) : std::nullopt;
        if (!s || std::fabs(*s - b.base) >= 1e-9 || d->base_duration != b.base)
            bases_ok = false;
    }
    CHECK(bases_ok, "at rating 0 the channel is the base: 11.25 s for a plant, 15 s for a large stone (and 7.5 s for a medium one)");
    check_duration_curve(w, rig);
    bool all_in = true;
    size_t seen = 0;
    for (const cg::Harvest &h : cg::kHarvests) {
        if (h.beats < 20)
            continue;
        const content::GatherDefRow *d = w.g->def(h.def);
        if (!d) {
            all_in = false;
            continue;
        }
        ++seen;
        const double factor = ((h.frame_ms - h.open_ms + 87) / 1000.0) / d->base_duration;
        if (factor <= 0.5 || factor > 1.0)
            all_in = false;
    }
    CHECK(seen == 9 && all_in, "the nine captured channels are 0.5 to 1.0 of their node's base duration");
    content::GatherDefRow odd = *w.g->def(id::kDaffodil);
    odd.duration_expr = 0;
    odd.base_duration = 1.0e12;
    const auto cut = w.g->channel_seconds(odd, *rig.run, why);
    CHECK(cut && *cut == 3600.0, "a base duration of 10^12 s is cut at an hour");
    odd.base_duration = std::nan("");
    CHECK(!w.g->channel_seconds(odd, *rig.run, why), "a base duration that is no number is refused");
}

void run_gather_rolls_tests(World &w) {
    test_rules(w);
    test_rules_of_rolls(w);
    test_bonus_1470(w);
    test_durations(w);
    test_rarity(w);
    test_fresh_seed();
}
