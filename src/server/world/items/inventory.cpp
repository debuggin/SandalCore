// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  items/inventory.cpp
//
//  See inventory.h.
// ============================================================================
#include "items/inventory.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aoc::worldd::items {

namespace {

constexpr int64_t kTicksAtUnixEpoch = 621355968000000000ll;   // 1970-01-01 as FDateTime ticks
constexpr int64_t kTicksPerMs = 10000;

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

template <class T>
bool contains(const std::vector<T>& v, T x) { return std::find(v.begin(), v.end(), x) != v.end(); }

int bag_slot_of(int index) { return equip::ResourceBag0 + index; }
int bag_container_of(int equip_slot) { return equip_slot >= equip::ResourceBag0 && equip_slot <= equip::ResourceBag2 ? equip_slot - equip::ResourceBag0 : -1; }

}  // namespace

/// What is wrong with an item, "" when nothing: the constraints of the character service's table.
std::string item_problem(const svc::InventoryItem& it) {
    if (!valid_container(it.container)) return "its storage is none of the Backpack, the Equipment, the Quest storage and the three resource containers";
    if (it.slot < 0 || it.slot >= slot_count(it.container)) {
        return "slot " + std::to_string(it.slot) + " is outside the " + container_name(it.container) + " (0 to " +
               std::to_string(slot_count(it.container) - 1) + ")";
    }
    if (it.item_record_id <= 0) return "record " + std::to_string(it.item_record_id) + " is none";
    if (const auto g = guid_of(it.instance_guid); !g || guid_hex(*g) != it.instance_guid) {
        return "instance guid '" + it.instance_guid + "' is not 32 lowercase hex digits (or is the zero guid)";
    }
    if (it.quantity < 1 || it.quantity > kMaxQuantity) return "quantity " + std::to_string(it.quantity) + " is not 1 to " + std::to_string(kMaxQuantity);
    if (it.quality < kMinQuality || it.quality > kMaxQuality) return "quality " + std::to_string(it.quality) + " is not " + std::to_string(kMinQuality) + " to " + std::to_string(kMaxQuality);
    if (!std::isfinite(it.durability) || it.durability < 0) return "durability is not a number from 0";
    if (it.created_at_ms < 0 || it.created_at_ms > kMaxMs) return "creation time " + std::to_string(it.created_at_ms) + " ms is out of range";
    for (const std::optional<float>* p : {&it.persisted_health, &it.persisted_mana}) {
        if (*p && (!std::isfinite(**p) || **p < 0 || **p > 1e9f)) return "a persisted Health or Mana is not a number from 0 to 1e9";
    }
    if (it.lock_expiration < 0 || it.lock_expiration > kMaxLockExpiration) return "lock expiration " + std::to_string(it.lock_expiration) + " is out of range";
    return {};
}

int slot_count(svc::ItemContainer c) {
    switch (c) {
        case svc::ItemContainer::Backpack: return kBackpackSlots;
        case svc::ItemContainer::Equipment: return kEquipmentSlots;
        case svc::ItemContainer::Quest: return kQuestSlots;
        case svc::ItemContainer::Resource0:
        case svc::ItemContainer::Resource1:
        case svc::ItemContainer::Resource2: return kMaxResourceCells;
    }
    return 0;
}

const char* container_name(svc::ItemContainer c) {
    switch (c) {
        case svc::ItemContainer::Backpack: return "Backpack";
        case svc::ItemContainer::Equipment: return "Equipment";
        case svc::ItemContainer::Quest: return "Quest storage";
        case svc::ItemContainer::Resource0: return "Resource0";
        case svc::ItemContainer::Resource1: return "Resource1";
        case svc::ItemContainer::Resource2: return "Resource2";
    }
    return "?";
}

// ---- items as the wire carries them --------------------------------------------------------------------------------------

