// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace aoc { namespace worldd { namespace svc {

static constexpr uint32_t kAdminRoleRead = 1u << 0;
static constexpr uint32_t kAdminRoleGm = 1u << 1;
static constexpr uint32_t kAdminRoleHotfix = 1u << 2;
static constexpr uint32_t kAdminRoleSuperuser = 1u << 3;

struct CommandAuditMeta {
    std::string operator_id{};
    uint32_t    role_mask{0};
    bool        signature_verified{false};
    std::string correlation_id{};

    [[nodiscard]] bool is_authorized(uint32_t required_roles = kAdminRoleGm) const noexcept {
        if (!signature_verified || operator_id.empty()) return false;
        return (role_mask & kAdminRoleSuperuser) != 0u || (role_mask & required_roles) == required_roles;
    }
};

struct ReloadContentHotfixesCommand {
    uint64_t         expected_version{0};
    std::string      operator_id{};
    std::string      reason{};
    CommandAuditMeta audit{};

    [[nodiscard]] bool is_valid() const noexcept {
        if (expected_version == 0 || (operator_id.empty() && audit.operator_id.empty())) return false;
        if (audit.signature_verified || audit.role_mask != 0u || !audit.correlation_id.empty()) {
            return audit.is_authorized(kAdminRoleHotfix);
        }
        return true;
    }
};

class CommandBus {
public:
    static constexpr size_t kDefaultCapacity = 256, kMaxCapacity = 4096;

    explicit CommandBus(size_t capacity = kDefaultCapacity) noexcept
        : capacity_(capacity == 0 ? 1 : (capacity > kMaxCapacity ? kMaxCapacity : capacity)) {}

    [[nodiscard]] bool submit(ReloadContentHotfixesCommand cmd) {
        if (cmd.operator_id.empty() && !cmd.audit.operator_id.empty()) cmd.operator_id = cmd.audit.operator_id;
        if (!cmd.is_valid()) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (queue_.size() >= capacity_) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queue_.push_back(std::move(cmd));
        enqueued_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool enqueue(ReloadContentHotfixesCommand cmd) { return submit(std::move(cmd)); }

    template <typename Handler>
        requires std::invocable<Handler&, const ReloadContentHotfixesCommand&>
    size_t drain(Handler&& handler, size_t max_batch = 32) {
        const size_t limit = max_batch > kMaxCapacity ? kMaxCapacity : max_batch;
        size_t drained = 0;
        while (drained < limit) {
            ReloadContentHotfixesCommand cmd;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (queue_.empty()) break;
                cmd = std::move(queue_.front());
                queue_.pop_front();
            }
            handler(cmd);
            drained_count_.fetch_add(1, std::memory_order_relaxed);
            ++drained;
        }
        return drained;
    }

    [[nodiscard]] size_t pending() const { std::lock_guard<std::mutex> lock(mu_); return queue_.size(); }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] uint64_t enqueued_count() const noexcept { return enqueued_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t dropped_count() const noexcept { return dropped_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t drained_count() const noexcept { return drained_count_.load(std::memory_order_relaxed); }

private:
    size_t                                   capacity_;
    mutable std::mutex                       mu_;
    std::deque<ReloadContentHotfixesCommand> queue_;
    std::atomic<uint64_t>                    enqueued_count_{0}, dropped_count_{0}, drained_count_{0};
};

}}}  // namespace aoc::worldd::svc
