// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "protocol/emit/actor_builder.h"
#include "protocol/emit/intrepid_netguid.h"
#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

namespace aoc { namespace protocol { namespace emit {

namespace {

void write_spawn_location(BunchWriter& out, const ActorRuntime& runtime) {
    const bool has_loc = runtime.serialize_location ||
                         runtime.spawn_location.x != 0.0f ||
                         runtime.spawn_location.y != 0.0f ||
                         runtime.spawn_location.z != 0.0f ||
                         runtime.quantize_location;
    out.write_bit(has_loc ? 1 : 0);
    if (!has_loc) return;
    out.write_bit(runtime.quantize_location ? 1 : 0);
    if (runtime.quantize_location) {
        write_quantized_vector(out, runtime.location_scaled_x,
                               runtime.location_scaled_y,
                               runtime.location_scaled_z);
        return;
    }
    const double coords[3] = {
        runtime.spawn_location.x,
        runtime.spawn_location.y,
        runtime.spawn_location.z,
    };
    for (double d : coords) {
        uint64_t u = 0;
        std::memcpy(&u, &d, 8);
        out.write_uint64(u);
    }
}

void write_spawn_rotation(BunchWriter& out, const ActorRuntime& runtime) {
    const bool has_rot = runtime.serialize_rotation ||
                         runtime.spawn_rotation.pitch != 0 ||
                         runtime.spawn_rotation.yaw   != 0 ||
                         runtime.spawn_rotation.roll  != 0;
    out.write_bit(has_rot ? 1 : 0);
    if (!has_rot) return;
    const int16_t axes[3] = {
        runtime.spawn_rotation.pitch,
        runtime.spawn_rotation.yaw,
        runtime.spawn_rotation.roll,
    };
    for (int16_t v : axes) {
        out.write_bit(v != 0 ? 1 : 0);
        if (v != 0) out.write_uint16(static_cast<uint16_t>(v));
    }
}

void emit_complex_property(const schema::PropertySchema& prop,
                           const SchemaValue& value,
                           BunchWriter& out) {
    using schema::PropType;
    switch (prop.type) {
    case PropType::FString:
        out.write_fstring_ansi(value.str);
        break;
    case PropType::FName:
        out.write_sip(value.u);
        break;
    case PropType::FVector: {
        uint32_t u = 0;
        std::memcpy(&u, &value.vec.x, 4); out.write_uint32(u);
        std::memcpy(&u, &value.vec.y, 4); out.write_uint32(u);
        std::memcpy(&u, &value.vec.z, 4); out.write_uint32(u);
        break;
    }
    case PropType::FRotator:
        out.write_bit(1);
        out.write_bit(value.rot.pitch != 0 ? 1 : 0);
        if (value.rot.pitch != 0) out.write_uint16(value.rot.pitch);
        out.write_bit(value.rot.yaw != 0 ? 1 : 0);
        if (value.rot.yaw != 0) out.write_uint16(value.rot.yaw);
        out.write_bit(value.rot.roll != 0 ? 1 : 0);
        if (value.rot.roll != 0) out.write_uint16(value.rot.roll);
        break;
    case PropType::FQuat: {
        uint32_t u = 0;
        const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (int i = 0; i < 4; ++i) {
            std::memcpy(&u, &q[i], 4);
            out.write_uint32(u);
        }
        (void)value;
        break;
    }
    case PropType::NetGUID:
        out.write_sip(value.netguid);
        break;
    case PropType::ByteArray:
        if (value.bytes.size() >= 0xFFFFu) {
            spdlog::warn("[ActorBuilder] ByteArray too large ({} bytes) for "
                         "UE5 uint16 length prefix", value.bytes.size());
        }
        out.write_uint16(static_cast<uint16_t>(value.bytes.size()));
        for (uint8_t b : value.bytes) out.write_uint8(b);
        break;
    case PropType::CustomDelta:
        out.write_int32(static_cast<int32_t>(value.bytes.size()));
        for (uint8_t b : value.bytes) out.write_uint8(b);
        break;
    default:
        break;
    }
}

} // namespace

void write_quantized_vector(BunchWriter& out, int64_t x, int64_t y, int64_t z) {
    auto bits_needed = [](int64_t v) -> uint32_t {
        uint64_t m = static_cast<uint64_t>(v ^ (v >> 63));
        uint32_t n = 1;
        for (int i = 0; i < 64 && m != 0; ++i) { m >>= 1; ++n; }
        return n;
    };
    const uint32_t bits = std::max(bits_needed(x), std::max(bits_needed(y), bits_needed(z)));
    out.write_serialize_int((1u << 6) | bits, 1u << 7);
    const uint64_t mask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1);
    out.write_bits(static_cast<uint64_t>(x) & mask, static_cast<int>(bits));
    out.write_bits(static_cast<uint64_t>(y) & mask, static_cast<int>(bits));
    out.write_bits(static_cast<uint64_t>(z) & mask, static_cast<int>(bits));
}

void write_player_controller_open_fields(BunchWriter& out, uint8_t net_player_index,
                                         uint32_t client_handshake_id) {
    out.write_bits(net_player_index, 8);
    out.write_bits(client_handshake_id, 32);
}

size_t write_serialize_new_actor(BunchWriter& out, const ActorRuntime& runtime,
                                  const schema::ActorSchema& schema) {
    size_t start = out.bit_pos();

    FIntrepidNetworkGUID actor{
        runtime.actor_netguid, runtime.actor_server_id, runtime.actor_randomizer};
    write_intrepid_guid(out, actor);

    FIntrepidNetworkGUID archetype{
        runtime.archetype_netguid ? runtime.archetype_netguid : schema.archetype_netguid,
        runtime.archetype_server_id,
        runtime.archetype_randomizer};
    write_intrepid_guid(out, archetype);

    FIntrepidNetworkGUID level{
        runtime.level_netguid ? runtime.level_netguid : schema.level_netguid,
        runtime.level_server_id,
        runtime.level_randomizer};
    write_intrepid_guid(out, level);

    write_spawn_location(out, runtime);
    write_spawn_rotation(out, runtime);

    out.write_bit(runtime.serialize_scale ? 1 : 0);
    if (runtime.serialize_scale) {
        write_quantized_vector(out, 0, 0, 0);
    }

    out.write_bit(runtime.serialize_velocity ? 1 : 0);
    if (runtime.serialize_velocity) {
        write_quantized_vector(out, 0, 0, 0);
    }

    return out.bit_pos() - start;
}

void ActorBuilder::emit_property(const schema::PropertySchema& prop,
                                  const SchemaValue& value,
                                  BunchWriter& out) {
    using schema::PropType;
    switch (prop.type) {
    case PropType::Bool:
        out.write_bit(value.b ? 1 : 0);
        break;
    case PropType::UInt8:
        out.write_uint8(static_cast<uint8_t>(value.u));
        break;
    case PropType::UInt16:
        out.write_uint16(static_cast<uint16_t>(value.u));
        break;
    case PropType::UInt32:
        out.write_uint32(static_cast<uint32_t>(value.u));
        break;
    case PropType::UInt64:
        out.write_uint64(value.u);
        break;
    case PropType::Int8:
    case PropType::Int16:
    case PropType::Int32:
        out.write_int32(static_cast<int32_t>(value.i));
        break;
    case PropType::Int64:
        out.write_uint64(static_cast<uint64_t>(value.i));
        break;
    case PropType::Float: {
        float f = static_cast<float>(value.f);
        uint32_t u = 0;
        std::memcpy(&u, &f, 4);
        out.write_uint32(u);
        break;
    }
    case PropType::Double: {
        double d = value.f;
        uint64_t u = 0;
        std::memcpy(&u, &d, 8);
        out.write_uint64(u);
        break;
    }
    default:
        emit_complex_property(prop, value, out);
        break;
    }
}

}}} // namespace aoc::protocol::emit
