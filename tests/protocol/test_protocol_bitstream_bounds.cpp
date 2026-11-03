#include "../world/test_harness.h"

#include "protocol/emit/actor_bunch.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/channeling_codec.h"
#include "protocol/wire/bunch_reassembler.h"
#include "protocol/wire/bunch_types.h"
#include "protocol/wire/packet_parser.h"
#include "protocol/wire/packet_reader.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

namespace wire = aoc::protocol::wire;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;

int test_channeling_codec_roundtrip_and_bounds() {
    repl::ChannelingData expected{};
    expected.target.ObjectId = 2U;
    expected.target.ServerId = 9U;
    expected.flags = 6U;
    expected.active = true;
    expected.start_time = 1234.5;
    expected.timed = true;
    expected.duration = 4.25F;
    expected.tool_record = 0x1020304050LL;
    expected.tool_guid = repl::GuidValue{1U, 2U, 3U, 4U};
    SANDAL_ASSERT(repl::channeling_data_fits(expected));

    repl::ChannelingData bad_clear = expected;
    bad_clear.timed = false;
    SANDAL_ASSERT(!repl::channeling_data_fits(bad_clear));

    emit::BunchWriter bw{};
    SANDAL_ASSERT(repl::write_channeling_data(bw, expected));

    wire::PacketReader pr(bw.data(), bw.byte_size(), bw.bit_pos());
    const auto decoded = repl::read_channeling_data(pr);
    SANDAL_ASSERT(decoded.has_value() && *decoded == expected);
    SANDAL_ASSERT(pr.remaining_bits() == 0U && !pr.overflowed());

    wire::PacketReader truncated(bw.data(), bw.byte_size(), bw.bit_pos() - 8U);
    SANDAL_ASSERT(!repl::read_channeling_data(truncated).has_value());
    return 0;
}

int test_bit_reader_varint_and_tail_bounds() {
    emit::BunchWriter bw{};
    bw.write_sip(42U);
    bw.write_sip(257U);
    bw.write_serialize_int(19U, 32U);
    bw.write_bits(21ULL, 5);

    wire::PacketReader reader(bw.data(), bw.byte_size(), bw.bit_pos());
    SANDAL_ASSERT(reader.read_sip() == 42U);
    SANDAL_ASSERT(reader.read_sip() == 257U);

    const auto saved = reader.save();
    SANDAL_ASSERT(reader.read_serialize_int(32U) == 19U);
    reader.restore(saved);
    SANDAL_ASSERT(reader.read_serialize_int(32U) == 19U);

    SANDAL_ASSERT(reader.remaining_bits() == 5U && !reader.overflowed());
    SANDAL_ASSERT(reader.read_bits(5) == 21ULL);
    SANDAL_ASSERT(reader.remaining_bits() == 0U && !reader.overflowed());

    (void)reader.read_bit();
    SANDAL_ASSERT(reader.overflowed());
    return test_channeling_codec_roundtrip_and_bounds();
}

wire::ParsedBunch make_partial_fragment(
    uint32_t seq, bool initial, bool final_frag, size_t start_bit, uint32_t bits) {
    wire::ParsedBunch b{};
    b.channel = 3U;
    b.ch_sequence = seq;
    b.is_reliable = true;
    b.is_partial = true;
    b.partial_initial = initial;
    b.partial_final = final_frag;
    b.data_start_bit = start_bit;
    b.bunch_data_bits = bits;
    return b;
}

int test_partial_bunch_reassembler() {
    const uint8_t raw1[] = {0xAAU};
    wire::ParsedPacket pkt1{};
    pkt1.seq = 10U;
    pkt1.bunches.push_back(make_partial_fragment(100U, true, false, 0U, 8U));

    const uint8_t raw2[] = {0x12U, 0x34U, 0x05U, 0x7FU};
    wire::ParsedPacket pkt2{};
    pkt2.seq = 11U;
    pkt2.bunches.push_back(make_partial_fragment(101U, true, false, 0U, 8U));
    pkt2.bunches.push_back(make_partial_fragment(102U, false, false, 8U, 8U));
    pkt2.bunches.push_back(make_partial_fragment(103U, false, true, 16U, 4U));
    wire::ParsedBunch bogus = make_partial_fragment(200U, true, true, 24U, 7U);
    bogus.channel = 5U;
    pkt2.bunches.push_back(bogus);
    SANDAL_ASSERT(pkt2.bunches[3].is_complete_bunch() && !pkt2.bunches[0].is_complete_bunch());

    const std::vector<wire::ReassemblyInput> inputs{
        {&pkt1, raw1, sizeof(raw1)},
        {&pkt2, raw2, sizeof(raw2)},
    };
    const auto res = wire::reassemble_partial_bunches(inputs);
    SANDAL_ASSERT(res.bunches.size() == 1U && res.skip_set.size() == 3U);
    SANDAL_ASSERT(res.bunches[0].header.channel == 3U && res.bunches[0].total_bits == 20U);
    SANDAL_ASSERT(res.bunches[0].data[0] == 0x12U && res.bunches[0].data[1] == 0x34U);
    SANDAL_ASSERT((res.bunches[0].data[2] & 0x0FU) == 0x05U);
    return 0;
}

int test_bunch_header_framing_roundtrip() {
    emit::BunchWriter payload{};
    payload.write_uint32(0xCAFEBABEU);
    payload.write_bits(0x13U, 5);

    emit::BunchWriter packet_stream{};
    emit::write_reliable_actor_bunch(
        packet_stream, 7U, 42U, true, payload.data(), 0U, payload.bit_pos());

    wire::PacketReader hdr_reader(
        packet_stream.data(), packet_stream.byte_size(), packet_stream.bit_pos());
    const auto hdr = wire::parse_bunch_header(hdr_reader, packet_stream.bit_pos());
    SANDAL_ASSERT(hdr.has_value() && hdr->is_complete_bunch());
    SANDAL_ASSERT(hdr->channel == 7U && hdr->ch_sequence == 42U);
    SANDAL_ASSERT(hdr->is_reliable && hdr->has_package_map_exports);
    SANDAL_ASSERT(hdr->bunch_data_bits == 37U);

    wire::PacketReader payload_reader(
        packet_stream.data(),
        packet_stream.byte_size(),
        hdr->data_start_bit + hdr->bunch_data_bits);
    payload_reader.set_pos(hdr->data_start_bit);
    SANDAL_ASSERT(payload_reader.read_bits(32) == 0xCAFEBABEU);
    SANDAL_ASSERT(payload_reader.read_bits(5) == 0x13U);

    std::vector<wire::ParsedBunch> walked_bunches{};
    const auto walked = wire::walk_bunches_ex(
        packet_stream.data(),
        packet_stream.byte_size(),
        0U,
        packet_stream.bit_pos(),
        [&](const wire::ParsedBunch& b) { walked_bunches.push_back(b); });
    SANDAL_ASSERT(walked.complete && walked.visited == 1U && walked_bunches.size() == 1U);
    SANDAL_ASSERT(walked_bunches[0].is_complete_bunch());
    return test_partial_bunch_reassembler();
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "bit_reader_varint_and_tail_bounds")) {
        if (test_bit_reader_varint_and_tail_bounds() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "bunch_header_framing_roundtrip")) {
        if (test_bunch_header_framing_roundtrip() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
