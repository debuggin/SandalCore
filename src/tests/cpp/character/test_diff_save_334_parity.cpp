// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "items/inventory.h"
#include "items/wallet.h"
#include "session/position_saver.h"
#include "svc/services.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace aoc::worldd;

class RecordingCharacterApi final : public svc::CharacterApi {
public:
    std::mutex mu;
    std::unordered_map<std::string, svc::Position> positions;
    std::unordered_map<std::string, std::vector<svc::InventoryItem>> inventories;
    std::unordered_map<std::string, std::vector<svc::WalletEntry>> wallets;
    std::unordered_map<std::string, svc::Progress> progress_map;
    std::atomic<int> pos_calls{0};
    std::atomic<int> inv_calls{0};
    std::atomic<int> prog_calls{0};
    std::atomic<int> transient_inv_failures{0};

    svc::CharacterResult get_character(const std::string& id) override {
        svc::CharacterResult res;
        res.status = svc::CallStatus::Ok;
        res.character.id = id;
        return res;
    }

    svc::CallStatus save_position(const std::string& id, const svc::Position& position) override {
        std::lock_guard<std::mutex> lk(mu);
        ++pos_calls;
        positions[id] = position;
        return svc::CallStatus::Ok;
    }

    svc::CallStatus save_inventory(
        const std::string& id,
        const std::vector<svc::InventoryItem>& items,
        const std::vector<svc::WalletEntry>& wallet) override {
        ++inv_calls;
        const int rem = transient_inv_failures.load();
        if (rem > 0) {
            transient_inv_failures.fetch_sub(1);
            return svc::CallStatus::Unavailable;
        }
        std::lock_guard<std::mutex> lk(mu);
        inventories[id] = items;
        wallets[id] = wallet;
        return svc::CallStatus::Ok;
    }

    svc::CallStatus save_progress(const std::string& id, const svc::Progress& progress) override {
        std::lock_guard<std::mutex> lk(mu);
        ++prog_calls;
        progress_map[id] = progress;
        return svc::CallStatus::Ok;
    }
};

class SimpleCatalog final : public items::ItemCatalog {
public:
    std::optional<items::ItemInfo> info(int64_t record) const override {
        items::ItemInfo ii;
        ii.max_stack = (record == 5001) ? 20 : 1;
        return ii;
    }
};

svc::InventoryItem make_slot_item(int32_t slot, int64_t rec, const char* guid, int32_t qty) {
    svc::InventoryItem it;
    it.container = svc::ItemContainer::Backpack;
    it.slot = slot;
    it.item_record_id = rec;
    it.instance_guid = guid;
    it.quantity = qty;
    it.quality = 10;
    it.durability = 100.0f;
    return it;
}

int run_dirty_slot_diff_save_10k_parity() {
    SimpleCatalog cat;
    std::vector<svc::InventoryItem> init = {
        make_slot_item(0, 5001, "00000000000000000000000000000501", 10),
        make_slot_item(1, 5001, "00000000000000000000000000000502", 5)};
    std::string err;
    auto inv = items::Inventory::from_items(init, err, &cat);
    if (!inv || !err.empty()) return 1;
    const auto mv = inv->move(
        svc::ItemContainer::Backpack, 1, 5, "00000000000000000000000000000502",
        svc::ItemContainer::Backpack, 2, &cat);
    if (!mv.ok || !inv->at(svc::ItemContainer::Backpack, 2)) return 1;

    items::Wallet wallet({{items::kCopperCurrency, 12500}});
    (void)wallet.give(500);

    RecordingCharacterApi api;
    CharacterSaver::Config cfg;
    cfg.max_queued = 256;
    cfg.retry_delay = std::chrono::milliseconds(5);
    CharacterSaver saver(api, cfg);

    for (int i = 0; i < 50; ++i) {
        svc::Position pos;
        pos.x = 100.0 * static_cast<double>(i + 1);
        pos.y = -200.0 * static_cast<double>(i + 1);
        pos.z = 50.0;
        pos.yaw = 90.0f;
        saver.save("char-334", pos);
    }
    saver.save_inventory("char-334", inv->items(), wallet.entries());

    svc::Progress prog;
    prog.level = 12;
    prog.experience = {{108651085518458LL, 45000, 0}};
    saver.save_progress("char-334", prog);
    saver.flush(std::chrono::milliseconds(5000));

    if (saver.failed() != 0 || saver.dropped() != 0) return 1;
    if (api.positions["char-334"].x != 5000.0) return 1;
    if (api.inventories["char-334"].size() != 2 || api.inventories["char-334"][1].slot != 2) {
        return 1;
    }
    return api.progress_map["char-334"].level == 12 ? 0 : 1;
}

int run_diff_save_fencing_epoch_and_rollback() {
    SimpleCatalog cat;
    std::vector<svc::InventoryItem> init = {
        make_slot_item(0, 5001, "00000000000000000000000000000601", 12)};
    std::string err;
    auto inv = items::Inventory::from_items(init, err, &cat);
    if (!inv || !err.empty()) return 1;

    const auto bad_move = inv->move(
        svc::ItemContainer::Backpack, 0, 12, "00000000000000000000000000000999",
        svc::ItemContainer::Backpack, 2, &cat);
    if (bad_move.ok || inv->at(svc::ItemContainer::Backpack, 0)->quantity != 12) return 1;

    items::Wallet wallet({{items::kCopperCurrency, 1000}});
    if (wallet.pay(2500) || wallet.copper() != 1000) return 1;

    RecordingCharacterApi api;
    api.transient_inv_failures.store(1);
    CharacterSaver::Config cfg;
    cfg.max_queued = 2;
    cfg.retry_delay = std::chrono::milliseconds(5);
    CharacterSaver saver(api, cfg);

    saver.save_inventory("char-fence", inv->items(), wallet.entries());
    saver.flush(std::chrono::milliseconds(5000));

    if (api.inv_calls.load() < 2 || saver.inventories_saved() < 1 || saver.failed() != 0) {
        return 1;
    }
    return api.inventories["char-fence"].size() == 1 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const char* case_name = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            case_name = argv[++i];
        }
    }
    if (!case_name) {
        if (run_dirty_slot_diff_save_10k_parity() != 0) return 1;
        if (run_diff_save_fencing_epoch_and_rollback() != 0) return 1;
        return 0;
    }
    if (std::strcmp(case_name, "dirty_slot_diff_save_10k_parity") == 0) {
        return run_dirty_slot_diff_save_10k_parity();
    }
    if (std::strcmp(case_name, "diff_save_fencing_epoch_and_rollback") == 0) {
        return run_diff_save_fencing_epoch_and_rollback();
    }
    std::fprintf(stderr, "Unknown --case %s\n", case_name);
    return 2;
}
