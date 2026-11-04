// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include <cstdint>

namespace aoc::worldd::session {

enum class TradePhase : uint8_t {
    Idle = 0,
    Open = 1,
    Locked = 2,
    Confirmed = 3,
    Completed = 4,
    Cancelled = 5,
};

class TradeStatechart {
public:
    constexpr TradeStatechart() noexcept = default;

    constexpr bool open_trade() noexcept {
        if (phase_ != TradePhase::Idle) return false;
        phase_ = TradePhase::Open;
        offer_revision_ = 0;
        party_a_locked_ = false;
        party_b_locked_ = false;
        party_a_confirmed_ = false;
        party_b_confirmed_ = false;
        return true;
    }

    // Any offer mutation while either party is Locked or Confirmed unlocks both parties
    // back to Open and advances offer_revision to prevent bait-and-switch exploits.
    constexpr bool mutate_offer(bool is_party_a) noexcept {
        if (phase_ != TradePhase::Open &&
            phase_ != TradePhase::Locked &&
            phase_ != TradePhase::Confirmed) {
            return false;
        }
        if (is_party_a) {
            ++party_a_mutations_;
        } else {
            ++party_b_mutations_;
        }
        ++offer_revision_;
        party_a_locked_ = false;
        party_b_locked_ = false;
        party_a_confirmed_ = false;
        party_b_confirmed_ = false;
        phase_ = TradePhase::Open;
        return true;
    }

    constexpr bool lock_party(bool is_party_a) noexcept {
        if (phase_ != TradePhase::Open && phase_ != TradePhase::Locked) {
            return false;
        }
        if (is_party_a) {
            party_a_locked_ = true;
        } else {
            party_b_locked_ = true;
        }
        if (party_a_locked_ && party_b_locked_) {
            phase_ = TradePhase::Locked;
        }
        return true;
    }

    constexpr bool confirm_party(bool is_party_a) noexcept {
        if (!party_a_locked_ || !party_b_locked_) return false;
        if (phase_ != TradePhase::Locked && phase_ != TradePhase::Confirmed) {
            return false;
        }
        if (is_party_a) {
            party_a_confirmed_ = true;
        } else {
            party_b_confirmed_ = true;
        }
        if (party_a_confirmed_ && party_b_confirmed_) {
            phase_ = TradePhase::Completed;
        } else {
            phase_ = TradePhase::Confirmed;
        }
        return true;
    }

    constexpr bool confirm_party(bool is_party_a, uint64_t expected_revision) noexcept {
        if (expected_revision != offer_revision_) return false;
        return confirm_party(is_party_a);
    }

    constexpr bool cancel_trade() noexcept {
        if (phase_ == TradePhase::Idle ||
            phase_ == TradePhase::Completed ||
            phase_ == TradePhase::Cancelled) {
            return false;
        }
        party_a_locked_ = false;
        party_b_locked_ = false;
        party_a_confirmed_ = false;
        party_b_confirmed_ = false;
        phase_ = TradePhase::Cancelled;
        return true;
    }

    [[nodiscard]] constexpr TradePhase phase() const noexcept { return phase_; }
    [[nodiscard]] constexpr uint64_t offer_revision() const noexcept { return offer_revision_; }
    [[nodiscard]] constexpr uint32_t party_mutations(bool is_party_a) const noexcept {
        return is_party_a ? party_a_mutations_ : party_b_mutations_;
    }

    [[nodiscard]] constexpr bool is_party_locked(bool is_party_a) const noexcept {
        return is_party_a ? party_a_locked_ : party_b_locked_;
    }

    [[nodiscard]] constexpr bool is_party_confirmed(bool is_party_a) const noexcept {
        return is_party_a ? party_a_confirmed_ : party_b_confirmed_;
    }

    [[nodiscard]] constexpr bool both_locked() const noexcept {
        return party_a_locked_ && party_b_locked_;
    }

    [[nodiscard]] constexpr bool both_confirmed() const noexcept {
        return party_a_confirmed_ && party_b_confirmed_;
    }

    [[nodiscard]] constexpr TradePhase party_phase(bool is_party_a) const noexcept {
        if (phase_ == TradePhase::Idle ||
            phase_ == TradePhase::Completed ||
            phase_ == TradePhase::Cancelled) {
            return phase_;
        }
        if (is_party_confirmed(is_party_a)) return TradePhase::Confirmed;
        if (is_party_locked(is_party_a)) return TradePhase::Locked;
        return TradePhase::Open;
    }

private:
    TradePhase phase_ = TradePhase::Idle;
    uint64_t offer_revision_ = 0;
    uint32_t party_a_mutations_ = 0;
    uint32_t party_b_mutations_ = 0;
    bool party_a_locked_ = false;
    bool party_b_locked_ = false;
    bool party_a_confirmed_ = false;
    bool party_b_confirmed_ = false;
};

}  // namespace aoc::worldd::session

namespace aoc::worldd {
using session::TradePhase;
using session::TradeStatechart;
}  // namespace aoc::worldd
