#include "../world/test_harness.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/move_packed.h"
#include "protocol/wire/packet_reader.h"

namespace {

struct FuzzPrng {
    uint64_t state = 0x9E3779B97F4A7C15ULL;

    uint64_t next_u64() {
        uint64_t x = state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state = x;
        return x * 0x2545F4914F6CDD1DULL;
    }

    uint32_t next_u32(uint32_t bound) {
        return bound == 0 ? 0u : static_cast<uint32_t>(next_u64() % bound);
    }
};

void probe_wire_primitives(const uint8_t* data, size_t size) {
    aoc::protocol::wire::PacketReader r(data, size);
    (void)r.read_bit();
    (void)r.read_bits(17);
    (void)r.read_serialize_int(128);
    (void)r.read_sip();
    (void)r.read_fstring(64);
}

void probe_repl_codecs(const uint8_t* data, size_t size) {
    aoc::protocol::wire::PacketReader r_str(data, size);
    (void)aoc::protocol::repl::read_fstring(r_str);

    aoc::protocol::wire::PacketReader r_name(data, size);
    (void)aoc::protocol::repl::read_name(r_name);

    aoc::protocol::wire::PacketReader r_obj(data, size);
    auto val = aoc::protocol::repl::read_object_value(r_obj, true);
    if (!val.has_value()) {
        return;
    }
    const auto* inl = val->get_if<aoc::protocol::repl::InlineObject>();
    if (inl != nullptr && aoc::protocol::repl::inline_object_fits(*inl)) {
        aoc::protocol::emit::BunchWriter bw;
        (void)aoc::protocol::repl::write_inline_object(bw, *inl);
    }
}

bool verify_move_packed_roundtrip(const aoc::protocol::repl::MovePacked& move) {
    aoc::protocol::emit::BunchWriter bw;
    if (!aoc::protocol::repl::write_move_packed(bw, move)) {
        return false;
    }
    auto roundtrip = aoc::protocol::repl::read_move_packed(
        bw.data(), bw.byte_size(), 0, bw.bit_pos());
    if (!roundtrip.has_value() || !(*roundtrip == move)) {
        return false;
    }
    aoc::protocol::emit::BunchWriter bw2;
    if (!aoc::protocol::repl::write_move_packed(bw2, *roundtrip)) {
        return false;
    }
    return bw2.bit_pos() == bw.bit_pos() && bw2.bytes() == bw.bytes();
}

void probe_move_packed_codec(const uint8_t* data, size_t size) {
    auto move = aoc::protocol::repl::read_move_packed(data, size, 0, size * 8);
    if (move.has_value()) {
        (void)verify_move_packed_roundtrip(*move);
    }
}

aoc::protocol::repl::MovePacked make_seed_move() {
    aoc::protocol::repl::MovePacked m{};
    m.new_move.time_stamp = 10.25f;
    m.new_move.acceleration = aoc::protocol::repl::quantize_vector(10, 12.0, -4.5, 0.0);
    m.new_move.location = aoc::protocol::repl::quantize_vector(100, 1024.5, -2048.25, 312.0);
    m.new_move.control_rotation = aoc::protocol::repl::Rot{0, 0x4000u, 0};
    m.new_move.compressed_flags = 0x03u;
    m.new_move.base = aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(77);
    m.new_move.base_bone = aoc::protocol::repl::NameValue::hardcoded_name(0);
    m.new_move.movement_mode = aoc::protocol::repl::kMoveFalling;
    m.new_move.input_bits = 0x05u;
    aoc::protocol::repl::NetworkMove pending{};
    pending.time_stamp = 10.08f;
    pending.location = aoc::protocol::repl::quantize_vector(100, 1018.0, -2044.0, 310.0);
    m.pending = pending;
    aoc::protocol::repl::NetworkMove old{};
    old.time_stamp = 9.92f;
    old.location = aoc::protocol::repl::quantize_vector(100, 1010.0, -2040.0, 308.0);
    m.old = old;
    return m;
}

int test_farchive_varint_and_bit_bounds_fuzz() {
    aoc::protocol::wire::PacketReader r_empty(nullptr, 0);
    SANDAL_ASSERT(r_empty.read_bit() == 0 && r_empty.overflowed());

    const std::array<uint8_t, 9> nine_bytes = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
    aoc::protocol::wire::PacketReader r_64(nine_bytes.data(), nine_bytes.size(), 65);
    SANDAL_ASSERT(r_64.read_bits(64) == ~uint64_t{0} && !r_64.overflowed());
    SANDAL_ASSERT(r_64.read_bit() == 1 && r_64.remaining_bits() == 0);

    aoc::protocol::wire::PacketReader r_65(nine_bytes.data(), nine_bytes.size());
    SANDAL_ASSERT(r_65.read_bits(65) == 0 && r_65.overflowed());

    const std::array<uint8_t, 5> sip_overflow = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    aoc::protocol::wire::PacketReader r_sip(sip_overflow.data(), sip_overflow.size());
    SANDAL_ASSERT(!r_sip.read_sip().has_value() && r_sip.overflowed());

    const std::array<uint8_t, 10> sip_10byte_overflow = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    aoc::protocol::wire::PacketReader r_sip10(
        sip_10byte_overflow.data(), sip_10byte_overflow.size());
    SANDAL_ASSERT(!r_sip10.read_sip().has_value() && r_sip10.overflowed());

    const std::array<uint8_t, 4> len_bomb = {0xFF, 0xFF, 0xFF, 0x7F};
    aoc::protocol::wire::PacketReader r_bomb(len_bomb.data(), len_bomb.size());
    SANDAL_ASSERT(!aoc::protocol::repl::read_fstring(r_bomb).has_value());

    FuzzPrng prng{};
    std::vector<uint8_t> buf(96, 0);
    for (int i = 0; i < 2000; ++i) {
        const size_t len = prng.next_u32(static_cast<uint32_t>(buf.size() + 1));
        for (size_t j = 0; j < len; ++j) {
            buf[j] = static_cast<uint8_t>(prng.next_u64() & 0xFFu);
        }
        probe_wire_primitives(buf.data(), len);
        probe_repl_codecs(buf.data(), len);
        probe_move_packed_codec(buf.data(), len);
    }
    return 0;
}

int test_bunch_and_move_packed_langsec_fuzz() {
    const auto seed_move = make_seed_move();
    SANDAL_ASSERT(verify_move_packed_roundtrip(seed_move));

    aoc::protocol::emit::BunchWriter move_bw;
    SANDAL_ASSERT(aoc::protocol::repl::write_move_packed(move_bw, seed_move));
    for (size_t cut = 0; cut < move_bw.bit_pos(); cut += 7) {
        auto truncated = aoc::protocol::repl::read_move_packed(
            move_bw.data(), move_bw.byte_size(), 0, cut);
        if (cut < 8) {
            SANDAL_ASSERT(!truncated.has_value());
        }
    }

    FuzzPrng prng{0xD1B54A32D192ED03ULL};
    std::vector<uint8_t> mutated = move_bw.bytes();
    for (int i = 0; i < 500; ++i) {
        mutated = move_bw.bytes();
        const size_t idx = prng.next_u32(static_cast<uint32_t>(mutated.size()));
        mutated[idx] ^= static_cast<uint8_t>(1u << prng.next_u32(8));
        const size_t bit_limit = prng.next_u32(static_cast<uint32_t>(move_bw.bit_pos() + 1));
        auto cand = aoc::protocol::repl::read_move_packed(
            mutated.data(), mutated.size(), 0, bit_limit);
        if (cand.has_value()) {
            SANDAL_ASSERT(verify_move_packed_roundtrip(*cand));
        }
    }
    return 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    probe_wire_primitives(data, size);
    probe_repl_codecs(data, size);
    probe_move_packed_codec(data, size);
    return 0;
}

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    if (sandal::test::should_run_case(cfg, "farchive_varint_and_bit_bounds_fuzz")) {
        if (test_farchive_varint_and_bit_bounds_fuzz() != 0) {
            return 1;
        }
    }
    if (sandal::test::should_run_case(cfg, "bunch_and_move_packed_langsec_fuzz")) {
        if (test_bunch_and_move_packed_langsec_fuzz() != 0) {
            return 1;
        }
    }
    return 0;
}
