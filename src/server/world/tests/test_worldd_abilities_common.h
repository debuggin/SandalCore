// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "bootstrap/captured_setup.h"
#include "capture_blocks.h"
#include "content_db_fixture.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/abilities.h"
#include "session/player_world.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace bs = aoc::worldd::bootstrap;
namespace content = aoc::worldd::content;
namespace fs = std::filesystem;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;

inline int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                                                                                                                                                                                       \
    do {                                                                                                                                                                                                                                       \
        if (cond) {                                                                                                                                                                                                                            \
            std::printf("  [ok ] %s\n", msg);                                                                                                                                                                                                  \
            g_pass++;                                                                                                                                                                                                                          \
        } else {                                                                                                                                                                                                                               \
            std::printf("  [FAIL] %s\n", msg);                                                                                                                                                                                                 \
            g_fail++;                                                                                                                                                                                                                          \
        }                                                                                                                                                                                                                                      \
    } while (0)

inline constexpr int64_t kHealthRegen = 109343399171806ll;
inline constexpr int64_t kManaRegen = 109343399171796ll;
inline constexpr int64_t kMaxHealth = 108986356618873ll;
inline constexpr int64_t kMaxMana = 109183578756729ll;
inline constexpr int64_t kRestEffect = 109394092054139ll;
inline constexpr int64_t kEvasionEffect = 106223481149050ll;
inline constexpr int64_t kSprintEffect = 6064631641826394197ll;
inline constexpr int64_t kDisableRegenEffect = 6064631668308443138ll;
inline constexpr int64_t kMoveSpeedMult = 109228544644729ll;
inline constexpr int64_t kScepterCombo = 6064631845843960261ll;
inline constexpr int64_t kGuard = 107887054050936ll;
inline constexpr float k06 = 0.6f;

struct Rules {
    fs::path dir;
    std::optional<content::ContentDb> db;
    content::AbilityRules rules;

