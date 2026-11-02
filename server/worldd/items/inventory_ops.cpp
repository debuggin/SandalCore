// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "items/inventory.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace aoc::worldd::items {

namespace {

constexpr int32_t kMaxQuantity = 1000000;
constexpr int64_t kMaxMs = 9'000'000'000'000ll;
constexpr int64_t kMaxLockExpiration = 253402300799ll;

bool valid_container(svc::ItemContainer c) { return static_cast<uint8_t>(c) <= static_cast<uint8_t>(svc::ItemContainer::Resource2); }

bool is_craft_source(svc::ItemContainer c) { return is_resource(c) || c == svc::ItemContainer::Backpack; }

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

Event make_storage_event(repl::StorageEventType type, svc::ItemContainer c, int slot, const svc::InventoryItem& item, bool instigated = true) {
    Event e;
    e.type = type;
    e.container = c;
    e.slot = slot;
    e.item = item;
    e.instigated = instigated;
    return e;
}

std::string validate_tier_up_inputs(const Inventory& inv, svc::ItemContainer storage, int slot, const std::string& guid, int32_t full) {
    if (!is_resource(storage) || !valid_container(storage)) {
        return std::string("the ") + (valid_container(storage) ? container_name(storage) : "unknown storage") + " is no material bag";
    }
    const svc::InventoryItem* item = inv.at(storage, slot);
    if (!item) return std::string("no item in ") + container_name(storage) + " slot " + std::to_string(slot);
    if (!guid_of(guid) || item->instance_guid != guid) {
        return std::string("the item in ") + container_name(storage) + " slot " + std::to_string(slot) + " is not the instance named (" + guid + ")";
    }
    if (full < 1 || item->quantity != full) {
        return "the stack in " + std::string(container_name(storage)) + " slot " + std::to_string(slot) + " holds " + std::to_string(item->quantity) +
               ", a full stack is " + std::to_string(full);
    }
    return {};
}

}  // namespace

int64_t Inventory::count_of(int64_t record) const {
    int64_t n = 0;
    for (const auto& [key, it] : slots_) {
        const svc::ItemContainer c = static_cast<svc::ItemContainer>(key.first);
        if (it.item_record_id == record && (c == svc::ItemContainer::Backpack || is_resource(c))) n += it.quantity;
    }
    return n;
}

Outcome Inventory::take(int64_t record, int64_t count) {
    Outcome out;
    if (count < 1) {
        out.refusal = "nothing to take: the count is " + std::to_string(count);
        return out;
    }
    const int64_t held = count_of(record);
    if (held < count) {
        out.refusal = "the character holds " + std::to_string(held) + " of item " + std::to_string(record) + ", not " + std::to_string(count);
        return out;
    }
    std::vector<std::pair<Key, int32_t>> plan;
    int64_t left = count;
    for (const auto& [key, it] : slots_) {
        const svc::ItemContainer c = static_cast<svc::ItemContainer>(key.first);
        if (it.item_record_id != record || (c != svc::ItemContainer::Backpack && !is_resource(c))) continue;
        const int32_t give = static_cast<int32_t>(std::min<int64_t>(left, it.quantity));
        plan.push_back({key, give});
        left -= give;
        if (left == 0) break;
    }
    for (const auto& [key, give] : plan) {
        const svc::InventoryItem before = slots_.at(key);
        Event e = make_storage_event(repl::StorageEventType::Remove, static_cast<svc::ItemContainer>(key.first), key.second, before, false);
        e.count = give < before.quantity ? give : 0;
        if (give < before.quantity) slots_[key].quantity = before.quantity - give;
        else slots_.erase(key);
        out.events.push_back(std::move(e));
    }
    out.ok = true;
    return out;
}

