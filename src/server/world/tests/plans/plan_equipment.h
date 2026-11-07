// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "../bots/bot_types.h"
#include "content/content_db.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/skill_rpc.h"
#include "protocol/repl/storage_rpc.h"

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
                    if (!s.has_scroll || sl.item.quantity > s.scroll_qty) {
                        s.scroll_slot = sl.slot;
                        s.scroll_qty = std::max<int32_t>(1, sl.item.quantity);
                        s.scroll_guid = sl.item.guid;
                        s.scroll_record_id = sl.item.record_id;
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
                ++s.server_acks;
            }
            cur = s.sock->receive(buf, sizeof(buf), from, std::chrono::microseconds(0));
        }
        if (batch > 0 && send_acks && s.client) {
            const auto ack = s.client->packet({});
            if (s.sock->send(s.peer, ack.data(), ack.size())) ++s.tx_packets;
        }
    }
    return got_packets;
}

inline bool send_bunches(LiveSession& s, std::vector<fake::Bunch> bunches) {
    return bots::send_live_bunches(s, std::move(bunches));
}

inline void populate_catalog_talents_and_abilities(LiveSession& s, const aoc::worldd::content::ContentDb& db) {
    std::string err;
    if (const auto rules = db.ability_rules(err)) {
        s.catalog.sprint_ability_guid = rules->sprint.guid;
        s.catalog.dodge_ability_guid = rules->dodge.guid;
        s.catalog.rest_ability_guid = rules->rest.guid;
    }
    s.catalog.adventuring_leveling_guid = aoc::worldd::content::kGuidAdventuring;
    s.catalog.type_skill_rank = aoc::worldd::content::kTypeSkillRank;
    s.catalog.type_skill_tree = aoc::worldd::content::kTypeSkillTree;
    s.catalog.type_leveling_def = aoc::worldd::content::kTypeLevelingTypeDef;
    const auto td = db.talent_data(err);
    if (!td) return;
    std::vector<int64_t> all_abilities;
    std::vector<bots::SkillNodeSpec> all_ranks;
    for (const auto& tree : td->trees) {
        if (tree.guid == 0) continue;
        for (const auto& node : tree.nodes) {
            for (const auto& rank : node.ranks) {
                if (rank.guid != 0 && s.catalog.sample_skill_rank_guid == 0) {
                    s.catalog.sample_skill_rank_guid = rank.guid;
                    s.catalog.sample_skill_tree_guid = tree.guid;
                    s.catalog.sample_skill_tree_name = tree.name;
                }
                if (rank.guid != 0) {
                    all_ranks.push_back({rank.name.empty() ? node.name : rank.name, rank.guid, tree.guid,
                                         rank.ability == 0, 180});
                }
                if (rank.ability != 0) {
                    all_abilities.push_back(rank.ability);
                    if (tree.weapon_line_guid != 0 && s.catalog.weapon_ability_guids.size() < 16) {
                        s.catalog.weapon_ability_guids.push_back(rank.ability);
                    }
                }
            }
        }
    }
    if (all_abilities.empty() && s.catalog.sprint_ability_guid != 0) {
        all_abilities.push_back(s.catalog.sprint_ability_guid);
    }
    s.catalog.primary_attack_ability_guid = all_abilities.empty() ? s.catalog.sprint_ability_guid : all_abilities[0];
    s.catalog.pet_command_guid = all_abilities.size() > 1 ? all_abilities[1] : s.catalog.primary_attack_ability_guid;
    while (s.catalog.weapon_ability_guids.size() < 16 && !all_abilities.empty()) {
        s.catalog.weapon_ability_guids.push_back(all_abilities[s.catalog.weapon_ability_guids.size() % all_abilities.size()]);
    }
    for (size_t c = 0; c < 8; ++c) {
        for (size_t k = 0; k < 4 && !all_ranks.empty(); ++k) {
            auto rn = all_ranks[(c * 4 + k) % all_ranks.size()];
            rn.is_passive = (k % 2 == 0);
            rn.power_bonus = 160 + static_cast<int32_t>(k * 20);
            s.catalog.skill_nodes_by_class[c].push_back(std::move(rn));
        }
        for (size_t k = 0; k < 4 && !all_abilities.empty(); ++k) {
            const int64_t ab_guid = all_abilities[(c * 4 + k) % all_abilities.size()];
            const bool is_proj = (k == 0 || k == 2);
            const int32_t heal = (c >= 5 || k == 3) ? 1800 : 0;
            const bool is_buff = (k == 1 || k == 3);
            s.catalog.abilities_by_class[c].push_back(
                {"Ability_" + std::to_string(c) + "_" + std::to_string(k), ab_guid, is_proj ? 3200.0 : 350.0,
                 2400 + static_cast<int32_t>(k * 400), heal, 150, 1500, is_proj, is_buff});
        }
    }
}

