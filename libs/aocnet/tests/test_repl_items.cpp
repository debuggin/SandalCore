// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_repl_items_common.h"

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
using wire::PacketReader;
namespace gen = aoc::protocol::schema::gen;
namespace cap = capture_items;

struct Read {
    bool                            ok = false;
    std::string                     why;
    std::string                     dump;
    std::vector<ItemInstance>       items;
    std::optional<ItemStorageEvent> event;
    std::optional<StoragePayload>   payload;
};

static bool decode_trigger_ability(const schema::RpcSchema& rpc, const RpcParams& params, Read& out) {
    auto value = [&](const char* path) -> const Value* {
        const schema::RepCommand* cmd = rpc_command(rpc, path);
        const auto it = cmd ? params.values.find(cmd->handle) : params.values.end();
        return it == params.values.end() ? nullptr : &it->second;
    };
    const Value* item = value("ItemData.ItemInstance");
    const Value* slot = value("ItemData.Slot");
    const Value* loc = value("ItemData.Location");
    const Value* guid = value("AbilityGuid");
    const Value* targets = value("Targets");
    const Value* transforms = value("TargetTransforms");
    const ItemInstance* it = item ? item->get_if<ItemInstance>() : nullptr;
    if (!it || !slot || !loc) { out.why = "ServerTriggerAbility without ItemData"; return false; }
    out.items.push_back(*it);
    out.dump = "slot=" + snum(*slot->get_if<int64_t>()) + " loc=" + num(*loc->get_if<uint64_t>()) + " guid=" +
               snum(guid ? *guid->get_if<int64_t>() : 0) + " targets=" + num(targets ? targets->get_if<ArrayValue>()->elements.size() : 0) +
               " transforms=" + num(transforms ? transforms->get_if<ArrayValue>()->elements.size() : 0) + " item=" + short_item(*it);
    return true;
}

static Read read_captured(const cap::Rpc& c, Where where) {
    Read out;
    const schema::RpcSchema* rpc = class_of(where).rpc(c.rpc);
    if (!rpc) { out.why = "no such RPC on the class"; return out; }
    const Bits bits = bits_of_hex(c.hex, c.bits);
    const CodecOptions opts = client_options();
    std::optional<RpcParams> params;
    size_t pos = 0;
    with_exact(bits, [&](const uint8_t* d, size_t, size_t n) { params = read_rpc_params(*rpc, d, n, pos, opts); return 0; });
    if (!params || pos != c.bits) { out.why = "read_rpc_params"; return out; }
    BunchWriter w;
    bool failed = false;
    write_rpc_params(*rpc, *params, w, opts, &failed);
    if (failed || !(Bits(w) == bits)) { out.why = "write_rpc_params gives other bits"; return out; }

    const std::string name = c.rpc;
    if (name == "ClientReceiveStorageEvent" || name == "ServerRequestStorageEvent") {
        out.event = parse_storage_event(*rpc, *params);
        if (!out.event) { out.why = "parse_storage_event"; return out; }
        const auto back = storage_event_params(*rpc, *out.event);
        if (!back || back->values != params->values) { out.why = "storage_event_params differs"; return out; }
        out.dump = dump_event(*out.event);
        out.items.push_back(out.event->item);
    } else if (name == "ClientReceiveStoragePayload") {
        out.payload = parse_storage_payload(*rpc, *params);
        if (!out.payload) { out.why = "parse_storage_payload"; return out; }
        const auto back = storage_payload_params(*rpc, *out.payload);
        if (!back || back->values != params->values) { out.why = "storage_payload_params differs"; return out; }
        BunchWriter w2;
        write_rpc_params(*rpc, *back, w2, opts, &failed);
        if (failed || !(Bits(w2) == bits)) { out.why = "typed payload writes other bits"; return out; }
        out.dump = dump_payload(*out.payload);
        for (const StorageSlot& s : out.payload->storage) out.items.push_back(s.item);
    } else if (name == "ServerTriggerAbility") {
        if (!decode_trigger_ability(*rpc, *params, out)) return out;
    } else {
        out.why = "unknown RPC";
        return out;
    }
    out.ok = true;
    return out;
}

