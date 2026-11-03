// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "ability_rig.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/schema/generated/index.h"
// C.O.A.C_Y_B

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <random>
#include <set>
#include <sstream>

namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace content = aoc::worldd::content;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace svc = aoc::worldd::svc;
namespace world = aoc::worldd::world;
namespace fs = std::filesystem;
using aoc::worldd::AbilityRuntime;
using aoc::worldd::Clock;
using aoc::worldd::Connection;
using aoc::worldd::ConnectionConfig;
using aoc::worldd::InBunch;
using aoc::worldd::PlayerSession;
using aoc::worldd::PlayerWorld;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;

inline std::ostringstream g_log;
inline int g_pass = 0, g_fail = 0;

inline void check_impl(bool cond, const char *msg) {
    std::printf(cond ? "  [ok ] %s\n" : "  [FAIL] %s\n", msg);
    cond ? ++g_pass : ++g_fail;
}
#define CHECK(cond, msg) check_impl(static_cast<bool>(cond), (msg))
#define CHECKF(cond, ...)                                                                                                                                                          \
    do {                                                                                                                                                                           \
        std::printf((cond) ? "  [ok ] " : "  [FAIL] ");                                                                                                                            \
        std::printf(__VA_ARGS__);                                                                                                                                                  \
        std::printf("\n");                                                                                                                                                         \
        (cond) ? ++g_pass : ++g_fail;                                                                                                                                              \
    } while (0)

inline constexpr int64_t kTempHealth = 6064629069966082048ll;
inline const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const Clock::time_point t0{seconds(5000)};
inline constexpr uint32_t kServerId = 60, kRandomizer = 0x6ee87ee4u;
inline constexpr double kX = -777664.5, kY = 616578.6875, kZ = 15952.0498046875;
inline constexpr uint16_t kSpawnYaw = 16550;
// C:Y:B captures, C.O.A code

inline constexpr int64_t kStamina = 6064629575339934476ll, kHealthRegen = 109343399171806ll, kManaRegen = 109343399171796ll;
inline constexpr int64_t kSprint = 6064631641648332884ll, kDodge = 107258680465016ll, kRest = 109393746155128ll;
inline constexpr int64_t kScepterCombo = 6064631845843960261ll, kQuickWit = 6064630736949673984ll, kNotRun = 4242424242ll;
inline constexpr int64_t kStaminaRegeneration = 6064631663405498419ll, kRestEffect = 109394092054139ll, kSprintEffect = 6064631641826394197ll;
inline constexpr int64_t kEvasionEffect = 106223481149050ll, kDisableRegenEffect = 6064631668308443138ll, kMoveSpeedMult = 109228544644729ll;
inline constexpr int64_t kFlourish = 6064630877217018486ll, kFortissimo = 6064632972407039861ll, kMaddening = 6064630735633055744ll;
inline constexpr int64_t kGleeEffect = 6064630759601406637ll, kPepEffect = 6064631156112822511ll, kResonanceEffect = 6064632468947286527ll;
inline constexpr int64_t kHealthStat = 108986352686712ll, kManaStat = 109183576135288ll, kMaxManaStat = 109183578756729ll, kResonanceStat = 6064632920367763592ll;
inline constexpr int64_t kMitigationStat = 6064628617716498440ll, kTargetingSelf = 6064628968549646338ll;
inline constexpr int64_t kShielding = 6064630809885999134ll, kLingerTick = 6064630809886064675ll, kAoeTargeting = 6064630809886064674ll;

inline void fail_exit(const char *label, const std::string &detail) {
    std::printf("  [FAIL] %s: %s\n", label, detail.c_str());
    std::exit(2);
}

inline const schema::ClassSchema &class_named(const char *name) {
    const schema::ClassSchema *c = gen::find_class_schema(name);
    if (!c) fail_exit("no schema", name);
    return *c;
}

struct Fixture {
    fs::path dir;
    std::optional<content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;

