#pragma once

#include <map>
#include <type_traits>
#include <variant>

#include "protocol/repl/value_types.h"

namespace aoc { namespace protocol { namespace repl {

struct HitStatMod {
    int64_t guid = 0;
    float   value = 0;
    float   effective_value = 0;
    bool operator==(const HitStatMod& o) const {
        return guid == o.guid && same_bits(value, o.value) && same_bits(effective_value, o.effective_value);
    }
};

struct HitModInst {
    int64_t                    guid = 0;
    emit::FIntrepidNetworkGUID owner;
    bool operator==(const HitModInst&) const = default;
};

struct HitTarget {
    emit::FIntrepidNetworkGUID hit_comp;
    Vec3d                      impact_location;
    Vec3d                      impact_normal;
    std::vector<HitStatMod>    stat_mods;
    std::vector<int64_t>       interrupts;
    bool                       attempted_interrupt = false;
    uint8_t                    flags = 0;
    std::vector<HitModInst>    hit_mods;
    bool operator==(const HitTarget&) const = default;
};

struct HitInstance {
    int64_t                    inst_guid = 0;
    int64_t                    record_guid = 0;
    int64_t                    source_record_type = 0;
    int64_t                    source_record_guid = 0;
    bool                       pvp_force = false;
    emit::FIntrepidNetworkGUID instigator;
    emit::FIntrepidNetworkGUID source;
    std::vector<HitTarget>     targets;
    bool                       predicted_on_autonomous = false;
    bool                       predicted_on_simulated = false;
    bool operator==(const HitInstance&) const = default;
};

struct HitConfirmations {
    std::vector<HitInstance> hits;
    bool operator==(const HitConfirmations&) const = default;
};

struct VendorCartItem {
    int64_t                item_record = 0;
    int32_t                quantity = 0;
    std::optional<int64_t> optional_a, optional_b;
    bool operator==(const VendorCartItem&) const = default;
};

struct VendorPurchaseCart {
    int64_t                     vendor = 0;
    std::vector<VendorCartItem> items;
    bool operator==(const VendorPurchaseCart&) const = default;
};

struct VendorTransactionCart {
    int64_t                vendor = 0;
    std::vector<GuidValue> instances;
    uint8_t                operation = 0;
    bool operator==(const VendorTransactionCart&) const = default;
};

struct NodeTaxEntry {
    std::string tag;
    int64_t     details = 0;
    int32_t     flat_tax = 0;
    float       tax_percent = 0.0f;
    float       owning_node_percent = 0.0f;
    bool        flag_a = false;
    bool        flag_b = false;
    bool operator==(const NodeTaxEntry&) const = default;
};

struct NodeTaxDetails {
    int64_t                   node = 0;
    std::vector<NodeTaxEntry> entries;
    bool operator==(const NodeTaxDetails&) const = default;
};

struct ChannelingData {
    emit::FIntrepidNetworkGUID target;
    uint32_t                   flags = 0;
    bool                       active = false;
    double                     start_time = 0;
    bool                       timed = false;
    float                      duration = 0;
    int64_t                    tool_record = 0;
    GuidValue                  tool_guid;
    bool operator==(const ChannelingData& o) const {
        return target == o.target && flags == o.flags && active == o.active && same_bits(start_time, o.start_time) && timed == o.timed && same_bits(duration, o.duration) &&
               tool_record == o.tool_record && tool_guid == o.tool_guid;
    }
};

/// FCharacterCustomizationSaveData::NetSerialize sends the struct as a delta
/// against a preset default that never reaches the wire; absent fields use std::nullopt.
enum class CustomizationField : uint8_t {
    SkinColorHue, SkinColorPigmentation, NormalDetailStrength01, NormalDetailStrength02, SkinSet,
    EyeColor, EyeShape, ScleraShape, Eyebrows, HeadHair, HeadHairRootColor, HeadHairTipColor,
    HeadHairLength, HeadHairContrast, HeadHairGradient, FacialHairLip, FacialHairChin, FacialHairCheek,
    FacialHairLipLength, FacialHairChinLength, FacialHairCheekLength, EyelashLength,
    FacialHairRootColor, FacialHairTipColor, FacialHairContrast, FacialHairGradient,
    RacialHorns, RacialHornsLength, NailColor, NailOpacity,
};
inline constexpr size_t kCustomizationFieldCount = 30;

struct CustomizationDecal {
    uint64_t guid = 0;
    uint32_t strength = 0, scale_u = 0, scale_v = 0;
    uint8_t  slot_index = 0;
    uint64_t merge_section_material_base = 0;
    uint32_t offset_u = 0, offset_v = 0, angle = 0;
    uint64_t color = 0;
    uint32_t discoloration = 0;
    bool operator==(const CustomizationDecal&) const = default;
};

struct CustomizationBlendGroup {
    uint64_t decal_appearance_guid = 0, color_guid = 0;
    uint32_t intensity = 0;
    bool operator==(const CustomizationBlendGroup&) const = default;
};

struct CustomizationSection {
    uint8_t key = 0;
    Vec3d   value;
    bool operator==(const CustomizationSection&) const = default;
};

struct CustomizationMorph {
    NameValue name;
    uint32_t  weight = 0;
    bool operator==(const CustomizationMorph&) const = default;
};

struct CharacterCustomization {
    std::optional<uint64_t> preset_guid;
    std::optional<uint32_t> random_seed;
    struct RaceGender {
        uint8_t race = 0, gender = 0;
        bool operator==(const RaceGender&) const = default;
    };
    std::optional<RaceGender> race_gender;
    std::map<CustomizationField, uint64_t> deltas;
    std::optional<std::vector<std::optional<CustomizationDecal>>>      decals;
    std::optional<std::vector<std::optional<CustomizationBlendGroup>>> blend_groups;
    std::optional<bool> helmet_visible, cape_visible;
    std::optional<std::vector<CustomizationSection>> sections_values;
    std::optional<std::vector<CustomizationMorph>>   face_morph_weights;
    bool operator==(const CharacterCustomization&) const = default;
};

struct CraftRarityCount {
    uint8_t rarity = 0;
    int32_t count = 0;
    bool    from_warehouse = false;
    bool operator==(const CraftRarityCount&) const = default;
};

struct CraftResource {
    int64_t                      record = 0;
    std::vector<CraftRarityCount> rarities;
    bool operator==(const CraftResource&) const = default;
};

struct CraftExperience {
    int64_t guid = 0;
    int64_t type = 0;
    int64_t amount = 0;
    bool operator==(const CraftExperience&) const = default;
};

struct CraftTagSelection {
    std::vector<CraftResource> resources;
    std::vector<std::string>   tags;
    bool operator==(const CraftTagSelection&) const = default;
};

/// Only a finished job (status 3) carries experience, products, and level on the wire.
struct CraftItemState {
    int64_t                      recipe = 0;
    int64_t                      sub_recipe = 0;
    double                       start_time = 0;
    float                        duration = 0;
    int32_t                      quantity = 0;
    std::vector<CraftResource>   general;
    std::vector<CraftResource>   flexible;
    GuidValue                    transaction;
    GuidValue                    job;
    uint8_t                      status = 0;
    std::vector<CraftExperience> experience;
    std::vector<ItemInstance>    products;
    int32_t                      unknown_a = 0, unknown_b = 0;
    int32_t                      level = 0;
    bool                         flag = true;
    uint8_t                      extra = 0;
    bool operator==(const CraftItemState& o) const {
        return recipe == o.recipe && sub_recipe == o.sub_recipe && same_bits(start_time, o.start_time) && same_bits(duration, o.duration) && quantity == o.quantity && general == o.general &&
               flexible == o.flexible && transaction == o.transaction && job == o.job && status == o.status && experience == o.experience && products == o.products && unknown_a == o.unknown_a &&
               unknown_b == o.unknown_b && level == o.level && flag == o.flag && extra == o.extra;
    }
};

struct RewardInstance {
    std::vector<ItemInstance> items;
    int32_t node_activity_experience = 0;
    int32_t guild_activity_experience = 0;
    bool operator==(const RewardInstance&) const = default;
};

struct HashedVariable {
    std::vector<std::pair<uint64_t, int32_t>> variables;
    bool operator==(const HashedVariable&) const = default;
};

struct WarehouseCost {
    int64_t currency = 0;
    int64_t type_id = 0;
    double  value = 0;
    bool operator==(const WarehouseCost& o) const { return currency == o.currency && type_id == o.type_id && same_bits(value, o.value); }
};

struct WarehouseSummaryEntry {
    int64_t                    record = 0;
    bool                       available = false;
    std::vector<WarehouseCost> costs;
    bool operator==(const WarehouseSummaryEntry&) const = default;
};

struct ElementState;

struct ArrayValue {
    std::vector<ElementState> elements;
    ArrayValue();
    ~ArrayValue();
    ArrayValue(const ArrayValue&);
    ArrayValue(ArrayValue&&) noexcept;
    ArrayValue& operator=(const ArrayValue&);
    ArrayValue& operator=(ArrayValue&&) noexcept;
    bool operator==(const ArrayValue&) const;
};

enum class ValueKind : uint8_t {
    None, Bool, Int, UInt, Float, Double, String, Name, Text, Object, SoftObject, Struct, Array,
};

class Value {
public:
    using Storage = std::variant<
        std::monostate, bool, int64_t, uint64_t, float, double, std::string,
        NameValue, TextValue, emit::FIntrepidNetworkGUID, SoftObjectValue, ArrayValue,
        Vec3d, Vec2d, Rot, QuantVec, Normal16, RepMovement, LoadTracker, UniqueNetId, Ticks,
        GameplayTag, GameplayTagContainer, StatInt32Inst,
        Quat, GuidToBoolWrapper, AlignmentContainer, AppearanceId, CharacterCustomization, ItemInstance, InlineObject,
        EventBox, HitConfirmations, PlayerMovementInput, VendorPurchaseCart, VendorTransactionCart, NodeTaxDetails, LocalizedMessage, ChannelingData, CraftItemState,
        RewardInstance, HashedVariable, WarehouseSummaryEntry, CraftTagSelection>;

