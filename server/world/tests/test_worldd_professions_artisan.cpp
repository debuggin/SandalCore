// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_worldd_professions_common.h"

using namespace aoc::worldd;

static const aoc::worldd::content::CraftRecipe *
recipe_named(const aoc::worldd::content::CraftRecipeData &d, const char *name) {
  for (const auto &[guid, r] : d.recipes) {
    if (r.name == name) {
      return &r;
    }
  }
  return nullptr;
}

static std::set<int64_t>
collect_vendor_listed_items(const std::vector<content::VendorDefRow> &vendors) {
  std::set<int64_t> listed;
  for (const content::VendorDefRow &v : vendors) {
    for (const content::VendorInventoryRow &inv : v.inventories) {
      for (const content::VendorListing &l : inv.listings) {
        listed.insert(l.item);
      }
    }
  }
  return listed;
}

static void test_vendor_listings(const std::string &path,
                                 const content::ContentDb &db) {
  std::string error;
  const auto vendors = db.vendor_defs_with_prefix("PTR_ONLY_Vendor_", error);
  CHECK(vendors.has_value(), "the GM vendors read");
  if (!vendors) {
    return;
  }
  const std::set<int64_t> listed = collect_vendor_listed_items(*vendors);
  size_t scrolls = 0, scrolls_listed = 0;
  for (const char *pattern : {"GM_Scroll_MakeMeLevel9Artisanship_%",
                              "GM_Scroll_MakeMeLevel15Artisanship_%",
                              "GM_Scroll_MakeMeLevel25Artisanship_%"}) {
    for (const auto &[guid, name] : items_like(path, pattern)) {
      ++scrolls;
      scrolls_listed += listed.count(guid);
    }
  }
  CHECK(scrolls == 67 && scrolls_listed == 67,
        "all 67 GM level scrolls are on a GM vendor's list");
  const char *t1[] = {"Gear_Artisan_Mining_Tool_Pickaxe_T1_0",
                      "Gear_Artisan_Lumberjacking_Tool_Axe_T1_0",
                      "Gear_Artisan_Herbalism_Tool_Sickle_T1_0",
                      "Gear_Artisan_Fishing_Tool_Pole_T1_0",
                      "Gear_Artisan_Hunting_Tool_Bow_T1_0",
                      "Gear_Artisan_Mining_Tool_Pickaxe_Starter",
                      "Gear_Artisan_Lumberjacking_Tool_Axe_Starter",
                      "Gear_Artisan_Herbalism_Tool_Sickle_Starter",
                      "Gear_Artisan_Fishing_Tool_Pole_Starter",
                      "Gear_Artisan_Hunting_Tool_Bow_Starter"};
  size_t tool_items = 0, tools_listed = 0;
  for (const char *name : t1) {
    for (const auto &[guid, n] : items_like(path, name)) {
      ++tool_items;
      tools_listed += listed.count(guid);
    }
  }
  CHECK(tool_items == 10 && tools_listed == 10,
        "the Starter and T1 tools of Mining, Lumberjacking, Herbalism, Fishing "
        "and Hunting are on a GM vendor's list");
  SAYOK("the tools and the GM vendor");
}

struct ToolSpec {
  const char *scroll;
  uint8_t slot;
  int tier;
  int64_t profession;
};

