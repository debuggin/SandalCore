// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once
#include "bootstrap/captured_setup.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/schema/generated/index.h"
#include "world_stats.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
using aoc::worldd::Address;
using aoc::worldd::CharacterSaver;
using aoc::worldd::Clock;
using aoc::worldd::Connection;
using aoc::worldd::ConnectionConfig;
using aoc::worldd::CookieSigner;
using aoc::worldd::Endpoint;
using aoc::worldd::EndpointConfig;
using aoc::worldd::InBunch;
using aoc::worldd::LoginQueue;
using aoc::worldd::LoginRequest;
using aoc::worldd::LoginResult;
using aoc::worldd::MoveLimits;
using aoc::worldd::MovementConfig;
using aoc::worldd::PlayerSession;
using aoc::worldd::PlayerWorld;
using aoc::worldd::PositionSink;
namespace abilities = aoc::worldd::abilities;
namespace actionbar = aoc::worldd::actionbar;
namespace bs = aoc::worldd::bootstrap;
namespace cf = content_fixture;
namespace conn = aoc::protocol::conn;
namespace content = aoc::worldd::content;
namespace emit = aoc::protocol::emit;
namespace fi = content_fixture::item;
namespace fr = content_fixture::resource;
namespace fs = std::filesystem;
namespace gen = aoc::protocol::schema::gen;
namespace gm = content_fixture::gm;
namespace items = aoc::worldd::items;
namespace leveling = aoc::worldd::leveling;
namespace login_text = aoc::worldd::login_text;
namespace professions = aoc::worldd::professions;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace sg = aoc::worldd::stats;
namespace stats = aoc::worldd::stats;
namespace svc = aoc::worldd::svc;
namespace tl = aoc::worldd::talents;
namespace wire = aoc::protocol::wire;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;
using svc::ItemContainer;
inline std::ostringstream g_log;
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
inline const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const Clock::time_point t0{seconds(5000)};
inline const Address kA{0x7F000001u, 50510};
inline const Address kB{0x7F000001u, 50511};
inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;
inline constexpr uint32_t kAny = 0xFFFFFFFFu;
inline const ItemContainer Bag = ItemContainer::Backpack;
inline const ItemContainer Worn = ItemContainer::Equipment;
inline uint8_t g_counter = 1;
inline void counting_random(uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = g_counter++;
}
template <class Pred> inline bool wait_until(Pred pred, milliseconds limit = milliseconds(3000)) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}
inline const schema::ClassSchema &class_named(const char *name) {
    const schema::ClassSchema *c = gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}
inline std::string guid_n(uint32_t n) {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0xA0000000u + n, 0x1000u + n, 0x2000u + n, n);
    return buf;
}
inline svc::InventoryItem make_item(ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1) {
    svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = guid_n(n);
    it.quantity = quantity;
    it.quality = 1;
    it.durability = 11;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}
