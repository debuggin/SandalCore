// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once
#include "bootstrap/captured_replies.h"
#include "bootstrap/character_setup.h"
#include "capture_leveling.h"
#include "capture_packets.h"
#include "protocol/repl/ability_rpc.h"
#include "test_worldd_items_session_common.h"
#include <sqlite3.h>
struct Harness {
    Fixture fx;
    RecordingSink sink;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::vector<size_t> packet_of;
    size_t packets = 0;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0;
    Mirror mirror;
    size_t mirrored = 0;
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
        const repl::Subobject *info = s->bootstrap().pawn().find(mirror.guids.base_character_info);
        const repl::Subobject *app = s->bootstrap().pawn().find(mirror.guids.appearance);
        mirror.info_class = info ? info->obj.schema : nullptr;
        const repl::Subobject *stats = s->bootstrap().pawn().find(mirror.guids.stats);
        mirror.stats_class = stats ? stats->obj.schema : nullptr;
        const repl::Subobject *ability = s->bootstrap().pawn().find(mirror.guids.ability);
        mirror.ability_class = ability ? ability->obj.schema : nullptr;
        {
            const sg::StatBundle *bundle = world_stats::of(*fx.content).bundle;
            mirror.stat_extended = [bundle](int64_t record) {
                const auto it = bundle->by_guid.find(record);
                return it != bundle->by_guid.end() && (bundle->stats[it->second].flags & sg::kStatBuckets) != 0;
            };
        }
        mirror.appearance_class = app ? app->obj.schema : nullptr;
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
    size_t chains_since(size_t from) const {
        return got.size() - from;
    }
};
inline BunchWriter bag_move_or_swap(Harness &h, repl::StorageEventType type, const std::pair<int, int> &from_slot_count, const std::pair<std::string, std::string> &guids, const std::pair<int, int> &to_slot_count) {
    const bs::SessionGuids &g = h.s->bootstrap().setup().guids;
    repl::ItemStorageEvent ev;
    ev.type = static_cast<uint8_t>(type);
    const auto gv = items::guid_of(guids.first);
    ev.item.has_item = true;
    ev.item.record_id = fi::kPotion;
    ev.item.quantity = static_cast<uint32_t>(from_slot_count.second);
    ev.item.quality = 1;
    if (gv) ev.item.guid = *gv;
    ev.item.creation_time = items::ticks_of_ms(1'771'299'474'000ll);
    ev.move_source.object = g.backpack;
    ev.move_source.slots.push_back({from_slot_count.first, from_slot_count.second, gv ? *gv : repl::GuidValue{}});
    repl::StorageEventContainer target;
    target.object = g.backpack;
    const auto tg = items::guid_of(guids.second);
    target.slots.push_back({to_slot_count.first, to_slot_count.second, tg ? *tg : repl::GuidValue{}});
    ev.targets.push_back(target);
    ev.instigator = repl::GuidValue{0xbbec5c86u, 0xeb101fe5u, 0xf38c87f0u, 0xfa22e7d0u};
    return storage_request(ev);
}
inline BunchWriter bag_move(Harness &h, int from, int count, const std::string &guid, int to) {
    return bag_move_or_swap(h, repl::StorageEventType::Move, {from, count}, {guid, {}}, {to, 1});
}
inline BunchWriter bag_swap_potions_and_heavy_chest(Harness &h) {
    return bag_move_or_swap(h, repl::StorageEventType::Swap, {32, 5}, {guid_n(201), guid_n(203)}, {3, 1});
}
inline void append_pc_calls_from_block(const fake::Delivered &chn, const repl::BlockView &b, std::vector<std::string> &out) {
    size_t p = b.payload_pos;
    const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), chn.content.data(), b.payload_pos + b.payload_bits, p);
    if (!calls) return;
    for (const auto &c : *calls) out.push_back(c.rpc->name);
}
inline std::vector<std::string> pc_calls_since(const Harness &h, size_t from_chain) {
    std::vector<std::string> out;
    for (size_t i = from_chain; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch) continue;
        const auto blocks = repl::read_content_blocks(h.got[i].content.data(), h.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
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
    std::string source_guid = guid_n(201);
    std::string event_guid = guid_n(201);
    std::string target_guid;
    bool has_source = true;
    size_t targets = 1;
};
inline MoveSpec potion_move(const Harness &h) {
    MoveSpec m;
    m.from_object = h.s->bootstrap().setup().guids.backpack;
    m.to_object = m.from_object;
    return m;
}
inline BunchWriter storage_move(const MoveSpec &m) {
    repl::ItemStorageEvent ev;
    ev.type = m.type;
    const auto eg = items::guid_of(m.event_guid);
    ev.item.has_item = true;
    ev.item.record_id = static_cast<uint64_t>(fi::kPotion);
    ev.item.quantity = static_cast<uint32_t>(std::max(1, m.from_count));
    ev.item.quality = 1;
    if (eg) ev.item.guid = *eg;
    ev.item.creation_time = items::ticks_of_ms(1'771'299'474'000ll);
    ev.move_source.object = m.from_object;
    const auto sg = items::guid_of(m.source_guid);
    if (m.has_source) ev.move_source.slots.push_back({m.from_slot, m.from_count, sg ? *sg : repl::GuidValue{}});
    const auto tg = items::guid_of(m.target_guid);
    for (size_t i = 0; i < m.targets; ++i) {
        repl::StorageEventContainer t;
        t.object = m.to_object;
        t.slots.push_back({m.to_slot + static_cast<int>(i), m.to_count, tg ? *tg : repl::GuidValue{}});
        ev.targets.push_back(std::move(t));
    }
    ev.instigator = repl::GuidValue{0xbbec5c86u, 0xeb101fe5u, 0xf38c87f0u, 0xfa22e7d0u};
    return storage_request(ev);
}
inline bool refused_cleanly(Harness &h, const items::Inventory &before, const char *what, const BunchWriter &bunch, uint32_t channel) {
    h.step(milliseconds(150));
    const size_t refused = h.s->item_requests_refused(), applied = h.s->item_requests_applied(), events = h.mirror.events.size();
    h.request(bunch, channel);
    const bool ok =
        bunch.bit_pos() > 0 && h.s->item_requests_refused() == refused + 1 && h.s->item_requests_applied() == applied && h.mirror.events.size() == events && h.s->inventory() == before && !h.s->inventory_dirty() && !h.s->inventory_changed();
    if (!ok) std::printf("    not refused cleanly: %s\n", what);
    return ok;
}
inline svc::Character cyb_with_a_full_bag() {
    svc::Character c = cyb_record();
    std::vector<svc::InventoryItem> items;
    for (const auto &it : c.items)
        if (it.container != Bag) items.push_back(it);
    items.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    for (int slot = 1; slot < 70; ++slot) items.push_back(make_item(Bag, slot, fi::kPotion, 300 + slot, 1 + slot % 7));
    c.items = items;
    return c;
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
    svc::CharacterResult get_character(const std::string &) override {
        return {};
    }
    svc::CallStatus save_progress(const std::string &id, const svc::Progress &p) override {
        std::lock_guard<std::mutex> lock(mu);
        progress.emplace_back(id, p);
        cv.notify_all();
        return svc::CallStatus::Ok;
    }
    size_t progress_count() {
        std::lock_guard<std::mutex> l(mu);
        return progress.size();
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
    bool in_flight() {
        std::lock_guard<std::mutex> l(mu);
        return running > 0;
    }
    size_t inventory_count() {
        std::lock_guard<std::mutex> l(mu);
        return inventories.size();
    }
    size_t position_count() {
        std::lock_guard<std::mutex> l(mu);
        return positions.size();
    }
    size_t inventory_call_count() {
        std::lock_guard<std::mutex> l(mu);
        return inventory_calls;
    }
    size_t position_call_count() {
        std::lock_guard<std::mutex> l(mu);
        return position_calls;
    }
    std::vector<Inv> saved_inventories() {
        std::lock_guard<std::mutex> l(mu);
        return inventories;
    }
};
inline svc::Position pos_of(double x) {
    return {x, 0, 0, 0};
}
inline CharacterSaver::Config quick(milliseconds destructor_wait = milliseconds(2000)) {
    CharacterSaver::Config c;
    c.retry_delay = milliseconds(5);
    c.destructor_wait = destructor_wait;
    return c;
}
inline std::vector<svc::InventoryItem> first_items(size_t n) {
    auto all = bard_items();
    all.resize(std::min(n, all.size()));
    return all;
}
inline repl::CodecOptions client_options() {
    repl::CodecOptions o;
    o.inline_objects = true;
    return o;
}
inline BunchWriter trigger_with_item(const emit::FIntrepidNetworkGUID &ability, int64_t guid, int32_t id, const repl::ItemInstance &item, int slot = 15) {
    const schema::ClassSchema &cls = class_named("UAoCAbilityComponent");
    const schema::RpcSchema *rpc = cls.rpc("ServerTriggerAbility");
    BunchWriter out;
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(slot));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(item));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(4));
    BunchWriter field;
    bool failed = false;
    if (!ok || repl::write_rpc_field(cls, *rpc, p, field, client_options(), &failed) == 0 || failed) return out;
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = ability;
    repl::write_content_block(out, header, field, false);
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
inline const char *array_of(uint8_t replication) {
    return replication == 1 ? kArrays[0] : replication == 5 ? kArrays[1] : kArrays[2];
}
inline uint32_t bits_of(float f) {
    return std::bit_cast<uint32_t>(f);
}
inline bool stat_item_matches_value(const repl::StatInt32Inst &item, const sg::StatValue &v) {
    if (item.value != bits_of(v.value)) return false;
    if (!v.buckets()) return true;
    return item.extended && item.base == bits_of(v.base) && item.equipment == bits_of(v.equip);
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
inline bool is_replicated_int32_stat(const sg::StatValue &v) {
    return !v.float_array() && (v.replication == 1 || v.replication == 5 || v.replication == 7);
}
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
    if (!cs) return false;
    size_t expected = 0;
    if (!stats_match_values(h, *cs, expected, why, unlaid)) return false;
    size_t have = 0;
    for (const auto &[name, items] : h.mirror.stat_items) have += items.size();
    if (unlaid) return have >= expected;
    if (have != expected && why) *why = std::to_string(have) + " items on the client, " + std::to_string(expected) + " stats in the session";
    return have == expected;
}
inline repl::DesignDataId adventuring_id() {
    return repl::DesignDataId::named(content::kGuidAdventuring, content::kTypeLevelingTypeDef, "Adventuring");
}
inline repl::DesignDataId tree_id(int64_t guid) {
    repl::DesignDataId d;
    d.guid = guid;
    d.type_id = content::kTypeSkillTree;
    return d;
}
inline repl::DesignDataId universal_id() {
    return tree_id(content::kGuidUniversalTree);
}
inline svc::Character record_with_scrolls() {
    return record_with(bard_items_with_scrolls());
}
inline size_t count_calls(const std::vector<std::string> &calls, const char *name) {
    size_t n = 0;
    for (const std::string &c : calls) n += c == name ? 1 : 0;
    return n;
}
inline const tl::Catalog &catalog_of(const Harness &h) {
    return *h.fx.world->talents();
}
inline int64_t tree_guid(const Harness &h, const char *name) {
    const tl::Tree *t = catalog_of(h).tree_named(name);
    return t ? t->guid : 0;
}
inline int64_t rank_named(const Harness &h, const char *tree, const char *node) {
    if (const tl::Tree *t = catalog_of(h).tree_named(tree)) {
        for (const tl::Node &n : t->nodes) {
            if (n.name == node && !n.ranks.empty()) return n.ranks.front().guid;
        }
    }
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
    if (const tl::Tree *t = catalog_of(h).tree_named(tree)) {
        for (const tl::Node &n : t->nodes) {
            if (n.name == node) return &n;
        }
    }
    return nullptr;
}
struct Buy {
    int64_t rank, tree;
    bool purchase;
};
inline repl::SkillBuild build_of(std::initializer_list<Buy> entries) {
    repl::SkillBuild b;
    for (const Buy &e : entries) {
        repl::SkillRankChange c;
        c.rank.guid = e.rank;
        c.rank.type_id = content::kTypeSkillRank;
        c.tree.guid = e.tree;
        c.tree.type_id = content::kTypeSkillTree;
        c.tree.name = repl::NameValue::plain("Prototype_Assassin_SkillTable");
        c.leveling_type.guid = content::kGuidAdventuring;
        c.leveling_type.type_id = content::kTypeLevelingTypeDef;
        c.purchase = e.purchase;
        b.changes.push_back(c);
    }
    return b;
}
inline BunchWriter purchase_rpc(const repl::SkillBuild &b) {
    const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema *rpc = pawn.rpc("ServerPurchaseSkillBuild");
    const auto params = repl::purchase_skill_build_params(*rpc, b);
    return params ? actor_rpc(pawn, "ServerPurchaseSkillBuild", *params) : BunchWriter();
}
inline BunchWriter set_slot_rpc(const repl::ActionbarSlot &slot) {
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema *rpc = pc.rpc("ServerSetActionbarSlotUseData");
    const auto params = repl::set_actionbar_slot_params(*rpc, slot);
    return params ? actor_rpc(pc, "ServerSetActionbarSlotUseData", *params) : BunchWriter();
}
inline BunchWriter client_load_rpc() {
    return actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerRequestClientLoad", repl::RpcParams{});
}
inline void advance(Harness &h, int ms) {
    for (int i = 0; i < ms / 20; ++i) h.step(milliseconds(20));
}
inline std::vector<std::string> sequence_since(const Harness &h, size_t from) {
    return std::vector<std::string>(h.mirror.sequence.begin() + static_cast<std::ptrdiff_t>(from), h.mirror.sequence.end());
}
inline constexpr int64_t kBardTree = 6064629885900499588ll, kInstrumentFlute = 6064630871707615237ll, kInstrumentFiddle = 6064630871711875083ll;
inline constexpr int64_t kScepterLine = 6064632150851584017ll, kAxe2HLine = 6064632150845227015ll;
inline constexpr int64_t kLineXpLevel2 = 400;
inline svc::Character bard_at(int level) {
    svc::Character c = record_with_scrolls();
    c.level = level;
    c.skill_ranks.push_back({kBardTree, {kInstrumentFlute}});
    return c;
}
inline int count_tag(const std::vector<std::string> &seq, const char *tag) {
    return static_cast<int>(std::count(seq.begin(), seq.end(), std::string(tag)));
}
inline std::vector<int64_t> client_list(const Harness &h, int64_t tree) {
    std::vector<int64_t> out;
    if (const auto ids = h.mirror.tracker.purchased_ranks(tree_id(tree))) {
        for (const repl::DesignDataId &d : *ids) out.push_back(d.guid);
    }
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
struct HostileFuzzer {
    Harness h;
    std::mt19937_64 rng{0xB2B2B2B2ull};
    std::set<std::string> guids;
    size_t applied_by_use = 0, applied_by_move = 0;
    HostileFuzzer() {
        for (const auto &it : h.s->inventory().items()) guids.insert(it.instance_guid);
    }
    size_t pick(uint64_t n) {
        return static_cast<size_t>(rng() % n);
    }
    int number() {
        static const int64_t extremes[] = {0, 1, -1, 2, 3, 69, 70, 71, 82, 83, 99, 100, 255, 256, 65535, INT32_MAX, INT32_MIN, INT32_MAX - 1};
        if (pick(3) == 0) return static_cast<int>(extremes[pick(sizeof extremes / sizeof extremes[0])]);
        return static_cast<int>(pick(90));
    }
    std::string some_guid(const std::vector<svc::InventoryItem> &items) {
        if (!items.empty() && pick(3) != 0) return items[pick(items.size())].instance_guid;
        return guid_n(static_cast<uint32_t>(900 + pick(40)));
    }
    void step_use(const std::vector<svc::InventoryItem> &items, size_t applied) {
        const svc::InventoryItem *it = items.empty() ? nullptr : &items[pick(items.size())];
        const bool real = it && pick(4) != 0;
        const int storage = real ? (it->container == Bag ? 3 : it->container == Worn ? 4 : 18) : static_cast<int>(pick(4) == 0 ? number() & 0xFF : (pick(2) ? 3 : 4));
        const int slot = real ? it->slot : number();
        const std::string guid = real && pick(5) != 0 ? it->instance_guid : some_guid(items);
        const uint32_t requested = pick(3) == 0 ? static_cast<uint32_t>(number()) : kAny;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(storage, slot, guid, requested), true)});
        if (h.s->item_requests_applied() > applied) ++applied_by_use;
    }
    void step_move_source(MoveSpec &m, const std::vector<svc::InventoryItem> &items) {
        const svc::InventoryItem *it = items.empty() ? nullptr : &items[pick(items.size())];
        const bool real = it && pick(4) != 0;
        m.from_slot = real ? it->slot : number();
        m.from_count = real && pick(5) != 0 ? it->quantity : number();
        m.source_guid = m.event_guid = real && pick(6) != 0 ? it->instance_guid : some_guid(items);
        if (pick(10) == 0) m.event_guid = some_guid(items);
    }
    void step_move_target(MoveSpec &m, const std::vector<svc::InventoryItem> &items) {
        const bs::SessionGuids &g = h.s->bootstrap().setup().guids;
        m.to_slot = pick(2) ? static_cast<int>(pick(40)) : number();
        const svc::InventoryItem *other = items.empty() ? nullptr : &items[pick(items.size())];
        if (other && pick(2)) {
            m.to_slot = other->slot;
            m.target_guid = pick(4) ? other->instance_guid : some_guid(items);
            m.to_count = pick(4) ? other->quantity : number();
        }
        m.has_source = pick(20) != 0;
        m.targets = pick(25) == 0 ? pick(3) : 1;
    }
    void step_move(const std::vector<svc::InventoryItem> &items, size_t applied) {
        const bs::SessionGuids &g = h.s->bootstrap().setup().guids;
        MoveSpec m;
        m.from_object = m.to_object = g.backpack;
        if (pick(10) == 0) m.to_object = pick(2) ? g.equipment : g.quest_storage;
        if (pick(12) == 0) m.from_object = g.equipment;
        m.type = pick(3) == 0 ? static_cast<uint8_t>(pick(8)) : static_cast<uint8_t>(pick(2) ? 2 : 4);
        step_move_source(m, items);
        step_move_target(m, items);
        h.client_sends({h.client.actor(h.pc_ch, storage_move(m), true)});
        if (h.s->item_requests_applied() > applied) ++applied_by_move;
    }
    BunchWriter corrupt_bunch(const BunchWriter &base) {
        BunchWriter bad;
        switch (pick(3)) {
        case 0:
            return with_bits(base, pick(base.bit_pos() + 1));
        case 1: {
            std::vector<size_t> at;
            for (size_t k = 0, n = 1 + pick(3); k < n; ++k) at.push_back(pick(std::max<size_t>(1, base.bit_pos())));
            return flipped_bits(base, at);
        }
        default: {
            for (size_t k = 0, n = pick(300); k < n; ++k) bad.write_bit(pick(2) != 0);
            return bad;
        }
        }
    }
    void step_damaged(const std::vector<svc::InventoryItem> &items) {
        BunchWriter base;
        switch (pick(3)) {
        case 0:
            base = item_used(pick(2) ? 3 : 4, static_cast<int>(pick(4)), some_guid(items));
            break;
        case 1:
            base = item_activate(pick(2) ? 3 : 4, static_cast<int>(pick(4)), some_guid(items));
            break;
        default:
            base = bag_move(h, 1, 5, guid_n(201), static_cast<int>(pick(40)));
            break;
        }
        const uint32_t channel = base.bit_pos() > 0 && pick(2) ? h.pawn_ch : h.pc_ch;
        const BunchWriter bad = corrupt_bunch(base);
        h.client_sends({h.client.actor(channel, bad, pick(2) != 0)});
    }
};
