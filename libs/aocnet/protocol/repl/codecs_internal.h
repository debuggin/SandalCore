// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "protocol/repl/codecs.h"

#include <bit>
#include <climits>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

enum class S {
    Unknown, Vec3, Vec2, Ticks, Rotator, Quantized, Normal, RepMovement, LoadTracker, UniqueId, Tag, TagContainer,
    StatInt32, Quat, GuidToBool, Alignment, AppearanceId, Customization, ItemInstance, ItemStorageEvent, HitConfirmations,
    MovementInput, VendorPurchase, VendorTransaction, NodeTax, LocalizedMessage, ChannelingData, CraftState, RewardInstance, HashedVariable,
    WarehouseEntry, CraftTags,
};

inline uint32_t bits_needed(int64_t v) {
    uint64_t m = static_cast<uint64_t>(v ^ (v >> 63));
    uint32_t n = 1;
    for (int i = 0; i < 64 && m != 0; ++i) {
        m >>= 1;
        ++n;
    }
    return n;
}

inline double read_double(wire::PacketReader& r) { return std::bit_cast<double>(r.read_uint64()); }
inline void   write_double(emit::BunchWriter& out, double d) { out.write_uint64(std::bit_cast<uint64_t>(d)); }

inline std::optional<uint32_t> read_count(wire::PacketReader& r, uint64_t min_bits) {
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    if (overran(r) || n < 0 || static_cast<uint64_t>(n) * min_bits > r.remaining_bits()) return std::nullopt;
    return static_cast<uint32_t>(n);
}

inline bool write_count(emit::BunchWriter& out, size_t n) {
    if (n > static_cast<size_t>(INT32_MAX)) return false;
    out.write_int32(static_cast<int32_t>(n));
    return true;
}

inline GuidValue read_guid(wire::PacketReader& r) {
    GuidValue g;
    g.a = r.read_uint32();
    g.b = r.read_uint32();
    g.c = r.read_uint32();
    g.d = r.read_uint32();
    return g;
}

inline void write_guid(emit::BunchWriter& out, const GuidValue& g) {
    out.write_uint32(g.a);
    out.write_uint32(g.b);
    out.write_uint32(g.c);
    out.write_uint32(g.d);
}

inline auto guid_key(const GuidValue& g) { return std::make_tuple(g.a, g.b, g.c, g.d); }

template <class T, class Key>
inline bool unique_keys(const std::vector<T>& v, Key key) {
    if (v.empty()) return true;
    std::set<decltype(key(v.front()))> seen;
    for (const T& e : v) {
        if (!seen.insert(key(e)).second) return false;
    }
    return true;
}

std::optional<Rot> read_rotator(wire::PacketReader& r, int bits);
bool write_rotator(emit::BunchWriter& out, const Rot& rot, int bits);
std::optional<Value> read_rep_movement(wire::PacketReader& r, const CodecOptions& opts);
bool write_rep_movement(emit::BunchWriter& out, const RepMovement& m, const CodecOptions& opts);
bool rep_movement_fits(const RepMovement& m);
std::optional<Value> read_unique_net_id(wire::PacketReader& r);
bool write_unique_net_id(emit::BunchWriter& out, const UniqueNetId& id);
bool unique_net_id_fits(const UniqueNetId& id);

bool combat_struct_accepts(S kind, const Value& v);
std::optional<Value> read_combat_struct(S kind, wire::PacketReader& r);
bool write_combat_struct(S kind, const Value& v, emit::BunchWriter& out);

std::optional<Value> read_guid_to_bool(wire::PacketReader& r);
bool write_guid_to_bool(emit::BunchWriter& out, const GuidToBoolWrapper& w);
bool guid_to_bool_fits(const GuidToBoolWrapper& w);
std::optional<Value> read_alignment(wire::PacketReader& r);
bool write_alignment(emit::BunchWriter& out, const AlignmentContainer& c);
bool alignment_fits(const AlignmentContainer& c);
std::optional<Value> read_customization(wire::PacketReader& r, const CodecOptions& opts);
bool write_customization(emit::BunchWriter& out, const CharacterCustomization& c);
bool customization_fits(const CharacterCustomization& c);

S find_struct(std::string_view n);
bool domain_struct_accepts(S kind, const Value& v);
std::optional<Value> read_domain_struct(S kind, wire::PacketReader& r);
bool write_domain_struct(S kind, const Value& v, emit::BunchWriter& out);

}}}  // namespace aoc::protocol::repl
