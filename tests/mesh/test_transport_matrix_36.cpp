// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "include/aocnet/mesh_envelope.h"
#include "protocol/conn/packet_framing.h"
#include "protocol/conn/packet_notify.h"
#include "world/mesh_border.h"

namespace {

using namespace aoc::protocol::conn;
using namespace aoc::worldd::world;

int verify_notify_and_mesh_header_roundtrip() {
    const aocnet::MeshForwardHeader hdr_in{101u, 202u, 77u};
    std::array<std::uint8_t, aocnet::kMeshForwardHeaderBytes> packed{};
    SANDAL_ASSERT(aocnet::pack_mesh_forward_header(hdr_in, packed));
    aocnet::MeshForwardHeader hdr_out{};
    SANDAL_ASSERT(aocnet::unpack_mesh_forward_header(packed.data(), packed.size(), hdr_out));
    SANDAL_ASSERT(hdr_out.source_zone_id == hdr_in.source_zone_id);
    SANDAL_ASSERT(hdr_out.target_zone_id == hdr_in.target_zone_id);
    SANDAL_ASSERT(hdr_out.authority_epoch == hdr_in.authority_epoch);

    SequenceHistory hist{};
    hist.add_delivery_status(true);
    hist.add_delivery_status(false);
    hist.add_delivery_status(true);
    NotifyHeader notify_in{1234u, 1230u, 1u, {hist.word(0)}};

    aoc::protocol::emit::BunchWriter writer(64);
    write_notify_header(writer, notify_in);
    std::size_t off = 0u;
    const auto notify_out = read_notify_header(writer.data(), writer.byte_size(), writer.bit_pos(), off);
    SANDAL_ASSERT(notify_out.has_value());
    SANDAL_ASSERT(notify_out->seq == notify_in.seq);
    SANDAL_ASSERT(notify_out->acked_seq == notify_in.acked_seq);
    SANDAL_ASSERT(notify_out->history[0] == notify_in.history[0]);
    return 0;
}

int frame_and_verify_axis_a(int state_a, const std::uint8_t* payload, std::size_t payload_len) {
    constexpr std::uint16_t kBaseSeq = 100u;
    const std::uint16_t seq = (state_a == 0) ? seq14_advance(kBaseSeq, 1)
                            : (state_a == 1) ? seq14_advance(kBaseSeq, 4)
                            : (state_a == 4) ? seq14_advance(0x3FFEu, 3)
                                             : seq14_advance(kBaseSeq, 2);
    const OuterHeader outer{0xA0C0u, 2u, 3u};
    SequenceHistory hist{};
    hist.add_delivery_status(state_a != 1);
    const NotifyHeader notify{seq, kBaseSeq, 1u, {hist.word(0)}};
    const CustomField cf{1u, 2u, 3u, 4u, 5u, 6u};

    aoc::protocol::emit::BunchWriter writer(128);
    begin_client_packet(writer, outer, notify, cf);
    BunchHeader bunch{};
    bunch.channel = 1u;
    bunch.reliable = true;
    bunch.ch_sequence = 1u;
    write_bunch(writer, bunch, payload, payload_len * 8u);
    std::vector<std::uint8_t> bytes = finish_data_packet(writer);

    if (state_a == 5) {
        bytes.back() = 0x00u;
        SANDAL_ASSERT(!read_data_packet(bytes.data(), bytes.size(), aoc::protocol::wire::Direction::ClientToServer).has_value());
        return 0;
    }
    const auto parsed = read_data_packet(bytes.data(), bytes.size(), aoc::protocol::wire::Direction::ClientToServer);
    SANDAL_ASSERT(parsed.has_value());
    const auto bunches = packet_bunches(bytes.data(), bytes.size(), *parsed);
    SANDAL_ASSERT(bunches.size() == 1u && bunches[0].bunch_data_bits == payload_len * 8u);

    if (state_a == 0) SANDAL_ASSERT(seq14_is_newer(seq, kBaseSeq) && parsed->notify.is_delivered(0));
    if (state_a == 1) SANDAL_ASSERT(seq_diff(seq, kBaseSeq) == 4 && !parsed->notify.is_delivered(0));
    if (state_a == 2) SANDAL_ASSERT(seq14_in_ack_window(seq, kBaseSeq + 4u) && !seq14_is_newer(seq, kBaseSeq + 4u));
    if (state_a == 3) SANDAL_ASSERT(!seq14_is_newer(seq, seq) && seq14_in_ack_window(seq, seq));
    if (state_a == 4) SANDAL_ASSERT(seq == 1u && seq14_is_newer(seq, 0x3FFEu));
    return 0;
}

int evaluate_and_verify_axis_b(int state_b, std::uint32_t active_epoch, const std::uint8_t* raw_hdr) {
    constexpr float kHystCm = 500.0f;
    aocnet::MeshForwardHeader unpacked{};
    if (state_b == 0) {
        SANDAL_ASSERT(aocnet::unpack_mesh_forward_header(raw_hdr, aocnet::kMeshForwardHeaderBytes, unpacked));
        SANDAL_ASSERT(evaluate_border_handover(-1200.0f, AuthorityState::Local, kHystCm) == AuthorityState::Local);
        SANDAL_ASSERT(aocnet::resolve_hysteresis_authority(-1200.0f, 1u, 1u, 2u, kHystCm) == 1u);
        SANDAL_ASSERT(aocnet::is_fenced_epoch_valid(unpacked.authority_epoch, active_epoch));
    } else if (state_b == 1) {
        SANDAL_ASSERT(aocnet::classify_boundary_hysteresis(-200.0f, kHystCm) == aocnet::MeshBorderBand::HysteresisBand);
        SANDAL_ASSERT(evaluate_border_handover(-200.0f, AuthorityState::Local, kHystCm) == AuthorityState::Local);
        SANDAL_ASSERT(aocnet::resolve_hysteresis_authority(-200.0f, 1u, 1u, 2u, kHystCm) == 1u);
    } else if (state_b == 2) {
        SANDAL_ASSERT(evaluate_border_handover(200.0f, AuthorityState::Local, kHystCm) == AuthorityState::Local);
        SANDAL_ASSERT(aocnet::resolve_hysteresis_authority(200.0f, 1u, 1u, 2u, kHystCm) == 1u);
    } else if (state_b == 3) {
        const auto pending = evaluate_border_handover(600.0f, AuthorityState::Local, kHystCm);
        SANDAL_ASSERT(pending == AuthorityState::HandoverPending);
        SANDAL_ASSERT(complete_border_handover(pending, active_epoch + 1u, active_epoch) == AuthorityState::RemoteGhost);
        SANDAL_ASSERT(aocnet::resolve_hysteresis_authority(600.0f, 1u, 1u, 2u, kHystCm) == 2u);
    } else if (state_b == 4) {
        SANDAL_ASSERT(!aocnet::is_fenced_epoch_valid(active_epoch - 1u, active_epoch));
        SANDAL_ASSERT(complete_border_handover(AuthorityState::HandoverPending, active_epoch, active_epoch) ==
                      AuthorityState::HandoverPending);
    } else {
        SANDAL_ASSERT(!aocnet::unpack_mesh_forward_header(raw_hdr, aocnet::kMeshForwardHeaderBytes - 1u, unpacked));
        SANDAL_ASSERT(!aocnet::is_fenced_epoch_valid(0u, active_epoch));
    }
    return 0;
}

int run_transport_matrix_36_case() {
    SANDAL_ASSERT(verify_notify_and_mesh_header_roundtrip() == 0);
    int cells_verified = 0;
    for (int axis_a = 0; axis_a < 6; ++axis_a) {
        for (int axis_b = 0; axis_b < 6; ++axis_b) {
            const std::uint32_t active_epoch = static_cast<std::uint32_t>(10 + axis_a * 6 + axis_b);
            const std::uint32_t hdr_epoch = (axis_b == 5) ? 0u
                                          : (axis_b == 4) ? (active_epoch - 1u)
                                                          : (active_epoch + 1u);
            std::array<std::uint8_t, aocnet::kMeshForwardHeaderBytes> packed_hdr{};
            SANDAL_ASSERT(aocnet::pack_mesh_forward_header({1u, 2u, hdr_epoch}, packed_hdr));
            SANDAL_ASSERT(frame_and_verify_axis_a(axis_a, packed_hdr.data(), packed_hdr.size()) == 0);
            SANDAL_ASSERT(evaluate_and_verify_axis_b(axis_b, active_epoch, packed_hdr.data()) == 0);
            ++cells_verified;
        }
    }
    SANDAL_ASSERT(cells_verified == 36);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto opts = sandal::test::parse_test_cli(argc, argv);
    if (sandal::test::should_run_case(opts, "transport_matrix_36")) {
        return run_transport_matrix_36_case();
    }
    return 0;
}