inline void populate_catalog_world_and_economy(LiveSession& s, const aoc::worldd::content::ContentDb& db) {
    std::string err;
    if (const auto gm = db.gm_items(err)) {
        for (const auto& row : *gm) {
            if (row.guid != 0 && s.catalog.consumable_item_guids.size() < 16) {
                s.catalog.consumable_item_guids.push_back(row.guid);
            }
        }
    }
    if (const auto gd = db.gather_data(err)) {
        for (const auto& [guid, tool] : gd->tools) {
            if (guid != 0 && s.catalog.consumable_item_guids.size() < 16) s.catalog.consumable_item_guids.push_back(guid);
        }
        size_t idx = 0;
        for (const auto& [guid, def] : gd->defs) {
            if (guid == 0 || idx >= 5) continue;
            const auto kind = static_cast<bots::ResourceKind>((idx % 5) + 1);
            const int64_t yield_guid = !s.catalog.consumable_item_guids.empty()
                                           ? s.catalog.consumable_item_guids[idx % s.catalog.consumable_item_guids.size()]
                                           : guid;
            s.catalog.resource_nodes.push_back({static_cast<uint64_t>(guid), kind, def.name,
                                                {s.spawn[0] + 150.0, s.spawn[1], bots::ground_z_cm(s)},
                                                "aoc-world-riverlands", yield_guid, 4});
            ++idx;
        }
    }
    if (s.catalog.consumable_item_guids.empty() && s.scroll_record_id != 0) {
        s.catalog.consumable_item_guids.push_back(s.scroll_record_id);
    }
    static constexpr const char* kSlots[4] = {"MainHand", "OffHand", "ChestArmor", "GatheringTool"};
    for (size_t i = 0; i < 4 && !s.catalog.consumable_item_guids.empty(); ++i) {
        const int64_t ig = s.catalog.consumable_item_guids[i % s.catalog.consumable_item_guids.size()];
        s.catalog.gear_items.push_back(
            {kSlots[i], std::string(kSlots[i]) + "_Gear", ig, 320, 320, 280, i == 3 ? 40 : 0});
    }
    if (const auto rd = db.respawn_data(err)) {
        for (const auto& [guid, pl] : rd->places) {
            if (guid == 0 || s.catalog.ember_springs.size() >= 6) continue;
            s.catalog.ember_springs.push_back(
                {pl.name, s.catalog.ember_springs.size() == 5 ? "sandsquall" : "riverlands",
                 {pl.x, pl.y, pl.z}, static_cast<uint64_t>(guid)});
        }
    }
    if (const auto md = db.mount_data(err)) {
        for (const auto& m : md->mounts) {
            if (m.item == 0) continue;
            if (s.catalog.ground_mount_item_guid == 0 && m.ground_pct > 0 && !m.mule) {
                s.catalog.ground_mount_item_guid = m.item;
                s.catalog.ground_mount_ability_guid = m.cast_ability != 0 ? m.cast_ability : s.catalog.sprint_ability_guid;
            }
            if (s.catalog.aquatic_mount_item_guid == 0 && m.water_pct > 0) {
                s.catalog.aquatic_mount_item_guid = m.item;
                s.catalog.aquatic_mount_ability_guid = m.cast_ability != 0 ? m.cast_ability : s.catalog.sprint_ability_guid;
            }
            if (s.catalog.flying_mount_item_guid == 0 && m.flying_pct > 0) {
                s.catalog.flying_mount_item_guid = m.item;
                s.catalog.flying_mount_ability_guid = m.cast_ability != 0 ? m.cast_ability : s.catalog.sprint_ability_guid;
            }
            if (s.catalog.pack_mule_item_guid == 0 && (m.mule || m.crate_capacity > 0)) {
                s.catalog.pack_mule_item_guid = m.item;
                s.catalog.pack_mule_ability_guid = m.cast_ability != 0 ? m.cast_ability : s.catalog.sprint_ability_guid;
            }
        }
    }
    if (s.catalog.aquatic_mount_item_guid == 0) s.catalog.aquatic_mount_item_guid = s.catalog.ground_mount_item_guid;
    if (s.catalog.flying_mount_item_guid == 0) s.catalog.flying_mount_item_guid = s.catalog.ground_mount_item_guid;
    if (s.catalog.pack_mule_item_guid == 0) s.catalog.pack_mule_item_guid = s.catalog.ground_mount_item_guid;
}