static void record_survey_from_read(int corpus, const Read& r) {
    if (r.payload) g_survey.payloads.push_back(*r.payload);
    if (!r.event || corpus >= 2) return;
    const ItemStorageEvent& e = *r.event;
    ++g_survey.events;
    if (e.type == static_cast<uint8_t>(StorageEventType::Add)) ++g_survey.event_adds;
    else ++g_survey.event_other;
    if (e.type == 0 && e.move_source == StorageEventContainer{} && e.targets.size() == 1 &&
        e.targets[0].slots.size() == 1 && e.targets[0].status == kStorageNoError &&
        e.targets[0].slots[0].count == static_cast<int32_t>(e.item.quantity) &&
        e.targets[0].slots[0].item_guid == e.item.guid && !e.extra_guid_a && !e.extra_i64 && !e.extra_guid_b) {
        ++g_survey.add_shape_ok;
    }
}

struct CorpusRunStats {
    size_t distinct = 0, calls = 0, bad = 0, dump_differs = 0;
    std::string first_why;
};

static void eval_captured_rpc_entry(int corpus, const cap::Rpc& c, CorpusRunStats& st) {
    ++st.distinct;
    const uint8_t occurrences[3] = {c.pc, c.pawn, c.ability};
    bool first_of_rpc = true;
    for (int w = 0; w < 3; ++w) {
        if (!occurrences[w]) continue;
        const Read r = read_captured(c, static_cast<Where>(w));
        st.calls += occurrences[w];
        if (!r.ok) {
            ++st.bad;
            if (st.first_why.empty()) st.first_why = std::string(c.rpc) + ": " + r.why;
            continue;
        }
        if (r.dump != c.dump) {
            ++st.dump_differs;
            if (st.first_why.empty()) st.first_why = std::string(c.rpc) + " dump:\n  " + r.dump + "\n  " + c.dump;
        }
        for (const ItemInstance& it : r.items) {
            g_items[corpus] += occurrences[w];
            if (!it.has_item) g_empty[corpus] += occurrences[w];
            else g_counts[dump_item(it)] += occurrences[w];
        }
        if (first_of_rpc) {
            first_of_rpc = false;
            record_survey_from_read(corpus, r);
        }
    }
}

static void test_captured_rpcs() {
    std::printf("the captured RPCs: read, typed, built again and written back\n");
    static const char* labels[3] = {"login", "bard", "client"};
    for (int corpus = 0; corpus < 3; ++corpus) {
        size_t n = 0;
        const cap::Rpc* rpcs = corpus_rpcs(corpus, &n);
        CorpusRunStats st;
        for (size_t i = 0; i < n; ++i) eval_captured_rpc_entry(corpus, rpcs[i], st);
        char msg[256];
        std::snprintf(msg, sizeof msg, "%s: %zu distinct payloads (%zu in the capture) read, typed, rebuilt and written to the same bits", labels[corpus], st.distinct, st.calls);
        CHECK(st.bad == 0, msg);
        if (st.bad) std::printf("    first: %s\n", st.first_why.c_str());
        std::snprintf(msg, sizeof msg, "%s: every event, payload and call dumps as the independent decoder's text says", labels[corpus]);
        CHECK(st.dump_differs == 0, msg);
        if (st.dump_differs) std::printf("    first: %s\n", st.first_why.c_str());
    }
    CHECK(g_items[0] == cap::kLoginItems && g_empty[0] == cap::kLoginEmpty && cap::kLoginItems == 2342 && cap::kLoginEmpty == 1512,
          "the login: 2,342 FItemInstance (1,512 empty, 830 items) in its payloads and events");
    CHECK(g_items[1] == cap::kBardItems && g_empty[1] == cap::kBardEmpty && cap::kBardItems == 1217 && cap::kBardEmpty == 1152,
          "the Bard: 1,217 FItemInstance (1,152 empty, 65 items)");
    CHECK(g_items[2] == cap::kClientItems && g_empty[2] == 0 && cap::kClientItems == 46, "the client's 46 requests and triggers each carry an item");
}

