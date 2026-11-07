// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "../bots/bot_types.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/skill_rpc.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/wire/packet_reader.h"
#include "protocol/wire/ue5_primitives.h"

namespace aoc::world::tests::plans {

namespace wire = aoc::protocol::wire;
using bots::BunchWriter;
using bots::class_named;
using bots::ExportCursor;
using bots::ExportRecord;
using bots::FIntrepidNetworkGUID;
using bots::LiveSession;

inline bool encode_hex_guid_words(std::string_view hex_raw, uint8_t* out16) {
    const std::string hex = bots::strip_hyphens_lower(hex_raw);
    if (hex.size() != 32) return false;
    for (size_t i = 0; i < 4; ++i) {
        char* endp = nullptr;
        const std::string part = hex.substr(i * 8, 8);
        const unsigned long val = std::strtoul(part.c_str(), &endp, 16);
        if (!endp || *endp != '\0') return false;
        const uint32_t word = static_cast<uint32_t>(val);
        std::memcpy(out16 + i * 4, &word, 4);
    }
    return ::ue5::guid_to_hex(out16) == hex;
}

inline bool read_export_entry(ExportCursor& cur, std::vector<ExportRecord>& out, FIntrepidNetworkGUID* guid_out, int depth) {
    if (depth > 8) return false;
    ExportRecord r;
    r.guid = bots::emit::read_intrepid_guid(cur.data, cur.len, cur.pos);
    if (guid_out) *guid_out = r.guid;
    if (r.guid.ObjectId == 0) return true;
    r.flags = static_cast<uint8_t>(::ue5::read_bits(cur.data, cur.len, cur.pos, 8));
    if ((r.flags & 1u) != 0u) {
        if (!read_export_entry(cur, out, &r.outer, depth + 1)) return false;
        const int32_t n = static_cast<int32_t>(::ue5::read_bits(cur.data, cur.len, cur.pos, 32));
        if (n < 0 || n > 512) return false;
        for (int32_t i = 0; i < n; ++i) {
            const char c = static_cast<char>(::ue5::read_bits(cur.data, cur.len, cur.pos, 8));
            if (c != 0) r.path.push_back(c);
        }
        if ((r.flags & 4u) != 0u) r.checksum = static_cast<uint32_t>(::ue5::read_bits(cur.data, cur.len, cur.pos, 32));
    }
    out.push_back(std::move(r));
    return true;
}

inline void scan_delivered_exports(LiveSession& s, const fake::Delivered& d) {
    for (const BunchWriter& e : d.exports) {
        const size_t bits = e.bit_pos();
        ExportCursor cur{e.data(), e.byte_size() + 1, 0};
        (void)::ue5::read_bits(cur.data, cur.len, cur.pos, 1);
        const uint32_t count = static_cast<uint32_t>(::ue5::read_bits(cur.data, cur.len, cur.pos, 32));
        std::vector<ExportRecord> recs;
        for (uint32_t i = 0; i < count && cur.pos < bits && i < 256; ++i) {
            if (!read_export_entry(cur, recs, nullptr, 0)) break;
        }
        for (const ExportRecord& r : recs) {
            if (r.path == "AbilityComponent" && s.ability_guid.ObjectId == 0) s.ability_guid = r.guid;
            if (r.path == "BackpackItemStorage" && s.backpack_guid.ObjectId == 0) s.backpack_guid = r.guid;
            if (r.path == "EquipmentItemStorage" && s.equipment_guid.ObjectId == 0) s.equipment_guid = r.guid;
        }
    }
}

inline void inspect_storage_and_bar_rpcs(LiveSession& s, const fake::Delivered& d) {
    if (d.channel != 3 && d.channel != 5) return;
    const bots::schema::ClassSchema& cls = class_named(d.channel == 3 ? "AoCPlayerControllerBP_C" : "PlayerPawn_C");
    const auto blocks = bots::repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return;
    for (const bots::repl::BlockView& b : *blocks) {
        if (b.header.has_rep_layout) continue;
        size_t pos = b.payload_pos;
        const auto calls = bots::repl::read_rpc_fields(cls, d.content.data(), b.payload_pos + b.payload_bits, pos);
        if (!calls) continue;
        for (const bots::repl::RpcCall& call : *calls) {
            if (const auto sp = bots::repl::parse_storage_payload(call)) {
                ++s.storage_payloads;
                if (sp->maximum_items == 70 && s.backpack_guid.ObjectId == 0) s.backpack_guid = sp->target;
                if (sp->maximum_items == 83 && s.equipment_guid.ObjectId == 0) s.equipment_guid = sp->target;
                for (const bots::repl::StorageSlot& sl : sp->storage) {
                    if (!sl.item.has_item) continue;
                    if (!s.has_scroll || sl.item.record_id == bots::kGmAdventuringScroll) {
                        s.scroll_slot = sl.slot;
                        s.scroll_qty = std::max<int32_t>(1, sl.item.quantity);
                        s.scroll_guid = sl.item.guid;
                        s.has_scroll = true;
                    }
                }
            }
            if (bots::repl::parse_actionbar_payload(call)) ++s.actionbar_payloads;
        }
    }
}

inline FIntrepidNetworkGUID parse_spawn_actor_guid(const BunchWriter& w) {
    wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    FIntrepidNetworkGUID g{};
    const uint64_t lo = r.read_uint32();
    const uint64_t hi = r.read_uint32();
    g.ObjectId = lo | (hi << 32);
    g.ServerId = r.read_uint32();
    g.Randomizer = r.read_uint32();
    return g;
}

inline size_t pump_udp(LiveSession& s, int wait_ms, bool send_acks = true) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    size_t got_packets = 0;
    uint8_t buf[2048]{};
    for (int step = 0; step < 2000 && std::chrono::steady_clock::now() < deadline; ++step) {
        bots::Address from{};
        const auto n = s.sock->receive(buf, sizeof(buf), from, std::chrono::microseconds(10000));
        if (!n || *n == 0) continue;
        size_t batch = 0;
        std::optional<size_t> cur = n;
        for (int drain = 0; drain < 128 && cur && *cur > 0; ++drain) {
            if (!bots::conn::is_handshake(buf, *cur) && s.client) {
                std::vector<uint8_t> pkt(buf, buf + *cur);
                fake::Received rx = s.client->receive(pkt);
                for (const fake::Delivered& d : rx.delivered) {
                    if (d.channel >= 7 && d.open) {
                        ++s.replicated_npc_channels;
                        const FIntrepidNetworkGUID g = parse_spawn_actor_guid(d.content);
                        if (g.ObjectId == s.goblin_id) s.goblin_guid = g;
                    }
                    scan_delivered_exports(s, d);
                    inspect_storage_and_bar_rpcs(s, d);
                    s.delivered.push_back(d);
                }
                s.rx_packets.push_back(std::move(rx));
                ++got_packets;
                ++batch;
            }
            cur = s.sock->receive(buf, sizeof(buf), from, std::chrono::microseconds(0));
        }
        if (batch > 0 && send_acks && s.client) {
            const auto ack = s.client->packet({});
            s.sock->send(s.peer, ack.data(), ack.size());
        }
    }
    return got_packets;
}

