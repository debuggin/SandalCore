// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "../world/test_harness.h"
#include "protocol/conn/packet_notify.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/move_packed.h"
#include "protocol/server_guid_allocator.h"
#include "protocol/wire/packet_reader.h"
#include <limits>

namespace {

constexpr uint8_t kExportHasPath = 0x01;
constexpr uint8_t kExportNoLoad = 0x02;
constexpr uint8_t kExportHasChecksum = 0x04;

aoc::protocol::repl::NetworkMove walking_move(float ts, double x, double y, double z) {
    aoc::protocol::repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = aoc::protocol::repl::quantize_vector(10, 0, 0, 0);
    m.location = aoc::protocol::repl::quantize_vector(100, x, y, z);
    m.control_rotation.yaw = 16384;
    return m;
}

bool same_bit_range(const uint8_t* a, const uint8_t* b, size_t bits) {
    for (size_t i = 0; i < bits; ++i) {
        if (((a[i >> 3] >> (i & 7)) & 1) != ((b[i >> 3] >> (i & 7)) & 1)) return false;
    }
    return true;
}

std::vector<uint8_t> written(const aoc::protocol::repl::MovePacked& p, size_t* bits) {
    aoc::protocol::emit::BunchWriter w;
    if (!aoc::protocol::repl::write_move_packed(w, p)) {
        *bits = 0;
        return {};
    }
    *bits = w.bit_pos();
    return std::vector<uint8_t>(w.data(), w.data() + w.byte_size());
}

int test_move_base_inline_bone_and_refusals(
    aoc::protocol::repl::MovePacked p,
    const aoc::protocol::emit::FIntrepidNetworkGUID& horse,
    const aoc::protocol::repl::InlineObject& cylinder) {
    using namespace aoc::protocol::repl;
    size_t bits = 0;
    p.new_move.base_bone = NameValue::plain("spine_01", 3);
    const auto with_bone = written(p, &bits);
    const auto rb = read_move_packed(with_bone.data(), with_bone.size(), 0, bits);
    SANDAL_ASSERT(rb.has_value());
    SANDAL_ASSERT(rb->new_move.base_bone.has_value());
    SANDAL_ASSERT(rb->new_move.base_bone->text == "spine_01");
    SANDAL_ASSERT(rb->new_move.base_inline.has_value());
    SANDAL_ASSERT(
        describe_base(rb->new_move) == "\"CollisionCylinder\" of object 17045704, bone spine_01");

    MovePacked both;
    both.new_move = walking_move(1.0f, 0, 0, 0);
    both.new_move.base = horse;
    both.new_move.base_inline = cylinder;
    aoc::protocol::emit::BunchWriter w;
    SANDAL_ASSERT(!write_move_packed(w, both) && w.bit_pos() == 0);

    MovePacked broken;
    broken.new_move = walking_move(1.0f, 0, 0, 0);
    broken.new_move.base_inline = cylinder;
    broken.new_move.base_inline->flags = kExportHasChecksum;
    aoc::protocol::emit::BunchWriter w2;
    SANDAL_ASSERT(!write_move_packed(w2, broken) && w2.bit_pos() == 0);
    return 0;
}

int test_move_base_inline() {
    using namespace aoc::protocol::repl;
    aoc::protocol::emit::FIntrepidNetworkGUID horse;
    horse.ObjectId = 17045704;
    horse.ServerId = 60;
    horse.Randomizer = 0x5a5a5a5a;
    InlineObject cylinder;
    cylinder.flags = static_cast<uint8_t>(kExportHasPath | kExportHasChecksum);
    cylinder.outer = horse;
    cylinder.path = "CollisionCylinder";
    cylinder.checksum = 0xb876932f;

    MovePacked p;
    p.new_move = walking_move(20.5f, 12.25, -30.5, 197.0);
    p.new_move.base_inline = cylinder;
    p.pending = walking_move(20.48f, 12.0, -30.0, 197.0);
    p.pending->base_inline = cylinder;
    p.old = walking_move(20.46f, 11.5, -29.5, 197.0);
    p.old->base = horse;

    size_t bits = 0;
    const auto data = written(p, &bits);
    SANDAL_ASSERT(bits > 0);
    std::string why;
    const auto r = read_move_packed(data.data(), data.size(), 0, bits, &why);
    SANDAL_ASSERT(r.has_value());
    SANDAL_ASSERT(r->new_move.base_inline && *r->new_move.base_inline == cylinder);
    SANDAL_ASSERT(!r->new_move.base && r->new_move.has_base());
    SANDAL_ASSERT((r->new_move.location_cm() == Vec3d{12.25, -30.5, 197.0}));
    SANDAL_ASSERT(r->new_move.time_stamp == 20.5f && r->new_move.control_rotation.yaw == 16384);
    SANDAL_ASSERT(r->pending && r->pending->base_inline && *r->pending->base_inline == cylinder);
    SANDAL_ASSERT(r->old && r->old->base && *r->old->base == horse && !r->old->base_inline);
    SANDAL_ASSERT(*r == p);

    size_t again_bits = 0;
    const auto again = written(*r, &again_bits);
    SANDAL_ASSERT(again_bits == bits && same_bit_range(again.data(), data.data(), bits));
    SANDAL_ASSERT(describe_base(r->new_move) == "\"CollisionCylinder\" of object 17045704");
    SANDAL_ASSERT(describe_base(*r->old) == "object 17045704");
    SANDAL_ASSERT(describe_base(walking_move(1, 0, 0, 0)) == "none");
    return test_move_base_inline_bone_and_refusals(p, horse, cylinder);
}

int test_move_packed_quantization_and_bounds() {
    using namespace aoc::protocol::repl;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    SANDAL_ASSERT(is_valid_inline_movement_base_object_id(1u));
    SANDAL_ASSERT(!is_valid_inline_movement_base_object_id(0u));
    SANDAL_ASSERT(!is_valid_inline_movement_base_object_id(2u));
    SANDAL_ASSERT(clamp_finite_component(nan, 500.0) == 0.0);
    SANDAL_ASSERT(clamp_finite_component(inf, 500.0) == 0.0);
    SANDAL_ASSERT(clamp_finite_component(-900.0, 500.0) == -500.0);

    const Vec3d clamped = clamp_move_vector(Vec3d{300.0, 400.0, 0.0}, 1000.0, 250.0);
    SANDAL_ASSERT(is_finite_vec3(clamped));
    SANDAL_ASSERT(clamped.x > 149.9 && clamped.x < 150.1);
    SANDAL_ASSERT(clamped.y > 199.9 && clamped.y < 200.1);

    SANDAL_ASSERT(quantize_rotation_axis_16(nan) == 0u);
    SANDAL_ASSERT(quantize_rotation_axis_16(180.0) == 32768u);
    SANDAL_ASSERT(quantize_rotation_axis_16(-90.0) == 49152u);
    SANDAL_ASSERT(dequantize_rotation_axis_16(32768u) == 180.0);

    MovePacked mp{};
    mp.new_move.time_stamp = 12.5f;
    mp.new_move.location = quantize_move_location_clamped(125.25, -450.5, 80.0);
    mp.new_move.acceleration = quantize_move_acceleration_clamped(10.5, 0.0, -5.0);
    mp.new_move.control_rotation = quantize_control_rotation(45.0, 90.0, 0.0);
    mp.new_move.base = aoc::protocol::emit::FIntrepidNetworkGUID{0x01000002ULL, 60u, 0x1234u};
    SANDAL_ASSERT(move_packed_is_finite_and_bounded(mp));

    aoc::protocol::emit::BunchWriter bw;
    SANDAL_ASSERT(write_move_packed(bw, mp));
    auto decoded = read_move_packed(bw.data(), bw.byte_size(), 0, bw.bit_pos());
    SANDAL_ASSERT(decoded.has_value() && *decoded == mp);

    MovePacked bad_base = mp;
    bad_base.new_move.base = aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(1u);
    SANDAL_ASSERT(!bad_base.new_move.has_valid_base());
    SANDAL_ASSERT(!move_packed_is_finite_and_bounded(bad_base));
    SANDAL_ASSERT(!write_move_packed(bw, bad_base));
    return 0;
}

int test_server_guid_allocator_pm52_and_shards() {
    using aoc::protocol::ServerGuidAllocator;
    ServerGuidAllocator alloc(77u, 0xA5A55A5Au);
    const auto g0 = alloc.next();
    const auto g1 = alloc.next();
    const auto g2 = alloc.next();
    SANDAL_ASSERT(g0.ObjectId == ServerGuidAllocator::kFirstObjectId);
    SANDAL_ASSERT(g1.ObjectId == g0.ObjectId + 2ULL);
    SANDAL_ASSERT(g2.ObjectId == g1.ObjectId + 2ULL);
    SANDAL_ASSERT(!aoc::protocol::has_pm52_inline_flag(g0.ObjectId));
    SANDAL_ASSERT(aoc::protocol::has_pm52_inline_flag(1ULL));
    SANDAL_ASSERT(aoc::protocol::is_server_allocated_guid(g0));
    SANDAL_ASSERT(!aoc::protocol::is_server_allocated_guid(1ULL));

    const uint64_t shard_guid = aoc::protocol::encode_shard_guid(0x1234u, 0x00ABCDEFULL);
    SANDAL_ASSERT(!aoc::protocol::has_pm52_inline_flag(shard_guid));
    SANDAL_ASSERT(aoc::protocol::extract_shard_id(shard_guid) == 0x1234u);
    SANDAL_ASSERT(aoc::protocol::extract_raw_counter(shard_guid) == 0x00ABCDEFULL);
    SANDAL_ASSERT(aoc::protocol::is_server_allocated_guid(shard_guid));

    ServerGuidAllocator near_max(1u, 2u, ServerGuidAllocator::kMaxObjectId - 2ULL);
    const auto pre_sat = near_max.next();
    const auto sat_0 = near_max.next();
    const auto sat_1 = near_max.next();
    SANDAL_ASSERT(pre_sat.ObjectId == ServerGuidAllocator::kMaxObjectId - 2ULL);
    SANDAL_ASSERT(sat_0.ObjectId == ServerGuidAllocator::kMaxObjectId);
    SANDAL_ASSERT(sat_1.ObjectId == ServerGuidAllocator::kMaxObjectId);
    SANDAL_ASSERT(!aoc::protocol::has_pm52_inline_flag(sat_1.ObjectId));
    return 0;
}

int test_packet_notify_14bit_wraparound() {
    using namespace aoc::protocol::conn;
    SANDAL_ASSERT(kSequenceMask == 0x3FFFu);
    SANDAL_ASSERT(seq14_advance(16383u, 1) == 0u);
    SANDAL_ASSERT(seq14_advance(0u, -1) == 16383u);
    SANDAL_ASSERT(seq14_distance(0u, 16383u) == 1);
    SANDAL_ASSERT(seq14_distance(16383u, 0u) == -1);
    SANDAL_ASSERT(seq14_is_newer(0u, 16383u));
    SANDAL_ASSERT(!seq14_is_newer(16383u, 0u));
    SANDAL_ASSERT(seq14_is_newer(8191u, 0u));
    SANDAL_ASSERT(!seq14_is_newer(8192u, 0u));
    SANDAL_ASSERT(seq14_in_ack_window(16383u, 2u, 32u));
    SANDAL_ASSERT(!seq14_in_ack_window(3u, 2u, 32u));

    PacketNotify sender;
    PacketNotify receiver;
    sender.init(99u, 16382u);
    receiver.init(16381u, 100u);

    for (int i = 0; i < 4; ++i) {
        const NotifyHeader h = sender.make_header();
        const uint16_t sent_seq = sender.commit_and_increment_out_seq();
        SANDAL_ASSERT(h.seq == sent_seq);
        const int delta = receiver.update(h, [](uint16_t, bool) {});
        SANDAL_ASSERT(delta == 1);
        receiver.ack_seq(h.seq);
    }

    const NotifyHeader reply = receiver.make_header();
    receiver.commit_and_increment_out_seq();
    SANDAL_ASSERT(reply.acked_seq == 1u);
    SANDAL_ASSERT(reply.is_seq_delivered(1u));
    SANDAL_ASSERT(reply.is_seq_delivered(0u));
    SANDAL_ASSERT(reply.is_seq_delivered(16383u));
    SANDAL_ASSERT(reply.is_seq_delivered(16382u));
    SANDAL_ASSERT(!reply.is_delivered(kMaxHistoryBits));

    int acked_packets = 0;
    bool all_delivered = true;
    const int rdelta = sender.update(reply, [&](uint16_t, bool delivered) {
        if (!delivered) all_delivered = false;
        ++acked_packets;
    });
    SANDAL_ASSERT(rdelta == 1 && acked_packets == 4 && all_delivered);
    return 0;
}

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
    if (test_move_base_inline() != 0) return 1;
    if (test_move_packed_quantization_and_bounds() != 0) return 1;
    if (test_server_guid_allocator_pm52_and_shards() != 0) return 1;
    return test_packet_notify_14bit_wraparound();
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
        if (test_object_id_1_inline_movement_base_roundtrip() != 0) return 1;
    }
    if (sandal::test::should_run_case(cfg, "inline_movement_base_invalid_flags_rejected")) {
        if (test_inline_movement_base_invalid_flags_rejected() != 0) return 1;
    }
    if (sandal::test::should_run_case(cfg, "move_base_inline")) {
        if (test_move_base_inline() != 0) return 1;
    }
    if (sandal::test::should_run_case(cfg, "move_packed_quantization_and_bounds")) {
        if (test_move_packed_quantization_and_bounds() != 0) return 1;
    }
    if (sandal::test::should_run_case(cfg, "server_guid_allocator_pm52_and_shards")) {
        if (test_server_guid_allocator_pm52_and_shards() != 0) return 1;
    }
    if (sandal::test::should_run_case(cfg, "packet_notify_14bit_wraparound")) {
        if (test_packet_notify_14bit_wraparound() != 0) return 1;
    }
    return 0;
}