    Rules() {
        dir = content_fixture::make_temp_dir("worldd_abilities_");
        if (!content_fixture::make_db(dir / "content.db")) std::exit(2);
        std::string error;
        db = content::ContentDb::open((dir / "content.db").string(), error);
        if (!db) {
            std::printf("  [FAIL] the fixture content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        const auto r = db->ability_rules(error);
        if (!r) {
            std::printf("  [FAIL] the fixture's ability rules do not read: %s\n", error.c_str());
            std::exit(2);
        }
        rules = *r;
    }
    ~Rules() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

inline std::map<int64_t, double> level1_stats() { return {{content::kGuidStamina, 100}, {content::kGuidMaxStamina, 100}, {kHealthRegen, k06}, {kManaRegen, k06}, {kMaxHealth, 90}, {kMaxMana, 90}, {kMoveSpeedMult, 1.0}}; }

inline std::vector<int32_t> ints(std::initializer_list<int32_t> l) { return l; }
inline bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

struct Sim {
    aoc::worldd::AbilityRuntime rt;
    std::vector<aoc::worldd::AbilityEvent> log;
    double now = 0;
    bool moving = false;
    int64_t us = 0;

    explicit Sim(const content::AbilityRules &rules, std::map<int64_t, double> stats = level1_stats(), double world_time = 1000) : rt(rules, std::move(stats), world_time) {}

    void run_to(double t, double step = 0.05) {
        const int64_t end = std::llround(t * 1e6), by = std::llround(step * 1e6);
        while (us + by <= end) {
            us += by;
            now = static_cast<double>(us) / 1e6;
            if (moving) rt.set_moving(true, now);
            rt.tick(now);
            take();
        }
    }
    void take() {
        for (aoc::worldd::AbilityEvent &e : rt.take_events())
            log.push_back(std::move(e));
    }
    aoc::worldd::Refusal trigger(int64_t guid, int32_t id, const aoc::worldd::TriggerInfo &info = {}) {
        const aoc::worldd::Refusal r = rt.trigger(guid, id, now, info);
        take();
        return r;
    }
    bool cancel(int32_t id) {
        const bool b = rt.cancel(id, now);
        take();
        return b;
    }
    void move(bool m) {
        moving = m;
        rt.set_moving(m, now);
        take();
    }
    std::vector<const aoc::worldd::AbilityEvent *> with(const char *part) const {
        std::vector<const aoc::worldd::AbilityEvent *> out;
        for (const aoc::worldd::AbilityEvent &e : log) {
            if (contains(e.note, part)) out.push_back(&e);
        }
        return out;
    }
    std::vector<int32_t> ends() const {
        std::vector<int32_t> out;
        for (const aoc::worldd::AbilityEvent &e : log) {
            if (e.end_ability) out.push_back(*e.end_ability);
        }
        return out;
    }
    std::vector<float> series_of(int64_t record) const {
        std::vector<float> out;
        for (const aoc::worldd::AbilityEvent &e : log) {
            for (const bs::StatChange &s : e.change.stats) {
                if (s.record == record) out.push_back(s.value);
            }
        }
        return out;
    }
    std::vector<float> stamina_series() const {
        std::vector<float> out;
        for (const aoc::worldd::AbilityEvent &e : log) {
            for (const bs::StatChange &s : e.change.stats) {
                if (s.record == content::kGuidStamina) out.push_back(s.value);
            }
        }
        return out;
    }
};

inline std::optional<float> stat_of(const bs::AbilityChange &change, int64_t record) {
    for (const bs::StatChange &s : change.stats) {
        if (s.record == record) return s.value;
    }
    return std::nullopt;
}

inline aoc::protocol::repl::CodecOptions stat_options() {
    aoc::protocol::repl::CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}

inline const schema::ClassSchema &class_named(const char *name) {
    const schema::ClassSchema *c = gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

inline std::optional<std::vector<aoc::protocol::repl::FastArrayField>> fast_fields(const aoc::protocol::emit::BunchWriter &payload, const aoc::protocol::emit::FIntrepidNetworkGUID &object, const char *cls_name) {
    const auto views = aoc::protocol::repl::read_content_blocks(payload.data(), payload.bit_pos(), 0);
    if (!views) return std::nullopt;
    for (const aoc::protocol::repl::BlockView &v : *views) {
        if (v.header.is_actor || !(v.header.object == object)) continue;
        size_t p = v.payload_pos;
        return aoc::protocol::repl::read_fast_array_fields(class_named(cls_name), payload.data(), v.payload_pos + v.payload_bits, p, stat_options());
    }
    return std::nullopt;
}

inline const aoc::protocol::repl::FastArrayField *field_named(const std::vector<aoc::protocol::repl::FastArrayField> &fields, const char *cls_name, const char *name) {
    const schema::NetField *f = class_named(cls_name).field(name);
    if (!f) return nullptr;
    for (const aoc::protocol::repl::FastArrayField &x : fields) {
        if (x.field == f->index) return &x;
    }
    return nullptr;
}

inline bs::PlayerSetup captured_setup_with_owner_key(int32_t owner_key) {
    auto setup = bs::captured_character_setup();
    if (!setup) {
        std::printf("  [FAIL] the captured character's setup does not build\n");
        std::exit(2);
    }
    setup->stats.owner.array_key = owner_key;
    return std::move(*setup);
}

struct Pawn {
    bs::PlayerBootstrap pb;
    explicit Pawn(int32_t owner_key = 690) : pb(captured_setup_with_owner_key(owner_key), bs::GameStateMode::Tier2) {
        const auto join = pb.ok() ? pb.join_sequence() : std::nullopt;
        if (!join) {
            std::printf("  [FAIL] the captured character does not join: %s\n", pb.error().c_str());
            std::exit(2);
        }
        for (const bs::Send &s : *join) {
            for (const auto &guids : s.emission.exported_by_bunch)
                pb.mark_exported(guids);
        }
    }
    std::optional<bs::Send> update(const bs::AbilityChange &c) {
        if (!pb.apply_ability_change(c)) return std::nullopt;
        return pb.ability_update();
    }
};

inline const aoc::protocol::repl::Value *item_value(const aoc::protocol::repl::ElementState &e, uint16_t handle) {
    const auto it = e.values.find(handle);
    return it == e.values.end() ? nullptr : &it->second;
}

template <class T> inline T item_get(const aoc::protocol::repl::ElementState &e, uint16_t handle, T def = T{}) {
    const aoc::protocol::repl::Value *v = item_value(e, handle);
    const T *p = v ? v->get_if<T>() : nullptr;
    return p ? *p : def;
}