std::optional<repl::GuidValue> guid_of(const std::string& hex) {
    if (hex.size() != 32) return std::nullopt;
    uint32_t w[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 32; ++i) {
        const int d = hex_digit(hex[i]);
        if (d < 0) return std::nullopt;
        w[i / 8] = (w[i / 8] << 4) | static_cast<uint32_t>(d);
    }
    if (!(w[0] | w[1] | w[2] | w[3])) return std::nullopt;
    return repl::GuidValue{w[0], w[1], w[2], w[3]};
}

std::string guid_hex(const repl::GuidValue& g) {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", g.a, g.b, g.c, g.d);
    return buf;
}

int64_t ticks_of_ms(int64_t unix_ms) { return unix_ms * kTicksPerMs + kTicksAtUnixEpoch; }
int64_t ms_of_ticks(int64_t ticks) { return (ticks - kTicksAtUnixEpoch) / kTicksPerMs; }

repl::ItemInstance to_instance(const svc::InventoryItem& item) {
    repl::ItemInstance i;
    i.has_item = true;
    i.record_id = static_cast<uint64_t>(item.item_record_id);
    i.quantity = static_cast<uint32_t>(item.quantity);
    i.quality = static_cast<uint32_t>(item.quality);
    i.durability = item.durability;
    if (const auto g = guid_of(item.instance_guid)) i.guid = *g;
    i.creation_time = ticks_of_ms(item.created_at_ms);
    // what a mount item persists, in the order retail's login has it (Health, then Mana)
    if (item.persisted_health) i.stats.push_back({kPersistedHealthStat, *item.persisted_health});
    if (item.persisted_mana) i.stats.push_back({kPersistedManaStat, *item.persisted_mana});
    i.lock_expiration = item.lock_expiration;
    return i;
}

std::optional<svc::InventoryItem> item_of(const repl::ItemInstance& instance, svc::ItemContainer container, int slot) {
    if (!instance.has_item) return std::nullopt;
    svc::InventoryItem it;
    it.container = container;
    it.slot = slot;
    it.item_record_id = static_cast<int64_t>(instance.record_id);
    it.instance_guid = guid_hex(instance.guid);
    it.quantity = static_cast<int32_t>(instance.quantity);
    it.quality = static_cast<int32_t>(instance.quality);
    it.durability = instance.durability;
    it.created_at_ms = ms_of_ticks(instance.creation_time);
    for (const repl::PersistedStat& s : instance.stats) {
        if (s.stat_id == kPersistedHealthStat) it.persisted_health = s.value;
        if (s.stat_id == kPersistedManaStat) it.persisted_mana = s.value;
    }
    it.lock_expiration = instance.lock_expiration;
    if (!item_problem(it).empty()) return std::nullopt;
    return it;
}

// ---- the catalog ---------------------------------------------------------------------------------------------------------

std::optional<ItemInfo> ContentItemCatalog::info(int64_t record) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto kept = kept_.find(record); kept != kept_.end()) return kept->second;
    }
    std::string error;
    const auto rec = db_.item(record, error);
    if (!rec) {
        if (!error.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!failure_logged_) {
                failure_logged_ = true;
                spdlog::error("[items] {}: items are read as unknown from now on (further failures are not logged)", error);
            }
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        kept_[record] = std::nullopt;
        return std::nullopt;
    }
    ItemInfo i;
    i.equip_slots = rec->equip_slots;
    i.two_handed = rec->occupies_all_equip_slots;
    i.resource = rec->resource;
    i.inv_w = rec->inv_w;
    i.inv_h = rec->inv_h;
    i.max_stack = std::max(1, rec->max_stack_size);
    if (std::any_of(i.equip_slots.begin(), i.equip_slots.end(), [](uint8_t s) { return bag_container_of(s) >= 0; })) {
        std::string bag_error;
        if (const auto bag = db_.resource_bag(record, bag_error)) {
            std::vector<std::vector<int>> matrix;
            for (const auto& column : bag->matrix) matrix.emplace_back(column.begin(), column.end());
            i.bag = ResourceGrid(bag->columns, bag->rows, bag->custom, std::move(matrix));
        } else if (!bag_error.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!failure_logged_) {
                failure_logged_ = true;
                spdlog::error("[items] {}: a resource bag is read as having no grid from now on (further failures are not logged)", bag_error);
            }
            return i;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    kept_[record] = i;
    return i;
}

