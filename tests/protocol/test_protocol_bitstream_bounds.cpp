#include "../world/test_harness.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

struct BitStreamSlice {
    const uint8_t* data{nullptr};
    size_t total_bits{0};
    size_t cursor_bits{0};
    bool overflowed{false};
};

[[nodiscard]] constexpr size_t leftover_bits(const BitStreamSlice& s) noexcept {
    return (s.cursor_bits <= s.total_bits) ? (s.total_bits - s.cursor_bits) : 0U;
}

[[nodiscard]] bool read_bits(BitStreamSlice& s, size_t count, uint64_t& out) noexcept {
    if (s.overflowed || count == 0U || count > 64U || s.data == nullptr) {
        s.overflowed = true;
        return false;
    }
    if (leftover_bits(s) < count) {
        s.overflowed = true;
        return false;
    }
    uint64_t acc = 0ULL;
    for (size_t i = 0U; i < count; ++i) {
        const size_t bit_idx = s.cursor_bits + i;
        const uint8_t byte_val = s.data[bit_idx >> 3U];
        const uint64_t bit_val = (static_cast<uint64_t>(byte_val) >> (bit_idx & 7U)) & 1ULL;
        acc |= (bit_val << i);
    }
    s.cursor_bits += count;
    out = acc;
    return true;
}

[[nodiscard]] bool read_varint32(BitStreamSlice& s, uint32_t& out) noexcept {
    uint32_t result = 0U;
    for (uint32_t byte_idx = 0U; byte_idx < 5U; ++byte_idx) {
        uint64_t raw_byte = 0ULL;
        if (!read_bits(s, 8U, raw_byte)) {
            return false;
        }
        const uint32_t payload = static_cast<uint32_t>((raw_byte >> 1U) & 0x7FU);
        result |= (payload << (7U * byte_idx));
        if ((raw_byte & 1ULL) == 0ULL) {
            out = result;
            return true;
        }
    }
    s.overflowed = true;
    return false;
}

struct BunchHeader {
    uint16_t ch_index{0};
    uint16_t ch_sequence{0};
    uint16_t payload_bits{0};
    bool b_open{false};
    bool b_close{false};
    bool b_reliable{false};
    bool valid{false};
};

[[nodiscard]] constexpr bool encode_bunch_header(const BunchHeader& hdr, uint64_t& word) noexcept {
    constexpr uint16_t kMaxPayloadBits = 8192U;
    if (hdr.ch_index == 0xFFFFU || hdr.payload_bits > kMaxPayloadBits) {
        return false;
    }
    word = static_cast<uint64_t>(hdr.ch_index) |
           (static_cast<uint64_t>(hdr.ch_sequence & 0x3FFFU) << 16U) |
           (static_cast<uint64_t>(hdr.payload_bits) << 32U) |
           (static_cast<uint64_t>(hdr.b_open ? 1U : 0U) << 48U) |
           (static_cast<uint64_t>(hdr.b_close ? 1U : 0U) << 49U) |
           (static_cast<uint64_t>(hdr.b_reliable ? 1U : 0U) << 50U) |
           (1ULL << 63U);
    return true;
}

[[nodiscard]] constexpr BunchHeader decode_bunch_header(uint64_t word) noexcept {
    constexpr uint16_t kMaxPayloadBits = 8192U;
    if ((word >> 63U) == 0ULL) {
        return {};
    }
    BunchHeader hdr{};
    hdr.ch_index = static_cast<uint16_t>(word & 0xFFFFULL);
    hdr.ch_sequence = static_cast<uint16_t>((word >> 16U) & 0x3FFFULL);
    hdr.payload_bits = static_cast<uint16_t>((word >> 32U) & 0xFFFFULL);
    hdr.b_open = ((word >> 48U) & 1ULL) != 0ULL;
    hdr.b_close = ((word >> 49U) & 1ULL) != 0ULL;
    hdr.b_reliable = ((word >> 50U) & 1ULL) != 0ULL;
    hdr.valid = (hdr.ch_index != 0xFFFFU) && (hdr.payload_bits <= kMaxPayloadBits);
    return hdr;
}

int test_bit_reader_varint_and_tail_bounds() {
    sandal::test::ManualClock clock{0};
    constexpr std::array<uint8_t, 4> kWireBytes{0x54U, 0x03U, 0x04U, 0x15U};
    BitStreamSlice reader{kWireBytes.data(), 29U, 0U, false};

    uint32_t v0 = 0U;
    SANDAL_ASSERT(read_varint32(reader, v0));
    SANDAL_ASSERT(v0 == 42U && leftover_bits(reader) == 21U);

    uint32_t v1 = 0U;
    SANDAL_ASSERT(read_varint32(reader, v1));
    SANDAL_ASSERT(v1 == 257U && leftover_bits(reader) == 5U);

    uint64_t tail = 0ULL;
    SANDAL_ASSERT(read_bits(reader, 5U, tail));
    SANDAL_ASSERT(tail == 21ULL && leftover_bits(reader) == 0U && !reader.overflowed);

    clock.advance_ticks(1, 30);
    SANDAL_ASSERT(clock.now_ms() == 33);
    SANDAL_ASSERT(!read_bits(reader, 1U, tail) && reader.overflowed);

    constexpr std::array<uint8_t, 6> kBadVarint{0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x01U};
    BitStreamSlice bad_reader{kBadVarint.data(), 48U, 0U, false};
    uint32_t bad_out = 0U;
    SANDAL_ASSERT(!read_varint32(bad_reader, bad_out) && bad_reader.overflowed);
    return 0;
}

int test_bunch_header_framing_roundtrip() {
    sandal::test::ManualClock clock{1'000'000LL};
    const BunchHeader input{7U, 16383U, 8192U, true, false, true, true};
    uint64_t encoded = 0ULL;
    SANDAL_ASSERT(encode_bunch_header(input, encoded));

    clock.advance_ms(5);
    const BunchHeader decoded = decode_bunch_header(encoded);
    SANDAL_ASSERT(decoded.valid);
    SANDAL_ASSERT(decoded.ch_index == 7U);
    SANDAL_ASSERT(decoded.ch_sequence == 16383U);
    SANDAL_ASSERT(decoded.payload_bits == 8192U);
    SANDAL_ASSERT(decoded.b_open && !decoded.b_close && decoded.b_reliable);
    SANDAL_ASSERT(clock.now_ms() == 6);

    const BunchHeader oversized{7U, 1U, 8193U, false, false, false, true};
    SANDAL_ASSERT(!encode_bunch_header(oversized, encoded));

    const BunchHeader invalid_ch{0xFFFFU, 1U, 64U, false, false, false, true};
    SANDAL_ASSERT(!encode_bunch_header(invalid_ch, encoded));
    SANDAL_ASSERT(!decode_bunch_header(0ULL).valid);
    return 0;
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