inline void populate_catalog_quests_and_crafting(LiveSession& s, const aoc::worldd::content::ContentDb& db) {
    std::string err;
    if (const auto dd = db.dialogue_data(err); dd && !dd->nodes.empty()) {
        s.catalog.dialogue_node_guid = dd->nodes.begin()->first;
    }
    if (const auto vd = db.vendor_data(err); vd && !vd->defs.empty()) {
        s.catalog.vendor_def_guid = vd->defs.begin()->first;
    }
    if (const auto wd = db.warehouse_data(err); wd && !wd->nodes.empty()) {
        s.catalog.warehouse_area_guid = wd->nodes.begin()->first;
    }
    if (const auto cd = db.craft_recipe_data(err)) {
        for (const auto& [guid, rec] : cd->recipes) {
            if (guid == 0 || s.catalog.craft_recipe_guids.size() >= 5) continue;
            s.catalog.craft_recipe_guids.push_back(guid);
            const int64_t in_item = !rec.inputs.empty() && rec.inputs[0].item != 0
                                        ? rec.inputs[0].item
                                        : s.catalog.consumable_item_guids[0];
            s.catalog.craft_input_item_guids.push_back(in_item);
            s.catalog.craft_output_item_guids.push_back(rec.reward != 0 ? rec.reward : guid);
        }
    }
    if (const auto qd = db.quest_data(err)) {
        for (const auto& [guid, q] : qd->quests) {
            if (guid != 0 && s.catalog.quest_guids.size() < 4) s.catalog.quest_guids.push_back(guid);
        }
    }
}

inline bool load_dynamic_world_catalog(LiveSession& s) {
    if (s.catalog.loaded) return true;
    std::optional<aoc::worldd::content::ContentDb> db;
    std::string err;
    if (const char* env = std::getenv("AOC_CONTENT_DB"); env && *env) {
        db = aoc::worldd::content::ContentDb::open(env, err);
    }
    for (const char* cand : {"/app/content/content.db", "dist/content/content.db", "deploy/content/content.db"}) {
        if (db) break;
        db = aoc::worldd::content::ContentDb::open(cand, err);
    }
    if (!db) {
        std::fprintf(stderr, "[FAIL] load_dynamic_world_catalog: could not open content.db: %s\n", err.c_str());
        return false;
    }
    populate_catalog_talents_and_abilities(s, *db);
    populate_catalog_world_and_economy(s, *db);
    populate_catalog_quests_and_crafting(s, *db);
    s.catalog.loaded = !s.catalog.ember_springs.empty() && !s.catalog.resource_nodes.empty() &&
                       !s.catalog.gear_items.empty() && !s.catalog.abilities_by_class[0].empty();
    return s.catalog.loaded;
}

inline bool run_stage6_inventory_scrolls(LiveSession& s) {
    if (!load_dynamic_world_catalog(s)) return false;
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

inline bool send_live_equip_storage_event(LiveSession& s, int32_t from_slot, int32_t to_slot) {
    const bots::schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    bots::repl::ItemStorageEvent ev{};
    ev.type = static_cast<uint8_t>(bots::repl::StorageEventType::Move);
    ev.move_source.object = s.backpack_guid;
    ev.move_source.slots = {{from_slot, 1, s.scroll_guid}};
    ev.move_source.status = bots::repl::kStorageNoError;
    ev.targets.push_back({s.equipment_guid.ObjectId != 0 ? s.equipment_guid : s.backpack_guid,
                          {{to_slot, 1, s.scroll_guid}},
                          bots::repl::kStorageNoError});
    if (const bots::schema::RpcSchema* rpc = pc.rpc("ServerRequestStorageEvent")) {
        if (const auto params = bots::repl::storage_event_params(*rpc, ev)) {
            return send_bunches(s, {s.client->actor(3, bots::build_actor_rpc(pc, "ServerRequestStorageEvent", *params), true)});
        }
    }
    return false;
}

inline bool equip_all_preset_items(LiveSession& s, bots::PlayerBot& bot) {
    if (bot.loadout_presets.size() < 4) return false;
    bot.equipped_items.clear();
    recompute_bot_equipment_stats(bot);
    const int32_t unequipped_phys = bot.phys_power;
    const int32_t unequipped_mag = bot.mag_power;
    const int32_t unequipped_armor = bot.armor;
    const int32_t unequipped_gather = bot.gathering_speed_pct;
    if (!send_live_equip_storage_event(s, s.scroll_slot, 0)) return false;
    for (const auto& item : bot.loadout_presets) bot.equipped_items.push_back(item);
    recompute_bot_equipment_stats(bot);
    return ((bot.phys_power > unequipped_phys) || (bot.mag_power > unequipped_mag)) &&
           bot.armor > unequipped_armor && bot.gathering_speed_pct > unequipped_gather;
}

inline bool verify_unequip_and_reequip_cycle(LiveSession& s, bots::PlayerBot& bot) {
    if (bot.equipped_items.empty()) return false;
    const int32_t full_armor = bot.armor;
    const auto saved = bot.equipped_items;
    if (!send_live_equip_storage_event(s, 0, s.scroll_slot)) return false;
    bot.equipped_items.clear();
    recompute_bot_equipment_stats(bot);
    if (bot.armor >= full_armor) return false;
    if (!send_live_equip_storage_event(s, s.scroll_slot, 0)) return false;
    bot.equipped_items = saved;
    recompute_bot_equipment_stats(bot);
    return bot.armor == full_armor && bot.equipped_items.size() >= 4;
}

inline bool run_plan_equipment(LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            return equip_all_preset_items(s, actor) &&
                   verify_unequip_and_reequip_cycle(s, actor) &&
                   bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
