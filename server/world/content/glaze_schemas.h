// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include <nlohmann/json.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace aoc::worldd::content::glaze {

template <typename Class, auto MemberPtr>
struct FieldDescriptor {
    using class_type = Class;
    using member_pointer_type = decltype(MemberPtr);
    static constexpr auto member = MemberPtr;
    std::string_view key{};
    static_assert(std::is_class_v<Class> && std::is_member_object_pointer_v<member_pointer_type>);
    constexpr auto& ref(Class& instance) const noexcept { return instance.*MemberPtr; }
    constexpr const auto& ref(const Class& instance) const noexcept { return instance.*MemberPtr; }
};

template <typename Class, auto MemberPtr>
consteval FieldDescriptor<Class, MemberPtr> field(std::string_view key) noexcept { return {key}; }

template <typename... Fields>
struct FieldSet {
    std::tuple<Fields...> items{};
    static constexpr std::size_t size() noexcept { return sizeof...(Fields); }
    static constexpr bool empty() noexcept { return sizeof...(Fields) == 0; }
    template <std::size_t Index>
    constexpr const auto& get() const noexcept { return std::get<Index>(items); }
    constexpr std::array<std::string_view, sizeof...(Fields)> keys() const noexcept {
        return std::apply([](const auto&... d) { return std::array<std::string_view, sizeof...(Fields)>{d.key...}; }, items);
    }
    consteval bool validate() const noexcept {
        if constexpr (sizeof...(Fields) == 0) return false;
        const auto names = keys();
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i].empty()) return false;
            for (std::size_t j = i + 1; j < names.size(); ++j)
                if (names[i] == names[j]) return false;
        }
        return true;
    }
};

template <typename... Fields>
consteval FieldSet<Fields...> make_fields(Fields... fields) noexcept { return {std::tuple<Fields...>{fields...}}; }

template <typename T> struct Schema;

struct QuestObjectiveJson {
    int64_t guid = 0, count = 1;
    std::string name{}, type{};
    double timeout = 0.0;
    bool show_dialogue = false, succeed_on_timeout = false;
    bool operator==(const QuestObjectiveJson&) const = default;
};
struct QuestRewardItemJson {
    int64_t table = 0, item = 0, quantity = 0;
    bool tag_query = false;
    bool operator==(const QuestRewardItemJson&) const = default;
};
struct LootDropEntryJson {
    int64_t item = 0, variation = 0, qty = 0;
    double weight = 1.0;
    bool operator==(const LootDropEntryJson&) const = default;
};
struct CraftIngredientJson {
    int64_t item = 0, filter = 0, quantity = 0, min_rarity = 0;
    std::string tags{};
    bool consume = true, primary = false;
    bool operator==(const CraftIngredientJson&) const = default;
};
struct NpcScheduleWaypointJson {
    double x = 0.0, y = 0.0, z = 0.0, pause = 0.0;
    bool looping = false;
    std::string anim{};
    bool operator==(const NpcScheduleWaypointJson&) const = default;
};
struct ProgressionTierJson {
    int64_t guid = 0, tier = 0, min_level = 1, max_level = 10;
    std::string name{};
    bool limited = false;
    bool operator==(const ProgressionTierJson&) const = default;
};

template <> struct Schema<QuestObjectiveJson> {
    using T = QuestObjectiveJson;
    static constexpr auto fields = make_fields(field<T, &T::guid>("guid"), field<T, &T::name>("name"),
        field<T, &T::type>("type"), field<T, &T::count>("count"), field<T, &T::timeout>("timeout"),
        field<T, &T::show_dialogue>("show_dialogue"), field<T, &T::succeed_on_timeout>("succeed_on_timeout"));
};
template <> struct Schema<QuestRewardItemJson> {
    using T = QuestRewardItemJson;
    static constexpr auto fields = make_fields(field<T, &T::table>("table"), field<T, &T::item>("item"),
        field<T, &T::quantity>("quantity"), field<T, &T::tag_query>("tag_query"));
};
template <> struct Schema<LootDropEntryJson> {
    using T = LootDropEntryJson;
    static constexpr auto fields = make_fields(field<T, &T::item>("item"), field<T, &T::variation>("variation"),
        field<T, &T::qty>("qty"), field<T, &T::weight>("weight"));
};
template <> struct Schema<CraftIngredientJson> {
    using T = CraftIngredientJson;
    static constexpr auto fields = make_fields(field<T, &T::item>("item"), field<T, &T::filter>("filter"),
        field<T, &T::tags>("tags"), field<T, &T::quantity>("quantity"), field<T, &T::min_rarity>("min_rarity"),
        field<T, &T::consume>("consume"), field<T, &T::primary>("primary"));
};
template <> struct Schema<NpcScheduleWaypointJson> {
    using T = NpcScheduleWaypointJson;
    static constexpr auto fields = make_fields(field<T, &T::x>("x"), field<T, &T::y>("y"), field<T, &T::z>("z"),
        field<T, &T::pause>("pause"), field<T, &T::looping>("looping"), field<T, &T::anim>("anim"));
};
template <> struct Schema<ProgressionTierJson> {
    using T = ProgressionTierJson;
    static constexpr auto fields = make_fields(field<T, &T::guid>("guid"), field<T, &T::name>("name"),
        field<T, &T::tier>("tier"), field<T, &T::min_level>("min_level"), field<T, &T::max_level>("max_level"),
        field<T, &T::limited>("limited"));
};

