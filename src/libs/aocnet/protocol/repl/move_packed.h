// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/value.h"

#include <cstdint>
#include <optional>
#include <string>

namespace aoc { namespace protocol { namespace repl {

constexpr uint32_t kMaxPackedMoveBits = 4096;
constexpr uint64_t kInlineMovementBaseObjectId = 1u;
constexpr int32_t  kMoveAccelerationScale = 10;
constexpr int32_t  kMoveLocationScale = 100;
constexpr double   kMaxMoveCoordCm = 1.0e8;
constexpr double   kMaxMoveAccelCmPerSec2 = 1.0e6;

constexpr uint8_t kMoveWalking = 1;
constexpr uint8_t kMoveFalling = 3;
constexpr uint8_t kMoveSwimming = 4;
constexpr uint8_t kMoveFlying = 5;

constexpr bool is_valid_inline_movement_base_object_id(uint64_t obj_id) noexcept {
    return obj_id == kInlineMovementBaseObjectId;
}

constexpr bool is_valid_movement_base_guid(const emit::FIntrepidNetworkGUID& g) noexcept {
    return g.ObjectId != 0u && g.ObjectId != kInlineMovementBaseObjectId;
}

bool is_finite_vec3(const Vec3d& v) noexcept;
double clamp_finite_component(double v, double max_abs) noexcept;
Vec3d clamp_move_vector(const Vec3d& v, double max_abs_component, double max_magnitude = 0.0) noexcept;

uint16_t quantize_rotation_axis_16(double degrees) noexcept;
constexpr double dequantize_rotation_axis_16(uint16_t compressed) noexcept {
    return static_cast<double>(compressed) * (360.0 / 65536.0);
}
Rot quantize_control_rotation(double pitch_deg, double yaw_deg, double roll_deg) noexcept;

QuantVec quantize_move_location_clamped(double x, double y, double z, double max_abs_cm = kMaxMoveCoordCm) noexcept;
QuantVec quantize_move_acceleration_clamped(
    double x, double y, double z, double max_abs_cm_s2 = kMaxMoveAccelCmPerSec2) noexcept;

struct NetworkMove;
/// The base of a move for a log line: `"CollisionCylinder" of object 17045704` for one named in place, `object 17045708` for a plain one, with `, bone X` when it has one.
std::string describe_base(const NetworkMove& m);

struct NetworkMove {
    float    time_stamp = 0;
    QuantVec acceleration;
    QuantVec location;
    Rot      control_rotation;
    uint8_t  compressed_flags = 0;
    std::optional<emit::FIntrepidNetworkGUID> base;
    /// MovementBase named in place (GUID ObjectId 1, then export flags, the outer and the path): an object the server never exported to the client, such as a component of an
    /// actor worldd never replicated a GUID for.  Unreal sends a base only when the move's Location (and maybe Acceleration) is RELATIVE to it, so a move with a base is
    /// not a world position until the base is placed.
    std::optional<InlineObject>               base_inline;
    std::optional<NameValue>                  base_bone;
    uint8_t  movement_mode = kMoveWalking;
    uint8_t  input_bits = 0;

    bool operator==(const NetworkMove&) const = default;

    /// The move stands on a movement base (plain or named in place): its Location is relative to it.
    bool has_base() const noexcept { return base.has_value() || base_inline.has_value(); }

    Vec3d  acceleration_cm() const { return dequantize_vector(acceleration, kMoveAccelerationScale); }
    Vec3d  location_cm() const { return dequantize_vector(location, kMoveLocationScale); }
    double pitch_degrees() const noexcept { return dequantize_rotation_axis_16(control_rotation.pitch); }
    double yaw_degrees() const noexcept { return dequantize_rotation_axis_16(control_rotation.yaw); }
    double roll_degrees() const noexcept { return dequantize_rotation_axis_16(control_rotation.roll); }
    bool   has_valid_base() const noexcept {
        if (base.has_value() && base_inline.has_value()) return false;
        if (base.has_value() && !is_valid_movement_base_guid(*base)) return false;
        if (base_inline.has_value() && !inline_object_fits(*base_inline)) return false;
        return true;
    }
    int    input_x() const { return (input_bits & 1) ? 1 : (input_bits & 2) ? -1 : 0; }
    int    input_y() const { return (input_bits & 4) ? 1 : (input_bits & 8) ? -1 : 0; }
};

struct MovePacked {
    NetworkMove                new_move;
    std::optional<NetworkMove> pending;
    bool                       dual_hybrid_root_motion = false;
    std::optional<NetworkMove> old;
    bool                       disable_combined_scoped_move = false;

    bool operator==(const MovePacked&) const = default;
};

bool network_move_is_finite_and_bounded(
    const NetworkMove& m,
    double max_abs_coord_cm = kMaxMoveCoordCm,
    double max_abs_accel_cm_s2 = kMaxMoveAccelCmPerSec2) noexcept;

bool move_packed_is_finite_and_bounded(
    const MovePacked& moves,
    double max_abs_coord_cm = kMaxMoveCoordCm,
    double max_abs_accel_cm_s2 = kMaxMoveAccelCmPerSec2) noexcept;

std::optional<MovePacked> read_move_packed(
    const uint8_t* data, size_t data_bytes, size_t pos, size_t end, std::string* why = nullptr);

bool write_move_packed(emit::BunchWriter& out, const MovePacked& moves);

}}}  // namespace aoc::protocol::repl
