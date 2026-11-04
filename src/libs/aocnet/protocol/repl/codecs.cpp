// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "protocol/repl/codecs_internal.h"
#include "protocol/emit/net_serializers.h"

#include <algorithm>

namespace aoc { namespace protocol { namespace repl {

using emit::BunchWriter;
using wire::PacketReader;

namespace {

constexpr uint8_t kExportHasPath = 1, kExportNoLoad = 2, kExportHasChecksum = 4;

bool valid_codepoint(uint32_t cp, size_t n) {
    const uint32_t min_cp[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < min_cp[n] || cp > 0x10FFFF) return false;
    return cp < 0xD800 || cp > 0xDFFF;
}

bool utf8_to_utf16(const std::string& s, std::u16string& out) {
    for (size_t i = 0; i < s.size(); ) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        size_t n = 0;
        if (c < 0x80)                { cp = c;        n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else return false;
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            const uint8_t cc = static_cast<uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!valid_codepoint(cp, n)) return false;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += n;
    }
    return true;
}

bool utf16_to_utf8(const std::u16string& u, std::string& out) {
    for (size_t i = 0; i < u.size(); ++i) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 >= u.size() || u[i + 1] < 0xDC00 || u[i + 1] > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return true;
}

std::optional<std::string> read_ascii_fstring(PacketReader& r, uint32_t len) {
    std::string s;
    for (uint32_t i = 0; i < len; ++i) {
        const uint8_t c = r.read_uint8();
        if (overran(r)) return std::nullopt;
        if (i + 1 == len) {
            if (c != 0) return std::nullopt;
        } else {
            if (c == 0 || c >= 0x80) return std::nullopt;
            s.push_back(static_cast<char>(c));
        }
    }
    return s;
}

bool geom_struct_accepts(S kind, const Value& v) {
    switch (kind) {
        case S::Vec3:      return v.get_if<Vec3d>() != nullptr;
        case S::Vec2:      return v.get_if<Vec2d>() != nullptr;
        case S::Ticks:     return v.get_if<Ticks>() != nullptr;
        case S::Rotator:   return v.get_if<Rot>() != nullptr;
        case S::Normal:    return v.get_if<Normal16>() != nullptr;
        case S::Quat:      return v.get_if<Quat>() != nullptr;
        case S::Quantized: {
            const auto* q = v.get_if<QuantVec>();
            return q && quant_vec_fits(*q);
        }
        case S::RepMovement: {
            const auto* m = v.get_if<RepMovement>();
            return m && rep_movement_fits(*m);
        }
        default:
            return false;
    }
}

bool actor_struct_accepts(S kind, const Value& v) {
    switch (kind) {
        case S::LoadTracker: {
            const auto* t = v.get_if<LoadTracker>();
            return t && t->flags.size() <= emit::kCharacterLoadTrackerMaxFlags;
        }
        case S::UniqueId: {
            const auto* id = v.get_if<UniqueNetId>();
            return id && unique_net_id_fits(*id);
        }
        case S::Tag: {
            const auto* t = v.get_if<GameplayTag>();
            return t && name_fits(t->name);
        }
        case S::TagContainer: {
            const auto* c = v.get_if<GameplayTagContainer>();
            if (!c || c->tags.size() > 63) return false;
            return std::all_of(c->tags.begin(), c->tags.end(), [](const NameValue& n) { return name_fits(n); });
        }
        case S::StatInt32: {
            const auto* t = v.get_if<StatInt32Inst>();
            return t && (t->extended || (t->base == 0 && t->equipment == 0));
        }
        case S::GuidToBool: {
            const auto* w = v.get_if<GuidToBoolWrapper>();
            return w && guid_to_bool_fits(*w);
        }
        case S::Alignment: {
            const auto* c = v.get_if<AlignmentContainer>();
            return c && alignment_fits(*c);
        }
        case S::AppearanceId:
            return v.get_if<AppearanceId>() != nullptr;
        case S::Customization: {
            const auto* c = v.get_if<CharacterCustomization>();
            return c && customization_fits(*c);
        }
        case S::MovementInput: {
            const auto* m = v.get_if<PlayerMovementInput>();
            return m && m->bits <= 15;
        }
        default:
            return false;
    }
}

std::optional<Value> read_geom_struct(S kind, PacketReader& r, const CodecOptions& opts) {
    switch (kind) {
        case S::Vec3: {
            Vec3d v{read_double(r), read_double(r), read_double(r)};
            return overran(r) ? std::nullopt : std::optional<Value>(Value(v));
        }
        case S::Vec2: {
            Vec2d v{read_double(r), read_double(r)};
            return overran(r) ? std::nullopt : std::optional<Value>(Value(v));
        }
        case S::Ticks: {
            Ticks t{static_cast<int64_t>(r.read_uint64())};
            return overran(r) ? std::nullopt : std::optional<Value>(Value(t));
        }
        case S::Rotator: {
            auto rot = read_rotator(r, 16);
            return rot ? std::optional<Value>(Value(*rot)) : std::nullopt;
        }
        case S::Quantized: {
            auto q = read_quantized_vector(r);
            return q ? std::optional<Value>(Value(*q)) : std::nullopt;
        }
        case S::Normal: {
            Normal16 n{
                static_cast<uint16_t>(r.read_bits(16)),
                static_cast<uint16_t>(r.read_bits(16)),
                static_cast<uint16_t>(r.read_bits(16)),
            };
            return overran(r) ? std::nullopt : std::optional<Value>(Value(n));
        }
        case S::RepMovement:
            return read_rep_movement(r, opts);
        case S::Quat: {
            Quat q{read_double(r), read_double(r), read_double(r)};
            return overran(r) ? std::nullopt : std::optional<Value>(Value(q));
        }
        default:
            return std::nullopt;
    }
}

std::optional<Value> read_actor_struct(S kind, PacketReader& r, const CodecOptions& opts) {
    switch (kind) {
        case S::LoadTracker: {
            const uint32_t n = static_cast<uint32_t>(r.read_bits(6));
            if (n > emit::kCharacterLoadTrackerMaxFlags) return std::nullopt;
            LoadTracker t;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t f = r.read_uint32();
                if (f > 1) return std::nullopt;
                t.flags.push_back(f != 0);
            }
            return overran(r) ? std::nullopt : std::optional<Value>(Value(std::move(t)));
        }
        case S::UniqueId:
            return read_unique_net_id(r);
        case S::Tag: {
            auto n = read_name(r);
            return n ? std::optional<Value>(Value(GameplayTag{std::move(*n)})) : std::nullopt;
        }
        case S::TagContainer: {
            GameplayTagContainer c;
            if (!r.read_bit()) {
                const uint32_t n = static_cast<uint32_t>(r.read_bits(6));
                for (uint32_t i = 0; i < n; ++i) {
                    auto name = read_name(r);
                    if (!name) return std::nullopt;
                    c.tags.push_back(std::move(*name));
                }
            }
            return overran(r) ? std::nullopt : std::optional<Value>(Value(std::move(c)));
        }
        case S::StatInt32: {
            StatInt32Inst t;
            t.record = static_cast<int64_t>(r.read_uint64());
            t.value = r.read_uint32();
            if (opts.stat_int32_extended && opts.stat_int32_extended(t.record)) {
                t.extended = true;
                t.base = r.read_uint32();
                t.equipment = r.read_uint32();
            }
            return overran(r) ? std::nullopt : std::optional<Value>(Value(t));
        }
        case S::GuidToBool:
            return read_guid_to_bool(r);
        case S::Alignment:
            return read_alignment(r);
        case S::AppearanceId: {
            AppearanceId a;
            a.type_id = r.read_uint64();
            a.record_guid = r.read_uint64();
            a.enchantment_level = r.read_uint8();
            a.item_equip_slot = r.read_uint8();
            return overran(r) ? std::nullopt : std::optional<Value>(Value(a));
        }
        case S::Customization:
            return read_customization(r, opts);
        case S::MovementInput: {
            PlayerMovementInput m{static_cast<uint8_t>(r.read_bits(4))};
            return overran(r) ? std::nullopt : std::optional<Value>(Value(m));
        }
        default:
            return std::nullopt;
    }
}

bool write_geom_struct(S kind, const Value& v, BunchWriter& out, const CodecOptions& opts) {
    switch (kind) {
        case S::Vec3: {
            const auto& a = *v.get_if<Vec3d>();
            write_double(out, a.x); write_double(out, a.y); write_double(out, a.z);
            return true;
        }
        case S::Vec2: {
            const auto& a = *v.get_if<Vec2d>();
            write_double(out, a.x); write_double(out, a.y);
            return true;
        }
        case S::Ticks:
            out.write_uint64(static_cast<uint64_t>(v.get_if<Ticks>()->ticks));
            return true;
        case S::Rotator:
            return write_rotator(out, *v.get_if<Rot>(), 16);
        case S::Quantized:
            return write_quantized_vector(out, *v.get_if<QuantVec>());
        case S::Normal: {
            const auto& n = *v.get_if<Normal16>();
            out.write_uint16(n.x); out.write_uint16(n.y); out.write_uint16(n.z);
            return true;
        }
        case S::RepMovement:
            return write_rep_movement(out, *v.get_if<RepMovement>(), opts);
        case S::Quat: {
            const auto& q = *v.get_if<Quat>();
            write_double(out, q.x); write_double(out, q.y); write_double(out, q.z);
            return true;
        }
        default:
            return false;
    }
}

bool write_actor_struct(S kind, const Value& v, BunchWriter& out) {
    switch (kind) {
        case S::LoadTracker:
            emit::write_character_load_tracker_value(out, v.get_if<LoadTracker>()->flags);
            return true;
        case S::UniqueId:
            return write_unique_net_id(out, *v.get_if<UniqueNetId>());
        case S::Tag:
            return write_name(out, v.get_if<GameplayTag>()->name);
        case S::TagContainer: {
            const auto& c = *v.get_if<GameplayTagContainer>();
            out.write_bit(c.tags.empty());
            if (c.tags.empty()) return true;
            out.write_bits(c.tags.size(), 6);
            for (const NameValue& n : c.tags) {
                if (!write_name(out, n)) return false;
            }
            return true;
        }
        case S::StatInt32: {
            const auto& t = *v.get_if<StatInt32Inst>();
            out.write_uint64(static_cast<uint64_t>(t.record));
            out.write_uint32(t.value);
            if (t.extended) {
                out.write_uint32(t.base);
                out.write_uint32(t.equipment);
            }
            return true;
        }
        case S::GuidToBool:
            return write_guid_to_bool(out, *v.get_if<GuidToBoolWrapper>());
        case S::Alignment:
            return write_alignment(out, *v.get_if<AlignmentContainer>());
        case S::AppearanceId: {
            const auto& a = *v.get_if<AppearanceId>();
            out.write_uint64(a.type_id);
            out.write_uint64(a.record_guid);
            out.write_uint8(a.enchantment_level);
            out.write_uint8(a.item_equip_slot);
            return true;
        }
        case S::Customization:
            return write_customization(out, *v.get_if<CharacterCustomization>());
        case S::MovementInput:
            out.write_bits(v.get_if<PlayerMovementInput>()->bits, 4);
            return true;
        default:
            return false;
    }
}

}  // namespace

bool valid_utf8(const std::string& s) {
    std::u16string u;
    return utf8_to_utf16(s, u);
}

std::optional<std::string> read_fstring(PacketReader& r) {
    const int32_t n = static_cast<int32_t>(r.read_uint32());
    if (overran(r)) return std::nullopt;
    if (n == 0) return std::string();
    if (n == INT32_MIN) return std::nullopt;
    const uint32_t len = static_cast<uint32_t>(n < 0 ? -n : n);
    if (len > kMaxFStringChars + 1) return std::nullopt;
    if (n > 0) return read_ascii_fstring(r, len);
    std::u16string u;
    for (uint32_t i = 0; i < len; ++i) {
        const uint16_t c = static_cast<uint16_t>(r.read_bits(16));
        if (overran(r)) return std::nullopt;
        if (i + 1 == len) {
            if (c != 0) return std::nullopt;
        } else {
            if (c == 0) return std::nullopt;
            u.push_back(static_cast<char16_t>(c));
        }
    }
    std::string s;
    if (!utf16_to_utf8(u, s)) return std::nullopt;
    return s;
}

bool fstring_fits(const std::string& s) {
    if (s.empty()) return true;
    if (s.find('\0') != std::string::npos) return false;
    const bool ascii = std::all_of(s.begin(), s.end(), [](char c) { return static_cast<uint8_t>(c) < 0x80; });
    if (ascii) return s.size() <= kMaxFStringChars;
    std::u16string u;
    return utf8_to_utf16(s, u) && u.size() <= kMaxFStringChars;
}

bool name_fits(const NameValue& n) { return n.hardcoded || fstring_fits(n.text); }

bool text_fits(const TextValue& t) {
    if (t.history == 0xFF) return !t.has_string || fstring_fits(t.source);
    if (t.history == 0) return fstring_fits(t.ns) && fstring_fits(t.key) && fstring_fits(t.source);
    return false;
}

bool write_fstring(BunchWriter& out, const std::string& utf8) {
    if (!fstring_fits(utf8)) return false;
    if (utf8.empty()) {
        out.write_int32(0);
        return true;
    }
    const bool ascii = std::all_of(utf8.begin(), utf8.end(), [](char c) { return static_cast<uint8_t>(c) < 0x80; });
    if (ascii) {
        out.write_int32(static_cast<int32_t>(utf8.size()) + 1);
        out.write_bytes(reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size());
        out.write_uint8(0);
        return true;
    }
    std::u16string u;
    if (!utf8_to_utf16(utf8, u)) return false;
    out.write_int32(-(static_cast<int32_t>(u.size()) + 1));
    for (char16_t c : u) out.write_uint16(c);
    out.write_uint16(0);
    return true;
}

std::optional<NameValue> read_name(PacketReader& r) {
    NameValue n;
    if (r.read_bit()) {
        const auto idx = r.read_sip();
        if (!idx || *idx > UINT32_MAX) return std::nullopt;
        n.hardcoded = true;
        n.index = static_cast<uint32_t>(*idx);
    } else {
        auto s = read_fstring(r);
        if (!s) return std::nullopt;
        n.text = std::move(*s);
        n.number = static_cast<int32_t>(r.read_uint32());
    }
    if (overran(r)) return std::nullopt;
    return n;
}

bool write_name(BunchWriter& out, const NameValue& n) {
    out.write_bit(n.hardcoded);
    if (n.hardcoded) {
        out.write_sip(n.index);
        return true;
    }
    if (!write_fstring(out, n.text)) return false;
    out.write_int32(n.number);
    return true;
}

std::optional<TextValue> read_text(PacketReader& r) {
    TextValue t;
    t.flags = r.read_uint32();
    t.history = r.read_uint8();
    if (t.history == 0xFF) {
        t.has_string = r.read_uint32() != 0;
        if (t.has_string) {
            auto s = read_fstring(r);
            if (!s) return std::nullopt;
            t.source = std::move(*s);
        }
    } else if (t.history == 0) {
        auto ns = read_fstring(r);
        auto key = read_fstring(r);
        auto src = read_fstring(r);
        if (!ns || !key || !src) return std::nullopt;
        t.ns = std::move(*ns);
        t.key = std::move(*key);
        t.source = std::move(*src);
    } else {
        return std::nullopt;
    }
    if (overran(r)) return std::nullopt;
    return t;
}

bool write_text(BunchWriter& out, const TextValue& t) {
    if (t.history != 0xFF && t.history != 0) return false;
    out.write_uint32(t.flags);
    out.write_uint8(t.history);
    if (t.history == 0xFF) {
        out.write_uint32(t.has_string ? 1u : 0u);
        return !t.has_string || write_fstring(out, t.source);
    }
    return write_fstring(out, t.ns) && write_fstring(out, t.key) && write_fstring(out, t.source);
}

std::optional<emit::FIntrepidNetworkGUID> read_object(PacketReader& r) {
    emit::FIntrepidNetworkGUID g;
    const uint32_t lo = r.read_uint32();
    const uint32_t hi = r.read_uint32();
    g.ObjectId = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    g.ServerId = r.read_uint32();
    g.Randomizer = r.read_uint32();
    if (overran(r) || g.ObjectId == 1) return std::nullopt;
    return g;
}

bool write_object(BunchWriter& out, const emit::FIntrepidNetworkGUID& g) {
    if (g.ObjectId == 1) return false;
    emit::write_intrepid_guid(out, g);
    return true;
}

std::optional<Value> read_object_value(PacketReader& r, bool allow_inline) {
    emit::FIntrepidNetworkGUID g;
    const uint32_t lo = r.read_uint32();
    const uint32_t hi = r.read_uint32();
    g.ObjectId = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    g.ServerId = r.read_uint32();
    g.Randomizer = r.read_uint32();
    if (overran(r)) return std::nullopt;
    if (g.ObjectId != 1) return Value(g);
    if (!allow_inline) return std::nullopt;
    InlineObject o;
    o.flags = r.read_uint8();
    if (overran(r) || (o.flags & ~(kExportHasPath | kExportNoLoad | kExportHasChecksum))) return std::nullopt;
    if ((o.flags & kExportHasChecksum) && !(o.flags & kExportHasPath)) return std::nullopt;
    if (o.flags & kExportHasPath) {
        auto outer = read_object(r);
        auto path = read_fstring(r);
        if (!outer || !path || path->empty()) return std::nullopt;
        o.outer = *outer;
        o.path = std::move(*path);
        if (o.flags & kExportHasChecksum) o.checksum = r.read_uint32();
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(o));
}

bool inline_object_fits(const InlineObject& o) {
    if (o.flags & ~(kExportHasPath | kExportNoLoad | kExportHasChecksum)) return false;
    if (o.flags & kExportHasPath) {
        if (o.path.empty() || !fstring_fits(o.path) || o.outer.ObjectId == 1) return false;
        return (o.flags & kExportHasChecksum) || o.checksum == 0;
    }
    return !(o.flags & kExportHasChecksum) && o.path.empty() && o.checksum == 0 && o.outer == emit::FIntrepidNetworkGUID{};
}

bool write_inline_object(BunchWriter& out, const InlineObject& o) {
    if (!inline_object_fits(o)) return false;
    emit::FIntrepidNetworkGUID root;
    root.ObjectId = 1;
    emit::write_intrepid_guid(out, root);
    out.write_uint8(o.flags);
    if (o.flags & kExportHasPath) {
        emit::write_intrepid_guid(out, o.outer);
        if (!write_fstring(out, o.path)) return false;
        if (o.flags & kExportHasChecksum) out.write_uint32(o.checksum);
    }
    return true;
}

std::optional<SoftObjectValue> read_soft_object(PacketReader& r) {
    SoftObjectValue s;
    if (r.read_bit()) {
        auto p = read_fstring(r);
        if (!p) return std::nullopt;
        s.is_path = true;
        s.path = std::move(*p);
    } else {
        auto o = read_object(r);
        if (!o) return std::nullopt;
        s.object = *o;
    }
    if (overran(r)) return std::nullopt;
    return s;
}

bool write_soft_object(BunchWriter& out, const SoftObjectValue& s) {
    out.write_bit(s.is_path);
    return s.is_path ? write_fstring(out, s.path) : write_object(out, s.object);
}

bool is_known_struct(std::string_view struct_name) { return find_struct(struct_name) != S::Unknown; }

bool struct_accepts(std::string_view struct_name, const Value& v) {
    const S kind = find_struct(struct_name);
    return geom_struct_accepts(kind, v) || actor_struct_accepts(kind, v) ||
           combat_struct_accepts(kind, v) || domain_struct_accepts(kind, v);
}

std::optional<Value> zero_struct(std::string_view struct_name) {
    switch (find_struct(struct_name)) {
        case S::Vec3:             return Value(Vec3d{});
        case S::Vec2:             return Value(Vec2d{});
        case S::Ticks:            return Value(Ticks{});
        case S::Rotator:          return Value(Rot{});
        case S::Quantized:        return Value(QuantVec{});
        case S::Tag:              return Value(GameplayTag{NameValue::hardcoded_name(0)});
        case S::TagContainer:     return Value(GameplayTagContainer{});
        case S::MovementInput:    return Value(PlayerMovementInput{});
        case S::LocalizedMessage: return Value(LocalizedMessage{});
        case S::CraftState:       return Value(CraftItemState{});
        case S::RewardInstance:   return Value(RewardInstance{});
        case S::HashedVariable:   return Value(HashedVariable{});
        default:                  return std::nullopt;
    }
}

std::optional<Value> read_struct(std::string_view struct_name, PacketReader& r, const CodecOptions& opts) {
    const S kind = find_struct(struct_name);
    if (auto v = read_geom_struct(kind, r, opts)) return v;
    if (auto v = read_actor_struct(kind, r, opts)) return v;
    if (auto v = read_combat_struct(kind, r)) return v;
    return read_domain_struct(kind, r);
}

bool write_struct(std::string_view struct_name, const Value& v, BunchWriter& out, const CodecOptions& opts) {
    if (!struct_accepts(struct_name, v)) return false;
    const S kind = find_struct(struct_name);
    return write_geom_struct(kind, v, out, opts) || write_actor_struct(kind, v, out) ||
           write_combat_struct(kind, v, out) || write_domain_struct(kind, v, out);
}

}}}  // namespace aoc::protocol::repl
