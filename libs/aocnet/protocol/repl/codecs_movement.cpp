// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "protocol/repl/codecs_internal.h"
#include "protocol/repl/callout_codec.h"
#include "protocol/repl/channeling_codec.h"
#include "protocol/repl/hit_codec.h"

#include <algorithm>
#include <cmath>

namespace aoc { namespace protocol { namespace repl {

using emit::BunchWriter;
using wire::PacketReader;

namespace {

bool read_encoded_unique_net_id(PacketReader& r, UniqueNetId& id, uint32_t type_hash) {
    if (type_hash == 30) {
        id.services = r.read_uint8();
        const int32_t n = static_cast<int32_t>(r.read_uint32());
        if (n < 0 || static_cast<size_t>(n) * 8 > r.remaining_bits()) return false;
        for (int32_t i = 0; i < n; ++i) id.bytes.push_back(r.read_uint8());
        return true;
    }
    if (type_hash == 31) {
        auto type = read_fstring(r);
        if (!type) return false;
        id.type = std::move(*type);
    }
    const uint32_t n = r.read_uint8();
    for (uint32_t i = 0; i < n; ++i) id.bytes.push_back(r.read_uint8());
    return true;
}

}  // namespace

std::optional<Rot> read_rotator(PacketReader& r, int bits) {
    Rot rot;
    uint16_t* axes[3] = {&rot.pitch, &rot.yaw, &rot.roll};
    for (uint16_t* a : axes) {
        if (r.read_bit()) *a = static_cast<uint16_t>(r.read_bits(bits));
    }
    if (overran(r)) return std::nullopt;
    return rot;
}

bool write_rotator(BunchWriter& out, const Rot& rot, int bits) {
    const uint16_t axes[3] = {rot.pitch, rot.yaw, rot.roll};
    for (uint16_t a : axes) {
        if (bits < 16 && (a >> bits) != 0) return false;
        out.write_bit(a != 0);
        if (a != 0) out.write_bits(a, bits);
    }
    return true;
}

std::optional<Value> read_rep_movement(PacketReader& r, const CodecOptions& opts) {
    RepMovement m;
    const uint32_t flags = static_cast<uint32_t>(r.read_bits(4));
    m.simulated_physic_sleep = (flags & 1) != 0;
    m.rep_physics = (flags & 2) != 0;
    auto loc = read_quantized_vector(r);
    auto rot = read_rotator(r, opts.rep_movement_short_rotation ? 16 : 8);
    auto vel = read_quantized_vector(r);
    if (!loc || !rot || !vel) return std::nullopt;
    m.location = *loc;
    m.rotation = *rot;
    m.linear_velocity = *vel;
    if (m.rep_physics) {
        auto ang = read_quantized_vector(r);
        if (!ang) return std::nullopt;
        m.angular_velocity = *ang;
    }
    if (flags & 4) {
        const auto v = r.read_sip();
        if (!v || *v > UINT32_MAX) return std::nullopt;
        m.server_frame = static_cast<uint32_t>(*v);
    }
    if (flags & 8) {
        const auto v = r.read_sip();
        if (!v || *v > UINT32_MAX) return std::nullopt;
        m.server_physics_handle = static_cast<uint32_t>(*v);
    }
    if (r.read_bit()) {
        auto acc = read_quantized_vector(r);
        if (!acc) return std::nullopt;
        m.acceleration = *acc;
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(m));
}

bool write_rep_movement(BunchWriter& out, const RepMovement& m, const CodecOptions& opts) {
    const uint32_t flags = (m.simulated_physic_sleep ? 1u : 0u) | (m.rep_physics ? 2u : 0u) |
                           (m.server_frame ? 4u : 0u) | (m.server_physics_handle ? 8u : 0u);
    out.write_bits(flags, 4);
    if (!write_quantized_vector(out, m.location)) return false;
    if (!write_rotator(out, m.rotation, opts.rep_movement_short_rotation ? 16 : 8)) return false;
    if (!write_quantized_vector(out, m.linear_velocity)) return false;
    if (m.rep_physics && !write_quantized_vector(out, m.angular_velocity)) return false;
    if (m.server_frame) out.write_sip(*m.server_frame);
    if (m.server_physics_handle) out.write_sip(*m.server_physics_handle);
    out.write_bit(m.acceleration.has_value());
    if (m.acceleration && !write_quantized_vector(out, *m.acceleration)) return false;
    return true;
}

bool rep_movement_fits(const RepMovement& m) {
    return quant_vec_fits(m.location) && quant_vec_fits(m.linear_velocity) &&
           (!m.rep_physics || quant_vec_fits(m.angular_velocity)) &&
           (!m.acceleration || quant_vec_fits(*m.acceleration));
}

std::optional<Value> read_unique_net_id(PacketReader& r) {
    UniqueNetId id;
    id.flags = r.read_uint8();
    if (overran(r)) return std::nullopt;
    const uint32_t type_hash = id.flags >> 3;
    if (id.flags & 1) {
        if (id.flags & 2) return Value(std::move(id));
        if (!read_encoded_unique_net_id(r, id, type_hash)) return std::nullopt;
    } else {
        if (type_hash == 31) {
            auto type = read_fstring(r);
            if (!type) return std::nullopt;
            id.type = std::move(*type);
        }
        auto s = read_fstring(r);
        if (!s) return std::nullopt;
        id.id = std::move(*s);
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(id));
}

bool write_unique_net_id(BunchWriter& out, const UniqueNetId& id) {
    out.write_uint8(id.flags);
    const uint32_t type_hash = id.flags >> 3;
    if (id.flags & 1) {
        if (id.flags & 2) return true;
        if (type_hash == 30) {
            out.write_uint8(id.services);
            out.write_int32(static_cast<int32_t>(id.bytes.size()));
        } else {
            if (type_hash == 31 && !write_fstring(out, id.type)) return false;
            if (id.bytes.size() > 255) return false;
            out.write_uint8(static_cast<uint8_t>(id.bytes.size()));
        }
        out.write_bytes(id.bytes.data(), id.bytes.size());
        return true;
    }
    if (type_hash == 31 && !write_fstring(out, id.type)) return false;
    return write_fstring(out, id.id);
}

bool unique_net_id_fits(const UniqueNetId& id) {
    const uint32_t type_hash = id.flags >> 3;
    if (id.flags & 1) {
        if (id.flags & 2) return true;
        if (type_hash == 30) return true;
        if (type_hash == 31 && !fstring_fits(id.type)) return false;
        return id.bytes.size() <= 255;
    }
    if (type_hash == 31 && !fstring_fits(id.type)) return false;
    return fstring_fits(id.id);
}

std::optional<QuantVec> read_quantized_vector(PacketReader& r) {
    QuantVec q;
    const uint32_t info = r.read_serialize_int(1u << 7);
    const uint32_t bits = info & 63, extra = info >> 6;
    if (bits) {
        int64_t* comps[3] = {&q.x, &q.y, &q.z};
        for (int64_t* c : comps) {
            const uint64_t v = r.read_bits(static_cast<int>(bits));
            const uint64_t sign = 1ull << (bits - 1);
            *c = static_cast<int64_t>((v ^ sign) - sign);
        }
        q.mode = extra ? QuantVec::Mode::Scaled : QuantVec::Mode::Unscaled;
    } else if (extra) {
        q.mode = QuantVec::Mode::RawDouble;
        q.x = static_cast<int64_t>(r.read_uint64());
        q.y = static_cast<int64_t>(r.read_uint64());
        q.z = static_cast<int64_t>(r.read_uint64());
    } else {
        q.mode = QuantVec::Mode::RawFloat;
        q.x = r.read_uint32();
        q.y = r.read_uint32();
        q.z = r.read_uint32();
    }
    if (overran(r)) return std::nullopt;
    return q;
}

bool quant_vec_fits(const QuantVec& q) {
    switch (q.mode) {
        case QuantVec::Mode::Scaled:
        case QuantVec::Mode::Unscaled:
            return std::max(bits_needed(q.x), std::max(bits_needed(q.y), bits_needed(q.z))) <= 63;
        case QuantVec::Mode::RawDouble:
            return true;
        case QuantVec::Mode::RawFloat:
            return q.x >= 0 && q.y >= 0 && q.z >= 0 && q.x <= UINT32_MAX && q.y <= UINT32_MAX && q.z <= UINT32_MAX;
    }
    return false;
}

bool write_quantized_vector(BunchWriter& out, const QuantVec& q) {
    if (!quant_vec_fits(q)) return false;
    switch (q.mode) {
        case QuantVec::Mode::Scaled:
        case QuantVec::Mode::Unscaled: {
            const uint32_t bits = std::max(bits_needed(q.x), std::max(bits_needed(q.y), bits_needed(q.z)));
            out.write_serialize_int((q.mode == QuantVec::Mode::Scaled ? (1u << 6) : 0u) | bits, 1u << 7);
            const uint64_t mask = (1ull << bits) - 1;
            out.write_bits(static_cast<uint64_t>(q.x) & mask, static_cast<int>(bits));
            out.write_bits(static_cast<uint64_t>(q.y) & mask, static_cast<int>(bits));
            out.write_bits(static_cast<uint64_t>(q.z) & mask, static_cast<int>(bits));
            return true;
        }
        case QuantVec::Mode::RawDouble:
            out.write_serialize_int(1u << 6, 1u << 7);
            out.write_uint64(static_cast<uint64_t>(q.x));
            out.write_uint64(static_cast<uint64_t>(q.y));
            out.write_uint64(static_cast<uint64_t>(q.z));
            return true;
        case QuantVec::Mode::RawFloat:
            out.write_serialize_int(0, 1u << 7);
            out.write_uint32(static_cast<uint32_t>(q.x));
            out.write_uint32(static_cast<uint32_t>(q.y));
            out.write_uint32(static_cast<uint32_t>(q.z));
            return true;
    }
    return false;
}

QuantVec quantize_vector(int32_t scale, double x, double y, double z) {
    // Non-finite vectors serialize as zero; magnitudes beyond 2^62 after scaling serialize as RawDouble.
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) x = y = z = 0;
    constexpr double kMaxValueToScale = 4503599627370496.0;
    constexpr double kMaxScaledValue = 4611686018427387904.0;
    const double f = scale;
    const double sx = x * f, sy = y * f, sz = z * f;
    QuantVec q;
    if (std::max({std::fabs(sx), std::fabs(sy), std::fabs(sz)}) < kMaxScaledValue) {
        const bool scaled = std::min({std::fabs(x), std::fabs(y), std::fabs(z)}) < kMaxValueToScale;
        auto round = [](double d) { return static_cast<int64_t>(d + (d >= 0 ? 0.5 : -0.5)); };
        q.mode = scaled ? QuantVec::Mode::Scaled : QuantVec::Mode::Unscaled;
        q.x = round(scaled ? sx : x);
        q.y = round(scaled ? sy : y);
        q.z = round(scaled ? sz : z);
    } else {
        q.mode = QuantVec::Mode::RawDouble;
        q.x = static_cast<int64_t>(std::bit_cast<uint64_t>(x));
        q.y = static_cast<int64_t>(std::bit_cast<uint64_t>(y));
        q.z = static_cast<int64_t>(std::bit_cast<uint64_t>(z));
    }
    return q;
}

QuantVec quantize_location(const CodecOptions& o, double x, double y, double z) {
    return quantize_vector(o.rep_movement_location_scale, x, y, z);
}

QuantVec quantize_velocity(const CodecOptions& o, double x, double y, double z) {
    return quantize_vector(o.rep_movement_velocity_scale, x, y, z);
}

Vec3d dequantize_location(const CodecOptions& o, const QuantVec& v) { return dequantize_vector(v, o.rep_movement_location_scale); }

Vec3d dequantize_velocity(const CodecOptions& o, const QuantVec& v) { return dequantize_vector(v, o.rep_movement_velocity_scale); }

Vec3d dequantize_vector(const QuantVec& q, int32_t scale) {
    switch (q.mode) {
        case QuantVec::Mode::Scaled:
            return {static_cast<double>(q.x) / scale, static_cast<double>(q.y) / scale, static_cast<double>(q.z) / scale};
        case QuantVec::Mode::Unscaled:
            return {static_cast<double>(q.x), static_cast<double>(q.y), static_cast<double>(q.z)};
        case QuantVec::Mode::RawFloat:
            return {std::bit_cast<float>(static_cast<uint32_t>(q.x)), std::bit_cast<float>(static_cast<uint32_t>(q.y)),
                    std::bit_cast<float>(static_cast<uint32_t>(q.z))};
        case QuantVec::Mode::RawDouble:
            return {std::bit_cast<double>(static_cast<uint64_t>(q.x)), std::bit_cast<double>(static_cast<uint64_t>(q.y)),
                    std::bit_cast<double>(static_cast<uint64_t>(q.z))};
    }
    return {};
}

Quat quat_to_wire(double x, double y, double z, double w) {
    // Match UE5 SIMD summation order: (x^2 + z^2) + (y^2 + w^2) for normalization check.
    constexpr double kSmallNumber = static_cast<double>(1.e-8f);
    constexpr double kThreshNormalized = static_cast<double>(0.01f);
    if ((x * x + y * y) + (z * z + w * w) <= kSmallNumber) return Quat{};
    const double sum = (x * x + z * z) + (y * y + w * w);
    if (kThreshNormalized < std::fabs(1.0 - sum)) {
        const double scale = 1.0 / std::sqrt(sum);
        x *= scale;
        y *= scale;
        z *= scale;
        w *= scale;
    }
    if (w < 0) {
        x = -x;
        y = -y;
        z = -z;
    }
    return Quat{x, y, z};
}

std::array<double, 4> quat_from_wire(const Quat& q) {
    double x = q.x, y = q.y, z = q.z, w;
    const double xyz2 = x * x + y * y + z * z;
    const double w2 = 1.0 - xyz2;
    if (w2 >= 0) {
        w = std::sqrt(w2);
    } else {
        w = 0;
        const double inv = 1.0 / std::sqrt(xyz2);
        x *= inv;
        y *= inv;
        z *= inv;
    }
    return {x, y, z, w};
}

bool combat_struct_accepts(S kind, const Value& v) {
    switch (kind) {
        case S::HitConfirmations: {
            const auto* c = v.get_if<HitConfirmations>();
            return c && hit_confirmations_fit(*c);
        }
        case S::LocalizedMessage: {
            const auto* m = v.get_if<LocalizedMessage>();
            return m && localized_message_fits(*m);
        }
        case S::ChannelingData: {
            const auto* d = v.get_if<ChannelingData>();
            return d && channeling_data_fits(*d);
        }
        default:
            return false;
    }
}

std::optional<Value> read_combat_struct(S kind, PacketReader& r) {
    switch (kind) {
        case S::HitConfirmations: {
            auto hits = read_hit_confirmations(r);
            return hits ? std::optional<Value>(Value(std::move(*hits))) : std::nullopt;
        }
        case S::LocalizedMessage: {
            auto m = read_localized_message(r);
            return m ? std::optional<Value>(Value(std::move(*m))) : std::nullopt;
        }
        case S::ChannelingData: {
            auto d = read_channeling_data(r);
            return d ? std::optional<Value>(Value(std::move(*d))) : std::nullopt;
        }
        default:
            return std::nullopt;
    }
}

bool write_combat_struct(S kind, const Value& v, BunchWriter& out) {
    switch (kind) {
        case S::HitConfirmations:
            return write_hit_confirmations(out, *v.get_if<HitConfirmations>());
        case S::LocalizedMessage:
            return write_localized_message(out, *v.get_if<LocalizedMessage>());
        case S::ChannelingData:
            return write_channeling_data(out, *v.get_if<ChannelingData>());
        default:
            return false;
    }
}

}}}  // namespace aoc::protocol::repl