int64_t Inventory::count_materials(int64_t record, const ItemRarityFacts& facts, int rarity_lo, int rarity_hi) const {
    int64_t held = 0;
    for (const auto& [key, it] : slots_) {
        if (it.item_record_id != record || !is_craft_source(static_cast<svc::ItemContainer>(key.first))) continue;
        const int rarity = facts.rarity_of(static_cast<double>(it.quality));
        if (rarity >= rarity_lo && rarity <= rarity_hi) held += it.quantity;
    }
    return held;
}

std::array<int64_t, kRarityCount> Inventory::materials_by_rarity(int64_t record, const ItemRarityFacts& facts) const {
    std::array<int64_t, kRarityCount> held{};
    for (const auto& [key, it] : slots_) {
        if (it.item_record_id != record || !is_craft_source(static_cast<svc::ItemContainer>(key.first))) continue;
        held[static_cast<size_t>(std::clamp(facts.rarity_of(static_cast<double>(it.quality)), 0, kRarityCount - 1))] += it.quantity;
    }
    return held;
}

Outcome Inventory::take_materials(int64_t record, const ItemRarityFacts& facts, int rarity_lo, int rarity_hi, int64_t count, bool lowest_first) {
    Outcome out;
    if (count < 1) {
        out.refusal = "nothing to take: the count is " + std::to_string(count);
        return out;
    }
    const int64_t held = count_materials(record, facts, rarity_lo, rarity_hi);
    if (held < count) {
        out.refusal = "the material bags and the item bag hold " + std::to_string(held) + " of item " + std::to_string(record) + " of rarity " + rarity_range_text(rarity_lo, rarity_hi) + ", not " + std::to_string(count);
        return out;
    }
    std::vector<Key> order;
    for (const auto& [key, it] : slots_) {
        if (it.item_record_id != record || !is_craft_source(static_cast<svc::ItemContainer>(key.first))) continue;
        const int rarity = facts.rarity_of(static_cast<double>(it.quality));
        if (rarity >= rarity_lo && rarity <= rarity_hi) order.push_back(key);
    }
    if (lowest_first) {
        std::stable_sort(order.begin(), order.end(), [&](const Key& a, const Key& b) { return slots_.at(a).quality < slots_.at(b).quality; });
    }
    std::vector<std::pair<Key, int32_t>> plan;
    int64_t left = count;
    for (const Key& key : order) {
        const int32_t give = static_cast<int32_t>(std::min<int64_t>(left, slots_.at(key).quantity));
        plan.push_back({key, give});
        left -= give;
        if (left == 0) break;
    }
    for (const auto& [key, give] : plan) {
        const svc::InventoryItem before = slots_.at(key);
        Event e = make_storage_event(repl::StorageEventType::Remove, static_cast<svc::ItemContainer>(key.first), key.second, before, false);
        e.count = give < before.quantity ? give : 0;
        if (give < before.quantity) slots_[key].quantity = before.quantity - give;
        else slots_.erase(key);
        out.events.push_back(std::move(e));
    }
    out.ok = true;
    return out;
}

Outcome Inventory::consume(svc::ItemContainer storage, int slot, const std::string& guid, int32_t count) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.events.clear();
        return out;
    };
    const svc::InventoryItem* item = at(storage, slot);
    if (!item) return refuse(std::string("no item in ") + container_name(storage) + " slot " + std::to_string(slot));
    if (!guid_of(guid) || item->instance_guid != guid) {
        return refuse(std::string("the item in ") + container_name(storage) + " slot " + std::to_string(slot) + " is not the instance named (" + guid + ")");
    }
    if (count < 1 || count > item->quantity) {
        return refuse("the stack in " + std::string(container_name(storage)) + " slot " + std::to_string(slot) + " holds " + std::to_string(item->quantity) +
                      ", not " + std::to_string(count) + " to consume");
    }
    const svc::InventoryItem before = *item;
    out.events.push_back(make_storage_event(repl::StorageEventType::Remove, storage, slot, before, true));
    slots_.erase({static_cast<uint8_t>(storage), slot});
    if (before.quantity > count) {
        svc::InventoryItem left = before;
        left.quantity = before.quantity - count;
        slots_[{static_cast<uint8_t>(storage), slot}] = left;
        out.events.push_back(make_storage_event(repl::StorageEventType::Add, storage, slot, left, true));
    }
    out.ok = true;
    out.equipment_changed = storage == svc::ItemContainer::Equipment;
    return out;
}

