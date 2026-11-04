// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "protocol/repl/replicator.h"
#include "session/view_worker.h"
#include "world/actor_registry.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aoc::worldd {

namespace repl = ::aoc::protocol::repl;

enum class ShareMode : uint8_t { Off, On, Verify };

/// Cached replication blocks, NAK record, and shadow redo built for an actor version and shape epoch.
struct UpdateMemo {
    struct Variant {
        uint32_t                        from_version = 0;
        std::vector<repl::ContentBlock> blocks;
        repl::SentRecord                record;
        repl::ShadowRedo                redo;
    };
    static constexpr size_t kMaxVariants = 4;

    uint32_t             version = 0;
    uint32_t             shape_epoch = 0;
    std::vector<Variant> variants;
};

class SharedUpdates {
public:
    struct Stats {
        uint64_t hits = 0;
        uint64_t builds = 0;
        uint64_t private_builds = 0;
        uint64_t verified = 0;
        uint64_t mismatches = 0;
    };

    explicit SharedUpdates(ShareMode mode, unsigned verify_every = 8) : mode_(mode), verify_every_(verify_every == 0 ? 1 : verify_every) {}

    ShareMode mode() const { return mode_.load(std::memory_order_relaxed); }
    bool      active() const { return mode() != ShareMode::Off; }
    Stats     stats() const;
    Stats&    mine() { return local_[view_worker()].stats; }

    static ShareMode   parse(std::string_view text, bool* ok = nullptr);
    static const char* name(ShareMode mode);

    static std::shared_ptr<SharedUpdates> from_test_environment();

    const UpdateMemo::Variant* find(const world::WorldActor& actor, uint32_t from_version) const;
    const UpdateMemo::Variant& publish(const world::WorldActor& actor, uint32_t from_version, std::vector<repl::ContentBlock>&& blocks, repl::SentRecord&& record, repl::ShadowRedo&& redo);

    bool verify_due() { return mode() == ShareMode::Verify && ++local_[view_worker()].verify_calls % verify_every_ == 0; }
    void mismatch(uint64_t actor, const char* what);

    enum class Fault : uint8_t { None, DropLastBlock, SkewRedo };
    void fault_for_test(Fault f) { fault_ = f; }

private:
    std::atomic<ShareMode> mode_;
    unsigned               verify_every_;
    struct alignas(64) Local {
        Stats    stats;
        uint64_t verify_calls = 0;
    };
    std::array<Local, kViewWorkers> local_;
    Fault                           fault_ = Fault::None;
};

}  // namespace aoc::worldd
