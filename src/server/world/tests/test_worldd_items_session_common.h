// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/repl/ability_rpc.h"
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
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
using aoc::worldd::CharacterSaver, aoc::worldd::Clock, aoc::worldd::Connection, aoc::worldd::ConnectionConfig, aoc::worldd::InBunch, aoc::worldd::PlayerSession, aoc::worldd::PlayerWorld, aoc::worldd::PositionSink;
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
        std::printf("  [%s] %s\n", (cond) ? (++g_pass, "ok ") : (++g_fail, "FAIL"), msg);                                                                                                                                                      \
    } while (0)
inline const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const Clock::time_point t0{seconds(5000)};
inline constexpr uint32_t kServerId = 60, kRandomizer = 0x6ee87ee4u, kAny = 0xFFFFFFFFu;
inline const ItemContainer Bag = ItemContainer::Backpack, Worn = ItemContainer::Equipment;
template <class Pred> inline bool wait_until(Pred pred, milliseconds limit = milliseconds(3000)) {
    for (const auto until = std::chrono::steady_clock::now() + limit; !pred(); std::this_thread::sleep_for(milliseconds(1)))
        if (std::chrono::steady_clock::now() > until) return false;
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
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    c.items = bard_items();
    return c;
}
inline bs::LoginIdentity identity() { return {kMachine, kMachine}; }
inline double world_time_at(Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }
struct RecordingSink : PositionSink {
    struct Saved {
        std::string id;
        std::vector<svc::InventoryItem> items;
    };
    std::vector<svc::Position> positions;
    std::vector<Saved> inventories;
    std::vector<std::pair<std::string, svc::Progress>> progress;
    void save(const std::string &, const svc::Position &p) override { positions.push_back(p); }
    void save_inventory(const std::string &id, std::vector<svc::InventoryItem> items) override { inventories.push_back({id, std::move(items)}); }
    void save_progress(const std::string &id, svc::Progress p) override { progress.emplace_back(id, std::move(p)); }
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
    const schema::ClassSchema *info_class = nullptr, *appearance_class = nullptr, *stats_class = nullptr, *ability_class = nullptr;
    std::map<std::pair<int, int>, repl::ItemInstance> storage;
    std::vector<std::pair<int64_t, int>> held_updates;
    std::map<int32_t, std::pair<int, int64_t>> held;
    int held_key = 0, level = 1;
    std::vector<std::optional<repl::AppearanceId>> appearance;
    size_t appearance_updates = 0, payloads = 0, stat_blocks = 0, unreadable = 0, tracker_updates = 0, effects_added = 0, effects_removed = 0, absorbed = 0;
    std::vector<std::string> pc_calls, sequence;
    std::vector<repl::ItemStorageEvent> events;
    std::map<std::string, std::map<int32_t, repl::StatInt32Inst>> stat_items;
    struct StatUpdate {
        size_t chain = 0;
        std::map<std::string, std::set<int64_t>> changed;
        size_t deleted = 0;
    };
    std::vector<StatUpdate> stat_updates;
    std::vector<size_t> held_chains;
    repl::LevelingState tracker;
    std::vector<int> level_ups;
    std::vector<int64_t> level_up_categories, known, records;
    struct EffectSeen {
        int64_t guid = 0;
        float duration = 0;
    };
    std::map<int32_t, EffectSeen> effect_items;
    std::vector<repl::SkillOnActionbar> skill_updates;
    std::vector<repl::ActionbarPayload> bar_payloads;
    std::map<std::pair<int, int>, repl::ActionbarUseData> bar;
    std::function<bool(int64_t)> stat_extended;
    bool check_read(bool ok) {
        if (!ok) ++unreadable;
        return ok;
    }
    static int storage_of(const bs::SessionGuids &g, const emit::FIntrepidNetworkGUID &o) {
        for (int i = 0; i < 3; ++i)
            if (o == g.resource_storage[static_cast<size_t>(i)]) return 3 + i;
        return o == g.backpack ? 0 : o == g.equipment ? 1 : o == g.quest_storage ? 2 : -1;
    }
    void absorb_pc_bar_payload(const repl::RpcCall &c) {
        const auto payload = repl::parse_actionbar_payload(c);
        if (!check_read(static_cast<bool>(payload))) return;
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
        if (!check_read(ev && ev->targets.size() == 1 && ev->targets[0].slots.size() == 1)) return;
        events.push_back(*ev);
        const std::pair<int, int> key{storage_of(guids, ev->targets[0].object), ev->targets[0].slots[0].slot};
        if (ev->type == static_cast<uint8_t>(repl::StorageEventType::Add)) {
            storage[key] = ev->item;
        } else {
            const auto held = storage.find(key);
            const int32_t leaves = ev->targets[0].slots[0].count;
            if (held != storage.end() && leaves > 0 && leaves < static_cast<int32_t>(held->second.quantity)) held->second.quantity -= static_cast<uint32_t>(leaves);
            else storage.erase(key);
        }
    }
    void absorb_pc_call(const repl::RpcCall &c) {
        if (!c.decoded) return;
        const std::string name = c.rpc->name;
        pc_calls.push_back(name);
        if (name == "ClientUpdateSkillOnActionbar") {
            const auto u = repl::parse_update_skill_on_actionbar(c);
            if (!check_read(static_cast<bool>(u))) return;
            skill_updates.push_back(*u);
            sequence.push_back("U");
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
        if (const auto calls = repl::read_rpc_fields(class_named("PlayerPawn_C"), d.content.data(), v.payload_pos + v.payload_bits, p))
            for (const repl::RpcCall &c : *calls) absorb_pawn_call(c);
    }
    void absorb_pc_view(const fake::Delivered &d, const repl::BlockView &v) {
        if (!v.header.is_actor && v.header.object == guids.xp_tracker) {
            tracker_block(d, v);
            return;
        }
        if (!v.header.is_actor || v.header.has_rep_layout) return;
        size_t p = v.payload_pos;
        if (const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), d.content.data(), v.payload_pos + v.payload_bits, p))
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
        if (!check_read(static_cast<bool>(payload))) return;
        ++payloads;
        const int s = storage_of(guids, payload->target);
        if (s < 0) return;
        for (auto it = storage.begin(); it != storage.end();) it = it->first.first == s ? storage.erase(it) : std::next(it);
        for (const repl::StorageSlot &slot : payload->storage)
            if (slot.item.has_item) storage[{s, slot.slot}] = slot.item;
    }
    void tracker_block(const fake::Delivered &d, const repl::BlockView &v) {
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        const schema::ClassSchema &cls = tracker.schema();
        if (v.header.has_rep_layout && !check_read(static_cast<bool>(repl::read_rep_block(cls, d.content.data(), end, p)))) return;
        if (p >= end) return;
        const auto fields = repl::read_fast_array_fields(cls, d.content.data(), end, p);
        if (!check_read(static_cast<bool>(fields))) return;
        for (const repl::FastArrayField &f : *fields) check_read(tracker.apply(f));
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
            if (!check_read(s != nullptr)) continue;
            items[c.id] = *s;
            u.changed[name].insert(s->record);
        }
    }
    void stats_block(const fake::Delivered &d, const repl::BlockView &v, size_t chain) {
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        repl::CodecOptions opts;
        opts.stat_int32_extended = stat_extended;
        if (v.header.has_rep_layout && !check_read(static_cast<bool>(repl::read_rep_block(*stats_class, d.content.data(), end, p, opts)))) return;
        if (p >= end) return;
        const auto fields = repl::read_fast_array_fields(*stats_class, d.content.data(), end, p, opts);
        if (!check_read(static_cast<bool>(fields))) return;
        StatUpdate u{chain, {}, 0};
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
        const auto st = repl::read_rep_block(*ability_class, d.content.data(), v.payload_pos + v.payload_bits, p);
        if (!check_read(static_cast<bool>(st))) return;
        bool changed = false;
        const std::pair<const char *, std::vector<int64_t> *> lists[] = {{"KnownAbilities", &known}, {"ReplicatedRecordsToLoad", &records}};
        for (const auto &[name, list] : lists) {
            const repl::Value *val = st->get(*ability_class, name);
            if (const repl::ArrayValue *a = val ? val->get_if<repl::ArrayValue>() : nullptr) {
                read_ability_id_array(*a, *list);
                changed = true;
            }
        }
        if (changed) sequence.push_back("A");
    }
    void absorb_held_items_field(const repl::FastArrayField &f) {
        for (const int32_t id : f.delta.deleted) held.erase(id);
        for (const auto &c : f.delta.changed) {
            const auto key = c.values.values.find(1), rec = c.values.values.find(2);
            held[c.id] = {key != c.values.values.end() && key->second.get_if<uint64_t>() ? static_cast<int>(*key->second.get_if<uint64_t>()) : 0,
                          rec != c.values.values.end() && rec->second.get_if<int64_t>() ? *rec->second.get_if<int64_t>() : 0};
        }
        held_key = f.delta.array_key;
        held_updates.emplace_back(f.delta.array_key, static_cast<int>(f.delta.changed.size()));
    }
    bool read_held_rep_level(const fake::Delivered &d, const schema::ClassSchema &cls, size_t end, size_t &p) {
        const auto st = repl::read_rep_block(cls, d.content.data(), end, p);
        if (!check_read(static_cast<bool>(st))) return false;
        if (const repl::Value *lv = st->get(cls, "Level"); lv && lv->get_if<int64_t>()) level = static_cast<int>(*lv->get_if<int64_t>());
        return true;
    }
    void held_block(const fake::Delivered &d, const repl::BlockView &v, const schema::ClassSchema *cls) {
        size_t p = v.payload_pos;
        const size_t end = v.payload_pos + v.payload_bits;
        if (v.header.has_rep_layout && !read_held_rep_level(d, *cls, end, p)) return;
        if (p >= end) return;
        const auto fields = repl::read_fast_array_fields(*cls, d.content.data(), end, p);
        if (!check_read(static_cast<bool>(fields))) return;
        for (const repl::FastArrayField &f : *fields)
            if (std::string(cls->field(f.field)->name) == "HeldItems") absorb_held_items_field(f);
    }
    void appearance_block(const fake::Delivered &d, const repl::BlockView &v) {
        if (!v.header.has_rep_layout) return;
        size_t p = v.payload_pos;
        const auto st = repl::read_rep_block(*appearance_class, d.content.data(), v.payload_pos + v.payload_bits, p);
        if (!check_read(static_cast<bool>(st))) return;
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
    BunchWriter out, field;
    const schema::RpcSchema *rpc = cls.rpc(name);
    bool failed = false;
    if (!rpc || repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 || failed) {
        if (rpc) std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    repl::write_content_block(out, repl::BlockHeader{}, field, /*server_form=*/false);
    return out;
}
inline BunchWriter item_use(const char *rpc_name, int storage, int slot, const std::string &guid_hex, uint32_t requested = kAny) {
    const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema *rpc = pawn.rpc(rpc_name);
    repl::RpcParams p;
    const repl::GuidValue gv = items::guid_of(guid_hex).value_or(repl::GuidValue{});
    repl::set_rpc_param(*rpc, p, "Storage", repl::Value::u64(static_cast<uint64_t>(storage)));
    repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
    repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(requested)));
    return actor_rpc(pawn, rpc_name, p);
}
inline BunchWriter item_used(int storage, int slot, const std::string &guid_hex, uint32_t requested = kAny) { return item_use("ServerItemUsed", storage, slot, guid_hex, requested); }
inline BunchWriter item_activate(int storage, int slot, const std::string &guid_hex) { return item_use("ServerActivateItem", storage, slot, guid_hex); }
inline BunchWriter storage_request(const repl::ItemStorageEvent &ev) {
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const auto params = repl::storage_event_params(*pc.rpc("ServerRequestStorageEvent"), ev);
    return params ? actor_rpc(pc, "ServerRequestStorageEvent", *params) : BunchWriter();
}
struct Harness {
    Fixture fx;
    RecordingSink sink;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::vector<size_t> packet_of;
    size_t packets = 0, mirrored = 0;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0;
    Mirror mirror;
    explicit Harness(svc::Character record = cyb_record(), bool with_sink = true, bool real_db = false, const char *real_path = nullptr) : fx(real_db, real_path) {
        if (with_sink) fx.world->set_position_sink(&sink);
        s = PlayerSession::join(conn, *fx.world, record, identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
        mirror.guids = s->bootstrap().setup().guids;
        const auto &pawn = s->bootstrap().pawn();
        mirror.info_class = pawn.find(mirror.guids.base_character_info) ? pawn.find(mirror.guids.base_character_info)->obj.schema : nullptr;
        mirror.appearance_class = pawn.find(mirror.guids.appearance) ? pawn.find(mirror.guids.appearance)->obj.schema : nullptr;
        mirror.stats_class = pawn.find(mirror.guids.stats) ? pawn.find(mirror.guids.stats)->obj.schema : nullptr;
        mirror.ability_class = pawn.find(mirror.guids.ability) ? pawn.find(mirror.guids.ability)->obj.schema : nullptr;
        const sg::StatBundle *bundle = world_stats::of(*fx.content).bundle;
        mirror.stat_extended = [bundle](int64_t rec) {
            const auto it = bundle->by_guid.find(rec);
            return it != bundle->by_guid.end() && (bundle->stats[it->second].flags & sg::kStatBuckets) != 0;
        };
        settle();
    }
    void flush() {
        for (auto &pkt : conn.flush(t)) {
            const auto r = client.receive(pkt);
            ++packets;
            for (const auto &d : r.delivered) {
                got.push_back(d);
                packet_of.push_back(packets);
            }
        }
        for (; mirrored < got.size(); ++mirrored) mirror.absorb(got[mirrored], pc_ch, pawn_ch, got[mirrored].channel);
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        for (const auto &pkt : client.packets(std::move(bunches))) {
            const std::vector<InBunch> in = conn.receive(pkt, t);
            s->take_acks(conn);
            for (const InBunch &b : in) s->on_bunch(conn, b, t);
            s->end_of_datagram(conn);
        }
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
    void request(const BunchWriter &pawn_or_pc, uint32_t channel, bool reliable = true) {
        client_sends({client.actor(channel, pawn_or_pc, reliable)});
        settle();
    }
    void request_both(const BunchWriter &a, const BunchWriter &b) {
        client_sends({client.actor(pawn_ch, a, true), client.actor(pawn_ch, b, true)});
        settle();
    }
    size_t chains_since(size_t from) const { return got.size() - from; }
};
inline BunchWriter bag_move_or_swap(Harness &h, repl::StorageEventType type, const std::pair<int, int> &from_slot_count, const std::pair<std::string, std::string> &guids, const std::pair<int, int> &to_slot_count) {
    const bs::SessionGuids &g = h.s->bootstrap().setup().guids;
    repl::ItemStorageEvent ev;
    ev.type = static_cast<uint8_t>(type);
    const auto gv = items::guid_of(guids.first), tg = items::guid_of(guids.second);
    ev.item.has_item = true;
    ev.item.record_id = fi::kPotion;
    ev.item.quantity = static_cast<uint32_t>(from_slot_count.second);
    ev.item.quality = 1;
    if (gv) ev.item.guid = *gv;
    ev.item.creation_time = items::ticks_of_ms(1'771'299'474'000ll);
    ev.move_source.object = g.backpack;
    ev.move_source.slots.push_back({from_slot_count.first, from_slot_count.second, gv.value_or(repl::GuidValue{})});
    ev.targets.push_back({g.backpack, {{to_slot_count.first, to_slot_count.second, tg.value_or(repl::GuidValue{})}}});
    ev.instigator = repl::GuidValue{0xbbec5c86u, 0xeb101fe5u, 0xf38c87f0u, 0xfa22e7d0u};
    return storage_request(ev);
}
inline BunchWriter bag_move(Harness &h, int from, int count, const std::string &guid, int to) { return bag_move_or_swap(h, repl::StorageEventType::Move, {from, count}, {guid, {}}, {to, 1}); }
inline void append_pc_calls_from_block(const fake::Delivered &chn, const repl::BlockView &b, std::vector<std::string> &out) {
    size_t p = b.payload_pos;
    if (const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), chn.content.data(), b.payload_pos + b.payload_bits, p))
        for (const auto &c : *calls) out.push_back(c.rpc->name);
}
inline std::vector<std::string> pc_calls_since(const Harness &h, size_t from_chain) {
    std::vector<std::string> out;
    for (size_t i = from_chain; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch) continue;
        if (const auto blocks = repl::read_content_blocks(h.got[i].content.data(), h.got[i].content.bit_pos(), 0))
            for (const auto &b : *blocks) append_pc_calls_from_block(h.got[i], b, out);
    }
    return out;
}
inline bool mirror_matches(const Harness &h) {
    const auto items = h.s->inventory().items();
    size_t n = 0;
    for (const svc::InventoryItem &it : items) {
        if (it.container == ItemContainer::Quest) continue;
        ++n;
        const auto f = h.mirror.storage.find({static_cast<int>(it.container), it.slot});
        if (f == h.mirror.storage.end() || !(f->second == items::to_instance(it))) return false;
    }
    return n == h.mirror.storage.size();
}
inline bool gear_matches(const Harness &h) {
    const auto want = items::held_records(h.s->inventory());
    const auto held = h.mirror.held_by_key();
    for (size_t i = 0; i < want.size(); ++i) {
        const auto f = held.find(items::kHeldSlots[i]);
        if (f == held.end() || f->second != want[i]) return false;
    }
    const auto app = items::appearance_ids(h.s->inventory());
    if (app.size() != h.mirror.appearance.size()) return false;
    for (size_t i = 0; i < app.size(); ++i)
        if (!h.mirror.appearance[i] || !(*h.mirror.appearance[i] == app[i])) return false;
    return true;
}
struct MoveSpec {
    uint8_t type = static_cast<uint8_t>(repl::StorageEventType::Move);
    emit::FIntrepidNetworkGUID from_object, to_object;
    int from_slot = 1, from_count = 5, to_slot = 32, to_count = 1;
    std::string source_guid = guid_n(201), event_guid = guid_n(201), target_guid;
    bool has_source = true;
    size_t targets = 1;
};
inline BunchWriter storage_move(const MoveSpec &m) {
    repl::ItemStorageEvent ev;
    ev.type = m.type;
    const auto eg = items::guid_of(m.event_guid), sg = items::guid_of(m.source_guid), tg = items::guid_of(m.target_guid);
    ev.item.has_item = true;
    ev.item.record_id = static_cast<uint64_t>(fi::kPotion);
    ev.item.quantity = static_cast<uint32_t>(std::max(1, m.from_count));
    ev.item.quality = 1;
    if (eg) ev.item.guid = *eg;
    ev.item.creation_time = items::ticks_of_ms(1'771'299'474'000ll);
    ev.move_source.object = m.from_object;
    if (m.has_source) ev.move_source.slots.push_back({m.from_slot, m.from_count, sg.value_or(repl::GuidValue{})});
    for (size_t i = 0; i < m.targets; ++i) ev.targets.push_back({m.to_object, {{m.to_slot + static_cast<int>(i), m.to_count, tg.value_or(repl::GuidValue{})}}});
    ev.instigator = repl::GuidValue{0xbbec5c86u, 0xeb101fe5u, 0xf38c87f0u, 0xfa22e7d0u};
    return storage_request(ev);
}
struct FakeCharacters : svc::CharacterApi {
    struct Inv {
        std::string id;
        std::vector<svc::InventoryItem> items;
    };
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<std::string, svc::Position>> positions;
    std::vector<Inv> inventories;
    size_t position_calls = 0, inventory_calls = 0;
    std::deque<svc::CallStatus> position_script, inventory_script;
    bool block = false;
    milliseconds max_block{2000};
    int running = 0;
    std::vector<std::pair<std::string, svc::Progress>> progress;
    svc::CharacterResult get_character(const std::string &) override { return {}; }
    svc::CallStatus save_progress(const std::string &id, const svc::Progress &p) override {
        std::lock_guard<std::mutex> lock(mu);
        progress.emplace_back(id, p);
        cv.notify_all();
        return svc::CallStatus::Ok;
    }
    svc::CallStatus save_position(const std::string &id, const svc::Position &p) override {
        std::unique_lock<std::mutex> lock(mu);
        ++position_calls;
        svc::CallStatus status = svc::CallStatus::Ok;
        if (!position_script.empty()) {
            status = position_script.front();
            position_script.pop_front();
        }
        if (status == svc::CallStatus::Ok) positions.emplace_back(id, p);
        cv.notify_all();
        return status;
    }
    svc::CallStatus save_inventory(const std::string &id, const std::vector<svc::InventoryItem> &items) override {
        std::unique_lock<std::mutex> lock(mu);
        ++inventory_calls;
        ++running;
        cv.notify_all();
        cv.wait_for(lock, max_block, [this] { return !block; });
        --running;
        svc::CallStatus status = svc::CallStatus::Ok;
        if (!inventory_script.empty()) {
            status = inventory_script.front();
            inventory_script.pop_front();
        }
        if (status == svc::CallStatus::Ok) inventories.push_back({id, items});
        cv.notify_all();
        return status;
    }
    void hold() {
        std::lock_guard<std::mutex> l(mu);
        block = true;
    }
    void release() {
        {
            std::lock_guard<std::mutex> l(mu);
            block = false;
        }
        cv.notify_all();
    }
    size_t progress_count() { return (std::lock_guard<std::mutex>(mu), progress.size()); }
    bool in_flight() { return (std::lock_guard<std::mutex>(mu), running > 0); }
    size_t inventory_count() { return (std::lock_guard<std::mutex>(mu), inventories.size()); }
    size_t position_count() { return (std::lock_guard<std::mutex>(mu), positions.size()); }
    size_t inventory_call_count() { return (std::lock_guard<std::mutex>(mu), inventory_calls); }
    size_t position_call_count() { return (std::lock_guard<std::mutex>(mu), position_calls); }
    std::vector<Inv> saved_inventories() { return (std::lock_guard<std::mutex>(mu), inventories); }
};
inline CharacterSaver::Config quick(milliseconds destructor_wait = milliseconds(2000)) {
    CharacterSaver::Config c;
    c.retry_delay = milliseconds(5);
    c.destructor_wait = destructor_wait;
    return c;
}
inline repl::CodecOptions client_options() {
    repl::CodecOptions o;
    o.inline_objects = true;
    return o;
}
inline BunchWriter trigger_with_item(const emit::FIntrepidNetworkGUID &ability, int64_t guid, int32_t id, const repl::ItemInstance &item, int slot = 15) {
    const schema::ClassSchema &cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema *rpc = cls.rpc("ServerTriggerAbility");
    BunchWriter out, field;
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0})) && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(slot));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(item)) && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(4));
    bool failed = false;
    if (!ok || repl::write_rpc_field(cls, *rpc, p, field, client_options(), &failed) == 0 || failed) return out;
    repl::write_content_block(out, repl::BlockHeader{false, false, ability}, field, false);
    return out;
}
inline svc::Character record_with(std::vector<svc::InventoryItem> items) {
    svc::Character c = cyb_record();
    c.items = std::move(items);
    return c;
}
inline void clog_channel(Harness &h, uint32_t channel, size_t bunches) {
    conn::BunchHeader header;
    header.channel = channel;
    header.reliable = true;
    header.name_index = Connection::kActorChannelName;
    BunchWriter filler;
    filler.write_bits(0xA5, 8);
    for (size_t i = 0; i < bunches; ++i) h.conn.send(header, filler);
}
inline const char *kArrays[] = {"StatRepInt32Owner", "StatRepInt32OwnerProxy", "StatRepInt32EveryoneProxy"};
inline const char *array_of(uint8_t replication) { return replication == 1 ? kArrays[0] : replication == 5 ? kArrays[1] : kArrays[2]; }
inline uint32_t bits_of(float f) { return std::bit_cast<uint32_t>(f); }
inline bool stat_item_matches_value(const repl::StatInt32Inst &item, const sg::StatValue &v) {
    return item.value == bits_of(v.value) && (!v.buckets() || (item.extended && item.base == bits_of(v.base) && item.equipment == bits_of(v.equip)));
}
inline bool find_matching_stat_item(const Harness &h, const sg::StatValue &v, std::string *why) {
    const auto arr = h.mirror.stat_items.find(array_of(v.replication));
    if (arr == h.mirror.stat_items.end()) return false;
    for (const auto &[id, item] : arr->second) {
        if (item.record != v.guid) continue;
        if (stat_item_matches_value(item, v)) return true;
        if (why) *why = "stat " + std::to_string(v.guid) + " is " + std::to_string(std::bit_cast<float>(item.value)) + " on the client, " + std::to_string(v.value) + " in the session";
    }
    return false;
}
inline bool is_replicated_int32_stat(const sg::StatValue &v) { return !v.float_array() && (v.replication == 1 || v.replication == 5 || v.replication == 7); }
inline bool fail_missing_stat(int64_t guid, std::string *why) {
    if (why && why->empty()) *why = "stat " + std::to_string(guid) + " is missing or different";
    return false;
}
inline bool stats_match_values(const Harness &h, const sg::CharacterStats &cs, size_t &expected, std::string *why, bool unlaid) {
    for (const sg::StatValue &v : cs.values()) {
        if (!is_replicated_int32_stat(v)) continue;
        ++expected;
        if (find_matching_stat_item(h, v, why)) continue;
        if (unlaid && !h.s->bootstrap().has_stat_item(v.guid)) {
            --expected;
            continue;
        }
        return fail_missing_stat(v.guid, why);
    }
    return true;
}
inline bool stats_match(const Harness &h, std::string *why = nullptr, bool unlaid = false) {
    const sg::CharacterStats *cs = h.s->character_stats();
    size_t expected = 0, have = 0;
    if (!cs || !stats_match_values(h, *cs, expected, why, unlaid)) return false;
    for (const auto &[name, items] : h.mirror.stat_items) have += items.size();
    if (unlaid) return have >= expected;
    if (have != expected && why) *why = std::to_string(have) + " items on the client, " + std::to_string(expected) + " stats in the session";
    return have == expected;
}
inline repl::DesignDataId tree_id(int64_t guid) { return {guid, content::kTypeSkillTree}; }
inline BunchWriter client_load_rpc() { return actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerRequestClientLoad", repl::RpcParams{}); }
inline svc::Character record_with_scrolls() { return record_with(bard_items_with_scrolls()); }
inline const tl::Catalog &catalog_of(const Harness &h) { return *h.fx.world->talents(); }
inline int64_t tree_guid(const Harness &h, const char *name) {
    const tl::Tree *t = catalog_of(h).tree_named(name);
    return t ? t->guid : 0;
}
inline int64_t rank_named(const Harness &h, const char *tree, const char *node) {
    if (const tl::Tree *t = catalog_of(h).tree_named(tree))
        for (const tl::Node &n : t->nodes)
            if (n.name == node && !n.ranks.empty()) return n.ranks.front().guid;
    return 0;
}
inline const tl::Node *node_where(const Harness &h, const char *tree, const std::function<bool(const tl::Node &)> &want) {
    if (const tl::Tree *t = catalog_of(h).tree_named(tree)) {
        for (const tl::Node &n : t->nodes) {
            const bool gated = std::any_of(n.requires_skills.begin(), n.requires_skills.end(), [](int64_t r) { return r != 0; }) || !n.requires_nodes.empty();
            if (!n.auto_granted && !n.ranks.empty() && !gated && n.point_requirement == 0 && n.level_requirement <= 3 && want(n)) return &n;
        }
    }
    return nullptr;
}
inline const tl::Node *node_named(const Harness &h, const char *tree, const char *node) {
    if (const tl::Tree *t = catalog_of(h).tree_named(tree))
        for (const tl::Node &n : t->nodes)
            if (n.name == node) return &n;
    return nullptr;
}
struct Buy {
    int64_t rank, tree;
    bool purchase;
};
inline repl::SkillBuild build_of(std::initializer_list<Buy> entries) {
    repl::SkillBuild b;
    for (const Buy &e : entries)
        b.changes.push_back({{e.rank, content::kTypeSkillRank}, {e.tree, content::kTypeSkillTree, repl::NameValue::plain("Prototype_Assassin_SkillTable")}, {content::kGuidAdventuring, content::kTypeLevelingTypeDef}, e.purchase});
    return b;
}
inline BunchWriter purchase_rpc(const repl::SkillBuild &b) {
    const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
    const auto params = repl::purchase_skill_build_params(*pawn.rpc("ServerPurchaseSkillBuild"), b);
    return params ? actor_rpc(pawn, "ServerPurchaseSkillBuild", *params) : BunchWriter();
}
inline BunchWriter set_slot_rpc(const repl::ActionbarSlot &slot) {
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const auto params = repl::set_actionbar_slot_params(*pc.rpc("ServerSetActionbarSlotUseData"), slot);
    return params ? actor_rpc(pc, "ServerSetActionbarSlotUseData", *params) : BunchWriter();
}
inline void advance(Harness &h, int ms) {
    for (int i = 0; i < ms / 20; ++i) h.step(milliseconds(20));
}
inline std::vector<std::string> sequence_since(const Harness &h, size_t from) { return {h.mirror.sequence.begin() + static_cast<std::ptrdiff_t>(from), h.mirror.sequence.end()}; }
inline constexpr int64_t kBardTree = 6064629885900499588ll, kInstrumentFlute = 6064630871707615237ll;
inline constexpr int64_t kScepterLine = 6064632150851584017ll, kLineXpLevel2 = 400;
inline svc::Character bard_at(int level) {
    svc::Character c = record_with_scrolls();
    c.level = level;
    c.skill_ranks.push_back({kBardTree, {kInstrumentFlute}});
    return c;
}
inline int count_tag(const std::vector<std::string> &seq, const char *tag) { return static_cast<int>(std::count(seq.begin(), seq.end(), std::string(tag))); }
inline std::vector<int64_t> client_list(const Harness &h, int64_t tree) {
    std::vector<int64_t> out;
    if (const auto ids = h.mirror.tracker.purchased_ranks(tree_id(tree)))
        for (const repl::DesignDataId &d : *ids) out.push_back(d.guid);
    return out;
}
inline bool client_bar_is(const Harness &h, const actionbar::Bar &bar) {
    const auto slots = bar.slots();
    if (slots.size() != h.mirror.bar.size()) return false;
    for (const repl::ActionbarSlot &s : slots) {
        const auto it = h.mirror.bar.find({s.bank_index, s.slot_index});
        if (it == h.mirror.bar.end() || !(it->second == s.use)) return false;
    }
    return true;
}
