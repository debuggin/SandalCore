// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "net/spsc_ring.h"
#include "protocol/conn/packet_framing.h"
#include "protocol/conn/packet_notify.h"

namespace sandal::test {

class DeterministicPrng {
public:
    explicit constexpr DeterministicPrng(uint64_t seed) noexcept
        : state_(seed != 0ULL ? seed : 0x9E3779B97F4A7C15ULL) {}
    [[nodiscard]] constexpr uint64_t next_u64() noexcept {
        uint64_t x = (state_ += 0x9E3779B97F4A7C15ULL);
        x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31U);
    }
    [[nodiscard]] constexpr uint32_t next_u32(uint32_t bound) noexcept {
        return bound == 0U ? 0U : static_cast<uint32_t>(next_u64() % bound);
    }
private:
    uint64_t state_;
};

template <typename Fn>
[[nodiscard]] int run_case_dispatch(int argc, char** argv, std::string_view case_name, Fn&& fn) {
    const DataFreeConfig cfg = parse_test_cli(argc, argv);
    return should_run_case(cfg, case_name) ? fn(cfg) : 0;
}

}  // namespace sandal::test

namespace {

namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace wire = aoc::protocol::wire;
namespace worldd = aoc::worldd;

constexpr uint32_t kMagicHeader = 0xA0C12345U;
constexpr conn::CustomField kTestCustomField{{0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U}};

struct DelayedDatagram {
    uint32_t deliver_step{0U};
    std::vector<uint8_t> bytes{};
};

std::vector<uint8_t> build_client_datagram(conn::PacketNotify& sender, uint16_t& out_seq) {
    const conn::OuterHeader outer{kMagicHeader, 1U, 2U};
    const conn::NotifyHeader notify = sender.make_header();
    out_seq = sender.commit_and_increment_out_seq();
    const std::array<uint8_t, 2> payload{{static_cast<uint8_t>(out_seq & 0xFFU), static_cast<uint8_t>((out_seq >> 8U) & 0xFFU)}};
    emit::BunchWriter writer(128);
    conn::begin_client_packet(writer, outer, notify, kTestCustomField, 512U);
    conn::BunchHeader bunch{};
    bunch.channel = 1U;
    bunch.reliable = true;
    bunch.ch_sequence = static_cast<uint16_t>(out_seq % 1024U);
    conn::write_bunch(writer, bunch, payload.data(), payload.size() * 8U);
    return conn::finish_data_packet(writer);
}

int drain_inbound_step(worldd::InboundNetRing& ring, conn::PacketNotify& sender, conn::PacketNotify& receiver,
                       std::array<bool, 2048>& accepted_seqs, uint32_t& duplicate_rejects) {
    worldd::InboundNetFrame frame{};
    while (ring.try_pop(frame)) {
        const auto parsed = conn::read_data_packet(frame.payload.data(), frame.len, wire::Direction::ClientToServer);
        SANDAL_ASSERT(parsed.has_value() && parsed->outer.magic_header == kMagicHeader);
        const auto bunches = conn::packet_bunches(frame.payload.data(), frame.len, *parsed);
        SANDAL_ASSERT(bunches.size() == 1U && bunches[0].bunch_data_bits == 16U);
        if (receiver.update(parsed->notify, [](uint16_t, bool) {}) <= 0) {
            ++duplicate_rejects;
            continue;
        }
        receiver.ack_seq(parsed->notify.seq);
        accepted_seqs[parsed->notify.seq] = true;
        const conn::NotifyHeader srv_hdr = receiver.make_header();
        const uint16_t srv_seq = receiver.commit_and_increment_out_seq();
        bool false_ack = false;
        const int srv_delta = sender.update(srv_hdr, [&](uint16_t acked_seq, bool delivered) {
            false_ack = false_ack || (delivered && !accepted_seqs[acked_seq]);
        });
        SANDAL_ASSERT(!false_ack && srv_delta > 0 && srv_seq <= conn::kSequenceMask);
        sender.ack_seq(srv_hdr.seq);
    }
    return 0;
}

int verify_tail_recovery(worldd::InboundNetRing& in_ring, conn::PacketNotify& sender, conn::PacketNotify& receiver,
                         std::array<bool, 2048>& accepted_seqs, const std::array<bool, 2048>& hard_dropped,
                         uint32_t& duplicate_rejects) {
    const worldd::Address peer{0x7F000001U, 7777U};
    conn::SequenceHistory tail_history{};
    uint16_t last_tail_seq = 0U;
    for (uint32_t tail = 0U; tail < 32U; ++tail) {
        const std::vector<uint8_t> clean = build_client_datagram(sender, last_tail_seq);
        tail_history.add_delivery_status(true);
        SANDAL_ASSERT(in_ring.try_push(worldd::InboundNetFrame::make(peer, clean.data(), clean.size())));
        SANDAL_ASSERT(drain_inbound_step(in_ring, sender, receiver, accepted_seqs, duplicate_rejects) == 0);
    }
    for (uint16_t seq = 1U; seq <= 1000U; ++seq) {
        SANDAL_ASSERT(!hard_dropped[seq] || !accepted_seqs[seq]);
    }
    const conn::NotifyHeader synced = receiver.make_header();
    SANDAL_ASSERT(receiver.in_seq() == last_tail_seq && receiver.in_ack_seq() == last_tail_seq);
    SANDAL_ASSERT(synced.history[0] == 0xFFFFFFFFU && tail_history.word(0) == 0xFFFFFFFFU);
    SANDAL_ASSERT(synced.is_seq_delivered(last_tail_seq) && in_ring.dropped_count() == 0U);
    return 0;
}

int test_impairment_simulator_1000_frames() {
    sandal::test::DeterministicPrng prng(0x5631554450434841ULL);
    conn::PacketNotify sender{}, receiver{};
    sender.init(99U, 1U);
    receiver.init(0U, 100U);
    worldd::InboundNetRing in_ring;
    const worldd::Address peer{0x7F000001U, 7777U};
    std::vector<DelayedDatagram> delay_queue{};
    std::array<bool, 2048> hard_dropped{}, accepted_seqs{};
    uint32_t stats[4]{0U, 0U, 0U, 0U};
    for (uint32_t step = 0U; step < 1000U; ++step) {
        uint16_t pkt_seq = 0U;
        const std::vector<uint8_t> dgram = build_client_datagram(sender, pkt_seq);
        const uint32_t roll = prng.next_u32(100U);
        if (roll < 15U) {
            hard_dropped[pkt_seq] = true;
            ++stats[0];
        } else if (roll < 25U) {
            delay_queue.push_back(DelayedDatagram{step + 1U + prng.next_u32(4U), dgram});
            ++stats[1];
        } else if (roll < 30U) {
            SANDAL_ASSERT(in_ring.try_push(worldd::InboundNetFrame::make(peer, dgram.data(), dgram.size())));
            SANDAL_ASSERT(in_ring.try_push(worldd::InboundNetFrame::make(peer, dgram.data(), dgram.size())));
            ++stats[2];
        } else {
            SANDAL_ASSERT(in_ring.try_push(worldd::InboundNetFrame::make(peer, dgram.data(), dgram.size())));
        }
        for (auto& item : delay_queue) {
            if (item.deliver_step <= step && !item.bytes.empty()) {
                SANDAL_ASSERT(in_ring.try_push(worldd::InboundNetFrame::make(peer, item.bytes.data(), item.bytes.size())));
                item.bytes.clear();
            }
        }
        SANDAL_ASSERT(drain_inbound_step(in_ring, sender, receiver, accepted_seqs, stats[3]) == 0);
    }
    for (auto& item : delay_queue) {
        if (!item.bytes.empty()) {
            SANDAL_ASSERT(in_ring.try_push(worldd::InboundNetFrame::make(peer, item.bytes.data(), item.bytes.size())));
        }
    }
    SANDAL_ASSERT(drain_inbound_step(in_ring, sender, receiver, accepted_seqs, stats[3]) == 0);
    SANDAL_ASSERT(stats[0] > 100U && stats[1] > 50U && stats[2] > 20U && stats[3] > 20U);
    return verify_tail_recovery(in_ring, sender, receiver, accepted_seqs, hard_dropped, stats[3]);
}

int test_ring_backpressure_and_corrupt_rejection() {
    auto burst_ring = std::make_unique<worldd::SpscRing<worldd::InboundNetFrame, 4096>>();
    const worldd::Address peer{0x0A000001U, 9000U};
    conn::PacketNotify sender{};
    sender.init(0U, 1U);
    uint16_t seq = 0U;
    const std::vector<uint8_t> valid = build_client_datagram(sender, seq);
    for (std::size_t i = 0U; i < burst_ring->capacity(); ++i) {
        SANDAL_ASSERT(burst_ring->try_push(worldd::InboundNetFrame::make(peer, valid.data(), valid.size())));
    }
    SANDAL_ASSERT(burst_ring->size_approx() == 4096U && burst_ring->dropped_count() == 0U);
    SANDAL_ASSERT(!burst_ring->try_push(worldd::InboundNetFrame::make(peer, valid.data(), valid.size())));
    SANDAL_ASSERT(burst_ring->dropped_count() == 1U && burst_ring->size_approx() == 4096U);
    std::vector<uint8_t> zero_tail = valid;
    zero_tail.back() = 0x00U;
    SANDAL_ASSERT(!conn::read_data_packet(zero_tail.data(), zero_tail.size(), wire::Direction::ClientToServer).has_value());
    SANDAL_ASSERT(wire::strip_termination(nullptr, 0U) == 0U);
    std::vector<uint8_t> bad_hdr = valid;
    bad_hdr[4] |= static_cast<uint8_t>(1U << 5U);
    SANDAL_ASSERT(!conn::read_data_packet(bad_hdr.data(), bad_hdr.size(), wire::Direction::ClientToServer).has_value());
    return 0;
}

int run_v1_udp_jitter_loss(const sandal::test::DataFreeConfig&) {
    return (test_impairment_simulator_1000_frames() == 0 && test_ring_backpressure_and_corrupt_rejection() == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    return sandal::test::run_case_dispatch(argc, argv, "v1_udp_jitter_loss", run_v1_udp_jitter_loss);
}