    explicit Fixture(bool use_real = false) {
        dir = content_fixture::make_temp_dir("worldd_ability_session_");
        fs::path db = dir / "content.db";
        const char *real = std::getenv("AOC_CONTENT_DB");
        db = use_real ? fs::path(ability_rig::real_db_path()) : (real && *real ? fs::path(real) : db);
        if (!use_real && !(real && *real) && !content_fixture::make_db(db)) std::exit(2);
        std::string error;
        content = content::ContentDb::open(db.string(), error);
        if (!content) fail_exit("the content.db does not open", error);
        world = std::make_unique<PlayerWorld>(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
        if (!world->ready()) fail_exit("the world is not ready", world->content_problem());
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

inline svc::Character cyb_record() {
    svc::Character c;
    c.id = kCyb;
    c.account_id = 3;
    c.name = "cyb";
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    return c;
}
inline svc::Character bard25() {
    svc::Character c = cyb_record();
    c.level = 25;
    return c;
}
inline svc::Character class25(int class_id, const char *name, int64_t archetype) {
    svc::Character c = cyb_record();
    c.class_id = class_id;
    c.class_name = name;
    c.archetype_id = archetype;
    c.level = 25;
    return c;
}
inline bs::LoginIdentity identity() { return {kMachine, kMachine}; }
inline double g_world_time_offset = 0.0;
inline double world_time_at(Clock::time_point t) { return std::chrono::duration<double>(t - t0).count() + g_world_time_offset; }
inline repl::CodecOptions client_options() {
    repl::CodecOptions o;
    o.inline_objects = true;
    return o;
}
inline repl::NetworkMove move_at(float ts, double x, bool moving) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, moving ? 100 : 0, 0, 0);
    m.location = repl::quantize_vector(100, x, kY, kZ);
    m.control_rotation.yaw = kSpawnYaw;
    m.movement_mode = repl::kMoveWalking;
    m.input_bits = moving ? 1 : 0;
    return m;
}
inline BunchWriter client_move(const repl::NetworkMove &mv) {
    const schema::ClassSchema &cls = class_named("PlayerPawn_C");
    const schema::NetField *f = cls.field("ServerMovePacked");
    BunchWriter out, params, field;
    repl::MovePacked p;
    p.new_move = mv;
    if (!f || !repl::write_move_packed(params, p) || repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}
inline BunchWriter ability_rpc(const emit::FIntrepidNetworkGUID &ability, const char *name, const repl::RpcParams &params) {
    const schema::ClassSchema &cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema *rpc = cls.rpc(name);
    BunchWriter out, field;
    bool failed = false;
    if (!rpc || repl::write_rpc_field(cls, *rpc, params, field, client_options(), &failed) == 0 || failed) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = ability;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}
// -- CoA x C_Y_B --
inline BunchWriter trigger_bunch(const emit::FIntrepidNetworkGUID &ability, int64_t guid, int32_t id) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    return ok ? ability_rpc(ability, "ServerTriggerAbility", p) : BunchWriter();
}

inline BunchWriter cancel_bunch(const emit::FIntrepidNetworkGUID &ability, int32_t id) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("ServerNotifyHeldAbilityCancelled");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "CancelledTime", repl::Value::f32(0.25f));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "CancelSectionIndex", repl::Value::i64(-1));
    return ok ? ability_rpc(ability, "ServerNotifyHeldAbilityCancelled", p) : BunchWriter();
}

inline repl::CodecOptions stat_options() {
    repl::CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}

struct Mirror {
    bs::PlayerSetup setup;
    std::map<int64_t, float> stats;
    struct ActiveAbility {
        int64_t ability = 0, instance = 0;
        float montage_position = 0;
        bool may_jump = false, has_target = false, has_montage = false;
        int32_t animation_counter = 0;
        std::string montage_group;
        uint16_t mesh_yaw = 0;
        uint64_t montage = 0;
    };
    std::map<int32_t, ActiveAbility> abilities;
    std::map<int32_t, int64_t> effects, cooldowns;
    std::vector<std::pair<double, float>> stamina;
    std::vector<std::pair<double, int32_t>> ends;
    bool end_before_removal = false;
    std::vector<repl::HitInstance> hits;
    std::vector<std::pair<float, float>> shields;
    size_t hit_calls = 0, unreadable = 0, chains = 0, chains_with_exports = 0;
    std::map<int32_t, int64_t> server_of;
    struct ChannelEvent {
        uint32_t channel = 0;
        bool open = false, close = false;
        double at = 0;
        size_t bits = 0, exports = 0;
    };
    std::vector<ChannelEvent> other;
    // [C | O | A + Cyb]
    std::vector<repl::LootPayload> loot_windows;
    std::vector<repl::ItemStorageEvent> storage_events;
    std::vector<std::string> pc_calls, pawn_calls;
    std::vector<double> set_times;
    size_t listeners_removed = 0, xp_updates = 0;
    std::vector<std::array<double, 3>> death_locations;
    struct Choices {
        std::vector<int64_t> closest;
        int64_t home = 0;
        bool previous_invalid = false;
    };
    std::vector<Choices> choices;