static void test_instances_retail_invariants() {
    size_t odd = 0, longest = 0;
    for (const cap::Instance& inst : cap::kInstances) {
        size_t n = 0;
        const cap::Rpc& c = corpus_rpcs(inst.corpus, &n)[inst.rpc];
        const auto it = read_item_bits(bits_of_hex(c.hex, c.bits).slice(inst.at, inst.bits));
        if (!it) { ++odd; continue; }
        if (!it->gems.empty() || !it->custom_data.empty() || it->stolen || it->crate_guid != GuidValue{} || it->enchantment_level ||
            it->tempering_value)
            ++odd;
        longest = std::max<size_t>(longest, inst.bits);
    }
    CHECK(odd == 0, "retail instances never carry gems, custom data, a stolen flag, a crate, an enchantment or a tempering value");
    const size_t plain_bits = 1096;
    bool plain_is_1096 = false, empty_is_1 = false;
    for (const cap::Instance& inst : cap::kInstances) plain_is_1096 = plain_is_1096 || inst.bits == plain_bits;
    BunchWriter w;
    write_item_instance(w, ItemInstance{});
    empty_is_1 = w.bit_pos() == 1;
    CHECK(plain_is_1096 && empty_is_1 && longest == 1096 + 2 * 96 + 160,
          "a plain instance is 1,096 bits, the empty one 1, the longest retail one (a mount: two persisted stats and a cooldown) 1,448");
}

static void test_instances() {
    std::printf("every distinct FItemInstance, read alone and written back\n");
    constexpr size_t kN = sizeof(cap::kInstances) / sizeof(cap::kInstances[0]);
    size_t bad = 0, text_differs = 0, count_differs = 0, total = 0, mount_instances = 0;
    std::set<std::string> seen;
    bool all_kinds[4] = {};
    bool mount_shape = true, mount_cooldown = true;
    for (const cap::Instance& inst : cap::kInstances) {
        size_t n = 0;
        const cap::Rpc& c = corpus_rpcs(inst.corpus, &n)[inst.rpc];
        const Bits mine = bits_of_hex(c.hex, c.bits).slice(inst.at, inst.bits);
        size_t consumed = 0;
        const auto it = read_item_bits(mine, &consumed);
        bool ok = false;
        if (!it || consumed != inst.bits || !it->has_item || !(write_item_bits(*it, &ok) == mine) || !ok) { ++bad; continue; }
        const std::string text = dump_item(*it);
        if (text != inst.dump) ++text_differs;
        const auto found = g_counts.find(text);
        if (found == g_counts.end() || found->second != inst.count) ++count_differs;
        seen.insert(text);
        total += inst.count;
        all_kinds[0] = all_kinds[0] || (it->stats.empty() && it->cooldowns.empty() && it->lock_expiration == 0);
        all_kinds[1] = all_kinds[1] || !it->stats.empty();
        all_kinds[2] = all_kinds[2] || !it->cooldowns.empty();
        all_kinds[3] = all_kinds[3] || (it->lock_expiration != 0);
        if (!it->stats.empty()) {
            ++mount_instances;
            mount_shape = mount_shape && it->stats.size() == 2 && it->stats[0].stat_id == 108986352686712ull && it->stats[1].stat_id == 109183576135288ull &&
                          it->stats[1].value == 1.0f && it->stats[0].value > 0 && it->lock_expiration > 1'700'000'000 && it->lock_expiration < 1'800'000'000;
            for (const PersistedCooldown& cd : it->cooldowns) mount_cooldown = mount_cooldown && cd.record_guid == 6064631555335526877ull && cd.saved_charges == 0;
        }
    }
    char msg[256];
    std::snprintf(msg, sizeof msg, "%zu distinct instances read from their own bits and written back to them, every bit", kN);
    CHECK(bad == 0, msg);
    CHECK(text_differs == 0, "each holds what the independent decoder read from the same bits");
    CHECK(count_differs == 0 && seen.size() == g_counts.size() && seen.size() == kN,
          "and the occurrences of each, counted from the RPCs, are the header's: no instance is missing from either side");
    std::snprintf(msg, sizeof msg, "%zu non-empty instances in all (%zu + %zu + %zu)", total, cap::kLoginItems - cap::kLoginEmpty,
                  cap::kBardItems - cap::kBardEmpty, cap::kClientItems);
    CHECK(total == (cap::kLoginItems - cap::kLoginEmpty) + (cap::kBardItems - cap::kBardEmpty) + cap::kClientItems, msg);
    CHECK(all_kinds[0] && all_kinds[1] && all_kinds[2] && all_kinds[3],
          "retail shows plain items, persisted stats (a mount's two), a persisted cooldown and a lock expiration");
    CHECK(mount_instances >= 3 && mount_shape && mount_cooldown,
          "a mount item persists Health (108986352686712) and Mana (109183576135288: 1), a LockExpiration in Unix seconds and, at most, the dash's cooldown (MountAbility_Sprint, no charge saved)");
    test_instances_retail_invariants();
}

