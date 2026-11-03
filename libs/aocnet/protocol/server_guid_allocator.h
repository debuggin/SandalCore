// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "protocol/emit/intrepid_netguid.h"
#include <atomic>
#include <cstdint>

namespace aoc { namespace protocol {

class ServerGuidAllocator {
public:
    static constexpr uint64_t kFirstObjectId  = 0x01000000ULL;
    static constexpr uint64_t kObjectIdStride = 2ULL;
    static constexpr uint32_t kShardShift     = 48u;
    static constexpr uint64_t kShardMask      = 0xFFFFULL;
    static constexpr uint64_t kCounterMask    = 0x00007FFFFFFFFFFFULL;
    static constexpr uint64_t kMaxObjectId    = 0xFFFFFFFFFFFFFFFEULL;

    static constexpr bool has_pm52_inline_flag(uint64_t guid) noexcept {
        return (guid & 1ULL) != 0ULL;
    }

    static constexpr uint64_t encode_pm52_object_id(uint64_t raw_counter) noexcept {
        return (raw_counter << 1u) & ~1ULL;
    }

    static constexpr uint64_t encode_shard_guid(uint16_t shard_id, uint64_t raw_counter) noexcept {
        return (static_cast<uint64_t>(shard_id) << kShardShift) |
               ((raw_counter & kCounterMask) << 1u);
    }

    static constexpr uint16_t extract_shard_id(uint64_t guid) noexcept {
        return static_cast<uint16_t>((guid >> kShardShift) & kShardMask);
    }

    static constexpr uint64_t extract_raw_counter(uint64_t guid) noexcept {
        return (guid >> 1u) & kCounterMask;
    }

    static constexpr bool is_server_allocated_guid(uint64_t guid) noexcept {
        return guid >= kFirstObjectId && !has_pm52_inline_flag(guid);
    }

    static constexpr bool is_server_allocated_guid(const emit::FIntrepidNetworkGUID& g) noexcept {
        return g.ServerId != 0u && is_server_allocated_guid(g.ObjectId);
    }

    ServerGuidAllocator(uint32_t server_id, uint32_t randomizer, uint64_t initial_object_id = kFirstObjectId)
        : server_id_(server_id),
          randomizer_(randomizer),
          next_object_id_(
              initial_object_id < kFirstObjectId ? kFirstObjectId : (initial_object_id & ~1ULL)) {}

    emit::FIntrepidNetworkGUID next() noexcept {
        uint64_t cur = next_object_id_.load(std::memory_order_relaxed);
        for (uint32_t spin = 0; spin < 1024u; ++spin) {
            if (cur >= kMaxObjectId) {
                cur = kMaxObjectId;
                break;
            }
            const uint64_t desired =
                (cur > kMaxObjectId - kObjectIdStride) ? kMaxObjectId : (cur + kObjectIdStride);
            if (next_object_id_.compare_exchange_weak(
                    cur, desired, std::memory_order_relaxed, std::memory_order_relaxed)) {
                break;
            }
        }
        emit::FIntrepidNetworkGUID g;
        g.ObjectId = cur & ~1ULL;
        g.ServerId = server_id_;
        g.Randomizer = randomizer_;
        return g;
    }

    uint64_t peek_next_object_id() const noexcept {
        return next_object_id_.load(std::memory_order_relaxed);
    }

    uint32_t server_id() const noexcept { return server_id_; }
    uint32_t randomizer() const noexcept { return randomizer_; }

private:
    const uint32_t server_id_;
    const uint32_t randomizer_;
    std::atomic<uint64_t> next_object_id_;
};

constexpr bool has_pm52_inline_flag(uint64_t guid) noexcept {
    return ServerGuidAllocator::has_pm52_inline_flag(guid);
}

constexpr uint64_t encode_pm52_object_id(uint64_t raw_counter) noexcept {
    return ServerGuidAllocator::encode_pm52_object_id(raw_counter);
}

constexpr uint64_t encode_shard_guid(uint16_t shard_id, uint64_t raw_counter) noexcept {
    return ServerGuidAllocator::encode_shard_guid(shard_id, raw_counter);
}

constexpr uint16_t extract_shard_id(uint64_t guid) noexcept {
    return ServerGuidAllocator::extract_shard_id(guid);
}

constexpr uint64_t extract_raw_counter(uint64_t guid) noexcept {
    return ServerGuidAllocator::extract_raw_counter(guid);
}

constexpr bool is_server_allocated_guid(uint64_t guid) noexcept {
    return ServerGuidAllocator::is_server_allocated_guid(guid);
}

constexpr bool is_server_allocated_guid(const emit::FIntrepidNetworkGUID& g) noexcept {
    return ServerGuidAllocator::is_server_allocated_guid(g);
}

}} // namespace aoc::protocol