std::optional<Inventory> Inventory::from_items(const std::vector<svc::InventoryItem>& items, std::string& error, const ItemCatalog* catalog) {
    Inventory inv;
    if (items.size() > static_cast<size_t>(kMaxItems)) {
        error = std::to_string(items.size()) + " items, a character holds at most " + std::to_string(kMaxItems);
        return std::nullopt;
    }
    std::map<std::string, int> guids;
    for (size_t n = 0; n < items.size(); ++n) {
        const svc::InventoryItem& it = items[n];
        if (const std::string why = item_problem(it); !why.empty()) {
            error = "item " + std::to_string(n) + ": " + why;
            return std::nullopt;
        }
        const Key key{static_cast<uint8_t>(it.container), it.slot};
        if (!inv.slots_.emplace(key, it).second) {
            error = "item " + std::to_string(n) + ": two items in " + container_name(it.container) + " slot " + std::to_string(it.slot);
            return std::nullopt;
        }
        if (!guids.emplace(it.instance_guid, static_cast<int>(n)).second) {
            error = "item " + std::to_string(n) + ": instance guid " + it.instance_guid + " is the guid of item " + std::to_string(guids[it.instance_guid]);
            return std::nullopt;
        }
    }
    if (!catalog) return inv;
    for (int i = 0; i < kResourceContainers; ++i) {
        const svc::InventoryItem* bag = inv.find(svc::ItemContainer::Equipment, bag_slot_of(i));
        if (!bag) continue;
        const auto info = catalog->info(bag->item_record_id);
        if (info && info->bag) inv.grids_[static_cast<size_t>(i)] = *info->bag;
    }
    std::array<std::vector<bool>, kResourceContainers> covered;
    for (const auto& [key, it] : inv.slots_) {
        if (!is_resource(it.container)) continue;
        const size_t i = static_cast<size_t>(resource_index(it.container));
        const ResourceGrid& grid = inv.grids_[i];
        const std::string where = std::string(container_name(it.container)) + " slot " + std::to_string(it.slot) + " (record " + std::to_string(it.item_record_id) + ")";
        if (grid.empty()) {
            error = where + ": the character wears no resource bag in Equipment slot " + std::to_string(bag_slot_of(static_cast<int>(i))) + ", the container has no cells";
            return std::nullopt;
        }
        const auto info = catalog->info(it.item_record_id);
        const int w = info ? info->inv_w : 1, h = info ? info->inv_h : 1;
        if (!grid.fits(it.slot, w, h, covered[i])) {
            error = where + ": a " + std::to_string(w) + " x " + std::to_string(h) + " item does not fit the " + std::to_string(grid.columns()) + " x " +
                    std::to_string(grid.rows()) + " grid there (outside it, on a blocked cell or on another item)";
            return std::nullopt;
        }
        grid.cover(covered[i], it.slot, w, h);
    }
    return inv;
}

const ResourceGrid& Inventory::grid(svc::ItemContainer container) const {
    static const ResourceGrid kNone;
    return is_resource(container) ? grids_[static_cast<size_t>(resource_index(container))] : kNone;
}

std::vector<bool> Inventory::covered_cells(svc::ItemContainer container, const ItemCatalog& catalog, const std::string* ignore) const {
    std::vector<bool> covered;
    const ResourceGrid& g = grid(container);
    if (g.empty()) return covered;
    covered.assign(static_cast<size_t>(g.cells()), false);
    for (auto it = slots_.lower_bound({static_cast<uint8_t>(container), 0}); it != slots_.end() && it->first.first == static_cast<uint8_t>(container); ++it) {
        if (ignore && it->second.instance_guid == *ignore) continue;
        const auto info = catalog.info(it->second.item_record_id);
        g.cover(covered, it->second.slot, info ? info->inv_w : 1, info ? info->inv_h : 1);
    }
    return covered;
}