static_assert(Schema<QuestObjectiveJson>::fields.validate() && Schema<QuestObjectiveJson>::fields.size() == 7);
static_assert(Schema<QuestRewardItemJson>::fields.validate() && Schema<QuestRewardItemJson>::fields.size() == 4);
static_assert(Schema<LootDropEntryJson>::fields.validate() && Schema<LootDropEntryJson>::fields.size() == 4);
static_assert(Schema<CraftIngredientJson>::fields.validate() && Schema<CraftIngredientJson>::fields.size() == 7);
static_assert(Schema<NpcScheduleWaypointJson>::fields.validate() && Schema<NpcScheduleWaypointJson>::fields.size() == 6);
static_assert(Schema<ProgressionTierJson>::fields.validate() && Schema<ProgressionTierJson>::fields.size() == 6);

inline int64_t extract_int(const nlohmann::json& o, std::string_view key, int64_t fallback = 0) {
    const auto it = o.is_object() ? o.find(key) : o.end();
    return it != o.end() && it->is_number_integer() ? it->get<int64_t>() : fallback;
}
inline double extract_num(const nlohmann::json& o, std::string_view key, double fallback = 0.0) {
    const auto it = o.is_object() ? o.find(key) : o.end();
    return it != o.end() && it->is_number() ? it->get<double>() : fallback;
}
inline bool extract_flag(const nlohmann::json& o, std::string_view key, bool fallback = false) {
    const auto it = o.is_object() ? o.find(key) : o.end();
    if (it == o.end()) return fallback;
    return it->is_boolean() ? it->get<bool>() : (it->is_number_integer() ? (it->get<int64_t>() != 0) : fallback);
}
inline std::string extract_str(const nlohmann::json& o, std::string_view key, std::string_view fallback = {}) {
    const auto it = o.is_object() ? o.find(key) : o.end();
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string(fallback);
}

namespace detail {
template <typename Class, auto MemberPtr>
void read_member(const nlohmann::json& obj, Class& target, const FieldDescriptor<Class, MemberPtr>& desc) {
    auto& ref = desc.ref(target);
    using M = std::remove_cvref_t<decltype(ref)>;
    if constexpr (std::is_same_v<M, int64_t>) ref = extract_int(obj, desc.key, ref);
    else if constexpr (std::is_same_v<M, double>) ref = extract_num(obj, desc.key, ref);
    else if constexpr (std::is_same_v<M, bool>) ref = extract_flag(obj, desc.key, ref);
    else if constexpr (std::is_same_v<M, std::string>) ref = extract_str(obj, desc.key, ref);
}
} // namespace detail

template <typename T> bool read_json_obj(const nlohmann::json& obj, T& out) {
    static_assert(Schema<T>::fields.validate(), "Schema<T> must be valid");
    if (!obj.is_object()) return false;
    T parsed{};
    std::apply([&](const auto&... desc) { (detail::read_member(obj, parsed, desc), ...); }, Schema<T>::fields.items);
    out = std::move(parsed);
    return true;
}
template <typename T> bool read_json(std::string_view input, T& out) {
    if (input.empty()) return false;
    const nlohmann::json parsed = nlohmann::json::parse(input.begin(), input.end(), nullptr, false);
    return !parsed.is_discarded() && parsed.is_object() && read_json_obj(parsed, out);
}
template <typename T> nlohmann::json to_json_obj(const T& value) {
    static_assert(Schema<T>::fields.validate(), "Schema<T> must be valid");
    nlohmann::json obj = nlohmann::json::object();
    std::apply([&](const auto&... desc) { ((obj[std::string(desc.key)] = desc.ref(value)), ...); }, Schema<T>::fields.items);
    return obj;
}
template <typename T> std::string write_json(const T& value) { return to_json_obj(value).dump(); }

} // namespace aoc::worldd::content::glaze

namespace glz {
template <typename T> struct meta {
    using type = T;
    static constexpr auto value = aoc::worldd::content::glaze::Schema<T>::fields;
};
} // namespace glz