    explicit Mirror(const bs::PlayerSetup &s) : setup(s) {
        for (const bs::StatArray *a : {&s.stats.owner, &s.stats.owner_proxy, &s.stats.everyone_proxy}) {
            for (const bs::StatItem &it : a->items) stats[it.record] = std::bit_cast<float>(it.value);
        }
    }
    float stamina_now() const {
        const auto it = stats.find(kStamina);
        return it == stats.end() ? -1.0f : it->second;
    }
    bool has_ability(int64_t guid) const {
        for (const auto &[id, a] : abilities)
            if (a.ability == guid) return true;
        return false;
    }
    bool has_effect(int64_t guid) const {
        for (const auto &[id, g] : effects)
            if (g == guid) return true;
        return false;
    }
    std::vector<int32_t> end_ids() const {
        std::vector<int32_t> out;
        for (const auto &e : ends) out.push_back(e.second);
        return out;
    }

    void absorb_pc_call(const repl::RpcCall &c) {
        if (!c.decoded) return;
        const std::string name = c.rpc->name;
        pc_calls.push_back(name);
        if (name == "ClientSetTime") {
            const repl::Value *v = repl::rpcf::value_of(*c.rpc, c.params, "NewSynchronizedTime");
            v && v->get_if<double>() ? set_times.push_back(*v->get_if<double>()) : (void)++unreadable;
        } else if (name == "ClientReceiveLootStoragePayload") {
            const auto w = repl::parse_loot_payload(c);
            w ? loot_windows.push_back(*w) : (void)++unreadable;
        } else if (name == "ClientReceiveStorageEvent") {
            const auto e = repl::parse_storage_event(c);
            e ? storage_events.push_back(*e) : (void)++unreadable;
        } else if (name == "ClientRemoveListenerFromStorage") {
            ++listeners_removed;
        } else if (name == "ClientHitConfirmationsReliable") {
            const auto conf = repl::parse_hit_confirmations(c);
            if (!conf) {
                ++unreadable;
                return;
            }
            ++hit_calls;
            for (const repl::HitInstance &h : conf->hits) hits.push_back(h);
        }
    }