int Inventory::place(svc::ItemContainer container, int w, int h, const ItemCatalog& catalog) const {
    const ResourceGrid& g = grid(container);
    if (g.empty()) return -1;
    return g.place(w, h, covered_cells(container, catalog));
}

const svc::InventoryItem* Inventory::find(svc::ItemContainer c, int slot) const {
    const auto it = slots_.find({static_cast<uint8_t>(c), slot});
    return it == slots_.end() ? nullptr : &it->second;
}

const svc::InventoryItem* Inventory::at(svc::ItemContainer container, int slot) const {
    if (!valid_container(container) || slot < 0 || slot >= slot_count(container)) return nullptr;
    return find(container, slot);
}

size_t Inventory::count(svc::ItemContainer container) const {
    size_t n = 0;
    for (const auto& [key, item] : slots_) n += key.first == static_cast<uint8_t>(container) ? 1 : 0;
    return n;
}

int Inventory::first_free(svc::ItemContainer container) const {
    if (is_resource(container)) return -1;
    for (int s = 0; s < slot_count(container); ++s) {
        if (!find(container, s)) return s;
    }
    return -1;
}

std::vector<svc::InventoryItem> Inventory::items() const {
    std::vector<svc::InventoryItem> out;
    out.reserve(slots_.size());
    for (const auto& [key, item] : slots_) out.push_back(item);   // the map orders by storage (Backpack, Equipment, Quest) and slot
    return out;
}

std::vector<int64_t> Inventory::records() const {
    std::vector<int64_t> out;
    out.reserve(slots_.size());
    for (const auto& [key, item] : slots_) out.push_back(item.item_record_id);
    return out;
}

Outcome Inventory::use(svc::ItemContainer storage, int slot, const std::string& expected_guid, uint32_t requested_equip_slot,
                       const ItemCatalog& catalog) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.events.clear();
        return out;
    };
    if (storage != svc::ItemContainer::Backpack && storage != svc::ItemContainer::Equipment) {
        return refuse(std::string("items of the ") + (valid_container(storage) ? container_name(storage) : "unknown storage") + " are not used");
    }
    const svc::InventoryItem* item = at(storage, slot);
    if (!item) return refuse(std::string("no item in ") + container_name(storage) + " slot " + std::to_string(slot));
    if (!guid_of(expected_guid) || item->instance_guid != expected_guid) {
        return refuse(std::string("the item in ") + container_name(storage) + " slot " + std::to_string(slot) +
                      " is not the instance the client named (" + expected_guid + ")");
    }
    return storage == svc::ItemContainer::Backpack ? equip(slot, catalog, requested_equip_slot) : unequip(slot);
}