Outcome Inventory::tier_up(svc::ItemContainer storage, int slot, const std::string& guid, int32_t full, svc::InventoryItem product, const ItemRarityFacts& facts, const ItemCatalog& catalog) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.events.clear();
        return out;
    };
    if (const std::string why = validate_tier_up_inputs(*this, storage, slot, guid, full); !why.empty()) return refuse(why);
    const auto info = catalog.info(product.item_record_id);
    if (!info || !info->resource) return refuse("item record " + std::to_string(product.item_record_id) + " is not an item of the material bags");
    product.container = storage;
    product.slot = 0;
    if (const std::string why = item_problem(product); !why.empty()) return refuse("the new item: " + why);
    if (find_guid(product.instance_guid)) return refuse("the new item's instance guid is one the character has");

    Inventory trial = *this;
    out.events.push_back(make_storage_event(repl::StorageEventType::Remove, storage, slot, *at(storage, slot), true));
    trial.slots_.erase({static_cast<uint8_t>(storage), slot});

    const int product_rarity = facts.rarity_of(static_cast<double>(product.quality));
    const int32_t max_stack = std::max(1, info->max_stack);
    bool placed = false;
    for (auto& [key, held] : trial.slots_) {
        const svc::ItemContainer c = static_cast<svc::ItemContainer>(key.first);
        if (!is_resource(c) || trial.grid(c).empty()) continue;
        if (held.item_record_id != product.item_record_id || held.quantity + product.quantity > max_stack || held.durability != product.durability ||
            facts.rarity_of(static_cast<double>(held.quality)) != product_rarity) {
            continue;
        }
        held.quantity += product.quantity;
        product.container = c;
        product.slot = held.slot;
        placed = true;
        break;
    }
    for (int i = 0; i < kResourceContainers && !placed; ++i) {
        const svc::ItemContainer c = resource_container(i);
        const int where = trial.place(c, info->inv_w, info->inv_h, catalog);
        if (where < 0) continue;
        product.container = c;
        product.slot = where;
        trial.slots_[{static_cast<uint8_t>(c), where}] = product;
        placed = true;
    }
    if (!placed) return refuse("the material bags have no place for the next kind (" + std::to_string(info->inv_w) + " x " + std::to_string(info->inv_h) + " cells)");
    out.events.push_back(make_storage_event(repl::StorageEventType::Add, product.container, product.slot, product, true));
    *this = std::move(trial);
    out.ok = true;
    return out;
}

const svc::InventoryItem* Inventory::find_guid(const std::string& guid) const {
    for (const auto& [key, item] : slots_) {
        if (item.instance_guid == guid) return &item;
    }
    return nullptr;
}

bool Inventory::set_durability(svc::ItemContainer container, int slot, const std::string& guid, float durability) {
    if (!(durability >= 0.0f) || !std::isfinite(durability) || !valid_container(container) || slot < 0 || slot >= slot_count(container)) return false;
    const auto it = slots_.find({static_cast<uint8_t>(container), slot});
    if (it == slots_.end() || it->second.instance_guid != guid) return false;
    it->second.durability = durability;
    return true;
}

Outcome Inventory::update_mount(const std::string& guid, std::optional<float> health, int64_t lock_expiration, bool events) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.ok = false;
        out.refusal = std::move(why);
        out.events.clear();
        return out;
    };
    if (health && (!std::isfinite(*health) || *health < 0 || *health > 1e9f)) return refuse("a Health that is not a number from 0 to 1e9");
    if (lock_expiration > kMaxLockExpiration) return refuse("lock expiration " + std::to_string(lock_expiration) + " is out of range");
    for (auto& [key, item] : slots_) {
        if (item.instance_guid != guid) continue;
        const svc::InventoryItem before = item;
        if (health) item.persisted_health = *health;
        if (lock_expiration >= 0) item.lock_expiration = lock_expiration;
        out.ok = true;
        out.equipment_changed = false;
        if (events && !(before == item)) {
            out.events.push_back(make_storage_event(repl::StorageEventType::Remove, item.container, item.slot, before, true));
            out.events.push_back(make_storage_event(repl::StorageEventType::Add, item.container, item.slot, item, true));
        }
        return out;
    }
    return refuse("no item holds the instance " + guid);
}

