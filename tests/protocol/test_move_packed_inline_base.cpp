#include "../world/test_harness.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/move_packed.h"
#include "protocol/wire/packet_reader.h"

namespace {

constexpr uint8_t kExportHasPath = 0x01;
constexpr uint8_t kExportNoLoad = 0x02;
constexpr uint8_t kExportHasChecksum = 0x04;

int test_object_id_1_inline_movement_base_roundtrip() {
    aoc::protocol::repl::InlineObject inline_base{};
    inline_base.flags = static_cast<uint8_t>(kExportHasPath | kExportHasChecksum);
    inline_base.outer = aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(42);
    inline_base.path = "/Game/Maps/Verra/FloorMesh.StaticMeshComponent0";
    inline_base.checksum = 0xCAFEBABEu;
    SANDAL_ASSERT(aoc::protocol::repl::inline_object_fits(inline_base));

    aoc::protocol::emit::BunchWriter bw;
    SANDAL_ASSERT(aoc::protocol::repl::write_inline_object(bw, inline_base));

    aoc::protocol::wire::PacketReader r(bw.data(), bw.byte_size(), bw.bit_pos());
    auto val = aoc::protocol::repl::read_object_value(r, true);
    SANDAL_ASSERT(val.has_value() && !r.overflowed() && r.remaining_bits() == 0);
    const auto* decoded = val->get_if<aoc::protocol::repl::InlineObject>();
    SANDAL_ASSERT(decoded != nullptr && *decoded == inline_base);

    aoc::protocol::wire::PacketReader r_disallowed(bw.data(), bw.byte_size(), bw.bit_pos());
    SANDAL_ASSERT(!aoc::protocol::repl::read_object_value(r_disallowed, false).has_value());

    aoc::protocol::wire::PacketReader r_plain(bw.data(), bw.byte_size(), bw.bit_pos());
    SANDAL_ASSERT(!aoc::protocol::repl::read_object(r_plain).has_value());

    aoc::protocol::repl::InlineObject no_load{};
    no_load.flags = kExportNoLoad;
    SANDAL_ASSERT(aoc::protocol::repl::inline_object_fits(no_load));
    aoc::protocol::emit::BunchWriter bw_noload;
    SANDAL_ASSERT(aoc::protocol::repl::write_inline_object(bw_noload, no_load));
    aoc::protocol::wire::PacketReader r_noload(
        bw_noload.data(), bw_noload.byte_size(), bw_noload.bit_pos());
    auto val_noload = aoc::protocol::repl::read_object_value(r_noload, true);
    SANDAL_ASSERT(val_noload.has_value() && r_noload.remaining_bits() == 0);
    const auto* dec_noload = val_noload->get_if<aoc::protocol::repl::InlineObject>();
    SANDAL_ASSERT(dec_noload != nullptr && *dec_noload == no_load);
    return 0;
}

void write_raw_inline_header(aoc::protocol::emit::BunchWriter& bw, uint8_t flags) {
    aoc::protocol::emit::FIntrepidNetworkGUID sentinel{};
    sentinel.ObjectId = 1;
    aoc::protocol::emit::write_intrepid_guid(bw, sentinel);
    bw.write_uint8(flags);
}

int test_inline_movement_base_invalid_flags_rejected() {
    aoc::protocol::repl::InlineObject bad_flags{};
    bad_flags.flags = 0x08;
    SANDAL_ASSERT(!aoc::protocol::repl::inline_object_fits(bad_flags));

    aoc::protocol::emit::BunchWriter bw_unknown;
    write_raw_inline_header(bw_unknown, 0x08);
    aoc::protocol::wire::PacketReader r_unknown(
        bw_unknown.data(), bw_unknown.byte_size(), bw_unknown.bit_pos());
    SANDAL_ASSERT(!aoc::protocol::repl::read_object_value(r_unknown, true).has_value());

    aoc::protocol::repl::InlineObject checksum_without_path{};
    checksum_without_path.flags = kExportHasChecksum;
    checksum_without_path.checksum = 0x11223344u;
    SANDAL_ASSERT(!aoc::protocol::repl::inline_object_fits(checksum_without_path));

    aoc::protocol::emit::BunchWriter bw_chk;
    write_raw_inline_header(bw_chk, kExportHasChecksum);
    bw_chk.write_uint32(0x11223344u);
    aoc::protocol::wire::PacketReader r_chk(bw_chk.data(), bw_chk.byte_size(), bw_chk.bit_pos());
    SANDAL_ASSERT(!aoc::protocol::repl::read_object_value(r_chk, true).has_value());

    aoc::protocol::repl::InlineObject empty_path{};
    empty_path.flags = kExportHasPath;
    empty_path.outer = aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(99);
    SANDAL_ASSERT(!aoc::protocol::repl::inline_object_fits(empty_path));

    aoc::protocol::emit::BunchWriter bw_empty;
    write_raw_inline_header(bw_empty, kExportHasPath);
    aoc::protocol::emit::write_intrepid_guid(bw_empty, empty_path.outer);
    SANDAL_ASSERT(aoc::protocol::repl::write_fstring(bw_empty, ""));
    aoc::protocol::wire::PacketReader r_empty(
        bw_empty.data(), bw_empty.byte_size(), bw_empty.bit_pos());
    SANDAL_ASSERT(!aoc::protocol::repl::read_object_value(r_empty, true).has_value());

    aoc::protocol::repl::InlineObject recursive_outer{};
    recursive_outer.flags = kExportHasPath;
    recursive_outer.outer = aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(1);
    recursive_outer.path = "/Game/Maps/Verra/FloorMesh";
    SANDAL_ASSERT(!aoc::protocol::repl::inline_object_fits(recursive_outer));

    aoc::protocol::emit::BunchWriter bw_rec;
    write_raw_inline_header(bw_rec, kExportHasPath);
    aoc::protocol::emit::write_intrepid_guid(bw_rec, recursive_outer.outer);
    SANDAL_ASSERT(aoc::protocol::repl::write_fstring(bw_rec, recursive_outer.path));
    aoc::protocol::wire::PacketReader r_rec(bw_rec.data(), bw_rec.byte_size(), bw_rec.bit_pos());
    SANDAL_ASSERT(!aoc::protocol::repl::read_object_value(r_rec, true).has_value());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    if (sandal::test::should_run_case(cfg, "object_id_1_inline_movement_base_roundtrip")) {
        if (test_object_id_1_inline_movement_base_roundtrip() != 0) {
            return 1;
        }
    }
    if (sandal::test::should_run_case(cfg, "inline_movement_base_invalid_flags_rejected")) {
        if (test_inline_movement_base_invalid_flags_rejected() != 0) {
            return 1;
        }
    }
    return 0;
}