namespace {

using InfoCache = std::map<int64_t, std::optional<ItemInfo>>;

const std::optional<ItemInfo>& cached_info(InfoCache& cache, const ItemCatalog& catalog, int64_t record) {
    auto it = cache.find(record);
    if (it == cache.end()) it = cache.emplace(record, catalog.info(record)).first;
    return it->second;
}

bool is_equip_slot_covered(const Inventory& inv, int s, InfoCache& cache, const ItemCatalog& catalog) {
    if (inv.at(svc::ItemContainer::Equipment, s)) return true;
    for (int e = 0; e < kEquipmentSlots; ++e) {
        const svc::InventoryItem* worn = inv.at(svc::ItemContainer::Equipment, e);
        if (!worn) continue;
        const auto& w = cached_info(cache, catalog, worn->item_record_id);
        if (w && w->two_handed && contains(w->equip_slots, static_cast<uint8_t>(s))) return true;
    }
    return false;
}

int pick_equip_target(const Inventory& inv, const ItemInfo& info, InfoCache& cache, const ItemCatalog& catalog, uint32_t requested) {
    if (requested != 0xFFFFFFFFu) {
        if (requested > 255 || !contains(info.equip_slots, static_cast<uint8_t>(requested))) return -1;
        return static_cast<int>(requested);
    }
    if (info.two_handed) return info.equip_slots.front();
    for (const uint8_t s : info.equip_slots) {
        if (!is_equip_slot_covered(inv, s, cache, catalog)) return s;
    }
    return info.equip_slots.front();
}

std::vector<int> collect_displaced_equip(const Inventory& inv, const ItemInfo& info, InfoCache& cache, const ItemCatalog& catalog, int target) {
    std::vector<int> footprint;
    if (info.two_handed) footprint.assign(info.equip_slots.begin(), info.equip_slots.end());
    else footprint.push_back(target);
    std::vector<int> displaced;
    for (const int s : footprint) {
        if (inv.at(svc::ItemContainer::Equipment, s) && !contains(displaced, s)) displaced.push_back(s);
    }
    for (int e = 0; e < kEquipmentSlots; ++e) {
        const svc::InventoryItem* worn = inv.at(svc::ItemContainer::Equipment, e);
        if (!worn || contains(displaced, e)) continue;
        const auto& w = cached_info(cache, catalog, worn->item_record_id);
        if (!w || !w->two_handed) continue;
        for (const uint8_t s : w->equip_slots) {
            if (contains(footprint, static_cast<int>(s))) {
                displaced.push_back(e);
                break;
            }
        }
    }
    return displaced;
}

bool validate_displaced_equip(const Inventory& inv, const std::vector<int>& displaced, Outcome& out) {
    for (const int d : displaced) {
        const int bag = bag_container_of(d);
        if (bag >= 0 && inv.count(resource_container(bag)) > 0) {
            out.holds_items = true;
            out.refusal = std::string("the resource bag in Equipment slot ") + std::to_string(d) + " holds " +
                          std::to_string(inv.count(resource_container(bag))) + " item stack(s) in " +
                          container_name(resource_container(bag)) + ": it is emptied before it is replaced";
            return false;
        }
    }
    const size_t free_after = static_cast<size_t>(kBackpackSlots) - inv.count(svc::ItemContainer::Backpack) + 1;
    if (displaced.size() > free_after) {
        out.refusal = "the bag has " + std::to_string(free_after) + " free slot(s) and wearing the item takes off " +
                      std::to_string(displaced.size()) + " item(s)";
        return false;
    }
    return true;
}

}  // namespace

Outcome Inventory::equip(int bag_slot, const ItemCatalog& catalog, uint32_t requested) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.events.clear();
        return out;
    };
    const svc::InventoryItem item = *find(svc::ItemContainer::Backpack, bag_slot);
    InfoCache cache;
    const std::optional<ItemInfo>& info = cached_info(cache, catalog, item.item_record_id);
    if (!info) return refuse("item record " + std::to_string(item.item_record_id) + " is not in content.db");
    if (!info->wearable()) return refuse("item record " + std::to_string(item.item_record_id) + " is not worn in any slot");

    const int target = pick_equip_target(*this, *info, cache, catalog, requested);
    if (target < 0) return refuse("slot " + std::to_string(requested) + " is not one of the item's slots");
    const std::vector<int> displaced = collect_displaced_equip(*this, *info, cache, catalog, target);
    if (!validate_displaced_equip(*this, displaced, out)) return out;

    const auto remove = [&](svc::ItemContainer c, int s) {
        Event e{repl::StorageEventType::Remove, c, s, *find(c, s), 0, false};
        slots_.erase({static_cast<uint8_t>(c), s});
        out.events.push_back(std::move(e));
    };
    const auto add = [&](svc::ItemContainer c, int s, svc::InventoryItem it) {
        it.container = c;
        it.slot = s;
        Event e{repl::StorageEventType::Add, c, s, it};
        slots_[{static_cast<uint8_t>(c), s}] = std::move(it);
        out.events.push_back(std::move(e));
    };
    remove(svc::ItemContainer::Backpack, bag_slot);
    for (const int d : displaced) {
        const svc::InventoryItem moved = *find(svc::ItemContainer::Equipment, d);
        add(svc::ItemContainer::Backpack, first_free(svc::ItemContainer::Backpack), moved);
        remove(svc::ItemContainer::Equipment, d);
    }
    add(svc::ItemContainer::Equipment, target, item);
    for (const int d : displaced) {
        if (const int bag = bag_container_of(d); bag >= 0) grids_[static_cast<size_t>(bag)] = ResourceGrid{};
    }
    if (const int bag = bag_container_of(target); bag >= 0) {
        const bool opens = info->bag.has_value();
        grids_[static_cast<size_t>(bag)] = opens ? *info->bag : ResourceGrid{};
        out.bags.push_back({bag, opens});
    }
    for (const int d : displaced) {
        const int bag = bag_container_of(d);
        if (bag >= 0 && bag != bag_container_of(target)) out.bags.push_back({bag, false});
    }
    out.ok = true;
    out.equipment_changed = true;
    return out;
}

