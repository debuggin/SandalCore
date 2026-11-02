// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "capture_foliage.h"
#include "protocol/repl/fname.h"
#include "session/player_world.h"
#include "test_support.h"

#include <spdlog/spdlog.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

inline int g_pass = 0;
inline int g_fail = 0;

#undef CHECK
#define CHECK(cond, msg)                       \
    do {                                       \
        if (cond) {                            \
            std::printf("  [ok ] %s\n", msg);  \
            g_pass++;                          \
        } else {                               \
            std::printf("  [FAIL] %s\n", msg); \
            g_fail++;                          \
        }                                      \
    } while (0)

inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;

inline bool file_exists(const char *p) {
    if (!p || !*p) {
        return false;
    }
    FILE *f = std::fopen(p, "rb");
    if (f) {
        std::fclose(f);
    }
    return f != nullptr;
}

inline const char *db_path() {
    const char *env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) {
        return env;
    }
#ifdef FOLIAGE_CONTENT_DB_PATH
    if (file_exists(FOLIAGE_CONTENT_DB_PATH)) {
        return FOLIAGE_CONTENT_DB_PATH;
    }
#endif
    return nullptr;
}

inline const char *tsv_path() {
    const char *env = std::getenv("AOC_FOLIAGE_MAP");
    if (env && *env) {
        return env;
    }
#ifdef FOLIAGE_TSV_PATH
    if (file_exists(FOLIAGE_TSV_PATH)) {
        return FOLIAGE_TSV_PATH;
    }
#endif
    return nullptr;
}

struct TsvPos {
    double x = 100;
    double y = 200;
    double z = 300;
};

inline std::string tsv_line(const char *def, const char *owner, const char *comp, int index, TsvPos p = {}) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "1\t%s\tCELL\t%s\t%s\t%d\t%.1f\t%.1f\t%.1f\t0\t0\t0\t1\t1\t1\tmesh\t\n", def, owner, comp, index, p.x, p.y, p.z);
    return buf;
}