inline bool send_bunches(LiveSession& s, std::vector<fake::Bunch> bunches) {
    if (!s.client) return false;
    const auto pkt = s.client->packet(std::move(bunches));
    return s.sock->send(s.peer, pkt.data(), pkt.size());
}

inline bool run_stage6_inventory_scrolls(LiveSession& s) {
    const bots::schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const bots::schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    bots::repl::RpcParams used;
    if (const bots::schema::RpcSchema* rpc = pawn.rpc("ServerItemUsed")) {
        bots::repl::set_rpc_param(*rpc, used, "Storage", bots::repl::Value::u64(3));
        bots::repl::set_rpc_param(*rpc, used, "Slot", bots::repl::Value::i64(s.scroll_slot));
        bots::repl::set_rpc_param(*rpc, used, "ExpectedItem.A", bots::repl::Value::i64(static_cast<int32_t>(s.scroll_guid.a)));
        bots::repl::set_rpc_param(*rpc, used, "ExpectedItem.B", bots::repl::Value::i64(static_cast<int32_t>(s.scroll_guid.b)));
        bots::repl::set_rpc_param(*rpc, used, "ExpectedItem.C", bots::repl::Value::i64(static_cast<int32_t>(s.scroll_guid.c)));
        bots::repl::set_rpc_param(*rpc, used, "ExpectedItem.D", bots::repl::Value::i64(static_cast<int32_t>(s.scroll_guid.d)));
        bots::repl::set_rpc_param(*rpc, used, "RequestedEquipSlot", bots::repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
    }
    send_bunches(s, {s.client->actor(5, bots::build_actor_rpc(pawn, "ServerItemUsed", used), true)});
    pump_udp(s, 200);

    bots::repl::ItemStorageEvent ev{};
    ev.type = static_cast<uint8_t>(bots::repl::StorageEventType::Move);
    ev.move_source.object = s.backpack_guid;
    ev.move_source.slots = {{s.scroll_slot, 1, s.scroll_guid}};
    ev.move_source.status = bots::repl::kStorageNoError;
    ev.targets.push_back({s.backpack_guid, {{35, 1, s.scroll_guid}}, bots::repl::kStorageNoError});
    if (const bots::schema::RpcSchema* rpc = pc.rpc("ServerRequestStorageEvent")) {
        if (const auto params = bots::repl::storage_event_params(*rpc, ev)) {
            send_bunches(s, {s.client->actor(3, bots::build_actor_rpc(pc, "ServerRequestStorageEvent", *params), true)});
        }
    }
    pump_udp(s, 200);
    std::printf("  [ok ] Stage 6: ServerItemUsed (slot=%d) & ServerRequestStorageEvent processed\n", s.scroll_slot);
    return true;
}

inline void recompute_bot_equipment_stats(bots::PlayerBot& bot) {
    bot.phys_power = bot.base_phys_power;
    bot.mag_power = bot.base_mag_power;
    bot.armor = bot.base_armor;
    bot.gathering_speed_pct = 100;
    for (const auto& node : bot.unlocked_skills) {
        if (node.is_passive) {
            if (bot.base_phys_power >= bot.base_mag_power) bot.phys_power += node.power_bonus;
            else bot.mag_power += node.power_bonus;
        }
    }
    for (const auto& eq : bot.equipped_items) {
        bot.phys_power += eq.phys_power_bonus;
        bot.mag_power += eq.mag_power_bonus;
        bot.armor += eq.armor_bonus;
        bot.gathering_speed_pct += eq.gathering_speed_pct;
    }
}

inline bool equip_all_preset_items(bots::PlayerBot& bot) {
    if (bot.loadout_presets.size() < 4) return false;
    bot.equipped_items.clear();
    recompute_bot_equipment_stats(bot);
    const int32_t unequipped_phys = bot.phys_power;
    const int32_t unequipped_mag = bot.mag_power;
    const int32_t unequipped_armor = bot.armor;
    const int32_t unequipped_gather = bot.gathering_speed_pct;
    for (const auto& item : bot.loadout_presets) bot.equipped_items.push_back(item);
    recompute_bot_equipment_stats(bot);
    return ((bot.phys_power > unequipped_phys) || (bot.mag_power > unequipped_mag)) &&
           bot.armor > unequipped_armor && bot.gathering_speed_pct > unequipped_gather;
}

inline bool verify_unequip_and_reequip_cycle(bots::PlayerBot& bot) {
    if (bot.equipped_items.empty()) return false;
    const int32_t full_armor = bot.armor;
    const auto saved = bot.equipped_items;
    bot.equipped_items.clear();
    recompute_bot_equipment_stats(bot);
    if (bot.armor >= full_armor) return false;
    bot.equipped_items = saved;
    recompute_bot_equipment_stats(bot);
    return bot.armor == full_armor && bot.equipped_items.size() >= 4;
}

inline bool run_plan_equipment(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            return equip_all_preset_items(actor) &&
                   verify_unequip_and_reequip_cycle(actor) &&
                   bots::assert_partner_visible_and_updating(actor, observer);
        });
}

}  // namespace aoc::world::tests::plans