    void absorb_pc(const fake::Delivered &d) {
        if (d.open || d.close || d.content.bit_pos() <= 8) return;
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) return;
        for (const repl::BlockView &v : *views) {
            if (!v.header.is_actor && v.header.object == setup.guids.xp_tracker) ++xp_updates;
            if (!v.header.is_actor || v.header.has_rep_layout) continue;
            size_t p = v.payload_pos;
            const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), d.content.data(), v.payload_pos + v.payload_bits, p);
            if (!calls) continue;
            for (const repl::RpcCall &c : *calls) absorb_pc_call(c);
        }
    }
    // Cyb + C.O.A

    void absorb(const fake::Delivered &d, double at) {
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) {
            ++unreadable;
            return;
        }
        ++chains;
        if (!d.exports.empty()) ++chains_with_exports;
        for (const repl::BlockView &v : *views) {
            if (v.header.is_actor) {
                if (!v.header.has_rep_layout) pawn_calls_of(d, v);
                continue;
            }
            const size_t end = v.payload_pos + v.payload_bits;
            size_t p = v.payload_pos;
            if (v.header.object == setup.guids.stats) {
                const auto fields = repl::read_fast_array_fields(class_named("UAoCStatsComponent"), d.content.data(), end, p, stat_options());
                if (!fields) ++unreadable;
                if (fields)
                    for (const repl::FastArrayField &f : *fields) stats_field(f, at);
            } else if (v.header.object == setup.guids.ability) {
                absorb_ability_block(d, v, at);
            }
        }
    }

  private:
    void absorb_ability_block(const fake::Delivered &d, const repl::BlockView &v, double at) {
        const size_t end = v.payload_pos + v.payload_bits;
        size_t p = v.payload_pos;
        if (const auto fields = repl::read_fast_array_fields(class_named("UAoCAbilityComponent"), d.content.data(), end, p, stat_options())) {
            for (const repl::FastArrayField &f : *fields) ability_field(f);
            return;
        }
        p = v.payload_pos;
        const auto calls = repl::read_rpc_fields(class_named("UAoCAbilityComponent"), d.content.data(), end, p);
        if (!calls) ++unreadable;
        if (calls)
            for (const repl::RpcCall &c : *calls) absorb_end_ability_call(c, at);
    }

    void absorb_end_ability_call(const repl::RpcCall &c, double at) {
        if (!c.decoded || std::string(c.rpc->name) != "ClientEndAbility") return;
        const repl::Value *v = rpc_param_val(c, "ClientInstanceId");
        const int32_t id = (v && v->get_if<int64_t>()) ? static_cast<int32_t>(*v->get_if<int64_t>()) : 0;
        const auto known = server_of.find(id);
        for (const auto &[e, a] : abilities)
            if (known != server_of.end() && a.instance == known->second) end_before_removal = true;
        ends.emplace_back(at, id);
    }

    static const repl::Value *rpc_param_val(const repl::RpcCall &c, const char *n) {
        const schema::RepCommand *cmd = repl::rpc_command(*c.rpc, n);
        const auto it = cmd ? c.params.values.find(cmd->handle) : c.params.values.end();
        return it == c.params.values.end() ? nullptr : &it->second;
    }

    void absorb_pawn_call(const repl::RpcCall &c) {
        if (!c.decoded) return;
        const std::string name = c.rpc->name;
        pawn_calls.push_back(name);
        if (name == "ClientSetLastDeathLocation") {
            const repl::Value *l = rpc_param_val(c, "Location");
            if (const repl::Vec3d *v3 = l ? l->get_if<repl::Vec3d>() : nullptr) death_locations.push_back({v3->x, v3->y, v3->z});
        } else if (name == "SendClientRespawnChoices") {
            Choices ch;
            const repl::Value *list = rpc_param_val(c, "RespawnChoices.ClosestLegalRespawns");
            if (const repl::ArrayValue *arr = list ? list->get_if<repl::ArrayValue>() : nullptr) {
                for (const repl::ElementState &e : arr->elements) ch.closest.push_back(i64(e, 1));
            }
            const repl::Value *home = rpc_param_val(c, "RespawnChoices.HomeRespawn");
            ch.home = (home && home->get_if<int64_t>()) ? *home->get_if<int64_t>() : 0;
            const repl::Value *inv = rpc_param_val(c, "RespawnChoices.bPreviousChoiceInvalid");
            ch.previous_invalid = inv && inv->get_if<bool>() && *inv->get_if<bool>();
            choices.push_back(std::move(ch));
        }
    }

    void pawn_calls_of(const fake::Delivered &d, const repl::BlockView &v) {
        size_t p = v.payload_pos;
        const auto calls = repl::read_rpc_fields(class_named("PlayerPawn_C"), d.content.data(), v.payload_pos + v.payload_bits, p);
        if (calls)
            for (const repl::RpcCall &c : *calls) absorb_pawn_call(c);
    }

    static const repl::Value *val(const repl::ElementState &e, uint16_t handle) {
        const auto it = e.values.find(handle);
        return it == e.values.end() ? nullptr : &it->second;
    }
    static int64_t i64(const repl::ElementState &e, uint16_t handle) {
        const repl::Value *v = val(e, handle);
        return v && v->get_if<int64_t>() ? *v->get_if<int64_t>() : 0;
    }
    static float f32_or(const repl::ElementState &e, uint16_t handle, float def) {
        const repl::Value *v = val(e, handle);
        return v && v->get_if<float>() ? *v->get_if<float>() : def;
    }

    void absorb_shield_element(const repl::ElementState &c_vals) {
        if (i64(c_vals, 1) != kTempHealth) return;
        shields.clear();
        const repl::Value *values = val(c_vals, 2);
        const repl::ArrayValue *arr = values ? values->get_if<repl::ArrayValue>() : nullptr;
        if (arr)
            for (const repl::ElementState &e : arr->elements) shields.emplace_back(f32_or(e, 5, -1.0f), f32_or(e, 6, -1.0f));
    }

    void stats_field(const repl::FastArrayField &f, double at) {
        const std::string name = class_named("UAoCStatsComponent").field(f.field)->name;
        if (name == "CurrentEffects") {
            for (const int32_t id : f.delta.deleted) effects.erase(id);
            for (const auto &c : f.delta.changed) effects[c.id] = i64(c.values, 2);
            return;
        }
        if (name == "StatRepFloatArrayEveryoneProxy") {
            for (const auto &c : f.delta.changed) absorb_shield_element(c.values);
            return;
        }
        for (const auto &c : f.delta.changed) {
            const repl::Value *v = val(c.values, 1);
            const repl::StatInt32Inst *item = v ? v->get_if<repl::StatInt32Inst>() : nullptr;
            if (!item) continue;
            stats[item->record] = std::bit_cast<float>(item->value);
            if (item->record == kStamina) stamina.emplace_back(at, std::bit_cast<float>(item->value));
        }
    }
    // CoA : C.Y.B

    static ActiveAbility parse_active_ability(const repl::ElementState &vals) {
        ActiveAbility a;
        a.ability = i64(vals, 1);
        a.instance = i64(vals, 2);
        const repl::Value *targets = val(vals, 3);
        const auto *t_arr = targets ? targets->get_if<repl::ArrayValue>() : nullptr;
        a.has_target = t_arr && !t_arr->elements.empty();
        const repl::Value *montage = val(vals, 11);
        const auto *m_guid = montage ? montage->get_if<emit::FIntrepidNetworkGUID>() : nullptr;
        a.has_montage = m_guid && m_guid->ObjectId != 0;
        a.montage = m_guid ? m_guid->ObjectId : 0;
        const repl::Value *group = val(vals, 10);
        a.montage_group = (group && group->get_if<repl::NameValue>()) ? group->get_if<repl::NameValue>()->text : std::string();
        const repl::Value *yaw = val(vals, 16);
        a.mesh_yaw = (yaw && yaw->get_if<uint64_t>()) ? static_cast<uint16_t>(*yaw->get_if<uint64_t>()) : 0;
        a.montage_position = f32_or(vals, 13, 0.0f);
        const repl::Value *jump = val(vals, 14);
        a.may_jump = jump && jump->get_if<bool>() && *jump->get_if<bool>();
        a.animation_counter = static_cast<int32_t>(i64(vals, 15));
        return a;
    }

    void ability_field(const repl::FastArrayField &f) {
        const std::string name = class_named("UAoCAbilityComponent").field(f.field)->name;
        if (name == "Cooldowns") {
            for (const int32_t id : f.delta.deleted) cooldowns.erase(id);
            for (const auto &c : f.delta.changed) cooldowns[c.id] = i64(c.values, 1);
            return;
        }
        if (name != "ReplicatedActiveAbilities") return;
        for (const int32_t id : f.delta.deleted) abilities.erase(id);
        for (const auto &c : f.delta.changed) abilities[c.id] = parse_active_ability(c.values);
    }
};

