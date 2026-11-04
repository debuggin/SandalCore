// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "include/aocnet/mesh_envelope.h"
#include "protocol/conn/packet_framing.h"
#include "protocol/conn/packet_notify.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_reader.h"

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

struct ReferenceBitstreams {
    std::vector<uint8_t> framed_packet{};
    std::vector<uint8_t> notify_bytes{};
    std::size_t notify_bits{0U};
    std::vector<uint8_t> subbyte_stream{};
    std::array<uint8_t, aocnet::kMeshForwardHeaderBytes> mesh_bytes{};
};

ReferenceBitstreams make_reference_bitstreams() {
    ReferenceBitstreams ref{};
    const conn::OuterHeader outer{0xC0DEC0DEU, 2U, 5U};
    const conn::NotifyHeader notify{777U, 770U, 1U, {0xA5A55A5AU}};
    const conn::CustomField cf{{1U, 2U, 3U, 4U, 5U, 6U}};
    const std::array<uint8_t, 4> payload{{0xDE, 0xAD, 0xBE, 0xEF}};

    emit::BunchWriter pkt_writer(128);
    conn::begin_client_packet(pkt_writer, outer, notify, cf, 333U);
    conn::BunchHeader bunch{};
    bunch.channel = 2U;
    bunch.reliable = true;
    bunch.ch_sequence = 19U;
    conn::write_bunch(pkt_writer, bunch, payload.data(), payload.size() * 8U);
    ref.framed_packet = conn::finish_data_packet(pkt_writer);

    emit::BunchWriter notify_writer(32);
    conn::write_notify_header(notify_writer, notify);
    ref.notify_bytes = notify_writer.bytes();
    ref.notify_bits = notify_writer.bit_pos();

    emit::BunchWriter sub_writer(64);
    sub_writer.write_bits(0x15ULL, 5);
    sub_writer.write_serialize_int(29U, 64U);
    sub_writer.write_sip(16385ULL);
    sub_writer.write_fstring_ansi("Sandal");
    sub_writer.write_bit(1);
    ref.subbyte_stream.assign(sub_writer.data(), sub_writer.data() + sub_writer.byte_size());

    const aocnet::MeshForwardHeader mesh_hdr{101U, 202U, 99U};
    if (!aocnet::pack_mesh_forward_header(mesh_hdr, ref.mesh_bytes)) {
        ref.mesh_bytes.fill(0U);
    }
    return ref;
}

int verify_reference_roundtrip(const ReferenceBitstreams& ref) {
    const auto parsed = conn::read_data_packet(ref.framed_packet.data(), ref.framed_packet.size(), wire::Direction::ClientToServer);
    SANDAL_ASSERT(parsed.has_value() && parsed->outer.magic_header == 0xC0DEC0DEU);
    SANDAL_ASSERT(parsed->outer.session_id == 2U && parsed->outer.client_id == 5U);
    SANDAL_ASSERT(parsed->notify.seq == 777U && parsed->notify.acked_seq == 770U);
    const auto bunches = conn::packet_bunches(ref.framed_packet.data(), ref.framed_packet.size(), *parsed);
    SANDAL_ASSERT(bunches.size() == 1U && bunches[0].bunch_data_bits == 32U);

    std::size_t notify_off = 0U;
    const auto dec_notify = conn::read_notify_header(ref.notify_bytes.data(), ref.notify_bytes.size(), ref.notify_bits, notify_off);
    SANDAL_ASSERT(dec_notify.has_value() && dec_notify->seq == 777U && dec_notify->history[0] == 0xA5A55A5AU);

    const std::size_t eff_bits = wire::strip_termination(ref.subbyte_stream.data(), ref.subbyte_stream.size());
    wire::PacketReader reader(ref.subbyte_stream.data(), ref.subbyte_stream.size(), eff_bits);
    SANDAL_ASSERT(reader.read_bits(5) == 0x15ULL && reader.read_serialize_int(64U) == 29U);
    const auto sip = reader.read_sip();
    const auto str = reader.read_fstring(32);
    SANDAL_ASSERT(sip.has_value() && *sip == 16385ULL && str.has_value() && *str == "Sandal");
    SANDAL_ASSERT(reader.remaining_bits() == 0U && !reader.overflowed());

    aocnet::MeshForwardHeader dec_mesh{};
    SANDAL_ASSERT(aocnet::unpack_mesh_forward_header(ref.mesh_bytes.data(), ref.mesh_bytes.size(), dec_mesh));
    SANDAL_ASSERT(dec_mesh.source_zone_id == 101U && dec_mesh.target_zone_id == 202U);
    SANDAL_ASSERT(dec_mesh.authority_epoch == 99U && aocnet::is_fenced_epoch_valid(dec_mesh.authority_epoch, 98U));
    return 0;
}

