// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "stats/character_stats.h"
#include "stats/damage.h"
#include "stats/gather_host.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

namespace aoc::worldd::stats {

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

inline double bits_to_double(uint64_t b) { return std::bit_cast<double>(b); }
inline uint64_t double_bits(double d) { return std::bit_cast<uint64_t>(d); }
inline uint64_t hex64(const std::string& s) { return std::stoull(s, nullptr, 16); }
inline uint32_t bits_of_f(float f) { return std::bit_cast<uint32_t>(f); }

class TableState : public StatState {
public:
    std::map<std::pair<uint64_t, int64_t>, double> effects;
    std::map<std::string, double> tags;
    std::set<std::tuple<uint64_t, int64_t, uint64_t, int64_t>> skills;
    bool combat = false, moving_ = false, unkillable_ = false;
    double actor = 0, npc = 0;
    double dist[8] = {};
    std::map<std::string, double> professions;
    std::map<std::pair<uint64_t, int64_t>, double> consumed;

    double effect_count(const RecordRef& r) const override {
        auto it = effects.find({r.type_id, r.guid});
        return it == effects.end() ? 0 : it->second;
    }
    double effect_count_by_tag(const std::string& t) const override {
        auto it = tags.find(t);
        return it == tags.end() ? 0 : it->second;
    }
    bool skill_purchased(const RecordRef& c, const RecordRef& s) const override {
        return skills.count({c.type_id, c.guid, s.type_id, s.guid}) != 0;
    }
    bool in_combat() const override { return combat; }
    bool moving() const override { return moving_; }
    bool unkillable() const override { return unkillable_; }
    double actor_guid() const override { return actor; }
    double profession_level(double, const std::string& p) const override {
        auto it = professions.find(p);
        return it == professions.end() ? 1 : it->second;
    }
    double consumed_item_stat(const RecordRef& r) const override {
        auto it = consumed.find({r.type_id, r.guid});
        return it == consumed.end() ? 0 : it->second;
    }
    double npc_elite_rank() const override { return npc; }
    double npc_stat_distribution(int i) const override { return i >= 0 && i < 8 ? dist[i] : 0; }
};

class MapView : public StatView {
public:
    std::map<int64_t, double> v;
    double stat_value(int64_t guid) const override {
        auto it = v.find(guid);
        return it == v.end() ? 0 : it->second;
    }
};

struct DamageCall {
    std::string name, profile, instigator, target;
    int fn = 0;
    DamageHit hit;
    uint64_t want = 0;
};

struct Want {
    int64_t guid;
    uint64_t value;
};

struct Cap {
    int64_t guid;
    uint32_t bits;
    std::string match;
};

struct Case {
    std::string name, bundle;
    StatInputs in;
    TableState state;
    std::vector<Want> want;
    uint64_t digest = 0;
    size_t members = 0;
    int faults = 0, ignored = 0;
    std::vector<Cap> caps;
};

struct Hostile {
    std::string name;
    bool fix;
    bool whole = false;
    size_t pos, del;
    std::vector<uint8_t> ins;
};

struct FuzzLine {
    std::string bundle;
    uint64_t seed;
    int n;
    size_t accepted;
    uint64_t hash;
};

struct Refusal {
    std::string name, bundle;
    int class_enum, level;
};

struct GatherQuery {
    int fn = 0;
    std::string name;
    int role = 0;
    uint64_t want = 0;
};

struct GatherBlock {
    std::string name;
    GatherContext ctx;
    std::vector<GatherQuery> queries;
};

struct Corpus {
    std::vector<GatherBlock> gathers;
    std::vector<FuzzLine> fuzzes;
    std::vector<Refusal> refusals;
    std::map<std::string, std::vector<uint8_t>> bundles;
    std::vector<std::pair<std::string, std::string>> derived;
    std::vector<Case> cases;
    std::vector<Hostile> hostiles;
    std::map<std::string, DamageProfile> profiles;
    std::map<std::string, MapView> views;
    std::vector<DamageCall> damages;
};

inline uint64_t digest(const StatResult& r) {
    uint64_t h = 14695981039346656037ull;
    auto put = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    auto u64 = [&](uint64_t v) {
        uint8_t b[8];
        for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
        put(b, 8);
    };
    for (const StatOut& s : r.stats) {
        u64(static_cast<uint64_t>(s.guid));
        u64(double_bits(s.value));
        u64(double_bits(s.base));
        u64(double_bits(s.equip));
        const uint8_t flag = s.has_regen ? 1 : 0;
        put(&flag, 1);
        if (s.has_regen) u64(double_bits(s.regen));
    }
    return h;
}

inline const char* compare(double got, double want) {
    const double g = static_cast<double>(static_cast<float>(got));
    if (g == want) return "exact";
    if (std::fabs(g - want) <= 1e-6 * std::max(1.0, std::fabs(want))) return "near";
    return "miss";
}

inline void fix_crc(std::vector<uint8_t>& b) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 16; i < b.size(); ++i) {
        c ^= b[i];
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    c ^= 0xFFFFFFFFu;
    for (int i = 0; i < 4; ++i) b[12 + i] = static_cast<uint8_t>(c >> (8 * i));
}

inline int64_t guid_named(const StatBundle& b, const char* name) {
    for (size_t i = 0; i < b.stats.size(); ++i) {
        if (b.stat_name(i) == name) return b.stats[i].guid;
    }
    return 0;
}

void test_damage_corpus(const Corpus& c);
void test_gather_reads(const Corpus& c);
void test_gear_rules();
void test_character_stats(const StatBundle& b);
void test_equipment_beyond_a_float(const StatBundle& b, const StatBundle* dbl);

}  // namespace aoc::worldd::stats
