// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "login/login_service.h"
#include "net/udp_socket.h"
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string_view>

namespace aoc { namespace worldd {

template <typename T, std::size_t Capacity>
class alignas(64) SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0 && Capacity >= 2, "SpscRing capacity must be a power of two >= 2");
    static constexpr std::size_t kMask = Capacity - 1;

public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    bool try_push(const T& item) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[h & kMask] = item;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool try_push(T&& item) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[h & kMask] = std::move(item);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        if (h == t) return false;
        out = std::move(slots_[t & kMask]);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() noexcept {
        T item{};
        if (!try_pop(item)) return std::nullopt;
        return item;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return h >= t ? (h - t) : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return size_approx() == 0; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
    [[nodiscard]] std::size_t dropped_count() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    void clear() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::atomic<std::size_t> dropped_{0};
    alignas(64) std::array<T, Capacity> slots_{};
};

struct InboundNetFrame {
    Address from{};
    uint16_t len = 0;
    std::array<uint8_t, PacketBatch::kMaxPayloadBytes> payload{};
    std::chrono::steady_clock::time_point received_at{};

    [[nodiscard]] static InboundNetFrame make(const Address& peer, const uint8_t* data, std::size_t n,
                                              std::chrono::steady_clock::time_point at = {}) noexcept {
        InboundNetFrame f{};
        f.from = peer;
        f.received_at = at;
        const std::size_t copy_len = n <= PacketBatch::kMaxPayloadBytes ? n : PacketBatch::kMaxPayloadBytes;
        f.len = static_cast<uint16_t>(copy_len);
        if (copy_len > 0 && data) std::memcpy(f.payload.data(), data, copy_len);
        return f;
    }
};

struct OutboundNetFrame {
    Address to{};
    uint16_t len = 0;
    std::array<uint8_t, PacketBatch::kMaxPayloadBytes> payload{};

    [[nodiscard]] static OutboundNetFrame make(const Address& peer, const uint8_t* data, std::size_t n) noexcept {
        OutboundNetFrame f{};
        f.to = peer;
        const std::size_t copy_len = n <= PacketBatch::kMaxPayloadBytes ? n : PacketBatch::kMaxPayloadBytes;
        f.len = static_cast<uint16_t>(copy_len);
        if (copy_len > 0 && data) std::memcpy(f.payload.data(), data, copy_len);
        return f;
    }
};

struct AsyncTaskCompletion {
    uint64_t session_id = 0;
    LoginResult result{};
};

inline constexpr std::size_t kDefaultNetRingCapacity = 64;
inline constexpr std::size_t kDefaultTaskRingCapacity = 256;

using InboundNetRing = SpscRing<InboundNetFrame, kDefaultNetRingCapacity>;
using OutboundNetRing = SpscRing<OutboundNetFrame, kDefaultNetRingCapacity>;
using AsyncTaskCompletionRing = SpscRing<AsyncTaskCompletion, kDefaultTaskRingCapacity>;

struct TokenSnapshot {
    uint64_t version = 0;
    std::string primary_token;
    std::string secondary_token;
    std::chrono::steady_clock::time_point updated_at{};

    [[nodiscard]] static bool constant_time_eq(std::string_view a, std::string_view b) noexcept {
        if (a.empty() || a.size() != b.size()) return false;
        uint8_t diff = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            diff |= static_cast<uint8_t>(static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]));
        }
        return diff == 0;
    }

    [[nodiscard]] bool matches(std::string_view candidate) const noexcept {
        return constant_time_eq(candidate, primary_token) || constant_time_eq(candidate, secondary_token);
    }
};

class alignas(64) TokenSnapshotStore {
public:
    TokenSnapshotStore() noexcept = default;

    void publish(TokenSnapshot next) {
        const auto cur = load();
        if (next.version == 0) next.version = cur ? cur->version + 1 : 1;
        snapshot_.store(std::make_shared<const TokenSnapshot>(std::move(next)), std::memory_order_release);
    }

    void publish(std::shared_ptr<const TokenSnapshot> next) noexcept {
        snapshot_.store(std::move(next), std::memory_order_release);
    }

    [[nodiscard]] std::shared_ptr<const TokenSnapshot> load() const noexcept {
        return snapshot_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::shared_ptr<const TokenSnapshot>> snapshot_{std::make_shared<const TokenSnapshot>()};
};

}} // namespace aoc::worldd
