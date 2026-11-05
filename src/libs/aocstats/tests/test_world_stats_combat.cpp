// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_stats_helpers.h"

#include <cstring>
#include <limits>

namespace aoc::worldd::stats {

namespace {

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

class TableCatalog : public GearCatalog {
public:
    std::map<int64_t, ItemGear> items;
    std::map<int64_t, SetBonus> sets;
    std::map<int, std::string> slots = {{2, "Chest"}, {15, "Primary"}, {16, "Secondary"}};
    const ItemGear* item(int64_t r) const override {
        const auto it = items.find(r);
        return it == items.end() ? nullptr : &it->second;
    }
    const SetBonus* set(int64_t g) const override {
        const auto it = sets.find(g);
        return it == sets.end() ? nullptr : &it->second;
    }
    std::string slot_name(int slot) const override {
        const auto it = slots.find(slot);
        return it != slots.end() ? it->second : "Slot" + std::to_string(slot);
    }
};

void default_curve(ItemGear& g) {
    const double xs[] = {1, 6, 16, 31, 51, 71, 101}, ys[] = {5, 15, 30, 50, 70, 100, 200};
    g.has_curve = true;
    for (size_t i = 0; i < 7; ++i) g.bands[i] = {xs[i], ys[i]};
}

BlockStat block_stat(int64_t stat, std::array<double, 7> mn, std::array<double, 7> mx) {
    BlockStat b;
    b.stat = stat;
    b.min = mn;
    b.max = mx;
    return b;
}

double mod_of(const GearInputs& g, int64_t stat, bool* found = nullptr) {
    for (const StatMod& m : g.mods) {
        if (m.stat == stat) {
            if (found) *found = true;
            return m.value;
        }
    }
    if (found) *found = false;
    return 0;
}

void verify_gear_quality_bands(TableCatalog& cat) {
    ItemGear a;
    a.record = 1;
    default_curve(a);
    a.stats = {block_stat(100, {10, 30, 50, 60, 70, 80, 0}, {20, 40, 50, 60, 70, 80, 0}), block_stat(101, {7, 7, 7, 7, 7, 7, 7}, {7, 7, 7, 7, 7, 7, 7})};
    a.tags = {"Item.Gear.Armor.Light", "Slot.Chest"};
    cat.items[1] = a;
    const auto one = [&](int quality, Rounding r) {
        const WornItem w{1, quality, 2};
        return gear_inputs(cat, std::span<const WornItem>(&w, 1), r);
    };
    CHECK(mod_of(one(1, Rounding::None), 100) == 10 && mod_of(one(5, Rounding::None), 100) == 20, "Common 1-5: Min at 1, Max at 5");
    CHECK(mod_of(one(3, Rounding::None), 100) == 15, "quality 3: 10 + 10 * (3 - 1) / (5 - 1) = 15");
    CHECK(mod_of(one(2, Rounding::None), 100) == 12.5 && mod_of(one(2, Rounding::HalfUp), 100) == 13, "quality 2: 12.5, or 13 with half-up rounding");
    CHECK(mod_of(one(4, Rounding::None), 100) == 17.5 && mod_of(one(4, Rounding::HalfUp), 100) == 18, "quality 4: 17.5, 18 half-up (a half goes up)");
    CHECK(mod_of(one(6, Rounding::None), 100) == 30 && mod_of(one(15, Rounding::None), 100) == 40 && std::fabs(mod_of(one(7, Rounding::None), 100) - (30 + 10.0 / 9.0)) < 1e-12,
          "Uncommon 6-15: 30 at 6, 40 at 15, 7 in between is 30 + 10 / 9");
    CHECK(mod_of(one(20, Rounding::None), 100) == 50 && mod_of(one(60, Rounding::None), 100) == 70 && mod_of(one(90, Rounding::None), 100) == 80, "Rare, Epic and Legendary read their own band");
    CHECK(mod_of(one(1, Rounding::None), 101) == 7 && mod_of(one(150, Rounding::None), 101) == 7, "a stat that is the same at every rarity is the same at every quality");
    CHECK(mod_of(one(150, Rounding::None), 100) == 0, "Artifact 101-200: 0 here");
    CHECK(mod_of(one(0, Rounding::None), 100) == 10 && mod_of(one(250, Rounding::None), 100) == 0, "quality 0 reads the Common band at its Min (the share is held to the band); 250 reads Artifact");
    ItemGear nc = a;
    nc.record = 2;
    nc.has_curve = false;
    cat.items[2] = nc;
    const WornItem w{2, 40, 2};
    CHECK(mod_of(gear_inputs(cat, std::span<const WornItem>(&w, 1), Rounding::None), 100) == 10, "no quality curve: CommonMin, whatever the quality");
}

void verify_gear_tags_and_multi_item_sum(TableCatalog& cat) {
    const WornItem w1{1, 1, 2};
    const GearInputs g = gear_inputs(cat, std::span<const WornItem>(&w1, 1), Rounding::None);
    CHECK(g.equipped.size() == 2 && g.equipped[0].slot == "Chest" && g.equipped[0].tag == "Item.Gear.Armor.Light" && g.equipped[1].tag == "Slot.Chest", "an item's tags, in order, under the slot's name");
    const WornItem w77{1, 1, 77};
    const GearInputs odd = gear_inputs(cat, std::span<const WornItem>(&w77, 1), Rounding::None);
    CHECK(odd.equipped.size() == 2 && odd.equipped[0].slot == "Slot77", "a slot value with no name is Slot<N>");
    ItemGear b;
    b.record = 3;
    b.stats = {block_stat(50, {1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 1}), block_stat(100, {5, 5, 5, 5, 5, 5, 5}, {5, 5, 5, 5, 5, 5, 5})};
    cat.items[3] = b;
    const WornItem worn[] = {{1, 1, 2}, {3, 1, 15}, {999, 1, 16}};
    const GearInputs sum = gear_inputs(cat, worn, Rounding::None);
    CHECK(sum.mods.size() == 3 && sum.mods[0].stat == 50 && sum.mods[1].stat == 100 && sum.mods[2].stat == 101 && mod_of(sum, 100) == 15 && sum.unknown_items == 1,
          "two items: the stat they share is summed (10 + 5), the mods come out by stat guid, the unknown record is counted and adds nothing");
}

void verify_gear_set_bonuses(TableCatalog& cat) {
    SetBonus set;
    SetTier t2;
    t2.pieces = 2;
    SetEntry e1;
    e1.stat = 200;
    e1.has = {true, false, true, false, false, false, false};
    e1.value = {10, 0, 30, 0, 0, 0, 0};
    SetEntry e2;
    e2.stat = 201;
    t2.entries = {e1, e2};
    SetTier t4;
    t4.pieces = 4;
    SetEntry e3;
    e3.stat = 200;
    e3.has = {true, false, true, false, false, false, false};
    e3.value = {5, 0, 8, 0, 0, 0, 0};
    t4.entries = {e3};
    set.tiers = {t2, t4};
    cat.sets[7] = set;
    for (int64_t r : {10, 11, 12, 13}) {
        ItemGear p;
        p.record = r;
        default_curve(p);
        p.sets = {7};
        cat.items[r] = p;
    }
    const WornItem pieces_1[] = {{10, 20, 2}};
    CHECK(gear_inputs(cat, pieces_1, Rounding::None).mods.empty(), "one piece of a set of 2: no bonus");
    const WornItem pieces_2[] = {{10, 20, 2}, {11, 1, 15}};
    const GearInputs two = gear_inputs(cat, pieces_2, Rounding::None);
    bool f200 = false, f201 = false;
    CHECK(mod_of(two, 200, &f200) == 10 && f200 && mod_of(two, 201, &f201) == 0 && f201 && two.mods.size() == 2,
          "two pieces, the lowest Common: tier 2 gives 10 (Common), and the bonus with no value is a zero mod");
    const WornItem pieces_4[] = {{10, 20, 2}, {11, 20, 15}, {12, 20, 16}, {13, 20, 17}};
    const GearInputs four = gear_inputs(cat, pieces_4, Rounding::None);
    CHECK(mod_of(four, 200) == 30 + 8, "four Rare pieces: both tiers apply, 30 + 8");
    const WornItem pieces_3[] = {{10, 20, 2}, {11, 20, 15}, {12, 20, 16}};
    CHECK(mod_of(gear_inputs(cat, pieces_3, Rounding::None), 200) == 30, "three pieces: the tier of 2 only");
    ItemGear orphan;
    orphan.record = 20;
    orphan.sets = {999};
    cat.items[20] = orphan;
    const WornItem o[] = {{20, 1, 2}};
    const GearInputs og = gear_inputs(cat, o, Rounding::None);
    CHECK(og.unknown_sets == 1 && og.mods.empty(), "a set the catalog does not know gives no bonus and is counted");
    CHECK(gear_inputs(cat, {}, Rounding::None).mods.empty() && gear_inputs(cat, {}, Rounding::None).equipped.empty(), "nothing worn: no mods, no tags");
}

struct CharStatIds {
    int64_t con = 0;
    int64_t ment = 0;
    int64_t health = 0;
    int64_t max_health = 0;
    int64_t mana = 0;
    int64_t max_mana = 0;
    int64_t power = 0;
};

void verify_changed_stat_order(const CharacterStats& cs, const std::vector<StatValue>& changed) {
    bool in_order = true;
    for (size_t i = 1; i < changed.size(); ++i) {
        int64_t prev = -1, now = -1;
        for (size_t k = 0; k < cs.values().size(); ++k) {
            if (cs.values()[k].guid == changed[i - 1].guid) prev = static_cast<int64_t>(k);
            if (cs.values()[k].guid == changed[i].guid) now = static_cast<int64_t>(k);
        }
        in_order = in_order && prev < now;
    }
    CHECK(in_order, "the changed stats come in stat order");
    for (const StatValue& v : changed) {
        const StatValue* f = cs.find(v.guid);
        CHECK(f && same_on_the_wire(*f, v), "(each changed stat is the character's value now)");
        break;
    }
}

void verify_character_start_and_heavy_gear(CharacterStats& cs, const CharStatIds& ids, float& bare_max) {
    const std::vector<WornItem> none;
    const WornItem with_heavy[] = {{1, 1, 2}};
    const WornItem with_plain[] = {{3, 1, 71}};
    std::vector<StatValue> changed;
    std::string e;
    CHECK(!cs.started() && !cs.update(none, changed, e) && contains(e, "not started"), "update before start is refused");
    CHECK(cs.start(none, e) && cs.started() && cs.values().size() == 466, "start: the Bard's 466 stats");
    CHECK(cs.find(ids.health) && cs.find(ids.max_health) && cs.find(ids.health)->value == cs.find(ids.max_health)->value &&
              cs.find(ids.health)->held() && !cs.find(ids.max_health)->held(),
          "Health starts at its maximum and is a held (current) value, MaxHealth is not");
    CHECK(cs.find(ids.power) && cs.find(ids.power)->buckets() && cs.find(ids.power)->equip == 0, "the fishing stat replicates base and equipment");
    CHECK(cs.faults() == 0 && cs.ignored_mods() == 0 && cs.unknown_items() == 0, "no fault, no ignored mod");
    CHECK(cs.update(none, changed, e) && changed.empty(), "the same gear again: no change");
    CHECK(cs.update(with_plain, changed, e) && changed.empty(), "an item that gives the engine nothing: no change");
    const float old_health = cs.find(ids.health)->value;
    bare_max = cs.find(ids.max_health)->value;
    CHECK(cs.update(with_heavy, changed, e) && changed.size() > 5, "wearing it changes stats");
    bool has_max = false, has_health = false, has_con = false;
    for (const StatValue& v : changed) {
        has_max = has_max || v.guid == ids.max_health;
        has_health = has_health || v.guid == ids.health;
        has_con = has_con || v.guid == ids.con;
    }
    CHECK(has_max && has_con && !has_health, "MaxHealth and Constitution changed, Health did not: a current value stays where it is");
    CHECK(cs.find(ids.max_health)->value > bare_max && cs.find(ids.health)->value == old_health, "...MaxHealth is up, Health is as it was");
    verify_changed_stat_order(cs, changed);
}

void verify_character_current_clamping(CharacterStats& cs, const CharStatIds& ids, float bare_max) {
    const std::vector<WornItem> none;
    const WornItem with_heavy[] = {{1, 1, 2}};
    const WornItem with_plain[] = {{3, 1, 71}};
    std::vector<StatValue> changed;
    std::string e;
    cs.set_current(ids.health, 100.0f);
    cs.set_current(ids.max_health, 5.0f);
    CHECK(cs.find(ids.health)->value == 100.0f && cs.find(ids.max_health)->value != 5.0f, "set_current sets a held stat and ignores one that is not");
    CHECK(cs.update(none, changed, e), "taking the gear off");
    CHECK(cs.find(ids.health)->value == std::min(100.0f, cs.find(ids.max_health)->value) && cs.find(ids.max_health)->value == bare_max, "the maximum is back to the bare one");
    if (bare_max < 100.0f) {
        CHECK(cs.find(ids.health)->value == bare_max, "Health 100 above the new maximum is clamped to it");
        bool clamp_sent = false;
        for (const StatValue& v : changed) clamp_sent = clamp_sent || (v.guid == ids.health && v.value == bare_max);
        CHECK(clamp_sent, "...and the clamp is a change like any other");
    } else {
        CHECK(cs.find(ids.health)->value == 100.0f, "Health 100 is below the bare maximum: it stays");
    }
    CHECK(cs.update(with_heavy, changed, e), "the gear on again");
    cs.set_current(ids.health, 10.0f);
    cs.set_current(ids.mana, 10.0f);
    CHECK(cs.update(none, changed, e), "and off again");
    bool health_in = false, mana_in = false;
    for (const StatValue& v : changed) {
        health_in = health_in || v.guid == ids.health;
        mana_in = mana_in || v.guid == ids.mana;
    }
    CHECK(cs.find(ids.health)->value == 10.0f && cs.find(ids.mana)->value == 10.0f && !health_in && !mana_in, "Health and Mana of 10 are below any maximum: the gear changes nothing of them");
    cs.set_current(ids.health, 10000.0f);
    cs.set_current(ids.mana, 10000.0f);
    CHECK(cs.update(with_plain, changed, e) && cs.find(ids.health)->value == cs.find(ids.max_health)->value && cs.find(ids.mana)->value == cs.find(ids.max_mana)->value,
          "a current value above its maximum is clamped at the next recompute, even if the gear did not move the maximum");
}

void verify_character_rod_and_refusals(const StatBundle& b, const TableCatalog& cat, CharacterStats& cs, int64_t kPower) {
    const std::vector<WornItem> none;
    const WornItem with_rod[] = {{2, 1, 35}};
    std::vector<StatValue> changed;
    std::string e;
    const float base_before = cs.find(kPower)->base;
    CHECK(cs.update(with_rod, changed, e), "a fishing rod");
    const StatValue* power = cs.find(kPower);
    bool power_changed = false;
    for (const StatValue& v : changed) power_changed = power_changed || v.guid == kPower;
    CHECK(power && power_changed && power->equip == 100.0f && power->base == base_before && power->value == base_before + 100.0f,
          "its stat changes: the equipment 100 on top of the base, and the base is not touched");
    CharacterStats bad(b, cat, 99, 1);
    CHECK(!bad.start(none, e) && !e.empty() && !bad.started(), "an unknown class is refused by start");
    CharacterStats low(b, cat, 2, 0);
    CHECK(!low.start(none, e), "level 0 is refused");
}

void verify_wire_equality_and_clamp_current() {
    StatValue x, y;
    x.value = 1.0f;
    y.value = 1.0f;
    CHECK(same_on_the_wire(x, y), "equal values are the same on the wire");
    y.value = std::nextafter(1.0f, 2.0f);
    CHECK(!same_on_the_wire(x, y), "one ulp is a difference");
    x.flags = y.flags = kStatBuckets;
    y.value = 1.0f;
    y.equip = 1.0f;
    CHECK(!same_on_the_wire(x, y), "base and equipment count for a stat that carries them");
    x.flags = y.flags = 0;
    CHECK(same_on_the_wire(x, y), "...and not for one that does not");
    StatValue pz, nz, nan1, nan2;
    pz.value = 0.0f;
    nz.value = -0.0f;
    nan1.value = std::numeric_limits<float>::quiet_NaN();
    nan2.value = nan1.value;
    CHECK(pz.value == nz.value && !same_on_the_wire(pz, nz), "+0.0 and -0.0 compare equal as numbers and are a difference on the wire");
    CHECK(nan1.value != nan2.value && same_on_the_wire(nan1, nan2), "a NaN is not equal to itself as a number and is no difference on the wire");
    StatValue pb, nb;
    pb.flags = nb.flags = kStatBuckets;
    nb.base = -0.0f;
    CHECK(!same_on_the_wire(pb, nb), "...also for the base of a stat that carries it");
    nb = pb;
    nb.equip = -0.0f;
    CHECK(!same_on_the_wire(pb, nb), "...and its equipment");
    CHECK(clamp_current(100.0f, 50.0f) == 50.0f && clamp_current(10.0f, 50.0f) == 10.0f && clamp_current(50.0f, 50.0f) == 50.0f, "a current value is clamped to a lower maximum and stays below a higher one");
    CHECK(bits_of_f(clamp_current(-0.0f, 0.0f)) == bits_of_f(-0.0f) && bits_of_f(clamp_current(0.0f, -0.0f)) == bits_of_f(0.0f),
          "equal numbers keep the stored float (and its sign of zero): the value is not rewritten");
    CHECK(clamp_current(std::numeric_limits<float>::quiet_NaN(), 50.0f) == 50.0f, "a NaN current value is no value: the maximum replaces it");
}

void verify_character_prepare_commit_and_nan(const StatBundle& b, const TableCatalog& cat, const CharStatIds& ids) {
    const std::vector<WornItem> none;
    const WornItem with_heavy[] = {{1, 1, 2}};
    std::vector<StatValue> changed, again;
    std::string e;
    CharacterStats pc(b, cat, 2, 1);
    CHECK(pc.start(none, e), "(a fresh character)");
    const std::vector<StatValue> at_start = pc.values();
    CharacterStats::Next next, next2;
    CHECK(pc.prepare(with_heavy, changed, next, e) && !changed.empty() && next.faults == 0, "prepare: the heavy chest would change stats");
    bool untouched = pc.values().size() == at_start.size();
    for (size_t i = 0; untouched && i < at_start.size(); ++i) untouched = bits_of_f(pc.values()[i].value) == bits_of_f(at_start[i].value);
    CHECK(untouched, "...and the character's stats are as they were");
    CHECK(pc.prepare(with_heavy, again, next2, e) && again.size() == changed.size(), "preparing again gives the same change (nothing was committed)");
    pc.commit(std::move(next));
    float start_max = 0;
    for (const StatValue& v : at_start) start_max = v.guid == ids.max_health ? v.value : start_max;
    CHECK(pc.find(ids.max_health)->value > start_max, "commit makes them the character's");
    CHECK(pc.prepare(with_heavy, again, next2, e) && again.empty(), "and the same gear again is no change");
    CHECK(!CharacterStats(b, cat, 2, 1).prepare(with_heavy, again, next2, e) && contains(e, "not started"), "prepare before start is refused");
    pc.set_current(ids.health, std::numeric_limits<float>::quiet_NaN());
    CHECK(pc.update(with_heavy, changed, e) && pc.find(ids.health)->value == pc.find(ids.max_health)->value, "a NaN Health is the maximum after the next recompute");
    bool health_sent = false;
    for (const StatValue& v : changed) health_sent = health_sent || v.guid == ids.health;
    CHECK(health_sent, "...and the repair is sent");
    pc.set_current(ids.health, -0.0f);
    CHECK(pc.update(with_heavy, changed, e), "(Health -0.0)");
    CHECK(pc.update(with_heavy, changed, e) && changed.empty(), "a current value of -0.0 under the same maximum is no change");
}

void verify_single_overflow_equip_case(const StatBundle& b, int64_t kHook, const char* name, double val) {
    TableCatalog cat;
    ItemGear item;
    item.record = 1;
    item.stats = {block_stat(kHook, {val, val, val, val, val, val, val}, {val, val, val, val, val, val, val})};
    cat.items[1] = item;
    const WornItem worn[] = {{1, 1, 14}};
    StatInputs in;
    in.class_enum = 2;
    in.level = 1;
    in.mods = gear_inputs(cat, worn, Rounding::None).mods;
    StatResult res;
    std::string err;
    CHECKF(run_stats(b, in, StatState{}, res, err), "%s: the run", name);
    const StatOut* hook = nullptr;
    for (const StatOut& o : res.stats) hook = o.guid == kHook ? &o : hook;
    CHECKF(hook && std::isfinite(hook->equip) && hook->equip == 0 && std::isfinite(hook->value) && res.faults != 0,
           "%s: the run faults (the clamp hid the sum from the value) and the equipment is 0", name);
    CharacterStats stats(b, cat, 2, 1);
    std::string e;
    CHECKF(stats.start({}, e) && stats.faults() == 0, "%s: (a bare start, no fault)", name);
    CharacterStats::Next next;
    std::vector<StatValue> changed;
    CHECKF(stats.prepare(worn, changed, next, e) && next.faults != 0, "%s: prepare reports the fault", name);
    bool finite = true;
    for (const StatValue& v : next.values) finite = finite && std::isfinite(v.value) && std::isfinite(v.base) && std::isfinite(v.equip);
    CHECKF(finite, "%s: no value, base or equipment is infinite or NaN", name);
    CHECKF(stats.update(worn, changed, e) && stats.faults() != 0, "%s: update reports it too (faults())", name);
}

void verify_corpus_gather_blocks(const Corpus& c) {
    CHECKF(c.gathers.size() >= 4, "the corpus has %zu gatherhost blocks, want 4", c.gathers.size());
    size_t asked = 0, right = 0, rated = 0;
    for (const GatherBlock& b : c.gathers) {
        for (const GatherQuery& q : b.queries) {
            double args[1] = {static_cast<double>(q.role)};
            double out = -12345;
            const bool known = gather_host_fn(b.ctx, q.fn, q.name, q.fn == kHostArtisanStat ? args : nullptr, q.fn == kHostArtisanStat ? 1 : 0, out);
            ++asked;
            if (q.fn == kHostArtisanStat && q.want != 0) ++rated;
            if (known && std::memcmp(&out, &q.want, sizeof out) == 0) {
                ++right;
            } else {
                std::printf("  [FAIL] %s: fn %d \"%s\" role %d = %g (known %d), the corpus says %g\n", b.name.c_str(), q.fn, q.name.c_str(), q.role, out, known ? 1 : 0,
                            bits_to_double(q.want));
                ++g_fail;
            }
        }
    }
    CHECKF(asked >= 40 && right == asked, "%zu reads asked, %zu as the corpus says", asked, right);
    CHECKF(rated >= 4, "%zu of the reads are ratings a stat has", rated);
}

}  // namespace

void test_damage_corpus(const Corpus& c) {
    std::printf("damage formulas\n");
    size_t n = 0, bad = 0;
    for (const DamageCall& d : c.damages) {
        auto p = c.profiles.find(d.profile);
        auto iv = c.views.find(d.instigator);
        auto tv = c.views.find(d.target);
        if (p == c.profiles.end() || iv == c.views.end() || tv == c.views.end()) {
            CHECKF(false, "damage %s: no profile %s or view %s or %s", d.name.c_str(), d.profile.c_str(), d.instigator.c_str(), d.target.c_str());
            continue;
        }
        const double got = p->second.calculate(d.fn, iv->second, tv->second, d.hit);
        ++n;
        if (double_bits(got) != d.want) {
            ++bad;
            if (bad <= 5)
                std::printf("    damage %s: function %d gives %016llx, Go has %016llx\n", d.name.c_str(), d.fn, static_cast<unsigned long long>(double_bits(got)),
                            static_cast<unsigned long long>(d.want));
        }
    }
    CHECKF(bad == 0, "%zu of %zu damage calls differ from the Go engine's", bad, n);
    CHECK(n >= 500, "the corpus has its damage calls");
    std::printf("  %zu damage calls checked\n", n);
}

void test_gather_reads(const Corpus& c) {
    std::printf("the gathering reads against the shared corpus\n");
    verify_corpus_gather_blocks(c);
    GatherContext g;
    double out = 7;
    CHECK(!gather_host_fn(g, kHostNodeLast, "", nullptr, 0, out) && !gather_host_fn(g, 179, "", nullptr, 0, out) && !gather_host_fn(g, 185, "", nullptr, 0, out) &&
              !gather_host_fn(g, kHostPlayerLevel, "", nullptr, 0, out) && out == 7,
          "a function outside 180..184 is no gathering read");
    g.ratings[1001] = 215.5;
    g.quality = 31;
    g.item_rarity = 3;
    g.source_tier = 2;
    g.base_duration = 11.25;
    double role0[1] = {0}, role1[1] = {1};
    CHECK(gather_host_fn(g, kHostArtisanStat, "1001", role0, 1, out) && out == 215.5, "the caster's rating");
    CHECK(gather_host_fn(g, kHostArtisanStat, "1001", role1, 1, out) && out == 0, "the target has none");
    CHECK(gather_host_fn(g, kHostArtisanStat, "1001", nullptr, 0, out) && out == 215.5, "no role given: the caster");
    CHECK(gather_host_fn(g, kHostArtisanStat, "1002", role0, 1, out) && out == 0, "a stat with no rating reads 0");
    CHECK(gather_host_fn(g, kHostArtisanStat, "10x", role0, 1, out) && out == 0 && gather_host_fn(g, kHostArtisanStat, "", role0, 1, out) && out == 0, "a name that is no guid reads 0");
    CHECK(gather_host_fn(g, kHostRolledItemQuality, "", nullptr, 0, out) && out == 31 && gather_host_fn(g, kHostItemRarity, "", nullptr, 0, out) && out == 3 &&
              gather_host_fn(g, kHostSourceCertTier, "", nullptr, 0, out) && out == 2 && gather_host_fn(g, kHostGatherBaseSecs, "", nullptr, 0, out) && out == 11.25,
          "the quality, the rarity, the tier and the base duration");
}

void test_gear_rules() {
    std::printf("gear: what the worn items add\n");
    TableCatalog cat;
    verify_gear_quality_bands(cat);
    verify_gear_tags_and_multi_item_sum(cat);
    verify_gear_set_bonuses(cat);
}

void test_character_stats(const StatBundle& b) {
    std::printf("character stats: join, recompute, deltas, the current values\n");
    const CharStatIds ids{
        guid_named(b, "Constitution"),
        guid_named(b, "Mentality"),
        guid_named(b, "Health"),
        guid_named(b, "MaxHealth"),
        guid_named(b, "Mana"),
        guid_named(b, "MaxMana"),
        guid_named(b, "Artisan_Gathering_Fishing_Sport_Power"),
    };
    CHECK(ids.con && ids.ment && ids.health && ids.max_health && ids.mana && ids.max_mana && ids.power, "the stats are in the real graph");
    TableCatalog cat;
    ItemGear heavy;
    heavy.record = 1;
    heavy.stats = {block_stat(ids.con, {100, 100, 100, 100, 100, 100, 100}, {100, 100, 100, 100, 100, 100, 100}),
                   block_stat(ids.ment, {100, 100, 100, 100, 100, 100, 100}, {100, 100, 100, 100, 100, 100, 100})};
    cat.items[1] = heavy;
    ItemGear rod;
    rod.record = 2;
    rod.stats = {block_stat(ids.power, {100, 100, 100, 100, 100, 100, 100}, {100, 100, 100, 100, 100, 100, 100})};
    cat.items[2] = rod;
    ItemGear plain;
    plain.record = 3;
    cat.items[3] = plain;

    CharacterStats cs(b, cat, 2, 1);
    float bare_max = 0;
    verify_character_start_and_heavy_gear(cs, ids, bare_max);
    verify_character_current_clamping(cs, ids, bare_max);
    verify_character_rod_and_refusals(b, cat, cs, ids.power);
    verify_wire_equality_and_clamp_current();
    verify_character_prepare_commit_and_nan(b, cat, ids);
}

void test_equipment_beyond_a_float(const StatBundle& b, const StatBundle* dbl) {
    std::printf("character stats: an equipment sum no float32 holds, behind a clamp\n");
    const int64_t kHook = guid_named(b, "Artisan_Gathering_Fishing_Sport_Hook_Rate");
    CHECK(kHook != 0, "(Hook_Rate is in the real graph)");
    const double inf = std::numeric_limits<double>::infinity();
    struct OverflowCase {
        const char* name;
        double v;
    };
    const OverflowCase cases[] = {{"1e39", 1e39}, {"-1e39", -1e39}, {"+inf", inf}, {"-inf", -inf}, {"3.5e38", 3.5e38}};
    for (const OverflowCase& cs : cases) {
        verify_single_overflow_equip_case(b, kHook, cs.name, cs.v);
    }
    {
        TableCatalog cat;
        ItemGear item;
        item.record = 1;
        item.stats = {block_stat(kHook, {3e38, 3e38, 3e38, 3e38, 3e38, 3e38, 3e38}, {3e38, 3e38, 3e38, 3e38, 3e38, 3e38, 3e38})};
        cat.items[1] = item;
        const WornItem worn[] = {{1, 1, 14}};
        CharacterStats stats(b, cat, 2, 1);
        std::string e;
        std::vector<StatValue> changed;
        const StatValue* v = nullptr;
        CHECK(stats.start({}, e) && stats.update(worn, changed, e) && (v = stats.find(kHook)) != nullptr && stats.faults() == 0 && v->equip == 3e38f && v->value == 100.0f,
              "an equipment of 3e38 fits a float32: no fault, the equipment is held as it is and the value is the clamp's");
    }
    if (dbl) {
        const int64_t hook_d = guid_named(*dbl, "Artisan_Gathering_Fishing_Sport_Hook_Rate");
        TableCatalog cat;
        ItemGear item;
        item.record = 1;
        item.stats = {block_stat(hook_d, {1e39, 1e39, 1e39, 1e39, 1e39, 1e39, 1e39}, {1e39, 1e39, 1e39, 1e39, 1e39, 1e39, 1e39})};
        cat.items[1] = item;
        const WornItem worn[] = {{1, 1, 14}};
        CharacterStats stats(*dbl, cat, 2, 1);
        std::string e;
        CharacterStats::Next next;
        std::vector<StatValue> changed;
        CHECK(stats.start({}, e) && stats.prepare(worn, changed, next, e) && next.faults != 0, "a double-mode bundle: 1e39 is a number to the engine and a fault in the float the client holds");
        bool finite = true;
        for (const StatValue& v : next.values) finite = finite && std::isfinite(v.value) && std::isfinite(v.base) && std::isfinite(v.equip);
        CHECK(finite, "...and what the stats hold is finite");
    }
}

}  // namespace aoc::worldd::stats