GrantResult Inventory::grant_what_fits(std::vector<svc::InventoryItem> items, const ItemCatalog& catalog) {
    GrantResult res;
    const auto refuse = [&](std::string why) {
        res.outcome.ok = false;
        res.outcome.refusal = std::move(why);
        res.outcome.events.clear();
        res.rest = std::move(items);
        return std::move(res);
    };
    if (items.empty()) return refuse("nothing to grant");
    std::set<std::string> guids;
    for (const auto& [key, held] : slots_) guids.insert(held.instance_guid);
    for (size_t n = 0; n < items.size(); ++n) {
        svc::InventoryItem probe = items[n];
        probe.container = svc::ItemContainer::Backpack;
        probe.slot = 0;
        if (const std::string why = item_problem(probe); !why.empty()) return refuse("granted item " + std::to_string(n) + ": " + why);
        if (!guids.insert(probe.instance_guid).second) return refuse("granted item " + std::to_string(n) + ": its instance guid is one the character has");
    }
    res.outcome.ok = true;
    for (svc::InventoryItem& it : items) {
        const auto info = catalog.info(it.item_record_id);
        if (info && info->resource) {
            grant_resource(std::move(it), *info, catalog, res);
            continue;
        }
        const int slot = first_free(svc::ItemContainer::Backpack);
        if (slot < 0) {
            res.rest.push_back(std::move(it));
            continue;
        }
        it.container = svc::ItemContainer::Backpack;
        it.slot = slot;
        slots_[{static_cast<uint8_t>(it.container), it.slot}] = it;
        res.outcome.events.push_back(make_storage_event(repl::StorageEventType::Add, it.container, it.slot, it, true));
    }
    return res;
}

void Inventory::grant_resource(svc::InventoryItem item, const ItemInfo& info, const ItemCatalog& catalog, GrantResult& out) {
    const int32_t max_stack = std::max(1, info.max_stack);
    for (auto& [key, held] : slots_) {
        if (item.quantity <= 0) break;
        const svc::ItemContainer c = static_cast<svc::ItemContainer>(key.first);
        if (!is_resource(c) || grid(c).empty()) continue;
        if (held.item_record_id != item.item_record_id || held.quality != item.quality || held.quantity >= max_stack) continue;
        const int32_t take = std::min(item.quantity, max_stack - held.quantity);
        out.outcome.events.push_back(make_storage_event(repl::StorageEventType::Remove, c, held.slot, held, true));
        held.quantity += take;
        out.outcome.events.push_back(make_storage_event(repl::StorageEventType::Add, c, held.slot, held, true));
        item.quantity -= take;
    }
    if (item.quantity <= 0) return;
    const int32_t piece = std::min(item.quantity, max_stack);
    for (int i = 0; i < kResourceContainers; ++i) {
        const svc::ItemContainer c = resource_container(i);
        const int slot = place(c, info.inv_w, info.inv_h, catalog);
        if (slot < 0) continue;
        svc::InventoryItem placed = item;
        placed.container = c;
        placed.slot = slot;
        placed.quantity = piece;
        slots_[{static_cast<uint8_t>(c), slot}] = placed;
        out.outcome.events.push_back(make_storage_event(repl::StorageEventType::Add, c, slot, placed, true));
        item.quantity -= piece;
        if (item.quantity > 0) item.instance_guid.clear();
        break;
    }
    if (item.quantity > 0) out.rest.push_back(std::move(item));
}

