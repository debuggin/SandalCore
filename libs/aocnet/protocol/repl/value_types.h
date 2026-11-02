// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "protocol/emit/intrepid_netguid.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

inline bool same_bits(double a, double b) { return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b); }
inline bool same_bits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

struct NameValue {
    bool        hardcoded = false;
    uint32_t    index = 0;
    std::string text;
    int32_t     number = 0;
    bool operator==(const NameValue&) const = default;

    static NameValue hardcoded_name(uint32_t ename_index) { NameValue n; n.hardcoded = true; n.index = ename_index; return n; }
    static NameValue plain(std::string s, int32_t number = 0) { NameValue n; n.text = std::move(s); n.number = number; return n; }
};

struct TextValue {
    uint32_t    flags = 0;
    uint8_t     history = 0xFF;
    bool        has_string = false;
    std::string ns, key, source;
    bool operator==(const TextValue&) const = default;
};

struct SoftObjectValue {
    bool                       is_path = false;
    std::string                path;
    emit::FIntrepidNetworkGUID object;
    bool operator==(const SoftObjectValue&) const = default;
};

struct Vec3d {
    double x = 0, y = 0, z = 0;
    bool operator==(const Vec3d& o) const { return same_bits(x, o.x) && same_bits(y, o.y) && same_bits(z, o.z); }
};

struct Vec2d {
    double x = 0, y = 0;
    bool operator==(const Vec2d& o) const { return same_bits(x, o.x) && same_bits(y, o.y); }
};

struct Rot {
    uint16_t pitch = 0, yaw = 0, roll = 0;
    bool operator==(const Rot&) const = default;
};

struct QuantVec {
    enum class Mode : uint8_t { Scaled, Unscaled, RawFloat, RawDouble };
    Mode    mode = Mode::Scaled;
    int64_t x = 0, y = 0, z = 0;
    bool operator==(const QuantVec&) const = default;
};

struct Normal16 {
    uint16_t x = 0, y = 0, z = 0;
    bool operator==(const Normal16&) const = default;
};

struct RepMovement {
    bool                    simulated_physic_sleep = false;
    bool                    rep_physics = false;
    QuantVec                location, linear_velocity, angular_velocity;
    Rot                     rotation;
    std::optional<uint32_t> server_frame;
    std::optional<uint32_t> server_physics_handle;
    std::optional<QuantVec> acceleration;
    bool operator==(const RepMovement&) const = default;
};

struct LoadTracker {
    std::vector<bool> flags;
    bool operator==(const LoadTracker&) const = default;
};

struct UniqueNetId {
    uint8_t              flags = 0;
    uint8_t              services = 0;
    std::string          type;
    std::vector<uint8_t> bytes;
    std::string          id;
    bool operator==(const UniqueNetId&) const = default;
};

struct Ticks {
    int64_t ticks = 0;
    bool operator==(const Ticks&) const = default;
};

struct GameplayTag {
    NameValue name;
    bool operator==(const GameplayTag&) const = default;
};

struct GameplayTagContainer {
    std::vector<NameValue> tags;
    bool operator==(const GameplayTagContainer&) const = default;
};

struct StatInt32Inst {
    int64_t  record = 0;
    uint32_t value = 0;
    bool     extended = false;
    uint32_t base = 0, equipment = 0;
    bool operator==(const StatInt32Inst&) const = default;
};

struct Quat {
    double x = 0, y = 0, z = 0;
    bool operator==(const Quat& o) const { return same_bits(x, o.x) && same_bits(y, o.y) && same_bits(z, o.z); }
};

struct PlayerMovementInput {
    uint8_t bits = 0;
    bool operator==(const PlayerMovementInput&) const = default;
    static constexpr uint8_t kForward = 1, kBackward = 2, kRight = 4, kLeft = 8;
};

struct GuidValue {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    bool operator==(const GuidValue&) const = default;
};

struct GuidToBoolWrapper {
    std::vector<std::pair<GuidValue, bool>> pairs;
    bool operator==(const GuidToBoolWrapper&) const = default;
};

struct AlignmentContainer {
    struct Instance {
        uint64_t               key = 0;
        std::vector<GuidValue> guids;
        bool operator==(const Instance&) const = default;
    };
    struct Timer {
        uint32_t key = 0;
        uint64_t set_time_stamp = 0;
        uint64_t alignment_id_a = 0, alignment_id_b = 0;
        bool operator==(const Timer&) const = default;
    };
    std::vector<Instance> instances;
    std::vector<Timer>    timers;
    bool operator==(const AlignmentContainer&) const = default;
};

struct AppearanceId {
    uint64_t record_guid = 0;
    uint64_t type_id = 0;
    uint8_t  enchantment_level = 0;
    uint8_t  item_equip_slot = 0;
    bool operator==(const AppearanceId&) const = default;
};

struct SocketedGem {
    uint8_t  socket_index = 0;
    uint64_t item_record_id = 0;
    uint64_t socket_record_id = 0;
    uint32_t quality = 0;
    bool     flag = false;
    bool operator==(const SocketedGem&) const = default;
};