static void test_synthetic() {
    std::printf("instances no capture shows, against the Python writer and reader\n");
    size_t bad = 0, differs = 0;
    for (const cap::Synthetic& s : cap::kSynthetic) {
        const Bits mine = bits_of_hex(s.hex, s.bits);
        size_t consumed = 0;
        const auto it = read_item_bits(mine, &consumed);
        if (!it || consumed != s.bits) { ++bad; continue; }
        bool ok = false;
        const Bits back = write_item_bits(*it, &ok);
        if (!ok || !(back == mine)) { ++bad; continue; }
        if (dump_item(*it) != s.dump) ++differs;
    }
    char msg[200];
    std::snprintf(msg, sizeof msg, "%zu synthetic instances (gems, stolen, crate, custom data in ANSI and UCS-2, 5-byte packed ints, 31 stats ...) read and written to the same bits",
                  sizeof(cap::kSynthetic) / sizeof(cap::kSynthetic[0]));
    CHECK(bad == 0, msg);
    CHECK(differs == 0, "and each holds what the Python reader made of them");
}

namespace at {
constexpr size_t kHas = 0, kRecord = 1, kQuantity = 65, kQuality = 73, kEnchant = 81, kTemper = 89, kGems = 97, kDurability = 129,
                 kStolen32 = 161, kGuid = 193, kCreation = 321, kCrate = 385, kStolen1 = 513, kGuid2 = 514, kCreation2 = 642,
                 kNode = 706, kCrate2 = 770, kLock = 898, kStats = 962, kCooldowns = 968, kCustom = 1000, kTimestamp = 1032, kEnd = 1096;
}

static void test_layout_plain_bits(const ItemInstance& plain, const Bits& bits, bool ok) {
    CHECK(ok && bits.size() == at::kEnd && bits.get(0, 1) == 1, "a plain instance is 1,096 bits and starts with the has-item bit");
    CHECK(bits.get(at::kRecord, 64) == plain.record_id && bits.get(at::kQuantity, 8) == (1u << 1) && bits.get(at::kQuality, 8) == (1u << 1),
          "the record id, then each packed int as a byte: the value in bits 1-7, bit 0 'more follows'");
    CHECK(bits.get(at::kDurability, 32) == std::bit_cast<uint32_t>(11.0f) && bits.get(at::kStolen32, 32) == 0 &&
              bits.get(at::kGuid, 32) == plain.guid.a && bits.get(at::kGuid + 96, 32) == plain.guid.d &&
              bits.get(at::kCreation, 64) == static_cast<uint64_t>(plain.creation_time),
          "durability, the stolen UBOOL, the GUID and the creation time");
    CHECK(bits.slice(at::kGuid2, 128) == bits.slice(at::kGuid, 128) && bits.slice(at::kCreation2, 64) == bits.slice(at::kCreation, 64) &&
              bits.slice(at::kCrate2, 128) == bits.slice(at::kCrate, 128) && bits.get(at::kStolen1, 1) == 0,
          "the GUID, creation time and crate guid are written twice, equal (and bIsStolen as a bit)");
    CHECK(bits.get(at::kStats, 6) == 0 && bits.get(at::kCooldowns, 32) == 0 && bits.get(at::kCustom, 32) == 0 && bits.get(at::kGems, 32) == 0,
          "the counts of gems, persisted stats (6 bits), cooldowns and custom data are zero");
    size_t consumed = 0;
    const auto back = read_item_bits(bits, &consumed);
    CHECK(back && *back == plain && consumed == at::kEnd, "and it reads back equal, to the last bit");
}