inline std::vector<svc::InventoryItem> bard_items() {
    const std::pair<int, int64_t> kit[] = {{1, fi::kHelmet}, {2, fi::kChest},    {5, fi::kLegs}, {6, fi::kFeet},    {15, fi::kScepter},    {16, fi::kFocus}, {17, fi::kBook},
                                           {23, fi::kBag},   {26, fi::kPickaxe}, {29, fi::kAxe}, {33, fi::kSickle}, {35, fi::kFishingRod}, {38, fi::kBow}};
    std::vector<svc::InventoryItem> out;
    for (const auto &[slot, record] : kit) out.push_back(make_item(Worn, slot, record, 100 + slot));
    out.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    out.push_back(make_item(Bag, 1, fi::kPotion, 201, 5));
    out.push_back(make_item(Bag, 2, fi::kRingA, 202));
    out.push_back(make_item(Bag, 3, fi::kHeavyChest, 203));
    return out;
}
inline std::vector<svc::InventoryItem> bard_items_with_scrolls() {
    std::vector<svc::InventoryItem> out = bard_items();
    const int64_t scrolls[] = {gm::kLevel10, gm::kLevel25, gm::kClassTank, gm::kClassBard, gm::kGear01, gm::kConsumables, gm::kOdd, gm::kKeep};
    for (int i = 0; i < 8; ++i) out.push_back(make_item(Bag, 4 + i, scrolls[i], 210 + static_cast<uint32_t>(i)));
    return out;
}
inline svc::Character cyb_record() {
    svc::Character c;
    c.id = kCyb;
    c.account_id = 3;
    c.name = "cyb";
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    c.items = bard_items();
    return c;
}
inline bs::LoginIdentity identity() {
    return {kMachine, kMachine};
}
inline double world_time_at(Clock::time_point t) {
    return std::chrono::duration<double>(t - t0).count();
}
struct RecordingSink : PositionSink {
    struct Saved {
        std::string id;
        std::vector<svc::InventoryItem> items;
    };
    std::vector<svc::Position> positions;
    std::vector<Saved> inventories;
    std::vector<std::pair<std::string, svc::Progress>> progress;
    void save(const std::string &, const svc::Position &p) override {
        positions.push_back(p);
    }
    void save_inventory(const std::string &id, std::vector<svc::InventoryItem> items) override {
        inventories.push_back({id, std::move(items)});
    }
    void save_progress(const std::string &id, svc::Progress p) override {
        progress.emplace_back(id, std::move(p));
    }
};
struct Fixture {
    fs::path dir;
    std::optional<content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    explicit Fixture(bool real = false, const char *real_path = nullptr) {
        dir = cf::make_temp_dir("worldd_items_session_");
        fs::path db = dir / "content.db";
        const char *real_db = std::getenv("AOC_CONTENT_DB");
        if (real && real_path && *real_path)
            db = real_path;
        else if (real && real_db && *real_db)
            db = real_db;
        else if (!cf::make_db(db))
            std::exit(2);
        std::string error;
        content = content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        world = std::make_unique<PlayerWorld>(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
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
struct Mirror {
    bs::SessionGuids guids;
    const schema::ClassSchema *info_class = nullptr;
    const schema::ClassSchema *appearance_class = nullptr;
    std::map<std::pair<int, int>, repl::ItemInstance> storage;
    std::vector<std::pair<int64_t, int>> held_updates;
    std::map<int32_t, std::pair<int, int64_t>> held;
    int held_key = 0;
    std::vector<std::optional<repl::AppearanceId>> appearance;
    size_t appearance_updates = 0;
    std::vector<std::string> pc_calls;
    std::vector<repl::ItemStorageEvent> events;
    size_t payloads = 0, stat_blocks = 0, unreadable = 0;
    const schema::ClassSchema *stats_class = nullptr;
    std::map<std::string, std::map<int32_t, repl::StatInt32Inst>> stat_items;
    struct StatUpdate {
        size_t chain = 0;
        std::map<std::string, std::set<int64_t>> changed;
        size_t deleted = 0;
    };
    std::vector<StatUpdate> stat_updates;
    std::vector<size_t> held_chains;
    repl::LevelingState tracker;
    size_t tracker_updates = 0;
    int level = 1;
    std::vector<int> level_ups;
    std::vector<int64_t> level_up_categories;
    size_t effects_added = 0, effects_removed = 0;
    size_t absorbed = 0;
    const schema::ClassSchema *ability_class = nullptr;
    std::vector<int64_t> known, records;
    struct EffectSeen {
        int64_t guid = 0;
        float duration = 0;
    };
    std::map<int32_t, EffectSeen> effect_items;
    std::vector<repl::SkillOnActionbar> skill_updates;
    std::vector<repl::ActionbarPayload> bar_payloads;
    std::map<std::pair<int, int>, repl::ActionbarUseData> bar;
    std::vector<std::string> sequence;
    std::function<bool(int64_t)> stat_extended;
    static int storage_of(const bs::SessionGuids &g, const emit::FIntrepidNetworkGUID &o) {
        for (int i = 0; i < 3; ++i) {
            if (o == g.resource_storage[static_cast<size_t>(i)]) return 3 + i;
        }
        return o == g.backpack ? 0 : o == g.equipment ? 1 : o == g.quest_storage ? 2 : -1;
    }
    void absorb_pc_bar_payload(const repl::RpcCall &c) {
        const auto payload = repl::parse_actionbar_payload(c);
        if (!payload) {
            ++unreadable;
            return;
        }
        bar_payloads.push_back(*payload);
        for (const repl::ActionbarSlot &slot : payload->slots) {
            if (slot.use.use_type == repl::ActionbarUseType::None)
                bar.erase({slot.bank_index, slot.slot_index});
            else
                bar[{slot.bank_index, slot.slot_index}] = slot.use;
        }
        sequence.push_back("P");
    }
    void absorb_pc_storage_event(const repl::RpcCall &c) {
        const auto ev = repl::parse_storage_event(c);
        if (!ev || ev->targets.size() != 1 || ev->targets[0].slots.size() != 1) {
            ++unreadable;
            return;
        }
        events.push_back(*ev);
        const int s = storage_of(guids, ev->targets[0].object);
        const std::pair<int, int> key{s, ev->targets[0].slots[0].slot};
        if (ev->type == static_cast<uint8_t>(repl::StorageEventType::Add))
            storage[key] = ev->item;
        else
            storage.erase(key);
    }
    void absorb_pc_call(const repl::RpcCall &c) {
        if (!c.decoded) return;
        const std::string name = c.rpc->name;
        pc_calls.push_back(name);
        if (name == "ClientUpdateSkillOnActionbar") {
            if (const auto u = repl::parse_update_skill_on_actionbar(c)) {
                skill_updates.push_back(*u);
                sequence.push_back("U");
            } else {
                ++unreadable;
            }
        } else if (name == "ClientReceiveActionbarSlotPayload") {
            absorb_pc_bar_payload(c);
        } else if (name == "ClientReceiveStorageEvent") {
            absorb_pc_storage_event(c);
        } else if (name == "ClientReceiveStoragePayload") {
            absorb_payload(repl::parse_storage_payload(c));
        }
    }
    void absorb_pawn_call(const repl::RpcCall &c) {
        if (!c.decoded) return;
        const std::string name = c.rpc->name;
        if (name == "ClientReceiveStoragePayload") absorb_payload(repl::parse_storage_payload(c));
        if (name != "BroadcastPlayLevelUpEffects") return;
        const schema::RepCommand *cmd = repl::rpc_command(*c.rpc, "InLevel");
        const auto it = cmd ? c.params.values.find(cmd->handle) : c.params.values.end();
        const int64_t *lv = it != c.params.values.end() ? it->second.get_if<int64_t>() : nullptr;
        level_ups.push_back(lv ? static_cast<int>(*lv) : -1);
        const schema::RepCommand *catcmd = repl::rpc_command(*c.rpc, "Category");
        const auto cit = catcmd ? c.params.values.find(catcmd->handle) : c.params.values.end();
        const int64_t *cat = cit != c.params.values.end() ? cit->second.get_if<int64_t>() : nullptr;
        level_up_categories.push_back(cat ? *cat : -1);
    }
    void absorb_pawn_held_and_appearance(const fake::Delivered &d, const repl::BlockView &v, size_t chain) {
        if (v.header.object == guids.base_character_info && info_class) {
            const size_t before = held_updates.size();
            held_block(d, v, info_class);
            if (held_updates.size() > before) held_chains.push_back(chain);
        }
        if (v.header.object == guids.appearance && appearance_class) appearance_block(d, v);
    }
    void absorb_pawn_subobject(const fake::Delivered &d, const repl::BlockView &v, size_t chain) {
        if (v.header.object == guids.ability && ability_class) ability_block(d, v);
        if (v.header.object == guids.stats) {
            ++stat_blocks;
            if (stats_class) stats_block(d, v, chain);
        }
        absorb_pawn_held_and_appearance(d, v, chain);
    }
    void absorb_pawn_view(const fake::Delivered &d, const repl::BlockView &v, size_t chain) {
        if (!v.header.is_actor) {
            absorb_pawn_subobject(d, v, chain);
            return;
        }
        if (v.header.has_rep_layout) return;
        size_t p = v.payload_pos;
        const auto calls = repl::read_rpc_fields(class_named("PlayerPawn_C"), d.content.data(), v.payload_pos + v.payload_bits, p);
        if (!calls) return;
        for (const repl::RpcCall &c : *calls) absorb_pawn_call(c);
    }
    void absorb_pc_view(const fake::Delivered &d, const repl::BlockView &v) {
        if (!v.header.is_actor && v.header.object == guids.xp_tracker) {
            tracker_block(d, v);
            return;
        }
        if (!v.header.is_actor || v.header.has_rep_layout) return;
        size_t p = v.payload_pos;
        const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), d.content.data(), v.payload_pos + v.payload_bits, p);
        if (!calls) return;
        for (const repl::RpcCall &c : *calls) absorb_pc_call(c);
    }
    void absorb(const fake::Delivered &d, uint32_t pc_ch, uint32_t pawn_ch, uint32_t channel) {
        const size_t chain = absorbed++;
        if (d.open || d.close || d.content.bit_pos() == 8) return;
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) {
            if (d.content.bit_pos() > 0) ++unreadable;
            return;
        }
        for (const repl::BlockView &v : *views) {
            if (channel == pc_ch)
                absorb_pc_view(d, v);
            else if (channel == pawn_ch)
                absorb_pawn_view(d, v, chain);
        }
    }
    void absorb_payload(const std::optional<repl::StoragePayload> &payload) {
        if (!payload) {
            ++unreadable;
            return;
        }
        ++payloads;
        const int s = storage_of(guids, payload->target);
        if (s < 0) return;
        for (auto it = storage.begin(); it != storage.end();) it = it->first.first == s ? storage.erase(it) : std::next(it);
        for (const repl::StorageSlot &slot : payload->storage) {
            if (slot.item.has_item) storage[{s, slot.slot}] = slot.item;
        }
    }
    void tracker_block(const fake::Delivered &d, const repl::BlockView &v) {
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        const schema::ClassSchema &cls = tracker.schema();
        if (v.header.has_rep_layout && !repl::read_rep_block(cls, d.content.data(), end, p)) {
            ++unreadable;
            return;
        }
        if (p >= end) return;
        const auto fields = repl::read_fast_array_fields(cls, d.content.data(), end, p);
        if (!fields) {
            ++unreadable;
            return;
        }
        for (const repl::FastArrayField &f : *fields) {
            if (!tracker.apply(f)) ++unreadable;
        }
        ++tracker_updates;
        sequence.push_back("L");
    }
    void absorb_current_effects(const repl::FastArrayField &f) {
        effects_added += f.delta.changed.size();
        effects_removed += f.delta.deleted.size();
        for (const int32_t id : f.delta.deleted) {
            effect_items.erase(id);
            sequence.push_back("E-");
        }
        for (const auto &c : f.delta.changed) {
            EffectSeen e;
            const auto g = c.values.values.find(2);
            if (g != c.values.values.end() && g->second.get_if<int64_t>()) e.guid = *g->second.get_if<int64_t>();
            const auto dur = c.values.values.find(4);
            if (dur != c.values.values.end() && dur->second.get_if<float>()) e.duration = *dur->second.get_if<float>();
            effect_items[c.id] = e;
            sequence.push_back("E+");
        }
    }
    void absorb_stat_int32_field(const std::string &name, const repl::FastArrayField &f, StatUpdate &u) {
        auto &items = stat_items[name];
        for (const int32_t id : f.delta.deleted) {
            items.erase(id);
            ++u.deleted;
        }
        for (const auto &c : f.delta.changed) {
            const auto it = c.values.values.find(1);
            const auto *s = it != c.values.values.end() ? it->second.get_if<repl::StatInt32Inst>() : nullptr;
            if (!s) {
                ++unreadable;
                continue;
            }
            items[c.id] = *s;
            u.changed[name].insert(s->record);
        }
    }
    void stats_block(const fake::Delivered &d, const repl::BlockView &v, size_t chain) {
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        repl::CodecOptions opts;
        opts.stat_int32_extended = stat_extended;
        if (v.header.has_rep_layout && !repl::read_rep_block(*stats_class, d.content.data(), end, p, opts)) {
            ++unreadable;
            return;
        }
        if (p >= end) return;
        const auto fields = repl::read_fast_array_fields(*stats_class, d.content.data(), end, p, opts);
        if (!fields) {
            ++unreadable;
            return;
        }
        StatUpdate u;
        u.chain = chain;
        for (const repl::FastArrayField &f : *fields) {
            const std::string name = stats_class->field(f.field)->name;
            if (name == "CurrentEffects")
                absorb_current_effects(f);
            else if (name.rfind("StatRepInt32", 0) == 0)
                absorb_stat_int32_field(name, f, u);
        }
        if (!u.changed.empty() || u.deleted) {
            stat_updates.push_back(std::move(u));
            sequence.push_back("S");
        }
    }
    static void read_ability_id_array(const repl::ArrayValue &a, std::vector<int64_t> &list) {
        list.resize(a.elements.size());
        for (size_t i = 0; i < a.elements.size(); ++i) {
            const auto it = a.elements[i].values.find(1);
            if (it != a.elements[i].values.end() && it->second.get_if<int64_t>()) list[i] = *it->second.get_if<int64_t>();
        }
    }
    void ability_block(const fake::Delivered &d, const repl::BlockView &v) {
        if (!v.header.has_rep_layout) return;
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        const auto st = repl::read_rep_block(*ability_class, d.content.data(), end, p);
        if (!st) {
            ++unreadable;
            return;
        }
        bool changed = false;
        const std::pair<const char *, std::vector<int64_t> *> lists[] = {{"KnownAbilities", &known}, {"ReplicatedRecordsToLoad", &records}};
        for (const auto &[name, list] : lists) {
            const repl::Value *val = st->get(*ability_class, name);
            const repl::ArrayValue *a = val ? val->get_if<repl::ArrayValue>() : nullptr;
            if (!a) continue;
            read_ability_id_array(*a, *list);
            changed = true;
        }
        if (changed) sequence.push_back("A");
    }
    void absorb_held_items_field(const repl::FastArrayField &f) {
        for (const int32_t id : f.delta.deleted) held.erase(id);
        for (const auto &c : f.delta.changed) {
            const auto key = c.values.values.find(1);
            const auto rec = c.values.values.find(2);
            held[c.id] = {key != c.values.values.end() && key->second.get_if<uint64_t>() ? static_cast<int>(*key->second.get_if<uint64_t>()) : 0,
                          rec != c.values.values.end() && rec->second.get_if<int64_t>() ? *rec->second.get_if<int64_t>() : 0};
        }
        held_key = f.delta.array_key;
        held_updates.emplace_back(f.delta.array_key, static_cast<int>(f.delta.changed.size()));
    }
    bool read_held_rep_level(const fake::Delivered &d, const schema::ClassSchema &cls, size_t end, size_t &p) {
        const auto st = repl::read_rep_block(cls, d.content.data(), end, p);
        if (!st) {
            ++unreadable;
            return false;
        }
        const repl::Value *lv = st->get(cls, "Level");
        if (lv && lv->get_if<int64_t>()) level = static_cast<int>(*lv->get_if<int64_t>());
        return true;
    }
    void held_block(const fake::Delivered &d, const repl::BlockView &v, const schema::ClassSchema *cls) {
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        if (v.header.has_rep_layout && !read_held_rep_level(d, *cls, end, p)) return;
        if (p >= end) return;
        const auto fields = repl::read_fast_array_fields(*cls, d.content.data(), end, p);
        if (!fields) {
            ++unreadable;
            return;
        }
        for (const repl::FastArrayField &f : *fields) {
            if (std::string(cls->field(f.field)->name) == "HeldItems") absorb_held_items_field(f);
        }
    }
    void appearance_block(const fake::Delivered &d, const repl::BlockView &v) {
        if (!v.header.has_rep_layout) return;
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        const auto st = repl::read_rep_block(*appearance_class, d.content.data(), end, p);
        if (!st) {
            ++unreadable;
            return;
        }
        const repl::Value *v2 = st->get(*appearance_class, "AppearanceIDs");
        const repl::ArrayValue *a = v2 ? v2->get_if<repl::ArrayValue>() : nullptr;
        if (!a) return;
        ++appearance_updates;
        appearance.resize(a->elements.size());
        for (size_t i = 0; i < a->elements.size(); ++i) {
            const auto it = a->elements[i].values.find(1);
            if (it != a->elements[i].values.end() && it->second.get_if<repl::AppearanceId>()) appearance[i] = *it->second.get_if<repl::AppearanceId>();
        }
    }
    std::map<int, int64_t> held_by_key() const {
        std::map<int, int64_t> out;
        for (const auto &[id, kv] : held) out[kv.first] = kv.second;
        return out;
    }
};
inline BunchWriter actor_rpc(const schema::ClassSchema &cls, const char *name, const repl::RpcParams &params) {
    BunchWriter out;
    const schema::RpcSchema *rpc = cls.rpc(name);
    if (!rpc) return out;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}