inline bool tsv_parse_fails(const std::string &body, const char *want) {
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

inline bool match_want_start(const aoc::worldd::foliage::FoliageMap &m, const capture_foliage::Start &s, const WantStart &w) {
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

inline bool check_real_cell_centre(const aoc::worldd::foliage::FoliageMap &m) {
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

inline aoc::worldd::foliage::DefTimes oak() {
    aoc::worldd::foliage::DefTimes d;
    d.paint = aoc::worldd::foliage::kAdult;
    d.empty_seconds = 300;
    d.has[0] = d.has[1] = d.has[2] = true;
    d.life[0] = 1800;
    d.life[1] = 1800;
    d.life[2] = 0;
    return d;
}

inline aoc::worldd::foliage::DefTimes plant() {
    aoc::worldd::foliage::DefTimes d;
    d.paint = aoc::worldd::foliage::kJuvenile;
    d.empty_seconds = 1800;
    d.has[0] = d.has[1] = true;
    d.life[0] = 300;
    return d;
}

inline aoc::worldd::foliage::DefTimes stone() {
    aoc::worldd::foliage::DefTimes d;
    d.paint = aoc::worldd::foliage::kAdult;
    d.empty_seconds = 840;
    d.has[2] = true;
    return d;
}

struct World {
    std::optional<aoc::worldd::content::ContentDb> db;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<aoc::worldd::PlayerWorld> world;
    aoc::worldd::gather::Gathering *g = nullptr;
    aoc::worldd::foliage::Foliage *f = nullptr;

    bool open(const char *path, const char *tsv, bool all_cells = true) {
        std::string error;
        db = aoc::worldd::content::ContentDb::open(path, error);
        if (!db) {
            return false;
        }
        world = std::make_unique<aoc::worldd::PlayerWorld>(&*db, "", aoc::worldd::bootstrap::GameStateMode::Tier2, kServerId, guids);
        if (!world->ready()) {
            return false;
        }
        aoc::worldd::gather::Config cfg;
        if (!world->start_gathering(cfg, error)) {
            return false;
        }
        g = world->gathering();
        aoc::worldd::foliage::Config fc;
        fc.map_path = tsv;
        fc.all_cells = all_cells;
        if (!world->start_foliage(fc, error)) {
            return false;
        }
        f = world->foliage();
        return true;
    }

    uint32_t row_of(const std::string &owner, const std::string &comp, uint32_t index) const {
        const auto r = f->map().find(owner, comp, index);
        if (!r) {
            std::exit(2);
        }
        return *r;
    }

    std::unique_ptr<aoc::worldd::stats::CharacterStats> make_stats(int level = 25) {
        const aoc::worldd::bootstrap::StatContext ctx = world->stat_context();
        auto cs = std::make_unique<aoc::worldd::stats::CharacterStats>(*ctx.bundle, *ctx.gear, 2, level, ctx.rounding);
        std::string err;
        if (!cs->start({}, err)) {
            std::exit(2);
        }
        return cs;
    }
};

inline constexpr const char *kCell41_19 = "InstancedFoliageActor_25600_-41_19_0";

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

inline const aoc::worldd::foliage::DefInfo *find_def_by_prefix(World &w, const char *prefix, bool exact) {
    for (uint32_t r = 0; r < w.f->map().rows().size(); ++r) {
        const std::string &n = w.f->name_of(r);
        if (exact ? (n == prefix) : (n.rfind(prefix, 0) == 0)) {
            return &w.f->def_of(r);
        }
    }
    return nullptr;
}

using ClientName = std::pair<std::string, int32_t>;

inline ClientName client_fname(const std::string &s) {
    const size_t us = s.rfind('_');
    if (us == std::string::npos || us == 0 || us + 1 >= s.size()) {
        return {s, 0};
    }
    const std::string tail = s.substr(us + 1);
    const bool bad = tail.size() > 9 || (tail.size() > 1 && tail[0] == '0') || tail.find_first_not_of("0123456789") != std::string::npos;
    if (bad) {
        return {s, 0};
    }
    return {s.substr(0, us), std::stoi(tail) + 1};
}

struct WireCell {
    ClientName name;
    std::map<ClientName, bool> components;
    size_t blocks = 0;
};

inline std::optional<ClientName> name_of(const aoc::protocol::repl::Value *v) {
    const aoc::protocol::repl::NameValue *n = v ? v->get_if<aoc::protocol::repl::NameValue>() : nullptr;
    if (!n || n->hardcoded) {
        return std::nullopt;
    }
    return ClientName{n->text, n->number};
}

inline bool read_comp_from_wire(const aoc::protocol::schema::ClassSchema &comp, const aoc::protocol::repl::ContentBlock &blk, WireCell &out) {
    size_t p = 0;
    const auto c = aoc::protocol::repl::read_rep_block(comp, blk.payload.data(), blk.payload.bit_pos(), p);
    if (!c) {
        return false;
    }
    const auto uuid = name_of(c->get(comp, "FoliageComponentUUID"));
    if (!uuid) {
        return false;
    }
    const aoc::protocol::repl::Value *init = c->get(comp, "bInitialized");
    out.components[*uuid] = init && init->get_if<bool>() && *init->get_if<bool>();
    return true;
}

inline bool read_cell_from_wire(const aoc::protocol::repl::Actor &actor, WireCell &out) {
    const aoc::protocol::schema::ClassSchema *ifa = aoc::protocol::schema::gen::find_class_schema("AIntrepidStateIFA");
    const aoc::protocol::schema::ClassSchema *comp = aoc::protocol::schema::gen::find_class_schema("UIntrepidFoliageStateComponent");
    if (!ifa || !comp) {
        return false;
    }
    aoc::protocol::repl::ActorShadow shadow;
    std::vector<aoc::protocol::repl::ContentBlock> blocks;
    if (!aoc::protocol::repl::collect_open_blocks(actor, shadow, blocks) || blocks.empty() || blocks[0].sub) {
        return false;
    }
    out.blocks = blocks.size();
    size_t pos = 0;
    const auto a = aoc::protocol::repl::read_rep_block(*ifa, blocks[0].payload.data(), blocks[0].payload.bit_pos(), pos);
    const auto name = a ? name_of(a->get(*ifa, "FoliageName")) : std::nullopt;
    if (!name) {
        return false;
    }
    out.name = *name;
    for (size_t i = 1; i < blocks.size(); ++i) {
        if (!read_comp_from_wire(*comp, blocks[i], out)) {
            return false;
        }
    }
    return true;
}

struct TsvRowStats {
    size_t rows = 0;
    size_t resolved = 0;
    size_t no_actor = 0;
    size_t no_component = 0;
    size_t uninitialized = 0;
    size_t trees = 0;
    size_t plants = 0;
    size_t stones = 0;
    size_t trees_ok = 0;
    size_t plants_ok = 0;
    size_t stones_ok = 0;
    size_t foreign = 0;
};

inline std::vector<std::string> split_six_cols(const std::string &line) {
    std::vector<std::string> col;
    size_t from = 0;
    for (int i = 0; i < 6; ++i) {
        const size_t t = line.find('\t', from);
        col.push_back(line.substr(from, t == std::string::npos ? std::string::npos : t - from));
        if (t == std::string::npos) {
            break;
        }
        from = t + 1;
    }
    return col;
}

inline std::vector<std::string> parse_tsv_row_cols(const std::string &raw) {
    if (raw.empty() || raw[0] == '#') {
        return {};
    }
    std::string line = raw;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    if (line.empty()) {
        return {};
    }
    return split_six_cols(line);
}

inline void bump_tsv_category(const std::string &def_col, size_t &stones, size_t &trees, size_t &plants) {
    if (def_col.rfind("Stone_", 0) == 0 || def_col.rfind("Riverlands_Rock", 0) == 0) {
        ++stones;
    } else if (def_col.rfind("Tree_", 0) == 0) {
        ++trees;
    } else {
        ++plants;
    }
}

inline bool check_retail_actor_block(const aoc::worldd::world::WorldActor &a, const capture_foliage::IfaOpen &open, std::set<uint64_t> &server_ids) {
    const tsupport::Bits theirs = tsupport::bits_of_hex(open.actor.hex, open.actor.bits);
    const aoc::protocol::schema::ClassSchema &ifa = *aoc::protocol::schema::gen::find_class_schema("AIntrepidStateIFA");
    const auto retail_state = tsupport::with_exact(theirs, [&](const uint8_t *d, size_t, size_t n) {
        size_t pos = 0;
        return aoc::protocol::repl::read_rep_block(ifa, d, n, pos);
    });
    const aoc::protocol::repl::Value *server = retail_state ? retail_state->get(ifa, "AuthServerIDReplicated") : nullptr;
    const uint64_t *server_id = server ? server->get_if<uint64_t>() : nullptr;
    if (!server_id) {
        return false;
    }
    aoc::protocol::repl::Actor patched = a.actor;
    const aoc::protocol::repl::Value *now = patched.obj.state.get(*patched.obj.schema, "AuthServerIDReplicated");
    const bool ours_zero = now && now->get_if<uint64_t>() && *now->get_if<uint64_t>() == 0;
    patched.obj.state.set(*patched.obj.schema, "AuthServerIDReplicated", aoc::protocol::repl::Value::u64(*server_id));
    aoc::protocol::repl::ActorShadow patched_shadow;
    std::vector<aoc::protocol::repl::ContentBlock> patched_blocks;
    if (!aoc::protocol::repl::collect_open_blocks(patched, patched_shadow, patched_blocks) || patched_blocks.empty()) {
        return false;
    }
    if (!ours_zero || !(tsupport::Bits(patched_blocks[0].payload) == theirs)) {
        return false;
    }
    server_ids.insert(*server_id);
    return true;
}

struct RetailCompStats {
    size_t order_ok = 0;
    size_t comps_total = 0;
    size_t comps_ok = 0;
    size_t whole_total = 0;
    size_t whole_ok = 0;
};

inline void check_retail_components(const aoc::worldd::world::WorldActor &a, const capture_foliage::IfaOpen &open, const std::vector<aoc::protocol::repl::ContentBlock> &blocks, RetailCompStats &st) {
    bool order = true;
    for (uint32_t i = 0; i < open.component_count; ++i) {
        const capture_foliage::Component &c = open.components[i];
        ++st.comps_total;
        const tsupport::Bits ours(blocks[1 + i].payload);
        const aoc::protocol::repl::Subobject &sub = a.actor.subobjects[i];
        const aoc::protocol::repl::Value *uuid = sub.obj.schema ? sub.obj.state.get(*sub.obj.schema, "FoliageComponentUUID") : nullptr;
        const aoc::protocol::repl::NameValue *uuid_name = uuid ? uuid->get_if<aoc::protocol::repl::NameValue>() : nullptr;
        if (!sub.dynamic_class.has_value() || !uuid_name || aoc::protocol::repl::fname_text(*uuid_name) != c.uuid) {
            order = false;
        }
        if (c.has_states) {
            if (ours.size() <= static_cast<size_t>(c.block.bits) && ours == tsupport::bits_of_hex(c.block.hex, ours.size())) {
                ++st.comps_ok;
            }
        } else {
            ++st.whole_total;
            if (ours == tsupport::bits_of_hex(c.block.hex, c.block.bits)) {
                ++st.whole_ok;
                ++st.comps_ok;
            }
        }
    }
    if (order) {
        ++st.order_ok;
    }
}

inline std::string name_text(const aoc::protocol::repl::ObjectState &o, const char *path) {
    const aoc::protocol::repl::Value *v = o.state.get(*o.schema, path);
    const aoc::protocol::repl::NameValue *n = v ? v->get_if<aoc::protocol::repl::NameValue>() : nullptr;
    return n ? aoc::protocol::repl::fname_text(*n) : std::string();
}

inline size_t subobject_states_listed(const aoc::protocol::repl::Subobject &sub, uint64_t state) {
    const aoc::protocol::repl::FastArrayState *fa = sub.obj.arrays.find(aoc::protocol::repl::fast_array_layout(*sub.obj.schema, "IndexedStates")->field);
    if (!fa) {
        return 0;
    }
    size_t n = 0;
    for (const auto &item : fa->items()) {
        const auto s = item.values.values.find(1);
        if (s != item.values.values.end() && *s->second.get_if<uint64_t>() == state) {
            ++n;
        }
    }
    return n;
}

inline size_t states_listed(const aoc::protocol::repl::Actor &a, uint64_t state) {
    size_t n = 0;
    for (const aoc::protocol::repl::Subobject &sub : a.subobjects) {
        n += subobject_states_listed(sub, state);
    }
    return n;
}

inline aoc::worldd::Clock::time_point after(aoc::worldd::Clock::time_point t, double seconds_later) {
    return t + std::chrono::duration_cast<aoc::worldd::Clock::duration>(std::chrono::duration<double>(seconds_later));
}

inline std::string temp_dup_tsv_path() {
    const char *d = std::getenv("TEMP");
    if (!d || !*d) {
        d = std::getenv("TMP");
    }
    return (d && *d ? std::string(d) + "/" : std::string()) + "aoc_foliage_dup.tsv";
}

inline size_t working_set_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        return static_cast<size_t>(pmc.WorkingSetSize);
    }
#endif
    return 0;
}
