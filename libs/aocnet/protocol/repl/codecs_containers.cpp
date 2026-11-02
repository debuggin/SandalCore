// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "protocol/repl/codecs_internal.h"
#include "protocol/repl/craft_codec.h"
#include "protocol/repl/item_codec.h"
#include "protocol/repl/quest_codec.h"
#include "protocol/repl/vendor_codec.h"
#include "protocol/repl/warehouse_codec.h"

#include <algorithm>

namespace aoc { namespace protocol { namespace repl {

using emit::BunchWriter;
using wire::PacketReader;

namespace {

constexpr uint8_t kCustomizationBits[kCustomizationFieldCount] = {
    32, 32, 32, 32, 32,
    64, 64, 64, 64, 64, 64, 64,
    32, 32, 32,
    64, 64, 64,
    32, 32, 32, 32,
    64, 64,
    32, 32,
    64, 32, 64, 32,
};

CustomizationDecal read_decal(PacketReader& r) {
    CustomizationDecal d;
    d.guid = r.read_uint64();
    d.strength = r.read_uint32();
    d.scale_u = r.read_uint32();
    d.scale_v = r.read_uint32();
    d.slot_index = r.read_uint8();
    d.merge_section_material_base = r.read_uint64();
    d.offset_u = r.read_uint32();
    d.offset_v = r.read_uint32();
    d.angle = r.read_uint32();
    d.color = r.read_uint64();
    d.discoloration = r.read_uint32();
    return d;
}

void write_decal(BunchWriter& out, const CustomizationDecal& d) {
    out.write_uint64(d.guid);
    out.write_uint32(d.strength);
    out.write_uint32(d.scale_u);
    out.write_uint32(d.scale_v);
    out.write_uint8(d.slot_index);
    out.write_uint64(d.merge_section_material_base);
    out.write_uint32(d.offset_u);
    out.write_uint32(d.offset_v);
    out.write_uint32(d.angle);
    out.write_uint64(d.color);
    out.write_uint32(d.discoloration);
}

bool read_customization_arrays(PacketReader& r, CharacterCustomization& c) {
    if (r.read_bit()) {
        const auto n = read_count(r, 1);
        if (!n) return false;
        std::vector<std::optional<CustomizationDecal>> v;
        for (uint32_t i = 0; i < *n; ++i) {
            if (r.read_bit()) v.push_back(read_decal(r));
            else v.push_back(std::nullopt);
        }
        c.decals = std::move(v);
    }
    if (overran(r)) return false;
    if (r.read_bit()) {
        const auto n = read_count(r, 1);
        if (!n) return false;
        std::vector<std::optional<CustomizationBlendGroup>> v;
        for (uint32_t i = 0; i < *n; ++i) {
            if (r.read_bit()) {
                CustomizationBlendGroup g;
                g.decal_appearance_guid = r.read_uint64();
                g.color_guid = r.read_uint64();
                g.intensity = r.read_uint32();
                v.push_back(g);
            } else {
                v.push_back(std::nullopt);
            }
        }
        c.blend_groups = std::move(v);
    }
    return !overran(r);
}

bool read_customization_tail(PacketReader& r, CharacterCustomization& c) {
    std::optional<bool>* bools[2] = {&c.helmet_visible, &c.cape_visible};
    for (std::optional<bool>* b : bools) {
        if (r.read_bit()) {
            const uint32_t v = r.read_uint32();
            if (v > 1) return false;
            *b = v != 0;
        }
    }
    if (overran(r)) return false;
    if (r.read_bit()) {
        const auto n = read_count(r, 8 + 3 * 64);
        if (!n) return false;
        std::vector<CustomizationSection> v;
        for (uint32_t i = 0; i < *n; ++i) {
            CustomizationSection s;
            s.key = r.read_uint8();
            s.value.x = read_double(r);
            s.value.y = read_double(r);
            s.value.z = read_double(r);
            v.push_back(s);
        }
        c.sections_values = std::move(v);
    }
    if (overran(r)) return false;
    if (r.read_bit()) {
        const auto n = read_count(r, 9 + 32);
        if (!n) return false;
        std::vector<CustomizationMorph> v;
        for (uint32_t i = 0; i < *n; ++i) {
            auto name = read_name(r);
            if (!name) return false;
            CustomizationMorph m;
            m.name = std::move(*name);
            m.weight = r.read_uint32();
            v.push_back(std::move(m));
        }
        c.face_morph_weights = std::move(v);
    }
    return !overran(r);
}

bool write_customization_tail(BunchWriter& out, const CharacterCustomization& c) {
    for (const std::optional<bool>* b : {&c.helmet_visible, &c.cape_visible}) {
        out.write_bit(b->has_value());
        if (*b) out.write_uint32(**b ? 1u : 0u);
    }
    out.write_bit(c.sections_values.has_value());
    if (c.sections_values) {
        if (!write_count(out, c.sections_values->size())) return false;
        for (const CustomizationSection& s : *c.sections_values) {
            out.write_uint8(s.key);
            write_double(out, s.value.x);
            write_double(out, s.value.y);
            write_double(out, s.value.z);
        }
    }
    out.write_bit(c.face_morph_weights.has_value());
    if (c.face_morph_weights) {
        if (!write_count(out, c.face_morph_weights->size())) return false;
        for (const CustomizationMorph& m : *c.face_morph_weights) {
            if (!write_name(out, m.name)) return false;
            out.write_uint32(m.weight);
        }
    }
    return true;
}

bool item_vendor_struct_accepts(S kind, const Value& v) {
    switch (kind) {
        case S::ItemInstance: {
            const auto* i = v.get_if<ItemInstance>();
            return i && item_instance_fits(*i);
        }
        case S::ItemStorageEvent: {
            const auto* e = v.get_if<ItemStorageEvent>();
            return e && item_storage_event_fits(*e);
        }
        case S::VendorPurchase: {
            const auto* c = v.get_if<VendorPurchaseCart>();
            return c && vendor_purchase_cart_fits(*c);
        }
        case S::VendorTransaction: {
            const auto* c = v.get_if<VendorTransactionCart>();
            return c && vendor_transaction_cart_fits(*c);
        }
        case S::NodeTax: {
            const auto* d = v.get_if<NodeTaxDetails>();
            return d && node_tax_details_fit(*d);
        }
        default:
            return false;
    }
}

bool craft_quest_struct_accepts(S kind, const Value& v) {
    switch (kind) {
        case S::CraftState: {
            const auto* s = v.get_if<CraftItemState>();
            return s && craft_state_fits(*s);
        }
        case S::RewardInstance: {
            const auto* s = v.get_if<RewardInstance>();
            return s && reward_instance_fits(*s);
        }
        case S::HashedVariable: {
            const auto* s = v.get_if<HashedVariable>();
            return s && hashed_variable_fits(*s);
        }
        case S::WarehouseEntry: {
            const auto* e = v.get_if<WarehouseSummaryEntry>();
            return e && warehouse_entry_fits(*e);
        }
        case S::CraftTags: {
            const auto* s = v.get_if<CraftTagSelection>();
            return s && craft_tag_selection_fits(*s);
        }
        default:
            return false;
    }
}

std::optional<Value> read_item_vendor_struct(S kind, PacketReader& r) {
    switch (kind) {
        case S::ItemInstance: {
            auto item = read_item_instance(r);
            return item ? std::optional<Value>(Value(std::move(*item))) : std::nullopt;
        }
        case S::ItemStorageEvent: {
            auto event = read_item_storage_event(r);
            return event ? std::optional<Value>(Value(std::move(*event))) : std::nullopt;
        }
        case S::VendorPurchase: {
            auto c = read_vendor_purchase_cart(r);
            return c ? std::optional<Value>(Value(std::move(*c))) : std::nullopt;
        }
        case S::VendorTransaction: {
            auto c = read_vendor_transaction_cart(r);
            return c ? std::optional<Value>(Value(std::move(*c))) : std::nullopt;
        }
        case S::NodeTax: {
            auto d = read_node_tax_details(r);
            return d ? std::optional<Value>(Value(std::move(*d))) : std::nullopt;
        }
        default:
            return std::nullopt;
    }
}

std::optional<Value> read_craft_quest_struct(S kind, PacketReader& r) {
    switch (kind) {
        case S::CraftState: {
            auto s = read_craft_item_state(r);
            return s ? std::optional<Value>(Value(std::move(*s))) : std::nullopt;
        }
        case S::RewardInstance: {
            auto s = read_reward_instance(r);
            return s ? std::optional<Value>(Value(std::move(*s))) : std::nullopt;
        }
        case S::HashedVariable: {
            auto s = read_hashed_variable(r);
            return s ? std::optional<Value>(Value(std::move(*s))) : std::nullopt;
        }
        case S::WarehouseEntry: {
            auto e = read_warehouse_entry(r);
            return e ? std::optional<Value>(Value(std::move(*e))) : std::nullopt;
        }
        case S::CraftTags: {
            auto s = read_craft_tag_selection(r);
            return s ? std::optional<Value>(Value(std::move(*s))) : std::nullopt;
        }
        default:
            return std::nullopt;
    }
}

}  // namespace

std::optional<Value> read_guid_to_bool(PacketReader& r) {
    const auto n = read_count(r, 128 + 32);
    if (!n) return std::nullopt;
    GuidToBoolWrapper w;
    for (uint32_t i = 0; i < *n; ++i) {
        const GuidValue g = read_guid(r);
        const uint32_t b = r.read_uint32();
        if (b > 1) return std::nullopt;
        w.pairs.emplace_back(g, b != 0);
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(w));
}

bool write_guid_to_bool(BunchWriter& out, const GuidToBoolWrapper& w) {
    if (!write_count(out, w.pairs.size())) return false;
    for (const auto& [g, b] : w.pairs) {
        write_guid(out, g);
        out.write_uint32(b ? 1u : 0u);
    }
    return true;
}

bool guid_to_bool_fits(const GuidToBoolWrapper& w) {
    return unique_keys(w.pairs, [](const auto& p) { return guid_key(p.first); });
}

std::optional<Value> read_alignment(PacketReader& r) {
    AlignmentContainer c;
    const auto n = read_count(r, 64 + 32);
    if (!n) return std::nullopt;
    for (uint32_t i = 0; i < *n; ++i) {
        AlignmentContainer::Instance inst;
        inst.key = r.read_uint64();
        const auto m = read_count(r, 128);
        if (!m) return std::nullopt;
        for (uint32_t j = 0; j < *m; ++j) inst.guids.push_back(read_guid(r));
        c.instances.push_back(std::move(inst));
    }
    const auto t = read_count(r, 32 + 3 * 64);
    if (!t) return std::nullopt;
    for (uint32_t i = 0; i < *t; ++i) {
        AlignmentContainer::Timer timer;
        timer.key = r.read_uint32();
        timer.set_time_stamp = r.read_uint64();
        timer.alignment_id_a = r.read_uint64();
        timer.alignment_id_b = r.read_uint64();
        c.timers.push_back(timer);
    }
    if (overran(r)) return std::nullopt;
    return Value(std::move(c));
}

bool write_alignment(BunchWriter& out, const AlignmentContainer& c) {
    if (!write_count(out, c.instances.size())) return false;
    for (const auto& inst : c.instances) {
        out.write_uint64(inst.key);
        if (!write_count(out, inst.guids.size())) return false;
        for (const GuidValue& g : inst.guids) write_guid(out, g);
    }
    if (!write_count(out, c.timers.size())) return false;
    for (const auto& t : c.timers) {
        out.write_uint32(t.key);
        out.write_uint64(t.set_time_stamp);
        out.write_uint64(t.alignment_id_a);
        out.write_uint64(t.alignment_id_b);
    }
    return true;
}

bool alignment_fits(const AlignmentContainer& c) {
    if (!unique_keys(c.instances, [](const auto& i) { return i.key; })) return false;
    if (!unique_keys(c.timers, [](const auto& t) { return t.key; })) return false;
    for (const auto& inst : c.instances) {
        if (!unique_keys(inst.guids, [](const GuidValue& g) { return guid_key(g); })) return false;
    }
    return true;
}

std::optional<Value> read_customization(PacketReader& r, const CodecOptions& opts) {
    CharacterCustomization c;
    if (r.read_bit()) c.preset_guid = r.read_uint64();
    if (r.read_bit()) c.random_seed = r.read_uint32();
    const bool preset_known = opts.customization_preset_known && opts.customization_preset_known(c.preset_guid.value_or(0));
    if (!preset_known) {
        CharacterCustomization::RaceGender rg;
        rg.race = r.read_uint8();
        rg.gender = r.read_uint8();
        c.race_gender = rg;
    }
    for (size_t i = 0; i < kCustomizationFieldCount; ++i) {
        if (r.read_bit()) c.deltas[static_cast<CustomizationField>(i)] = r.read_bits(kCustomizationBits[i]);
    }
    if (overran(r)) return std::nullopt;
    if (!read_customization_arrays(r, c)) return std::nullopt;
    if (!read_customization_tail(r, c)) return std::nullopt;
    return Value(std::move(c));
}

bool write_customization(BunchWriter& out, const CharacterCustomization& c) {
    out.write_bit(c.preset_guid.has_value());
    if (c.preset_guid) out.write_uint64(*c.preset_guid);
    out.write_bit(c.random_seed.has_value());
    if (c.random_seed) out.write_uint32(*c.random_seed);
    if (c.race_gender) {
        out.write_uint8(c.race_gender->race);
        out.write_uint8(c.race_gender->gender);
    }
    for (size_t i = 0; i < kCustomizationFieldCount; ++i) {
        const auto it = c.deltas.find(static_cast<CustomizationField>(i));
        out.write_bit(it != c.deltas.end());
        if (it != c.deltas.end()) out.write_bits(it->second, kCustomizationBits[i]);
    }
    out.write_bit(c.decals.has_value());
    if (c.decals) {
        if (!write_count(out, c.decals->size())) return false;
        for (const auto& d : *c.decals) {
            out.write_bit(d.has_value());
            if (d) write_decal(out, *d);
        }
    }
    out.write_bit(c.blend_groups.has_value());
    if (c.blend_groups) {
        if (!write_count(out, c.blend_groups->size())) return false;
        for (const auto& g : *c.blend_groups) {
            out.write_bit(g.has_value());
            if (g) {
                out.write_uint64(g->decal_appearance_guid);
                out.write_uint64(g->color_guid);
                out.write_uint32(g->intensity);
            }
        }
    }
    return write_customization_tail(out, c);
}

bool customization_fits(const CharacterCustomization& c) {
    for (const auto& [field, bits] : c.deltas) {
        const size_t i = static_cast<size_t>(field);
        if (i >= kCustomizationFieldCount) return false;
        if (kCustomizationBits[i] < 64 && (bits >> kCustomizationBits[i]) != 0) return false;
    }
    if (c.sections_values && !unique_keys(*c.sections_values, [](const CustomizationSection& s) { return s.key; })) return false;
    if (c.face_morph_weights) {
        const auto& v = *c.face_morph_weights;
        if (!std::all_of(v.begin(), v.end(), [](const CustomizationMorph& m) { return name_fits(m.name); })) return false;
        if (!unique_keys(v, [](const CustomizationMorph& m) {
                return std::make_tuple(m.name.hardcoded, m.name.index, m.name.text, m.name.number);
            }))
            return false;
    }
    return true;
}

S find_struct(std::string_view n) {
    static constexpr std::pair<std::string_view, S> kTable[] = {
        {"FVector", S::Vec3},
        {"FVector3d", S::Vec3},
        {"FVector2D", S::Vec2},
        {"FVector2d", S::Vec2},
        {"FDateTime", S::Ticks},
        {"FTimespan", S::Ticks},
        {"FRotator", S::Rotator},
        {"FVector_NetQuantize", S::Quantized},
        {"FVector_NetQuantize10", S::Quantized},
        {"FVector_NetQuantize100", S::Quantized},
        {"FVector_NetQuantizeNormal", S::Normal},
        {"FRepMovement", S::RepMovement},
        {"FCharacterLoadTracker", S::LoadTracker},
        {"FUniqueNetIdRepl", S::UniqueId},
        {"FGameplayTag", S::Tag},
        {"FGameplayTagContainer", S::TagContainer},
        {"FStatInt32InstRep", S::StatInt32},
        {"FQuat", S::Quat},
        {"FGuidToBoolWrapper", S::GuidToBool},
        {"FAlignmentContainer", S::Alignment},
        {"FAppearanceId", S::AppearanceId},
        {"FCharacterCustomizationSaveData", S::Customization},
        {"FItemInstance", S::ItemInstance},
        {"FItemStorageEvent", S::ItemStorageEvent},
        {"FSerializedClientHitConfirmations", S::HitConfirmations},
        {"FPlayerMovementInput", S::MovementInput},
        {"FVendorPurchaseCart", S::VendorPurchase},
        {"FVendorTransactionCart", S::VendorTransaction},
        {"FNodeTaxDetails", S::NodeTax},
        {"FLocalizedMessage", S::LocalizedMessage},
        {"FChannelingData", S::ChannelingData},
        {"FCraftItemState", S::CraftState},
        {"FRewardInstance", S::RewardInstance},
        {"FHashedVariable", S::HashedVariable},
        {"FWarehouseItemChunkSummary", S::WarehouseEntry},
        {"FWarehouseMaterialTabSummary", S::WarehouseEntry},
        {"FCraftingResourceTagCostSelection", S::CraftTags},
    };
    for (const auto& [name, kind] : kTable) {
        if (n == name) return kind;
    }
    return S::Unknown;
}

bool domain_struct_accepts(S kind, const Value& v) {
    return item_vendor_struct_accepts(kind, v) || craft_quest_struct_accepts(kind, v);
}

std::optional<Value> read_domain_struct(S kind, PacketReader& r) {
    if (auto v = read_item_vendor_struct(kind, r)) return v;
    return read_craft_quest_struct(kind, r);
}

bool write_domain_struct(S kind, const Value& v, BunchWriter& out) {
    switch (kind) {
        case S::ItemInstance:
            return write_item_instance(out, *v.get_if<ItemInstance>());
        case S::ItemStorageEvent:
            return write_item_storage_event(out, *v.get_if<ItemStorageEvent>());
        case S::VendorPurchase:
            return write_vendor_purchase_cart(out, *v.get_if<VendorPurchaseCart>());
        case S::VendorTransaction:
            return write_vendor_transaction_cart(out, *v.get_if<VendorTransactionCart>());
        case S::NodeTax:
            return write_node_tax_details(out, *v.get_if<NodeTaxDetails>());
        case S::CraftState:
            return write_craft_item_state(out, *v.get_if<CraftItemState>());
        case S::RewardInstance:
            return write_reward_instance(out, *v.get_if<RewardInstance>());
        case S::HashedVariable:
            return write_hashed_variable(out, *v.get_if<HashedVariable>());
        case S::WarehouseEntry:
            return write_warehouse_entry(out, *v.get_if<WarehouseSummaryEntry>());
        case S::CraftTags:
            return write_craft_tag_selection(out, *v.get_if<CraftTagSelection>());
        default:
            return false;
    }
}

}}}  // namespace aoc::protocol::repl
