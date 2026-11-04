// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "items/inventory.h"
#include "svc/services.h"

namespace aoc::worldd {

/// Tracks per-slot dirty and deleted bitmasks across all 6 character item containers (0..5, up to 256 slots each)
/// with a monotonic fencing token so periodic persistence flushes only mutated slots and safely restores failed ticks.
class InventoryDirtyTracker {
public:
    static constexpr size_t kContainerCount = 6;
    static constexpr size_t kMaxSlots = 256;

    struct SlotCoord {
        svc::ItemContainer container = svc::ItemContainer::Backpack;
        int32_t slot = 0;
    };

    struct DiffSnapshot {
        int64_t fencing_token = 0;
        std::vector<svc::InventoryItem> upserts;
        std::vector<SlotCoord> deletes;
        std::bitset<kMaxSlots> dirty[kContainerCount]{};
        std::bitset<kMaxSlots> deleted[kContainerCount]{};

        [[nodiscard]] bool empty() const noexcept { return upserts.empty() && deletes.empty(); }
    };

    explicit InventoryDirtyTracker(int64_t initial_fencing_token = 0) noexcept
        : fencing_token_(initial_fencing_token) {}

    void mark_dirty(svc::ItemContainer c, int slot) noexcept {
        const int idx = static_cast<int>(c);
        if (idx < 0 || idx >= static_cast<int>(kContainerCount) || slot < 0 || slot >= static_cast<int>(kMaxSlots)) {
            return;
        }
        const auto s = static_cast<size_t>(slot);
        dirty_[idx].set(s);
        deleted_[idx].reset(s);
    }

    void mark_deleted(svc::ItemContainer c, int slot) noexcept {
        const int idx = static_cast<int>(c);
        if (idx < 0 || idx >= static_cast<int>(kContainerCount) || slot < 0 || slot >= static_cast<int>(kMaxSlots)) {
            return;
        }
        const auto s = static_cast<size_t>(slot);
        deleted_[idx].set(s);
        dirty_[idx].reset(s);
    }

    void record_outcome(const items::Outcome& out) noexcept {
        if (!out.ok) {
            return;
        }
        for (const auto& ev : out.events) {
            if (ev.type == repl::StorageEventType::Remove && (ev.count == 0 || ev.count >= ev.item.quantity)) {
                mark_deleted(ev.container, ev.slot);
            } else {
                mark_dirty(ev.container, ev.slot);
            }
        }
    }

    [[nodiscard]] DiffSnapshot drain_snapshot(const items::Inventory& inv) {
        DiffSnapshot snap;
        snap.fencing_token = ++fencing_token_;
        for (size_t c_idx = 0; c_idx < kContainerCount; ++c_idx) {
            snap.dirty[c_idx] = dirty_[c_idx];
            snap.deleted[c_idx] = deleted_[c_idx];
            dirty_[c_idx].reset();
            deleted_[c_idx].reset();
            collect_container_diff(inv, static_cast<svc::ItemContainer>(c_idx), c_idx, snap);
        }
        return snap;
    }

    void restore_on_rollback(const DiffSnapshot& failed) noexcept {
        for (size_t i = 0; i < kContainerCount; ++i) {
            const std::bitset<kMaxSlots> untouched = ~(dirty_[i] | deleted_[i]);
            dirty_[i] |= (failed.dirty[i] & untouched);
            deleted_[i] |= (failed.deleted[i] & untouched);
        }
    }

    [[nodiscard]] bool has_pending() const noexcept {
        for (size_t i = 0; i < kContainerCount; ++i) {
            if (dirty_[i].any() || deleted_[i].any()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool is_dirty(svc::ItemContainer c, int slot) const noexcept {
        const int idx = static_cast<int>(c);
        return idx >= 0 && idx < static_cast<int>(kContainerCount) && slot >= 0 && slot < static_cast<int>(kMaxSlots) &&
               dirty_[idx].test(static_cast<size_t>(slot));
    }

    [[nodiscard]] bool is_deleted(svc::ItemContainer c, int slot) const noexcept {
        const int idx = static_cast<int>(c);
        return idx >= 0 && idx < static_cast<int>(kContainerCount) && slot >= 0 && slot < static_cast<int>(kMaxSlots) &&
               deleted_[idx].test(static_cast<size_t>(slot));
    }

    [[nodiscard]] int64_t fencing_token() const noexcept { return fencing_token_; }
    void set_fencing_token(int64_t token) noexcept {
        if (token > fencing_token_) {
            fencing_token_ = token;
        }
    }

private:
    static void collect_container_diff(const items::Inventory& inv, svc::ItemContainer c, size_t c_idx, DiffSnapshot& snap) {
        if (!snap.dirty[c_idx].any() && !snap.deleted[c_idx].any()) {
            return;
        }
        for (size_t slot = 0; slot < kMaxSlots; ++slot) {
            const bool was_dirty = snap.dirty[c_idx].test(slot);
            const bool was_deleted = snap.deleted[c_idx].test(slot);
            if (!was_dirty && !was_deleted) {
                continue;
            }
            const auto* item = inv.at(c, static_cast<int>(slot));
            if (item != nullptr) {
                snap.dirty[c_idx].set(slot);
                snap.deleted[c_idx].reset(slot);
                snap.upserts.push_back(*item);
            } else {
                snap.deleted[c_idx].set(slot);
                snap.dirty[c_idx].reset(slot);
                snap.deletes.push_back(SlotCoord{c, static_cast<int32_t>(slot)});
            }
        }
    }

    std::bitset<kMaxSlots> dirty_[kContainerCount]{};
    std::bitset<kMaxSlots> deleted_[kContainerCount]{};
    int64_t fencing_token_ = 0;
};

}  // namespace aoc::worldd