struct Harness {
    Fixture fx;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::vector<std::vector<uint8_t>> held;
    bool hold = false;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0;

    explicit Harness(size_t clog_pc_channel = 0, bool real = false, const svc::Character *record = nullptr, const std::function<void(PlayerWorld &)> &prepare = {}) : fx(real) {
        if (prepare) prepare(*fx.world);
        if (clog_pc_channel) {
            hold = true;
            filler(3, clog_pc_channel);
        }
        s = PlayerSession::join(conn, *fx.world, record ? *record : cyb_record(), identity(), world_time_at(t), error);
        if (!s) fail_exit("the session does not join", error);
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
        if (!clog_pc_channel) settle();
    }
    void filler(uint32_t channel, size_t n) {
        conn::BunchHeader header;
        header.channel = channel;
        header.reliable = true;
        header.name_index = Connection::kActorChannelName;
        BunchWriter one;
        one.write_bits(0xA5, 8);
        for (size_t i = 0; i < n; ++i) conn.send(header, one);
    }
    void deliver(const std::vector<uint8_t> &pkt) {
        if (lose_to_client && lose_to_client(pkt)) {
            ++lost_to_client;
            return;
        }
        const auto r = client.receive(pkt);
        for (const auto &d : r.delivered) got.push_back(d);
    }
    void flush() {
        for (auto &pkt : conn.flush(t)) hold ? held.push_back(pkt) : deliver(pkt);
    }
    std::function<bool(const std::vector<uint8_t> &)> lose_to_client, lose_to_server;
    size_t lost_to_client = 0, lost_to_server = 0;
    void release() {
        hold = false;
        for (const auto &pkt : held) deliver(pkt);
        held.clear();
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        const auto pkt = client.packet(std::move(bunches));
        if (lose_to_server && lose_to_server(pkt)) {
            ++lost_to_server;
            return;
        }
        const std::vector<InBunch> in = conn.receive(pkt, t);
        s->take_acks(conn);
        for (const InBunch &b : in) s->on_bunch(conn, b, t);
        s->end_of_datagram(conn);
    }
    void step(milliseconds d = milliseconds(20)) {
        t += d;
        s->tick(conn, t);
        flush();
        s->take_acks(conn);
        client_sends({});
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 3 && i < 400; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    const emit::FIntrepidNetworkGUID &ability_guid() const { return s->bootstrap().setup().guids.ability; }
};

struct Run {
    Harness h;
    Mirror m;
    size_t seen;
    float ts = 100.0f;
    double x = kX;
    int32_t next_id = 0;

