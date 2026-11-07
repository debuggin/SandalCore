// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_vendors.h"

namespace aoc::world::tests::plans {

struct CityNodeWarehouseVault {
    uint64_t owner_bot_id = 0;
    const char* city_node_name = "Winstead";
    int64_t stored_copper = 0;
    int64_t stored_glint = 0;
    std::vector<bots::InventoryItem> stored_items;
};

inline int32_t count_bot_item_qty(const bots::PlayerBot& bot, int64_t item_guid) {
    int32_t total = 0;
    for (const auto& item : bot.inventory) {
        if (item.item_guid == item_guid) total += item.quantity;
    }
    return total;
}

inline int32_t count_vault_item_qty(const CityNodeWarehouseVault& vault, int64_t item_guid) {
    int32_t total = 0;
    for (const auto& item : vault.stored_items) {
        if (item.item_guid == item_guid) total += item.quantity;
    }
    return total;
}

inline bool send_live_warehouse_storage_bunches(bots::LiveSession& s) {
    if (!send_live_equip_storage_event(s, s.scroll_slot, 35)) return false;
    bots::drain_live_udp_nonblocking(s);
    return true;
}

inline bool deposit_storage_item(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    CityNodeWarehouseVault& vault,
    int64_t item_guid,
    int32_t qty) {
    if (qty <= 0 || item_guid == 0) return false;
    if (!send_live_warehouse_storage_bunches(s)) return false;
    for (auto& item : bot.inventory) {
        if (item.item_guid == item_guid && item.quantity >= qty) {
            item.quantity -= qty;
            vault.stored_items.push_back(bots::InventoryItem{item_guid, item.name, qty, item.resource_kind});
            return true;
        }
    }
    return false;
}

inline bool withdraw_storage_item(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    CityNodeWarehouseVault& vault,
    int64_t item_guid,
    int32_t qty) {
    if (qty <= 0 || item_guid == 0) return false;
    if (!send_live_warehouse_storage_bunches(s)) return false;
    for (auto& item : vault.stored_items) {
        if (item.item_guid == item_guid && item.quantity >= qty) {
            item.quantity -= qty;
            bots::add_inventory_item(bot, item_guid, item.name.c_str(), qty, item.resource_kind);
            return true;
        }
    }
    return false;
}

inline bool deposit_storage_currency(bots::PlayerBot& bot, CityNodeWarehouseVault& vault, int64_t copper_amount, int64_t glint_amount) {
    if (copper_amount < 0 || glint_amount < 0 || bot.copper_balance < copper_amount) return false;
    bot.copper_balance -= copper_amount;
    vault.stored_copper += copper_amount;
    vault.stored_glint += glint_amount;
    return true;
}

inline bool withdraw_storage_currency(bots::PlayerBot& bot, CityNodeWarehouseVault& vault, int64_t copper_amount, int64_t glint_amount) {
    if (copper_amount < 0 || glint_amount < 0 || vault.stored_copper < copper_amount || vault.stored_glint < glint_amount) {
        return false;
    }
    vault.stored_copper -= copper_amount;
    vault.stored_glint -= glint_amount;
    bot.copper_balance += copper_amount;
    return true;
}

inline bool run_plan_storage(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16 || s.catalog.resource_nodes.empty()) return false;
    const int64_t timber_guid = s.catalog.resource_nodes[0].yield_item_guid;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            bots::add_inventory_item(actor, timber_guid, "Oak Timber", 10, bots::ResourceKind::Tree);
            CityNodeWarehouseVault vault{actor.bot_id, "Winstead", 0, 0, {}};
            const int32_t initial_qty = count_bot_item_qty(actor, timber_guid);
            if (!deposit_storage_item(s, actor, vault, timber_guid, 6)) return false;
            if (count_bot_item_qty(actor, timber_guid) + count_vault_item_qty(vault, timber_guid) != initial_qty) {
                return false;
            }
            if (!withdraw_storage_item(s, actor, vault, timber_guid, 4)) return false;
            if (count_bot_item_qty(actor, timber_guid) + count_vault_item_qty(vault, timber_guid) != initial_qty) {
                return false;
            }

            const int64_t total_copper = actor.copper_balance;
            if (!deposit_storage_currency(actor, vault, 2500, 15)) return false;
            if (actor.copper_balance + vault.stored_copper != total_copper || vault.stored_glint != 15) return false;
            if (!withdraw_storage_currency(actor, vault, 1500, 10)) return false;
            if (actor.copper_balance + vault.stored_copper != total_copper || vault.stored_copper != 1000 || vault.stored_glint != 5) {
                return false;
            }
            if (!withdraw_storage_currency(actor, vault, 1000, 5)) return false;
            if (actor.copper_balance != total_copper || vault.stored_copper != 0 || vault.stored_glint != 0) return false;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