int verify_explicit_truncation_and_length_bombs(const ReferenceBitstreams& ref) {
    SANDAL_ASSERT(!conn::read_data_packet(nullptr, 0U, wire::Direction::ClientToServer).has_value());
    SANDAL_ASSERT(wire::strip_termination(nullptr, 0U) == 0U);
    std::size_t off = 0U;
    SANDAL_ASSERT(!conn::read_notify_header(nullptr, 0U, 0U, off).has_value());
    aocnet::MeshForwardHeader mesh_out{};
    SANDAL_ASSERT(!aocnet::unpack_mesh_forward_header(nullptr, 0U, mesh_out));
    for (std::size_t prefix_len = 0U; prefix_len < aocnet::kMeshForwardHeaderBytes; ++prefix_len) {
        SANDAL_ASSERT(!aocnet::unpack_mesh_forward_header(ref.mesh_bytes.data(), prefix_len, mesh_out));
    }
    const std::array<uint8_t, 4> fstring_bomb{{0xFFU, 0xFFU, 0xFFU, 0x7FU}};
    wire::PacketReader bomb_reader(fstring_bomb.data(), fstring_bomb.size());
    SANDAL_ASSERT(!bomb_reader.read_fstring(64).has_value() && bomb_reader.read_bits(65) == 0ULL && bomb_reader.overflowed());
    return 0;
}

int run_langsec_fuzz_2000(const ReferenceBitstreams& ref) {
    sandal::test::DeterministicPrng prng(0x563342495446555AULL);
    uint32_t rejected_mutations = 0U;
    for (uint32_t iter = 0U; iter < 2000U; ++iter) {
        std::vector<uint8_t> mut_pkt = ref.framed_packet;
        std::vector<uint8_t> mut_sub = ref.subbyte_stream;
        const uint32_t mode = iter % 5U;
        if (mode == 0U) {
            const std::size_t cut = prng.next_u32(12U);
            mut_pkt.resize(cut);
            aocnet::MeshForwardHeader mh{};
            SANDAL_ASSERT(!aocnet::unpack_mesh_forward_header(ref.mesh_bytes.data(), cut, mh));
        } else if (mode == 1U) {
            mut_pkt[prng.next_u32(static_cast<uint32_t>(mut_pkt.size()))] ^= static_cast<uint8_t>(1U << prng.next_u32(8U));
            mut_sub[prng.next_u32(static_cast<uint32_t>(mut_sub.size()))] ^= static_cast<uint8_t>(1U << prng.next_u32(8U));
        } else if (mode == 2U) {
            for (uint32_t flip = 0U; flip < 4U; ++flip) {
                mut_pkt[prng.next_u32(static_cast<uint32_t>(mut_pkt.size()))] ^= 0x5AU;
                mut_sub[prng.next_u32(static_cast<uint32_t>(mut_sub.size()))] ^= 0xA5U;
            }
        } else if (mode == 3U) {
            mut_pkt.back() = 0x00U;
        } else {
            const std::size_t eff = wire::strip_termination(mut_sub.data(), mut_sub.size());
            wire::PacketReader over_reader(mut_sub.data(), mut_sub.size(), eff > 24U ? 24U : eff);
            SANDAL_ASSERT(over_reader.read_bits(32) == 0ULL && over_reader.overflowed());
        }
        const auto pkt_res = conn::read_data_packet(mut_pkt.data(), mut_pkt.size(), wire::Direction::ClientToServer);
        if (!pkt_res.has_value()) {
            ++rejected_mutations;
        } else {
            SANDAL_ASSERT(conn::packet_bunches(mut_pkt.data(), mut_pkt.size(), *pkt_res).size() <= 64U);
        }
        std::size_t notify_off = 0U;
        if (!conn::read_notify_header(mut_sub.data(), mut_sub.size(), prng.next_u32(80U), notify_off).has_value()) {
            ++rejected_mutations;
        }
        wire::PacketReader sub_r(mut_sub.data(), mut_sub.size(), wire::strip_termination(mut_sub.data(), mut_sub.size()));
        if (sub_r.read_bits(5) != 0x15ULL || !sub_r.read_sip().has_value() || !sub_r.read_fstring(64).has_value()) {
            ++rejected_mutations;
        }
    }
    SANDAL_ASSERT(rejected_mutations >= 1500U);
    return 0;
}

int run_v3_mutated_bitstreams(const sandal::test::DataFreeConfig&) {
    const ReferenceBitstreams ref = make_reference_bitstreams();
    return (verify_reference_roundtrip(ref) == 0 && verify_explicit_truncation_and_length_bombs(ref) == 0 &&
            run_langsec_fuzz_2000(ref) == 0 && verify_reference_roundtrip(ref) == 0)
               ? 0
               : 1;
}

}  // namespace

int main(int argc, char** argv) {
    return sandal::test::run_case_dispatch(argc, argv, "v3_mutated_bitstreams", run_v3_mutated_bitstreams);
}
