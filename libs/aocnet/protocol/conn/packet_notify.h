// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "protocol/conn/sequence.h"
#include "protocol/emit/bunch_writer.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace aoc { namespace protocol { namespace conn {

constexpr uint16_t kSequenceMask = kSeqMask;
constexpr uint16_t kSequenceHalfWindow = kSeqHalf;
constexpr size_t   kHistoryWordBits = 32;
constexpr size_t   kMaxHistoryWords = 8;
constexpr size_t   kMaxHistoryBits  = kMaxHistoryWords * kHistoryWordBits;

constexpr int seq14_distance(uint16_t a, uint16_t b) noexcept {
    const int d = static_cast<int>((a - b) & kSequenceMask);
    return d >= static_cast<int>(kSequenceHalfWindow) ? d - static_cast<int>(kSequenceMask + 1u) : d;
}

constexpr bool seq14_is_newer(uint16_t candidate, uint16_t baseline) noexcept {
    const uint16_t a = static_cast<uint16_t>(candidate & kSequenceMask);
    const uint16_t b = static_cast<uint16_t>(baseline & kSequenceMask);
    return a != b && ((a - b) & kSequenceMask) < kSequenceHalfWindow;
}

constexpr uint16_t seq14_advance(uint16_t seq, int32_t delta) noexcept {
    return static_cast<uint16_t>((static_cast<int32_t>(seq) + delta) & static_cast<int32_t>(kSequenceMask));
}

constexpr bool seq14_in_ack_window(
    uint16_t seq, uint16_t acked_seq, size_t window_bits = kMaxHistoryBits) noexcept {
    const size_t clamped_bits = window_bits > kMaxHistoryBits ? kMaxHistoryBits : window_bits;
    if (clamped_bits == 0) return false;
    const int dist = seq14_distance(acked_seq, seq);
    return dist >= 0 && static_cast<size_t>(dist) < clamped_bits;
}

struct NotifyHeader {
    uint16_t seq = 0;
    uint16_t acked_seq = 0;
    uint8_t  history_words = 1;
    std::array<uint32_t, kMaxHistoryWords> history{};

    bool is_delivered(size_t index) const noexcept {
        const size_t valid_words =
            history_words > kMaxHistoryWords ? kMaxHistoryWords : static_cast<size_t>(history_words);
        if (index >= valid_words * kHistoryWordBits) return false;
        return ((history[index / kHistoryWordBits] >> (index % kHistoryWordBits)) & 1u) != 0u;
    }

    bool is_seq_delivered(uint16_t pkt_seq) const noexcept {
        const size_t valid_words =
            history_words > kMaxHistoryWords ? kMaxHistoryWords : static_cast<size_t>(history_words);
        const size_t active_bits = valid_words * kHistoryWordBits;
        if (!seq14_in_ack_window(pkt_seq, acked_seq, active_bits)) return false;
        const int dist = seq14_distance(acked_seq, pkt_seq);
        return is_delivered(static_cast<size_t>(dist));
    }
};

void write_notify_header(emit::BunchWriter& w, const NotifyHeader& h);

std::optional<NotifyHeader> read_notify_header(const uint8_t* data, size_t len,
                                               size_t end_bit, size_t& off);

class SequenceHistory {
public:
    void reset() noexcept { words_.fill(0); }
    void add_delivery_status(bool delivered);
    bool is_delivered(size_t index) const noexcept {
        if (index >= kMaxHistoryBits) return false;
        return ((words_[index / kHistoryWordBits] >> (index % kHistoryWordBits)) & 1u) != 0u;
    }
    uint32_t word(size_t i) const noexcept {
        return i < kMaxHistoryWords ? words_[i] : 0u;
    }

private:
    std::array<uint32_t, kMaxHistoryWords> words_{};
};

class PacketNotify {
public:
    void init(uint16_t initial_in_seq, uint16_t initial_out_seq);

    template <class OnAck>
    int update(const NotifyHeader& h, OnAck&& on_ack) {
        const int delta = sequence_delta(h);
        if (delta <= 0) return 0;
        process_received_acks(h, on_ack);
        return internal_update(h, delta);
    }

    bool is_waiting_for_history_flush() const {
        return seq_greater(waiting_for_flush_seq_ack_, out_ack_seq_);
    }

    void ack_seq(uint16_t seq);

    NotifyHeader make_header() const;

    uint16_t commit_and_increment_out_seq();

    bool is_window_full() const {
        return seq_diff(out_seq_, out_ack_seq_) > static_cast<int>(kMaxHistoryBits);
    }

    uint16_t in_seq() const { return in_seq_; }
    uint16_t in_ack_seq() const { return in_ack_seq_; }
    uint16_t out_seq() const { return out_seq_; }
    uint16_t out_ack_seq() const { return out_ack_seq_; }

private:
    struct SentAck { uint16_t out_seq; uint16_t in_ack_seq; };

    int sequence_delta(const NotifyHeader& h) const;

    template <class OnAck>
    void process_received_acks(const NotifyHeader& h, OnAck& on_ack) {
        if (!seq_greater(h.acked_seq, out_ack_seq_)) return;
        int ack_count = seq_diff(h.acked_seq, out_ack_seq_);
        const uint16_t new_in_ack_seq_ack = update_in_ack_seq_ack(ack_count, h.acked_seq);
        if (seq_greater(new_in_ack_seq_ack, in_ack_seq_ack_)) in_ack_seq_ack_ = new_in_ack_seq_ack;
        uint16_t current = seq_add(out_ack_seq_, 1);
        const int history_bits = static_cast<int>(h.history_words * kHistoryWordBits);
        while (ack_count > history_bits) {
            --ack_count;
            on_ack(current, false);
            current = seq_add(current, 1);
        }
        while (ack_count > 0) {
            --ack_count;
            on_ack(current, h.is_delivered(static_cast<size_t>(ack_count)));
            current = seq_add(current, 1);
        }
        out_ack_seq_ = h.acked_seq;
        if (seq_greater(out_ack_seq_, waiting_for_flush_seq_ack_)) waiting_for_flush_seq_ack_ = out_ack_seq_;
    }

    uint16_t update_in_ack_seq_ack(int ack_count, uint16_t acked_seq);
    int internal_update(const NotifyHeader& h, int delta);
    bool will_sequence_fit_in_history(uint16_t seq) const;
    bool has_unacknowledged_acks() const;
    void ack_seq(uint16_t seq, bool delivered);
    size_t history_length() const;

    SequenceHistory in_history_;
    std::deque<SentAck> ack_record_;
    uint16_t in_seq_ = 0;
    uint16_t in_ack_seq_ = 0;
    uint16_t in_ack_seq_ack_ = 0;
    uint16_t out_seq_ = 0;
    uint16_t out_ack_seq_ = 0;
    uint16_t waiting_for_flush_seq_ack_ = 0;
};

}}} // namespace aoc::protocol::conn