static void test_layout_field_refusals(const Bits& bits) {
    auto refused = [&](Bits patched) { return !read_item_bits(patched).has_value(); };
    Bits b = bits; b.set(at::kStolen32, 2, 32);
    CHECK(refused(b), "a UBOOL of 2 (an archive error in the exe): refused");
    b = bits; b.set(at::kStolen32, 1, 32);
    CHECK(refused(b), "the two copies of bIsStolen differ: refused");
    b = bits; b.set(at::kStolen32, 1, 32); b.set(at::kStolen1, 1, 1);
    CHECK(read_item_bits(b).has_value() && read_item_bits(b)->stolen, "both set: a stolen item");
    b = bits; b.set(at::kGuid2, 1, 32);
    CHECK(refused(b), "a second GUID that differs: refused");
    b = bits; b.set(at::kCreation2, 5, 64);
    CHECK(refused(b), "a second creation time that differs: refused");
    b = bits; b.set(at::kCrate2, 7, 32);
    CHECK(refused(b), "a second crate guid that differs: refused");
    b = bits; b.set(at::kGems, kMaxItemGems + 1, 32);
    CHECK(refused(b), "more gems than the bound: refused");
    b = bits; b.set(at::kGems, 0xffffffffull, 32);
    CHECK(refused(b), "a gem count of -1: refused, nothing reserved");
    b = bits; b.set(at::kGems, kMaxItemGems, 32);
    CHECK(refused(b), "gems the bits left cannot hold: refused");
    b = bits; b.set(at::kStats, 32, 6);
    CHECK(refused(b), "32 persisted stats (the exe refuses more than 31): refused");
    b = bits; b.set(at::kStats, 63, 6);
    CHECK(refused(b), "63 persisted stats: refused");
    b = bits; b.set(at::kCooldowns, kMaxItemCooldowns + 1, 32);
    CHECK(refused(b), "more cooldowns than the bound: refused");
    b = bits; b.set(at::kCustom, kMaxItemCustomData + 1, 32);
    CHECK(refused(b), "more custom data than the bound: refused");
    b = bits; b.set(at::kCustom, 1, 32);
    CHECK(refused(b), "a custom data entry that ends the stream: refused");
}

static void test_layout_packed_ints_and_truncation(const Bits& bits) {
    auto with_quantity_bytes = [&](const std::vector<uint8_t>& bytes) {
        Bits b = bits;
        b.erase(at::kQuantity, 8);
        for (size_t i = 0; i < bytes.size(); ++i) b.insert(at::kQuantity + 8 * i, bytes[i], 8);
        return b;
    };
    const auto five = read_item_bits(with_quantity_bytes({0xff, 0xff, 0xff, 0xff, 0x1e}));
    CHECK(five && five->quantity == 0xffffffffu, "0xffffffff is five bytes");
    CHECK(!read_item_bits(with_quantity_bytes({0xff, 0xff, 0xff, 0xff, 0x20})).has_value(), "a fifth byte past bit 31: refused");
    CHECK(!read_item_bits(with_quantity_bytes({0xff, 0xff, 0xff, 0xff, 0x1f})).has_value(), "a fifth byte that says more follows: refused");
    CHECK(!read_item_bits(with_quantity_bytes({0x03, 0x00})).has_value(), "1 as two bytes (not the shortest form): refused");
    CHECK(!read_item_bits(with_quantity_bytes({0x81, 0x80, 0x00})).has_value(), "a zero payload byte after a continuation: refused");
    const auto two = read_item_bits(with_quantity_bytes({0x03, 0x02}));
    const auto other = read_item_bits(with_quantity_bytes({0x81, 0x02}));
    CHECK(two && two->quantity == 129 && other && other->quantity == 192, "two bytes: 0x03 0x02 is 129 (1 + 1 << 7), 0x81 0x02 is 192 (64 + 1 << 7)");

    bool all_refused = true;
    for (size_t n = 0; n < bits.size(); ++n) all_refused = all_refused && !read_item_bits(bits.slice(0, n)).has_value();
    CHECK(all_refused, "a plain instance cut at every length: refused each time");
    const auto& synth = cap::kSynthetic[sizeof(cap::kSynthetic) / sizeof(cap::kSynthetic[0]) - 1];
    const Bits every = bits_of_hex(synth.hex, synth.bits);
    all_refused = true;
    for (size_t n = 0; n < every.size(); ++n) all_refused = all_refused && !read_item_bits(every.slice(0, n)).has_value();
    CHECK(all_refused, "the instance with every variant (gems, stats, cooldowns, custom data in UCS-2) cut at every length: refused each time");
}