Outcome Inventory::grant(std::vector<svc::InventoryItem> items, const ItemCatalog& catalog) {
    Inventory trial = *this;
    GrantResult r = trial.grant_what_fits(std::move(items), catalog);
    if (!r.outcome.ok) return r.outcome;
    if (!r.rest.empty()) {
        Outcome out;
        out.refusal = "the storages have no room for " + std::to_string(r.rest.size()) + " of the granted item(s) (the bag has " +
                      std::to_string(static_cast<size_t>(kBackpackSlots) - count(svc::ItemContainer::Backpack)) + " free slot(s))";
        return out;
    }
    *this = std::move(trial);
    return r.outcome;
}

bool Inventory::can_grant(const std::vector<svc::InventoryItem>& items, const ItemCatalog& catalog) const {
    Inventory trial = *this;
    const GrantResult r = trial.grant_what_fits(items, catalog);
    return r.outcome.ok && r.rest.empty();
}

ResourceRepair Inventory::repair_resources(const ItemCatalog& catalog) {
    ResourceRepair out;
    std::vector<svc::InventoryItem> candidates;
    for (int s = 0; s < kBackpackSlots; ++s) {
        const svc::InventoryItem* it = find(svc::ItemContainer::Backpack, s);
        if (!it) continue;
        const auto info = catalog.info(it->item_record_id);
        if (info && info->resource) candidates.push_back(*it);
    }
    for (svc::InventoryItem it : candidates) {
        const ItemInfo info = *catalog.info(it.item_record_id);
        bool placed = false;
        for (int i = 0; i < kResourceContainers && !placed; ++i) {
            const svc::ItemContainer c = resource_container(i);
            const int slot = place(c, info.inv_w, info.inv_h, catalog);
            if (slot < 0) continue;
            slots_.erase({static_cast<uint8_t>(svc::ItemContainer::Backpack), it.slot});
            it.container = c;
            it.slot = slot;
            slots_[{static_cast<uint8_t>(c), slot}] = it;
            out.moved.push_back(it);
            placed = true;
        }
        if (!placed) out.stayed.push_back(it);
    }
    return out;
}

Outcome Inventory::move_resource(svc::ItemContainer from, int from_slot, int32_t count, const std::string& guid, svc::ItemContainer to, int to_slot, const ItemCatalog& catalog) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.refusal = std::move(why);
        return out;
    };
    const svc::InventoryItem* item = at(from, from_slot);
    if (!item) return refuse(std::string("no item in ") + container_name(from) + " slot " + std::to_string(from_slot));
    if (!guid_of(guid) || item->instance_guid != guid) return refuse(std::string("the item in ") + container_name(from) + " slot " + std::to_string(from_slot) + " is not the instance the client named");
    if (count != item->quantity) return refuse("the move names " + std::to_string(count) + " of a stack of " + std::to_string(item->quantity) + " (splitting is not made)");
    if (from == to && from_slot == to_slot) return refuse("the item is moved onto its own slot");
    const ResourceGrid& g = grid(to);
    if (g.empty()) return refuse(std::string(container_name(to)) + " has no bag: it has no cells");
    if (!g.contains(to_slot)) return refuse(std::string(container_name(to)) + " slot " + std::to_string(to_slot) + " does not exist");
    const auto info = catalog.info(item->item_record_id);
    const int w = info ? info->inv_w : 1, h = info ? info->inv_h : 1;
    const std::string moved_guid = item->instance_guid;
    if (!g.fits(to_slot, w, h, covered_cells(to, catalog, &moved_guid))) {
        return refuse(std::string("a ") + std::to_string(w) + " x " + std::to_string(h) + " item does not fit " + container_name(to) + " slot " + std::to_string(to_slot) +
                      " (outside the grid, on a blocked cell or on another item: a move onto an item is a swap or a fill, which are not made)");
    }
    svc::InventoryItem moved = *item;
    Event remove = make_storage_event(repl::StorageEventType::Remove, from, from_slot, moved, true);
    moved.container = to;
    moved.slot = to_slot;
    Event add = make_storage_event(repl::StorageEventType::Add, to, to_slot, moved, true);
    slots_.erase({static_cast<uint8_t>(from), from_slot});
    slots_[{static_cast<uint8_t>(to), to_slot}] = moved;
    out.events = {std::move(remove), std::move(add)};
    out.ok = true;
    return out;
}