    Value() = default;

    /// Non-scalar alternatives convert implicitly; scalars require named constructors to avoid integer/bool ambiguity.
    template <class T>
        requires(!std::is_arithmetic_v<std::remove_cvref_t<T>> && !std::is_pointer_v<std::remove_cvref_t<T>> &&
                 !std::is_same_v<std::remove_cvref_t<T>, Value> && std::is_constructible_v<Storage, T &&>)
    Value(T&& v) : v_(std::forward<T>(v)) {}

    static Value boolean(bool b)       { Value v; v.v_ = b; return v; }
    static Value i64(int64_t i)        { Value v; v.v_ = i; return v; }
    static Value u64(uint64_t u)       { Value v; v.v_ = u; return v; }
    static Value f32(float f)          { Value v; v.v_ = f; return v; }
    static Value f64(double d)         { Value v; v.v_ = d; return v; }
    static Value str(std::string s)    { Value v; v.v_ = std::move(s); return v; }

    ValueKind kind() const;

    template <class T> const T* get_if() const {
        if constexpr (std::is_same_v<T, ItemStorageEvent>) {
            const EventBox* b = std::get_if<EventBox>(&v_);
            return b ? b->get() : nullptr;
        } else {
            return std::get_if<T>(&v_);
        }
    }
    template <class T> T* get_if() {
        if constexpr (std::is_same_v<T, ItemStorageEvent>) {
            EventBox* b = std::get_if<EventBox>(&v_);
            return b ? b->get() : nullptr;
        } else {
            return std::get_if<T>(&v_);
        }
    }

    bool operator==(const Value& other) const;

private:
    Storage v_;
};

struct ElementState {
    std::map<uint16_t, Value> values;
    bool operator==(const ElementState& o) const { return values == o.values; }
};

}}}  // namespace aoc::protocol::repl