static void test_layout_empty_and_registry(const ItemInstance& plain) {
    BunchWriter w;
    CHECK(write_item_instance(w, ItemInstance{}) && w.bit_pos() == 1 && (w.data()[0] & 1) == 0, "the empty item is one 0 bit");
    ItemInstance odd;
    odd.quantity = 3;
    BunchWriter w2;
    CHECK(!write_item_instance(w2, odd) && w2.bit_pos() == 0 && !item_instance_fits(odd), "an empty item with fields set is not written (they would be lost)");
    ItemInstance many = plain_item();
    many.gems.resize(kMaxItemGems + 1);
    ItemInstance stats = plain_item();
    stats.stats.resize(kMaxItemStats + 1);
    ItemInstance text = plain_item();
    text.custom_data.push_back({0, std::string("a\0b", 3)});
    BunchWriter w3;
    CHECK(!item_instance_fits(many) && !item_instance_fits(stats) && !item_instance_fits(text) && !write_item_instance(w3, many) &&
              !write_item_instance(w3, stats) && !write_item_instance(w3, text) && w3.bit_pos() == 0,
          "too many gems or stats, a custom data string with a NUL: not written");

    CHECK(is_known_struct("FItemInstance") && is_known_struct("FItemStorageEvent"), "both structs have a codec in the registry");
    BunchWriter rw;
    CHECK(write_struct("FItemInstance", Value(plain), rw) && rw.bit_pos() == at::kEnd, "write_struct writes the instance");
    PacketReader r(rw.data(), rw.byte_size(), rw.bit_pos());
    const auto v = read_struct("FItemInstance", r);
    CHECK(v && *v == Value(plain) && v->kind() == ValueKind::Struct, "read_struct reads it as a Value");
    CHECK(struct_accepts("FItemInstance", Value(plain)) && !struct_accepts("FItemInstance", Value(Vec3d{})) &&
              !struct_accepts("FItemStorageEvent", Value(plain)) && struct_accepts("FItemStorageEvent", Value(ItemStorageEvent{})),
          "struct_accepts takes an instance for FItemInstance and an event for FItemStorageEvent, nothing else");
    CHECK(Value(ItemStorageEvent{}).kind() == ValueKind::Struct && !zero_struct("FItemStorageEvent").has_value(),
          "an event is a struct value; its zero form is not claimed");
    CHECK(Value(plain) != Value(ItemInstance{}) && Value(ItemInstance{}) == Value(ItemInstance{}), "values compare by their fields");
    CHECK(sizeof(Value) <= sizeof(ItemInstance) + 16, "a Value is no bigger than an ItemInstance and its tag (the event is boxed)");
    Value a{sample_event()};
    Value b = a;
    b.get_if<ItemStorageEvent>()->type = 4;
    CHECK(a.get_if<ItemStorageEvent>()->type == 2 && b.get_if<ItemStorageEvent>()->type == 4 && a != b,
          "a copy of a Value holding an event is deep");
    Value c = std::move(b);
    b = a;
    CHECK(*b.get_if<ItemStorageEvent>() == *a.get_if<ItemStorageEvent>() && b == a && c.get_if<ItemStorageEvent>()->type == 4 &&
              !Value(plain).get_if<ItemStorageEvent>(),
          "assigning to a moved-from Value works, and another alternative holds no event");
    ItemInstance nan_a = plain, nan_b = plain;
    nan_a.durability = std::bit_cast<float>(0x7fc00001u);
    nan_b.durability = std::bit_cast<float>(0x7fc00001u);
    CHECK(nan_a == nan_b && nan_a != plain, "floats compare by their bits (a NaN equals itself)");
}

static void test_layout_and_refusals() {
    std::printf("FItemInstance by hand: the layout, and what is refused\n");
    const ItemInstance plain = plain_item();
    bool ok = false;
    const Bits bits = write_item_bits(plain, &ok);
    test_layout_plain_bits(plain, bits, ok);
    test_layout_field_refusals(bits);
    test_layout_packed_ints_and_truncation(bits);
    test_layout_empty_and_registry(plain);
}

int main() {
    test_captured_rpcs();
    test_instances();
    test_synthetic();
    test_layout_and_refusals();
    run_repl_items_part2();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