Outcome Inventory::move(svc::ItemContainer from, int from_slot, int32_t count, const std::string& guid, svc::ItemContainer to, int to_slot, const ItemCatalog* catalog) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.refusal = std::move(why);
        return out;
    };
    if (is_resource(from) && is_resource(to)) {
        if (!catalog) return refuse("a move inside the resource containers needs the item catalog");
        return move_resource(from, from_slot, count, guid, to, to_slot, *catalog);
    }
    if (is_resource(from) != is_resource(to)) return refuse("a resource item and the Backpack never exchange items: the Backpack refuses a resource item and the resource containers anything else");
    if (from != svc::ItemContainer::Backpack || to != svc::ItemContainer::Backpack) return refuse("only moves inside the Backpack are made");
    if (from_slot == to_slot) return refuse("the item is moved onto its own slot");
    const svc::InventoryItem* item = at(from, from_slot);
    if (!item) return refuse("no item in Backpack slot " + std::to_string(from_slot));
    if (to_slot < 0 || to_slot >= kBackpackSlots) return refuse("Backpack slot " + std::to_string(to_slot) + " does not exist");
    if (!guid_of(guid) || item->instance_guid != guid) return refuse("the item in Backpack slot " + std::to_string(from_slot) + " is not the instance the client named");
    if (count != item->quantity) return refuse("the move names " + std::to_string(count) + " of a stack of " + std::to_string(item->quantity) + " (splitting is not made)");
    if (find(to, to_slot)) return refuse("Backpack slot " + std::to_string(to_slot) + " is not empty (a move onto an item is a swap)");
    svc::InventoryItem moved = *item;
    Event remove = make_storage_event(repl::StorageEventType::Remove, from, from_slot, moved, true);
    moved.slot = to_slot;
    Event add = make_storage_event(repl::StorageEventType::Add, to, to_slot, moved, true);
    slots_.erase({static_cast<uint8_t>(from), from_slot});
    slots_[{static_cast<uint8_t>(to), to_slot}] = moved;
    out.events = {std::move(remove), std::move(add)};
    out.ok = true;
    return out;
}

Outcome Inventory::add_at(svc::ItemContainer container, int slot, svc::InventoryItem item, const ItemCatalog& catalog) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.refusal = std::move(why);
        return out;
    };
    if (container != svc::ItemContainer::Backpack && !is_resource(container)) return refuse("only the item bag and the material bags take an item from the warehouse");
    item.container = container;
    item.slot = slot;
    if (const std::string problem = item_problem(item); !problem.empty()) return refuse(problem);
    if (find_guid(item.instance_guid)) return refuse("the inventory holds an item with that instance guid already");
    const auto info = catalog.info(item.item_record_id);
    if (!info) return refuse("the item's record is not one this server knows");
    if (info->resource != is_resource(container)) {
        return refuse(info->resource ? "a material is not put in the item bag (materials go to a material bag)" : "only a material is put in a material bag");
    }
    if (!is_resource(container)) {
        if (slot < 0 || slot >= kBackpackSlots) return refuse("Backpack slot " + std::to_string(slot) + " does not exist");
        if (find(container, slot)) return refuse("Backpack slot " + std::to_string(slot) + " is not empty");
    } else {
        const ResourceGrid& g = grid(container);
        if (g.empty()) return refuse(std::string(container_name(container)) + " has no bag: it has no cells");
        if (!g.contains(slot)) return refuse(std::string(container_name(container)) + " slot " + std::to_string(slot) + " does not exist");
        if (!g.fits(slot, info->inv_w, info->inv_h, covered_cells(container, catalog))) {
            return refuse("a " + std::to_string(info->inv_w) + " x " + std::to_string(info->inv_h) + " item does not fit " + container_name(container) + " slot " + std::to_string(slot) +
                          " (outside the grid, on a blocked cell or on another item)");
        }
    }
    slots_[{static_cast<uint8_t>(container), slot}] = item;
    out.events.push_back(make_storage_event(repl::StorageEventType::Add, container, slot, item, true));
    out.ok = true;
    return out;
}