static bool check_gather_tool_row(sqlite3 *raw, int64_t item_guid,
                                  const ToolSpec &tool) {
  if (!raw) {
    return false;
  }
  sqlite3_stmt *s = nullptr;
  bool ok = false;
  if (sqlite3_prepare_v2(raw,
                         "SELECT tool_slot, profession_guid, cert_tier FROM "
                         "gather_tool WHERE item_guid = ?1",
                         -1, &s, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(s, 1, item_guid);
    if (sqlite3_step(s) == SQLITE_ROW) {
      ok = sqlite3_column_int(s, 0) == tool.slot &&
           sqlite3_column_int64(s, 1) == tool.profession &&
           sqlite3_column_int(s, 2) == tool.tier;
    }
  }
  sqlite3_finalize(s);
  return ok;
}

static void check_t3_tool_slots(const std::string &path,
                                const content::ContentDb &db,
                                const items::ItemCatalog &catalog,
                                const use::Context &ctx) {
  const ToolSpec tools[] = {
      {"GM_Scroll_MakeMeLevel25Artisanship_fishing", 35, 2, 108752770061964ll},
      {"GM_Scroll_MakeMeLevel25Artisanship_Herbalism", 33, 2,
       108752770061963ll},
      {"GM_Scroll_MakeMeLevel25Artisanship_Hunting", 38, 2, 108752770061965ll},
      {"GM_Scroll_MakeMeLevel25Artisanship_Lumberjacking", 29, 2,
       108752769996426ll},
      {"GM_Scroll_MakeMeLevel25Artisanship_Mining", 26, 2, 108752769996425ll}};
  int worn = 0, for_the_tier = 0;
  sqlite3 *raw = nullptr;
  sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
  for (const ToolSpec &tool : tools) {
    const auto scroll = items_like(path, tool.scroll);
    const use::Plan plan = scroll.size() == 1
                               ? use::plan_use(db, scroll[0].first, ctx)
                               : use::Plan{};
    if (!plan.ok || plan.items.size() != 1) {
      continue;
    }
    const auto info = catalog.info(plan.items[0].record);
    worn += info && info->equip_slots.size() == 1 &&
            info->equip_slots[0] == tool.slot;
    for_the_tier += check_gather_tool_row(raw, plan.items[0].record, tool);
  }
  if (raw) {
    sqlite3_close(raw);
  }
  CHECK(worn == 5, "each of the five T3 tools is worn in its one slot: Pole "
                   "35, Sickle 33, Bow 38, Axe 29, Pickaxe 26");
  CHECK(for_the_tier == 5,
        "and gather_tool names that slot, its profession and the Journeyman "
        "tier");
}

static void test_tools_and_vendor(const std::string &path, PlayerWorld &world) {
  std::printf("the T3 tools wear in their slots; the GM vendor sells the "
              "scrolls and the T1 tools\n");
  const content::ContentDb &db = *world.content();
  const items::ItemCatalog *catalog = world.item_catalog();
  CHECK(catalog != nullptr, "the world has an item catalog");
  if (!catalog || !world.professions()) {
    return;
  }
  const pr::Book &book = *world.professions();
  stats::CharacterStats cs(*world.stat_context().bundle,
                           *world.stat_context().gear, 2, 1,
                           world.stat_context().rounding);
  std::string err;
  cs.start({}, err);
  use::Context ctx;
  ctx.class_enum = 2;
  ctx.curve = world.level_curve();
  ctx.professions = &book;
  ctx.held = pr::Professions(book);
  ctx.profession_xp = pr::make_xp_runner(*world.ability_catalog(), cs, {}, err);
  check_t3_tool_slots(path, db, *catalog, ctx);
  // C:Y:B and C.O.A
  test_vendor_listings(path, db);
}

static svc::JobResource
make_resource(int64_t record, std::initializer_list<svc::JobRarity> rarities) {
  svc::JobResource r;
  r.record_id = record;
  r.rarities = rarities;
  return r;
}

static void check_artisan_curve_points(const content::ArtisanData &a) {
  CHECK(a.ok(), "the artisan curves and the rating stats are read");
  CHECK(a.crafting_professions == 8 && a.processing_professions == 9,
        "8 crafting and 9 processing professions have rating stats");
  CHECK(all_true(
            {a.general_crafting.quality != 0, a.general_crafting.xp != 0,
             a.general_processing.time != 0, a.general_processing.quantity != 0,
             a.general_processing.rarity != 0, a.general_processing.xp != 0}),
        "the general ratings are found");
  size_t whole = 0;
  for (const auto &[guid, p] : a.by_profession) {
    const bool has_stats =
        p.processing
            ? all_true({p.time != 0, p.quantity != 0, p.rarity != 0, p.xp != 0})
            : (p.quality != 0 && p.xp != 0);
    whole += has_stats ? 1 : 0;
  }
  CHECK(whole == a.by_profession.size() && whole == 17,
        "every crafting profession has its quality and experience ratings and "
        "every processing one its time, quantity, rarity and experience "
        "ratings");
  CHECK(all_true(
            {cr::eval_points(a.crafting_quality, 0) == 0,
             std::fabs(cr::eval_points(a.crafting_quality, 316) - 3.0) < 1e-9,
             std::fabs(cr::eval_points(a.crafting_quality, 1984) - 10.0) < 1e-9,
             std::fabs(cr::eval_points(a.crafting_quality, 8409) - 25.0) < 1e-9,
             std::fabs(cr::eval_points(a.crafting_quality, 99999) - 25.0) <
                 1e-9}),
        "the crafting quality curve: 0 at 0, +3 at 316, +10 at 1,984, +25 at "
        "8,409 and flat past it");
  CHECK(std::fabs(cr::eval_points(a.processing_time, 0) - 1.0) < 1e-9 &&
            std::fabs(cr::eval_points(a.processing_time, 99999) - 0.2) < 1e-9,
        "the processing time curve runs from 1.0 to 0.2");
  CHECK(std::fabs(300.0 * cr::eval_points(a.processing_time, 450) - 235.25) <
            1e-6,
        "[R] a time rating of 450 makes the 300 s of the recorded Lumber "
        "Milling job 235.25 s");
  CHECK(
      all_true(
          {std::fabs(cr::eval_points(a.quantity_chance[0], 316) - 0.1) < 1e-9,
           std::fabs(cr::eval_points(a.quantity_chance[1], 316) - 0.05) < 1e-9,
           cr::eval_points(a.quantity_chance[5], 316) == 0}),
      "the quantity chance of a Common unit is 10 % at 316, an Uncommon "
      "unit's 5 %, a Legendary unit's 0");
  CHECK(std::fabs(cr::eval_points(a.rarity_step[0], 316) - 0.05) < 1e-9 &&
            std::fabs(cr::eval_points(a.rarity_step[1], 316) - 0.025) < 1e-9,
        "the step-up chance of a Common unit is 5 % at 316, an Uncommon unit's "
        "2.5 %");
}

static void test_artisan_curves_and_xp(const content::ArtisanData &a) {
  check_artisan_curve_points(a);
  constexpr int64_t kLumberMilling = 108752756954756ll;
  constexpr int64_t kCarpentry = 108752746927737ll;
  constexpr int64_t kGathering = 108752769996425ll;
  const auto lm = a.by_profession.find(kLumberMilling);
  const auto cp = a.by_profession.find(kCarpentry);
  CHECK(lm != a.by_profession.end() && lm->second.processing &&
            cp != a.by_profession.end() && !cp->second.processing,
        "Lumber Milling is a processing profession and Carpentry a crafting "
        "one");
  CHECK(a.by_profession.find(kGathering) == a.by_profession.end(),
        "a gathering profession has no crafting rating");
  if (lm == a.by_profession.end() || cp == a.by_profession.end()) {
    return;
  }
  std::map<int64_t, double> stats = {
      {a.general_processing.quantity, 100}, {lm->second.quantity, 237},
      {a.general_processing.xp, 150},       {lm->second.xp, 100},
      {a.general_crafting.quality, 400},    {cp->second.quality, 104}};
  const auto reader = [&](int64_t stat) {
    return stats.count(stat) != 0 ? stats[stat] : 0.0;
  };
  const cr::Ratings lumber = cr::ratings_of(a, kLumberMilling, reader);
  const cr::Ratings carpentry = cr::ratings_of(a, kCarpentry, reader);
  CHECK(all_true({lumber.quantity == 337, lumber.xp == 250, lumber.quality == 0,
                  carpentry.quality == 504, carpentry.xp == 0, lumber.any(),
                  !cr::ratings_of(a, kLumberMilling, {}).any()}),
        "a recipe's ratings are the general rating plus its profession's (the "
        "other profession's and a crafter with no stats add nothing)");
  cr::Ratings r250, r32;
  r250.xp = 250;
  r32.xp = 32;
  const double m250 = cr::xp_multiplier(a, r250, true);
  CHECK(cr::profession_xp(content::XpRule::Processing, 0, 1, 10, m250) == 1340,
        "[R] the Lumber Milling job x10 of the recording paid 1,340 profession "
        "experience (134 an unit)");
  CHECK(cr::profession_xp(content::XpRule::Processing, 1, 0, 2, m250) == 214,
        "[R] the Tanning job x2 of the recording paid 214 (107 an unit)");
  CHECK(cr::profession_xp(content::XpRule::Processing, 0, 2, 21,
                          cr::xp_multiplier(a, r32, true)) == 2310,
        "a rating of 32 on a Rare unit (100 an unit) pays 110 an unit: 2,310 "
        "for 21 (the arithmetic of a third recorded claim, whose paid rarity "
        "the recordings do not show)");
  CHECK(cr::profession_xp(content::XpRule::Processing, 1, 2, 3) == 360 &&
            cr::profession_xp(content::XpRule::Processing, 2, 6, 1) == 280,
        "a crafter with no rating is paid the base tables");
}

static void check_artisan_helm_and_crates(const content::ArtisanData &a) {
  const items::ItemRarityFacts &common_curve = items::default_rarity_facts();
  const std::vector<cr::PaidUnits> helm = {
      {1, 51, 5, 4}, {1, 31, 40, 3}, {2, 51, 1, 4}, {3, 51, 2, 4}};
  const double helm_mean = cr::craft_mean_rarity(helm);
  const double helm_base = cr::craft_base_quality(helm_mean, common_curve);
  CHECK(std::fabs(helm_mean - 3.7037037) < 1e-5 &&
            std::fabs(helm_base - (31.0 + 19.0 * 0.7037037)) < 1e-4,
        "a mean rarity of 3.70 is Heroic 31 and 13.37 of the band: 44.37, "
        "nothing cut off");
  cr::Ratings q;
  q.quality = 1503;
  CHECK(cr::quality_from(helm_base + cr::quality_bonus(a, q)) == 52,
        "a quality rating of 1,503 adds 7.996 to 44.37: 52.37, the nearest "
        "whole number 52");
  q.quality = 1504;
  CHECK(cr::quality_from(helm_base + cr::quality_bonus(a, q)) == 52,
        "[R] a quality rating of 1,504 makes the recorded Ironwake helm 52 "
        "(44.37 and 8 is 52.37)");
  const std::vector<cr::PaidUnits> timber = {{9, 6, 9, 1}};
  q.quality = 211;
  CHECK(cr::craft_base_quality(cr::craft_mean_rarity(timber), common_curve) ==
                6.0 &&
            cr::quality_from(6.0 + cr::quality_bonus(a, q)) == 8,
        "[R] nine Uncommon timber start at 6 and a rating of 211 makes the "
        "recorded carriage 8");
  const std::vector<cr::PaidUnits> citizen = {
      {1, 1, 4, 0}, {2, 1, 2, 0}, {3, 1, 2, 0}, {4, 51, 1, 4}};
  const std::vector<cr::PaidUnits> market = {{1, 1, 6, 0}, {2, 1, 3, 0}};
  CHECK(cr::craft_base_quality(cr::craft_mean_rarity(citizen), common_curve) ==
                6.0 &&
            cr::craft_base_quality(cr::craft_mean_rarity(market),
                                   common_curve) == 1.0,
        "[R] the recorded citizen crate (three Common stacks and an Epic "
        "certificate) starts at 6 and the market crate (all Common) at 1");
  CHECK(cr::craft_base_quality(cr::craft_mean_rarity({}), common_curve) == 1.0,
        "a craft that paid no material starts at 1");
}

static void check_artisan_band_shares() {
  const items::ItemRarityFacts &common_curve = items::default_rarity_facts();
  CHECK(all_true(
            {cr::craft_base_quality(0.5, common_curve) == 3.0,
             std::fabs(cr::craft_base_quality(0.6, common_curve) - 3.4) < 1e-9,
             std::fabs(cr::craft_base_quality(1.0 / 3.0 + 1.0, common_curve) -
                       9.0) < 1e-9,
             cr::craft_base_quality(2.5, common_curve) == 23.0}),
        "a share of the band is exact, whole number of qualities or not (half "
        "way up Common 1 to 5 is 3, 0.6 of the way 3.4), and every product "
        "has it: a piece of gear and a stackable one");
  CHECK(all_true({cr::quality_from(9.98) == 10, cr::quality_from(9.4) == 9,
                  cr::quality_from(9.5) == 10, cr::quality_from(4.979) == 5,
                  cr::quality_from(8.5347) == 9, cr::quality_from(0.2) == 0}),
        "the whole quality is the nearest whole number to the exact one, a "
        "half up (9.98 is 10, 9.4 is 9)");
  const std::vector<cr::PaidUnits> two_records = {
      {1, 1, 37, 0}, {1, 6, 37, 1}, {2, 1, 25, 0}};
  const std::vector<cr::PaidUnits> two_slots = {
      {1, 1, 37, 0}, {2, 6, 37, 1}, {3, 1, 25, 0}};
  CHECK(std::fabs(cr::craft_mean_rarity(two_records) - 0.25) < 1e-9 &&
            std::fabs(cr::craft_mean_rarity(two_slots) - 1.0 / 3.0) < 1e-9,
        "a slot paid with Common and Uncommon units is one slot (a mean of "
        "0.25 with a Common slot beside it), three slots would be a mean of "
        "0.33");
}

static void test_artisan_quality_and_calc(const content::ArtisanData &a) {
  check_artisan_helm_and_crates(a);
  check_artisan_band_shares();
  const items::ItemRarityFacts &common_curve = items::default_rarity_facts();
  struct CalcCase {
    std::vector<std::vector<std::pair<int64_t, int>>> slots;
    double rating;
    int quality;
  };
  const std::vector<CalcCase> calc = {
      {{{{74, 0}}, {{25, 0}}}, 0, 1},
      {{{{74, 0}}, {{25, 0}}}, 316, 4},
      {{{{74, 0}}, {{25, 0}}}, 1504, 9},
      {{{{74, 0}}, {{25, 0}}}, 8409, 26},
      {{{{12, 2}}, {{12, 2}}, {{5, 3}}, {{1, 5}}}, 0, 31},
      {{{{1, 0}, {1, 1}}}, 0, 3},
      {{{{37, 0}, {37, 1}}, {{25, 0}}}, 0, 2},
      {{{{2, 1}, {1, 2}}}, 0, 9},
      {{{{1, 2}, {1, 3}}}, 0, 23},
      {{{{5, 4}, {40, 3}}, {{1, 4}}, {{2, 4}}}, 1503, 52},
      {{{{5, 4}, {40, 3}}, {{1, 4}}, {{2, 4}}}, 1504, 52},
      {{{{9, 1}}}, 211, 8},
      {{{{20, 0}, {5, 0}}, {{18, 0}, {2, 4}}}, 700, 7},
      {{{{13, 0}, {7, 1}}}, 105, 3},
      {{{{135, 4}}, {{20, 4}}, {{6, 0}, {3, 1}, {1, 0}}}, 12000, 52},
      {{{{18, 4}, {56, 0}}}, 1264, 12},
      {{{{25, 4}}, {{6, 4}, {34, 0}}, {{3, 4}, {4, 3}, {3, 2}}}, 8409, 48},
      {{{{76, 1}, {124, 1}}, {{40, 4}}, {{43, 3}, {20, 2}, {11, 4}}}, 105, 26},
      {{{{9, 0}}}, 504, 5},
      {{{{9, 0}}}, 0, 1},
      {{{{8, 1}, {1, 0}}}, 0, 5},
      {{{{8, 1}, {1, 0}}}, 504, 9},
  };
  size_t calc_ok = 0;
  for (size_t i = 0; i < calc.size(); ++i) {
    std::vector<cr::PaidUnits> paid;
    for (size_t s = 0; s < calc[i].slots.size(); ++s) {
      for (const auto &[units, rarity] : calc[i].slots[s]) {
        paid.push_back({static_cast<int64_t>(s), 1, units, rarity});
      }
    }
    cr::Ratings rating;
    rating.quality = calc[i].rating;
    const double base =
        cr::craft_base_quality(cr::craft_mean_rarity(paid), common_curve);
    const int got = cr::quality_from(base + cr::quality_bonus(a, rating));
    calc_ok += got == calc[i].quality ? 1 : 0;
  }
  CHECK(calc_ok == calc.size(),
        "22 crafts (gear templates, the crate template, mixed slots, "
        "whole-number shares, ratings from 0 to 12,000, the first test's four "
        "crafts) start and end at the whole quality of the exact calculation");
}

static void check_artisan_tier_scale_and_steps(const content::ArtisanData &a) {
  const items::ItemRarityFacts &common_curve = items::default_rarity_facts();
  const std::vector<cr::PaidUnits> seventy_four = {{0, 6, 74, 1}};
  const std::vector<cr::PaidUnits> nine_timber = {{0, 6, 9, 1}};
  cr::Ratings r504;
  r504.quality = 504;
  const auto crafted = [&](const std::vector<cr::PaidUnits> &paid,
                           const cr::Ratings &rating) {
    return cr::quality_from(
        cr::craft_base_quality(cr::craft_mean_rarity(paid), common_curve) +
        cr::quality_bonus(a, rating));
  };
  CHECK(all_true({crafted(seventy_four, r504) == 10,
                  crafted(nine_timber, r504) == 10,
                  std::fabs(cr::tier_scale(common_curve, 10) -
                            (2.0 + 4.0 / 9.0)) < 1e-9,
                  std::fabs(cr::quality_bonus(a, r504) - 3.9791667) < 1e-6}),
        "74 Uncommon (or the carriage's 9 Uncommon timber) at a rating of 504 "
        "is 6 and 3.98: 9.98, the whole quality 10 (the calculator's 2.3333 is "
        "its cut-off 9)");
  CHECK(all_true({cr::tier_scale(common_curve, 1) == 1.0,
                  cr::tier_scale(common_curve, 6) == 2.0,
                  std::fabs(cr::tier_scale(common_curve, 5) - 1.9999) < 1e-9,
                  std::fabs(cr::tier_scale(common_curve, 15) - 2.9999) < 1e-9,
                  std::fabs(cr::tier_scale(common_curve, 71) - 6.0) < 1e-9,
                  std::fabs(cr::tier_scale(common_curve, 100) - 6.99) < 1e-9,
                  std::fabs(cr::tier_scale(common_curve, 23) -
                            (3.0 + 7.0 / 14.0)) < 1e-9}),
        "the game's quality in its bands and the calculator's tier scale: a "
        "band's first is its whole number, its last stops short of the next "
        "(Legendary at .99), Rare 23 is 3.5");
  const auto reached = [&](double base, double rating) {
    cr::Ratings x;
    x.quality = rating;
    return cr::quality_from(base + cr::quality_bonus(a, x));
  };
  CHECK(all_true({reached(1, 603) == 5, reached(1, 604) == 6,
                  reached(1, 700) == 6, reached(1, 800) == 6,
                  reached(6, 1863) == 15, reached(6, 1864) == 16,
                  reached(6, 2400) == 17, reached(16, 3575) == 30,
                  reached(16, 3576) == 31, reached(31, 5607) == 50,
                  reached(31, 5608) == 51, reached(51, 5607) == 70,
                  reached(51, 5608) == 71}),
        "from the bottom of a tier a craft moves up a rarity at a rating of "
        "604 (Common to Uncommon), 1,864 (Uncommon to Rare), 3,576 (Rare to "
        "Heroic) and 5,608 (Heroic to Epic, Epic to Legendary); 800 and 2,400 "
        "are past the first two");
}

static void test_artisan_paid_mean_and_t3(const content::ArtisanData &a) {
  check_artisan_tier_scale_and_steps(a);
  const cr::PaidMean uniform =
      cr::paid_mean({make_resource(1, {{1, 250, false}})});
  const cr::PaidMean otter = cr::paid_mean(
      {make_resource(1, {{0, 1, false}, {1, 1, false}}),
       make_resource(2, {{1, 1, false}}), make_resource(3, {{1, 1, false}})});
  const cr::PaidMean near_top =
      cr::paid_mean({make_resource(1, {{0, 2, false}, {1, 23, false}})});
  const cr::PaidMean wide =
      cr::paid_mean({make_resource(1, {{0, 1, false}, {5, 1, false}})});
  const cr::PaidMean none = cr::paid_mean({});
  CHECK(all_true(
            {uniform.points == 6.0f, uniform.quality == 6, uniform.rarity == 1,
             std::fabs(otter.points - 31.0f / 6.0f) < 1e-5f, otter.quality == 6,
             otter.rarity == 1, std::fabs(near_top.points - 5.6f) < 1e-5f,
             near_top.quality == 6, near_top.rarity == 1, wide.points == 36.0f,
             wide.quality == 36, wide.rarity == 3, none.points == 1.0f,
             none.quality == 1, none.rarity == 0}),
        "[R] the mean of what a job was paid is rounded up to a whole quality: "
        "uniform Uncommon is 6; the recorded mixed job (5.17) is 6, Uncommon, "
        "and so is 5.6; Common and Legendary (36) is Heroic; no item is 1");
  cr::Ratings lm_set;
  lm_set.time = 30;
  lm_set.quantity = 337;
  lm_set.rarity = 349;
  lm_set.xp = 74;
  CHECK(all_true(
            {std::fabs(cr::rarity_step_chance(a, lm_set, 1) - 0.0271484) < 1e-5,
             std::fabs(cr::rarity_step_chance(a, lm_set, 1) *
                           cr::rarity_step_chance(a, lm_set, 2) -
                       0.0003065) < 1e-6,
             std::fabs(cr::quantity_chance(a, lm_set, 1) - 0.052734) < 1e-5,
             std::fabs(1800.0 * cr::time_multiplier(a, lm_set) - 1775.0) < 0.5,
             std::fabs(cr::xp_multiplier(a, lm_set, true) - 1.2342) < 1e-4}),
        "the Lumber Milling T3 set: 2.71 % of the Uncommon oak comes out Rare "
        "and 0.03 % Heroic, 5.27 % of the units come with one more, 1,800 s "
        "is 1,775 s, and the experience is x1.234");
}

static void check_artisan_one_unit_3000(const content::CraftRecipeData &data,
                                        const PlayerWorld &world,
                                        const content::CraftRecipe &oak,
                                        svc::ProcessingJob one) {
  const auto facts = [&](int64_t item) {
    return world.item_rarity_facts(item);
  };
  cr::Ratings big;
  big.quantity = 99999;
  big.rarity = 99999;
  one.quantity = 1;
  one.resources = {{oak.inputs[0].item, {{1, 1, false}}}};
  bool none_empty = true, units_kept = true, shown_all = true;
  int deep = 0, one_runs = 0;
  for (int i = 1; i <= 3000; ++i) {
    char guid[40];
    std::snprintf(guid, sizeof(guid), "%032d", 100000 + i);
    one.job_guid = guid;
    const auto made = cr::roll_job(data, one, 20, big, facts);
    if (!made) {
      continue;
    }
    ++one_runs;
    int64_t total = 0;
    bool heroic = false;
    for (const cr::Product &p : made->products) {
      none_empty = none_empty && p.quantity >= 1;
      total += p.quantity;
      heroic = heroic || p.quality >= 31;
    }
    units_kept = units_kept && total == 2 + made->extra_units;
    shown_all =
        shown_all && cr::product_instances(
                         one, aoc::protocol::repl::GuidValue{1, 2, 3, 4}, *made)
                             .size() == made->products.size();
    deep += heroic ? 1 : 0;
  }
  CHECK(one_runs == 3000 && none_empty && units_kept && shown_all && deep > 20,
        "a job of one unit with huge ratings leaves no empty line (a unit that "
        "steps up twice, a few in a hundred of these jobs, used to leave one), "
        "loses no unit and shows every line");
}

static void test_artisan_one_unit_and_saved(
    const content::CraftRecipeData &data, const PlayerWorld &world,
    const content::CraftRecipe &oak, svc::ProcessingJob job,
    const crafting::Ratings &both) {
  check_artisan_one_unit_3000(data, world, oak, job);
  const auto facts = [&](int64_t item) {
    return world.item_rarity_facts(item);
  };
  cr::Ratings queued, odd;
  queued.xp = 285.25;
  queued.quantity = 337;
  queued.rarity = -4;
  queued.time = 30;
  odd.xp = std::nan("");
  odd.quantity = 1e12;
  odd.rarity = INFINITY;
  const aoc::protocol::repl::GuidValue txn{1, 2, 3, 4};
  const svc::ProcessingJob kept =
      cr::make_job({}, "0123456789abcdef", oak.guid, 250, 300000, txn,
                   job.resources, 1790000000000ll, queued);
  const svc::ProcessingJob oddly =
      cr::make_job({}, "0123456789abcdef", oak.guid, 250, 300000, txn,
                   job.resources, 1790000000000ll, odd);
  const cr::Ratings back = cr::job_ratings(kept);
  const cr::Ratings back_odd = cr::job_ratings(oddly);
  CHECK(all_true({back.xp == 285.25, back.quantity == 337, back.rarity == 0,
                  back.time == 0, back.quality == 0, back_odd.xp == 0,
                  back_odd.quantity == 1e9, back_odd.rarity == 0}),
        "a job keeps the experience, quantity and rarity ratings it was queued "
        "with (below 0 or no number as 0, the table's bound as it is) and no "
        "other rating");
  bool from_snapshot = true;
  for (int i = 1; i <= 20; ++i) {
    char guid[40];
    std::snprintf(guid, sizeof(guid), "%032d", i);
    svc::ProcessingJob saved =
        cr::make_job({}, "0123456789abcdef", oak.guid, 250, 300000, txn,
                     job.resources, 1790000000000ll, both);
    saved.job_guid = guid;
    job.job_guid = guid;
    const auto via =
        cr::roll_job(data, saved, 20, cr::job_ratings(saved), facts);
    const auto direct = cr::roll_job(data, job, 20, both, facts);
    from_snapshot = from_snapshot && via && direct &&
                    all_true({via->extra_units == direct->extra_units,
                              via->stepped_units == direct->stepped_units,
                              via->products.size() == direct->products.size(),
                              via->profession_xp == direct->profession_xp});
  }
  CHECK(from_snapshot, "a job rolls from its saved ratings exactly as from the "
                       "ratings it was started with");
}

static void check_artisan_plain_and_mixed(const content::CraftRecipeData &data,
                                          const PlayerWorld &world,
                                          const content::CraftRecipe &oak,
                                          const svc::ProcessingJob &job) {
  const auto facts = [&](int64_t item) {
    return world.item_rarity_facts(item);
  };
  const auto plain = cr::roll_job(data, job, 20, cr::Ratings{}, facts);
  CHECK(plain && plain->products.size() == 2 &&
            all_true({plain->products[0].quantity == 250,
                      plain->products[1].quantity == 250, plain->quality == 6,
                      plain->products[0].quality == 6, plain->extra_units == 0,
                      plain->stepped_units == 0, plain->rarity == 1}),
        "a crafter with no rating makes the quantity of the recipe, all at the "
        "paid quality (6), and its experience is worked out at the paid rarity "
        "(Uncommon)");
  svc::ProcessingJob mixed = job;
  svc::ProcessingJob common_only = job;
  mixed.resources = {
      make_resource(oak.inputs[0].item, {{0, 1, false}, {1, 1, false}}),
      make_resource(2, {{1, 1, false}}), make_resource(3, {{1, 1, false}})};
  common_only.resources = {
      make_resource(oak.inputs[0].item, {{0, 250, false}})};
  const auto mixed_made = cr::roll_job(data, mixed, 20, cr::Ratings{}, facts);
  const auto common_made =
      cr::roll_job(data, common_only, 20, cr::Ratings{}, facts);
  CHECK(mixed_made && common_made && !mixed_made->products.empty() &&
            all_true({mixed_made->quality == 6,
                      mixed_made->products[0].quality == 6,
                      mixed_made->rarity == 0,
                      mixed_made->profession_xp == common_made->profession_xp,
                      mixed_made->adventuring_xp == common_made->adventuring_xp,
                      mixed_made->profession_xp > 0}),
        "[R] a job paid with the recorded mixed rarities makes Uncommon "
        "products at quality 6 and pays the experience of a Common job (the "
        "experience rarity is the mean rarity rounded down)");
}

static bool verify_rolled_products(const cr::JobRewards &made, int64_t &total) {
  bool shapes = true;
  for (const cr::Product &p : made.products) {
    total += p.quantity;
    const bool valid_q =
        p.quality == 6 || p.quality == 16 || p.quality == 31 || p.quality == 51;
    shapes = shapes && p.quantity > 0 && valid_q;
  }
  return shapes;
}

static void check_artisan_400_jobs_sweep(const content::CraftRecipeData &data,
                                         const PlayerWorld &world,
                                         svc::ProcessingJob job,
                                         const cr::Ratings &both) {
  const auto facts = [&](int64_t item) {
    return world.item_rarity_facts(item);
  };
  int64_t extra = 0, stepped = 0, jobs_ok = 0, runs = 0;
  bool same = true, conserved = true, shapes = true;
  for (int i = 1; i <= 400; ++i) {
    char guid[40];
    std::snprintf(guid, sizeof(guid), "%032d", i);
    job.job_guid = guid;
    const auto made = cr::roll_job(data, job, 20, both, facts);
    const auto again = cr::roll_job(data, job, 20, both, facts);
    if (!made || !again) {
      continue;
    }
    ++runs;
    same = same && all_true({made->products.size() == again->products.size(),
                             made->extra_units == again->extra_units,
                             made->stepped_units == again->stepped_units});
    int64_t total = 0;
    shapes = shapes && verify_rolled_products(*made, total);
    conserved = conserved && total == 500 + made->extra_units;
    extra += made->extra_units;
    stepped += made->stepped_units;
    jobs_ok += made->profession_xp > 0 ? 1 : 0;
  }
  const double mean_extra = static_cast<double>(extra) /
                            static_cast<double>(std::max<int64_t>(1, runs));
  const double mean_stepped = static_cast<double>(stepped) /
                              static_cast<double>(std::max<int64_t>(1, runs));
  std::printf("  400 jobs of 250 units at quantity and rarity ratings of 316: "
              "%.2f extra units and %.2f steps up a job (expected 25.0 and "
              "about 13.3)\n",
              mean_extra, mean_stepped);
  CHECK(runs == 400 && same && conserved && shapes,
        "a job rolls the same every time it is looked at, loses no unit to a "
        "step up, and its lines come in the first quality of a rarity");
  CHECK(mean_extra > 24.0 && mean_extra < 26.0,
        "the extra units follow the quantity chance of an Uncommon unit (5 %): "
        "25 a job of two products of 250");
  CHECK(mean_stepped > 12.2 && mean_stepped < 14.4,
        "the steps up follow the step-up chance of an Uncommon unit (2.5 %), "
        "and again for the Rare units");
  CHECK(jobs_ok == runs, "a job with ratings still pays its experience");
}

static void test_artisan_roll_jobs(const content::CraftRecipeData &data,
                                   const PlayerWorld &world) {
  const content::CraftRecipe *oak =
      recipe_named(data, "LumberMilling_T1_Timber_Oak");
  CHECK(oak && all_true({oak->has_duration_formula, oak->has_quantity_formula,
                         oak->has_quality_formula, !oak->inputs.empty()}),
        "the Oak Timber recipe has a time, a quantity and a quality formula");
  if (!oak || oak->inputs.empty()) {
    return;
  }
  svc::ProcessingJob job;
  job.recipe_guid = oak->guid;
  job.quantity = 250;
  job.resources = {{oak->inputs[0].item, {{1, 250, false}}}};
  job.job_guid = "00000000000000000000000000000001";
  check_artisan_plain_and_mixed(data, world, *oak, job);
  cr::Ratings both;
  both.quantity = 316;
  both.rarity = 316;
  check_artisan_400_jobs_sweep(data, world, job, both);
  test_artisan_one_unit_and_saved(data, world, *oak, job, both);
}

static void test_artisan(const content::ContentDb &db,
                         const PlayerWorld &world) {
  std::printf("artisan ratings (crafting quality)\n");
  std::string error;
  const auto data = db.craft_recipe_data(error);
  if (!data) {
    std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
    g_fail++;
    return;
  }
  test_artisan_curves_and_xp(data->artisan);
  test_artisan_quality_and_calc(data->artisan);
  test_artisan_paid_mean_and_t3(data->artisan);
  test_artisan_roll_jobs(*data, world);
}

void run_professions_artisan_tests(const std::string &path,
                                   const content::ContentDb &db,
                                   PlayerWorld &world) {
  test_artisan(db, world);
  test_tools_and_vendor(path, world);
}