    explicit Run(size_t clog_pc_channel = 0, bool real = false, const svc::Character *record = nullptr, const std::function<void(PlayerWorld &)> &prepare = {})
        : h(clog_pc_channel, real, record, prepare), m(h.s->bootstrap().setup()), seen(h.got.size()) {}
    // built by C O A with Cyb

    double now() const { return world_time_at(h.t); }
    bool is_other_channel(uint32_t ch) const {
        return ch != h.pawn_ch && ch != h.pc_ch && ch != h.s->channel_of(bs::Role::GameState) && ch != h.s->channel_of(bs::Role::PlayerState);
    }
    void absorb() {
        for (; seen < h.got.size(); ++seen) {
            const fake::Delivered &d = h.got[seen];
            if (d.channel == h.pawn_ch && d.content.bit_pos() != 8) m.absorb(d, now());
            if (d.channel == h.pc_ch) m.absorb_pc(d);
            if (is_other_channel(d.channel) && (d.close || d.content.bit_pos() != 8)) {
                m.other.push_back({d.channel, d.open, d.close, now(), d.content.bit_pos(), d.exports.size()});
            }
        }
    }
    void send_reliable(const BunchWriter &b) {
        h.client_sends({h.client.actor(h.pawn_ch, b, /*reliable=*/true)});
        h.flush();
        absorb();
    }
    void send_trigger(int64_t guid, int32_t id, const BunchWriter *bunch = nullptr) {
        send_reliable(bunch ? *bunch : trigger_bunch(h.ability_guid(), guid, id));
        if (h.s->abilities()) {
            if (const auto instance = h.s->abilities()->instance_of(id)) m.server_of[id] = *instance;
        }
    }
    int32_t trigger(int64_t guid) {
        const int32_t id = next_id++;
        send_trigger(guid, id);
        return id;
    }
    void cancel(int32_t id) { send_reliable(cancel_bunch(h.ability_guid(), id)); }
    void tick(bool moving, milliseconds d = milliseconds(100)) {
        ts += static_cast<float>(d.count()) / 1000.0f;
        if (moving) x += 10.0;
        h.client_sends({h.client.actor(h.pawn_ch, client_move(move_at(ts, x, moving)), /*reliable=*/false)});
        h.step(d);
        absorb();
    }
    void play(double seconds, bool moving) {
        const int n = static_cast<int>(std::llround(seconds * 10));
        for (int i = 0; i < n; ++i) tick(moving);
    }
    void tick_at(double cm_per_s, milliseconds d = milliseconds(100)) {
        ts += static_cast<float>(d.count()) / 1000.0f;
        x += cm_per_s * static_cast<double>(d.count()) / 1000.0;
        h.client_sends({h.client.actor(h.pawn_ch, client_move(move_at(ts, x, true)), /*reliable=*/false)});
        h.step(d);
        absorb();
    }
    const AbilityRuntime &rt() const { return *h.s->abilities(); }
};
#undef near
inline bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