struct PersistedStat {
    uint64_t stat_id = 0;
    float    value = 0;
    bool operator==(const PersistedStat& o) const { return stat_id == o.stat_id && same_bits(value, o.value); }
};

struct PersistedCooldown {
    uint64_t record_guid = 0;
    double   date_time_start = 0;
    uint32_t saved_charges = 0;
    bool operator==(const PersistedCooldown& o) const {
        return record_guid == o.record_guid && same_bits(date_time_start, o.date_time_start) &&
               saved_charges == o.saved_charges;
    }
};

struct PersistedCustomData {
    uint8_t     type = 0;
    std::string text;
    bool operator==(const PersistedCustomData&) const = default;
};

struct ItemInstance {
    bool     has_item = false;
    uint64_t record_id = 0;
    uint32_t quantity = 0;
    uint32_t quality = 0;
    uint32_t enchantment_level = 0;
    uint32_t tempering_value = 0;
    std::vector<SocketedGem> gems;
    float    durability = 0;
    bool     stolen = false;
    GuidValue guid;
    int64_t  creation_time = 0;
    GuidValue crate_guid;
    uint64_t node_guid_purchased_from = 0;
    int64_t  lock_expiration = 0;
    std::vector<PersistedStat>       stats;
    std::vector<PersistedCooldown>   cooldowns;
    std::vector<PersistedCustomData> custom_data;
    double   synchronized_timestamp = 0;

    bool operator==(const ItemInstance& o) const {
        return has_item == o.has_item && record_id == o.record_id && quantity == o.quantity && quality == o.quality &&
               enchantment_level == o.enchantment_level && tempering_value == o.tempering_value && gems == o.gems &&
               same_bits(durability, o.durability) && stolen == o.stolen && guid == o.guid &&
               creation_time == o.creation_time && crate_guid == o.crate_guid &&
               node_guid_purchased_from == o.node_guid_purchased_from && lock_expiration == o.lock_expiration &&
               stats == o.stats && cooldowns == o.cooldowns && custom_data == o.custom_data &&
               same_bits(synchronized_timestamp, o.synchronized_timestamp);
    }
};

struct StorageEventSlot {
    int32_t   slot = 0;
    int32_t   count = 0;
    GuidValue item_guid;
    bool operator==(const StorageEventSlot&) const = default;
};

struct StorageEventContainer {
    emit::FIntrepidNetworkGUID     object;
    std::vector<StorageEventSlot>  slots;
    uint8_t                        status = 14;
    bool operator==(const StorageEventContainer&) const = default;
};

struct LocalizedMessage {
    std::string string_table_id;
    std::string key;
    bool operator==(const LocalizedMessage&) const = default;
};

struct CalloutQueueInfo {
    TextValue         text_message;
    std::string       message;
    LocalizedMessage  localized_message;
    uint8_t           callout_type = 0;
    int32_t           point_size = 0;
    uint8_t           chat_category = 0;
    std::string       formatted_on_screen;
    std::string       from_source_player;
    std::string       chat_emblems;
    uint8_t           node_level = 0;
    bool              sent_by_gm = false;
    ItemInstance      item;
    int64_t           toast_record_id = 0;
    int64_t           action_response_id = 0;
    Vec3d             context_location;
    Ticks             creation_time;
    bool operator==(const CalloutQueueInfo&) const = default;
};

struct ItemStorageEvent {
    StorageEventContainer              move_source;
    ItemInstance                       item;
    uint8_t                            type = 0;
    GuidValue                          instigator;
    std::optional<GuidValue>           extra_guid_a;
    std::optional<uint64_t>            extra_i64;
    std::optional<GuidValue>           extra_guid_b;
    std::vector<StorageEventContainer> targets;
    std::optional<CalloutQueueInfo>    callout;
    bool operator==(const ItemStorageEvent&) const = default;
};

/// Boxes ItemStorageEvent on the heap so Value's variant size stays bounded by ItemInstance.
class EventBox {
public:
    EventBox(ItemStorageEvent e) : p_(std::make_unique<ItemStorageEvent>(std::move(e))) {}
    EventBox(const EventBox& o) : p_(o.p_ ? std::make_unique<ItemStorageEvent>(*o.p_) : nullptr) {}
    EventBox(EventBox&&) noexcept = default;
    EventBox& operator=(const EventBox& o) { p_ = o.p_ ? std::make_unique<ItemStorageEvent>(*o.p_) : nullptr; return *this; }
    EventBox& operator=(EventBox&&) noexcept = default;
    const ItemStorageEvent* get() const { return p_.get(); }
    ItemStorageEvent*       get() { return p_.get(); }
    bool operator==(const EventBox& o) const { return p_ && o.p_ ? *p_ == *o.p_ : !p_ && !o.p_; }
private:
    std::unique_ptr<ItemStorageEvent> p_;
};

struct InlineObject {
    uint8_t                    flags = 0;
    emit::FIntrepidNetworkGUID outer;
    std::string                path;
    uint32_t                   checksum = 0;
    bool operator==(const InlineObject&) const = default;
};

}}}  // namespace aoc::protocol::repl