Outcome Inventory::unequip(int equip_slot) {
    Outcome out;
    const svc::InventoryItem item = *find(svc::ItemContainer::Equipment, equip_slot);
    const int worn_bag = bag_container_of(equip_slot);
    if (worn_bag >= 0 && count(resource_container(worn_bag)) > 0) {
        out.holds_items = true;
        out.refusal = std::string("the resource bag in Equipment slot ") + std::to_string(equip_slot) + " holds " + std::to_string(count(resource_container(worn_bag))) +
                      " item stack(s) in " + container_name(resource_container(worn_bag)) + ": it is emptied before it is taken off";
        return out;
    }
    const int bag = first_free(svc::ItemContainer::Backpack);
    if (bag < 0) {
        out.refusal = "the bag is full";
        return out;
    }
    svc::InventoryItem moved = item;
    moved.container = svc::ItemContainer::Backpack;
    moved.slot = bag;
    Event add{repl::StorageEventType::Add, svc::ItemContainer::Backpack, bag, moved};
    Event remove{repl::StorageEventType::Remove, svc::ItemContainer::Equipment, equip_slot, item, 0, false};
    slots_.erase({static_cast<uint8_t>(svc::ItemContainer::Equipment), equip_slot});
    slots_[{static_cast<uint8_t>(svc::ItemContainer::Backpack), bag}] = moved;
    out.events = {std::move(add), std::move(remove)};
    if (worn_bag >= 0) {
        grids_[static_cast<size_t>(worn_bag)] = ResourceGrid{};
        out.bags.push_back({worn_bag, false});
    }
    out.ok = true;
    out.equipment_changed = true;
    return out;
}

// ---- the visible gear ----------------------------------------------------------------------------------------------------

bool shows_in_appearance(int slot) { return (slot >= 1 && slot <= 9) || slot == equip::CargoHold || slot == equip::Costume || slot == equip::CloakCosmetic; }

std::array<int64_t, 23> held_records(const Inventory& inventory) {
    std::array<int64_t, 23> out{};
    for (size_t i = 0; i < kHeldSlots.size(); ++i) {
        if (const svc::InventoryItem* it = inventory.at(svc::ItemContainer::Equipment, kHeldSlots[i])) out[i] = it->item_record_id;
    }
    return out;
}

std::vector<repl::AppearanceId> appearance_ids(const Inventory& inventory) {
    std::vector<repl::AppearanceId> out;
    for (int s = 0; s < kEquipmentSlots; ++s) {
        if (!shows_in_appearance(s)) continue;
        const svc::InventoryItem* it = inventory.at(svc::ItemContainer::Equipment, s);
        if (!it) continue;
        repl::AppearanceId id;
        id.record_guid = static_cast<uint64_t>(it->item_record_id);
        id.type_id = kItemRecordTypeId;
        id.enchantment_level = 0;
        id.item_equip_slot = static_cast<uint8_t>(s);
        out.push_back(id);
    }
    return out;
}

}  // namespace aoc::worldd::items