Outcome Inventory::grant_cargo(svc::InventoryItem item, const ItemCatalog& catalog) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.refusal = std::move(why);
        return out;
    };
    const auto info = catalog.info(item.item_record_id);
    if (!info) return refuse("the item's record is not one this server knows");
    if (!info->cargo_crate()) return refuse("the item is no crate: its record does not list the cargo slot alone");
    if (item.quantity != 1) return refuse("a carried crate is one item, not " + std::to_string(item.quantity));
    if (find(svc::ItemContainer::Equipment, equip::CargoHold)) return refuse("the cargo slot holds a crate already");
    item.container = svc::ItemContainer::Equipment;
    item.slot = equip::CargoHold;
    if (const std::string problem = item_problem(item); !problem.empty()) return refuse(problem);
    if (find_guid(item.instance_guid)) return refuse("the inventory holds an item with that instance guid already");
    slots_[{static_cast<uint8_t>(item.container), item.slot}] = item;
    out.events.push_back(make_storage_event(repl::StorageEventType::Add, item.container, item.slot, item, true));
    out.equipment_changed = true;
    out.ok = true;
    return out;
}

Outcome Inventory::swap(svc::ItemContainer from, int from_slot, int32_t count, const std::string& guid, svc::ItemContainer to, int to_slot,
                        const std::string& to_guid, int32_t to_count) {
    Outcome out;
    const auto refuse = [&](std::string why) {
        out.refusal = std::move(why);
        return out;
    };
    if (is_resource(from) || is_resource(to)) return refuse("the items of the resource containers are not swapped");
    if (from != svc::ItemContainer::Backpack || to != svc::ItemContainer::Backpack) return refuse("only swaps inside the Backpack are made");
    if (from_slot == to_slot) return refuse("the item is swapped with itself");
    const svc::InventoryItem* a = at(from, from_slot);
    const svc::InventoryItem* b = at(to, to_slot);
    if (!a) return refuse("no item in Backpack slot " + std::to_string(from_slot));
    if (!b) return refuse("no item in Backpack slot " + std::to_string(to_slot) + " to swap with");
    if (!guid_of(guid) || a->instance_guid != guid) return refuse("the item in Backpack slot " + std::to_string(from_slot) + " is not the instance the client named");
    if (!guid_of(to_guid) || b->instance_guid != to_guid) return refuse("the item in Backpack slot " + std::to_string(to_slot) + " is not the instance the client named");
    if (count != a->quantity || to_count != b->quantity) return refuse("a swap names whole stacks only");
    svc::InventoryItem na = *a, nb = *b;
    Event remove_a{repl::StorageEventType::Remove, from, from_slot, na};
    Event remove_b{repl::StorageEventType::Remove, to, to_slot, nb};
    svc::InventoryItem a_at_to = na, b_at_from = nb;
    a_at_to.slot = to_slot;
    b_at_from.slot = from_slot;
    Event add_b{repl::StorageEventType::Add, from, from_slot, b_at_from};
    Event add_a{repl::StorageEventType::Add, to, to_slot, a_at_to};
    slots_[{static_cast<uint8_t>(from), from_slot}] = b_at_from;
    slots_[{static_cast<uint8_t>(to), to_slot}] = a_at_to;
    out.events = {std::move(remove_a), std::move(remove_b), std::move(add_b), std::move(add_a)};
    out.ok = true;
    return out;
}

}  // namespace aoc::worldd::items
