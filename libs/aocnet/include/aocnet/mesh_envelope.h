// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aoc::net {

inline constexpr size_t kMeshForwardHeaderBytes = 12;
inline constexpr float kMeshBoundaryHysteresisCm = 20000.0f;

enum class MeshHandoverState : uint8_t {
    Authority = 0,
    Mirror = 1,
    Promoting = 2,
    Demoting = 3,
};

enum class MeshBorderBand : uint8_t {
    Interior = 0,
    HysteresisBand = 1,
    Handover = 2,
};

struct MeshForwardHeader {
    uint32_t source_zone_id = 0;
    uint32_t target_zone_id = 0;
    uint32_t authority_epoch = 0;
};

static_assert(sizeof(MeshForwardHeader) == kMeshForwardHeaderBytes,
              "MeshForwardHeader wire layout must equal 12 bytes");

[[nodiscard]] constexpr uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}

constexpr void write_u32_le(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
}

[[nodiscard]] constexpr bool pack_mesh_forward_header(
    const MeshForwardHeader& hdr, uint8_t* out, size_t out_len) noexcept {
    if (out == nullptr || out_len < kMeshForwardHeaderBytes) {
        return false;
    }
    write_u32_le(out + 0, hdr.source_zone_id);
    write_u32_le(out + 4, hdr.target_zone_id);
    write_u32_le(out + 8, hdr.authority_epoch);
    return true;
}

[[nodiscard]] constexpr bool pack_mesh_forward_header(
    const MeshForwardHeader& hdr, std::span<uint8_t> out) noexcept {
    return pack_mesh_forward_header(hdr, out.data(), out.size());
}

[[nodiscard]] constexpr bool unpack_mesh_forward_header(
    const uint8_t* in, size_t in_len, MeshForwardHeader& out) noexcept {
    if (in == nullptr || in_len < kMeshForwardHeaderBytes) {
        return false;
    }
    out.source_zone_id = read_u32_le(in + 0);
    out.target_zone_id = read_u32_le(in + 4);
    out.authority_epoch = read_u32_le(in + 8);
    return true;
}

[[nodiscard]] constexpr bool unpack_mesh_forward_header(
    std::span<const uint8_t> in, MeshForwardHeader& out) noexcept {
    return unpack_mesh_forward_header(in.data(), in.size(), out);
}

[[nodiscard]] inline bool is_within_hysteresis_band(
    float signed_dist_cm, float hysteresis_cm = kMeshBoundaryHysteresisCm) noexcept {
    const float band = hysteresis_cm > 0.0f ? hysteresis_cm : kMeshBoundaryHysteresisCm;
    return std::fabs(signed_dist_cm) <= band;
}

[[nodiscard]] inline MeshBorderBand classify_boundary_hysteresis(
    float signed_dist_cm, float hysteresis_cm = kMeshBoundaryHysteresisCm) noexcept {
    const float band = hysteresis_cm > 0.0f ? hysteresis_cm : kMeshBoundaryHysteresisCm;
    if (signed_dist_cm < -band) {
        return MeshBorderBand::Interior;
    }
    if (signed_dist_cm <= band) {
        return MeshBorderBand::HysteresisBand;
    }
    return MeshBorderBand::Handover;
}

// Preserve current zone authority inside [-200m, +200m] to prevent boundary ping-pong flapping.
[[nodiscard]] inline uint32_t resolve_hysteresis_authority(
    float signed_dist_cm,
    uint32_t current_authority_zone,
    uint32_t source_zone_id,
    uint32_t target_zone_id,
    float hysteresis_cm = kMeshBoundaryHysteresisCm) noexcept {
    const MeshBorderBand band = classify_boundary_hysteresis(signed_dist_cm, hysteresis_cm);
    if (band == MeshBorderBand::Interior) {
        return source_zone_id;
    }
    if (band == MeshBorderBand::Handover) {
        return target_zone_id;
    }
    return current_authority_zone != 0 ? current_authority_zone : source_zone_id;
}

[[nodiscard]] constexpr bool is_fenced_epoch_valid(
    uint32_t incoming_epoch, uint32_t current_epoch) noexcept {
    return incoming_epoch > current_epoch;
}

}  // namespace aoc::net

namespace aocnet {
using aoc::net::classify_boundary_hysteresis;
using aoc::net::is_fenced_epoch_valid;
using aoc::net::is_within_hysteresis_band;
using aoc::net::kMeshBoundaryHysteresisCm;
using aoc::net::kMeshForwardHeaderBytes;
using aoc::net::MeshBorderBand;
using aoc::net::MeshForwardHeader;
using aoc::net::MeshHandoverState;
using aoc::net::pack_mesh_forward_header;
using aoc::net::resolve_hysteresis_authority;
using aoc::net::unpack_mesh_forward_header;
}  // namespace aocnet
