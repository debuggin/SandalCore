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

inline bool near_d(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct Ref {
    const char *name;
    size_t bits;
    const char *hex;
};
template <class B> inline Ref ref(const B &b) { return {b.name, b.bits, b.hex}; }

inline bool same_as(const aoc::protocol::emit::BunchWriter &w, const Ref &b) {
    const auto want = capture_blocks::bytes_of(b.hex);
    size_t diff = 0;
    if (w.bit_pos() != b.bits) {
        std::printf("    wrote %zu bits, expected %zu (%s)", w.bit_pos(), b.bits, b.name);
        const size_t n = w.bit_pos() < b.bits ? w.bit_pos() : b.bits;
        if (!capture_blocks::same_bit_range(w.data(), want.data(), n, &diff)) std::printf("; first differing bit %zu", diff);
        std::printf("\n");
        return false;
    }
    if (!capture_blocks::same_bit_range(w.data(), want.data(), b.bits, &diff)) {
        std::printf("    first differing bit %zu (%s)\n", diff, b.name);
        return false;
    }
    return true;
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

template <class B> inline double captured_start_time(const B &b, const bs::PlayerSetup &setup) {
    aoc::protocol::emit::BunchWriter w;
    const auto bytes = capture_blocks::bytes_of(b.hex);
    w.write_bit_range(bytes.data(), 0, b.bits);
    const auto fields = fast_fields(w, setup.guids.stats, "UAoCStatsComponent");
    const aoc::protocol::repl::FastArrayField *fx = fields ? field_named(*fields, "UAoCStatsComponent", "CurrentEffects") : nullptr;
    if (!fx || fx->delta.changed.size() != 1) return 0;
    const auto it = fx->delta.changed[0].values.values.find(3);
    return it != fx->delta.changed[0].values.values.end() && it->second.get_if<double>() ? *it->second.get_if<double>() : 0;
}

inline bs::ActiveAbilityItem rest_item(int64_t instance, int32_t counter, float position = 0, bool jump = false) {
    bs::ActiveAbilityItem a;
    a.ability_guid = content::kGuidRest;
    a.server_instance_id = instance;
    a.targets_self = true;
    a.montage_path = std::string(bs::kRestMontagePath);
    a.montage_position = position;
    a.may_jump_to_position = jump;
    a.animation_counter = counter;
    return a;
}

inline bs::EffectItem rest_effect(int64_t id, double start) {
    bs::EffectItem e;
    e.effect_id = id;
    e.effect_guid = kRestEffect;
    e.start_time = start;
    e.duration = aoc::worldd::AbilityRuntime::kInfiniteDuration;
    e.tick_time = 0;
    e.stacks = 1;
    return e;
}

inline void regen_to(bs::AbilityChange &c, float v) {
    c.stats.push_back({kHealthRegen, v});
    c.stats.push_back({kManaRegen, v});
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

inline std::map<int64_t, double> stats_of(const bs::PlayerSetup &setup) {
    std::map<int64_t, double> stats;
    for (const bs::StatArray *a : {&setup.stats.owner, &setup.stats.owner_proxy, &setup.stats.everyone_proxy}) {
        for (const bs::StatItem &it : a->items)
            stats[it.record] = std::bit_cast<float>(it.value);
    }
    return stats;
}

inline const aoc::protocol::repl::Value *item_value(const aoc::protocol::repl::ElementState &e, uint16_t handle) {
    const auto it = e.values.find(handle);
    return it == e.values.end() ? nullptr : &it->second;
}

template <class T> inline T item_get(const aoc::protocol::repl::ElementState &e, uint16_t handle, T def = T{}) {
    const aoc::protocol::repl::Value *v = item_value(e, handle);
    const T *p = v ? v->get_if<T>() : nullptr;
    return p ? *p : def;
}

inline std::vector<aoc::protocol::repl::FastArrayDelta::Changed> items_of(const std::optional<bs::Send> &s, const aoc::protocol::emit::FIntrepidNetworkGUID &object, const char *cls, const char *field) {
    const auto fields = s ? fast_fields(s->emission.payload, object, cls) : std::nullopt;
    const aoc::protocol::repl::FastArrayField *f = fields ? field_named(*fields, cls, field) : nullptr;
    return f ? f->delta.changed : std::vector<aoc::protocol::repl::FastArrayDelta::Changed>{};
}

inline bool same_guid(const aoc::protocol::repl::Value *v, uint64_t id) {
    const aoc::protocol::emit::FIntrepidNetworkGUID *g = v ? v->get_if<aoc::protocol::emit::FIntrepidNetworkGUID>() : nullptr;
    return g && g->ObjectId == id;
}

struct RetailItemSpec {
    int64_t ability = 0;
    const char *group = "";
    uint64_t montage = 0;
    uint16_t yaw = 0;
};

inline bool empty_array_field(const aoc::protocol::repl::ElementState &e, uint16_t handle) {
    const aoc::protocol::repl::Value *v = item_value(e, handle);
    if (!v) return true;
    const auto *arr = v->get_if<aoc::protocol::repl::ArrayValue>();
    return arr && arr->elements.empty();
}

inline bool retail_item_defaults(const aoc::protocol::repl::ElementState &e) {
    return empty_array_field(e, 3) && empty_array_field(e, 4) && !item_get<bool>(e, 5) && item_get<int64_t>(e, 6) == 0 && item_get<float>(e, 7) == 0.0f && item_get<float>(e, 8) == 0.0f && item_get<bool>(e, 9) &&
           item_get<float>(e, 12) == 1.0f && item_get<float>(e, 13) == 0.0f && !item_get<bool>(e, 14);
}

inline bool retail_item_shape(const aoc::protocol::repl::ElementState &e, const bs::PlayerSetup &setup, const RetailItemSpec &spec) {
    const auto *g = item_value(e, 10) ? item_value(e, 10)->get_if<aoc::protocol::repl::NameValue>() : nullptr;
    const auto *who = item_value(e, 17) ? item_value(e, 17)->get_if<aoc::protocol::emit::FIntrepidNetworkGUID>() : nullptr;
    return retail_item_defaults(e) && item_get<int64_t>(e, 1) == spec.ability && g && g->text == spec.group && same_guid(item_value(e, 11), spec.montage) && item_get<uint64_t>(e, 16) == spec.yaw && who && *who == setup.guids.pawn;
}

inline bool retail_effect_shape(const aoc::protocol::repl::ElementState &e, const bs::PlayerSetup &setup, int64_t guid, float duration, float tick) {
    bool same_character = true;
    for (uint16_t i = 0; i < 4; ++i) {
        const int64_t word = static_cast<int32_t>(setup.character_guid[i]);
        same_character = same_character && item_get<int64_t>(e, static_cast<uint16_t>(7 + i)) == word && item_get<int64_t>(e, static_cast<uint16_t>(11 + i)) == word;
    }
    return item_get<int64_t>(e, 2) == guid && item_get<float>(e, 4) == duration && item_get<float>(e, 5) == tick && item_get<int64_t>(e, 6) == 1 && same_character && !item_get<bool>(e, 15) && item_get<int64_t>(e, 16) == 0;
}