inline BunchWriter item_use(const char *rpc_name, int storage, int slot, const std::string &guid_hex, uint32_t requested = kAny) {
    const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema *rpc = pawn.rpc(rpc_name);
    repl::RpcParams p;
    const auto g = items::guid_of(guid_hex);
    const repl::GuidValue gv = g ? *g : repl::GuidValue{};
    repl::set_rpc_param(*rpc, p, "Storage", repl::Value::u64(static_cast<uint64_t>(storage)));
    repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
    repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(requested)));
    return actor_rpc(pawn, rpc_name, p);
}
inline BunchWriter item_used(int storage, int slot, const std::string &guid_hex, uint32_t requested = kAny) {
    return item_use("ServerItemUsed", storage, slot, guid_hex, requested);
}
inline BunchWriter item_activate(int storage, int slot, const std::string &guid_hex) {
    return item_use("ServerActivateItem", storage, slot, guid_hex);
}
inline BunchWriter storage_request(const repl::ItemStorageEvent &ev) {
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema *rpc = pc.rpc("ServerRequestStorageEvent");
    const auto params = repl::storage_event_params(*rpc, ev);
    return params ? actor_rpc(pc, "ServerRequestStorageEvent", *params) : BunchWriter();
}
inline BunchWriter with_bits(const BunchWriter &src, size_t bits) {
    BunchWriter w;
    if (bits > 0) w.write_bit_range(src.data(), 0, std::min(bits, src.bit_pos()));
    return w;
}
inline BunchWriter flipped_bits(const BunchWriter &src, const std::vector<size_t> &at) {
    std::vector<uint8_t> bytes(src.data(), src.data() + src.byte_size());
    for (const size_t b : at)
        if (b / 8 < bytes.size()) bytes[b / 8] ^= static_cast<uint8_t>(1u << (b % 8));
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, src.bit_pos());
    return w;
}
inline bool inventory_is_sane(const items::Inventory &inv, const std::set<std::string> &guids, size_t destroyed) {
    std::string error;
    const auto again = items::Inventory::from_items(inv.items(), error);
    if (!again || !(*again == inv)) return false;
    std::set<std::string> now;
    for (const auto &it : inv.items()) now.insert(it.instance_guid);
    if (inv.size() != now.size() || !std::includes(guids.begin(), guids.end(), now.begin(), now.end())) return false;
    return guids.size() - now.size() <= destroyed;
}
inline bool same_items(std::vector<svc::InventoryItem> a, std::vector<svc::InventoryItem> b) {
    const auto less = [](const svc::InventoryItem &x, const svc::InventoryItem &y) { return std::make_pair(static_cast<int>(x.container), x.slot) < std::make_pair(static_cast<int>(y.container), y.slot); };
    std::sort(a.begin(), a.end(), less);
    std::sort(b.begin(), b.end(), less);
    return a == b;
}
