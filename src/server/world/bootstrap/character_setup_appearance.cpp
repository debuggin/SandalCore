// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "bootstrap/character_setup.h"

#include "protocol/repl/codecs.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>

namespace aoc::worldd::bootstrap {

namespace {

using json = nlohmann::ordered_json;

constexpr const char* kRaceNames[] = {"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua", "Tulnar"};
constexpr const char* kGenderNames[] = {"Male", "Female"};
constexpr const char* kClassNames[] = {nullptr, "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"};

constexpr const char* kBodySections[kBodySectionCount] = {
    "None", "LowerArms", "UpperArms", "Clavicle", "Thigh", "Calf", "Neck", "LowerAbdomen", "Abdomen", "Chest",
    "Hips", "Size", "MouthGroup", "NoseGroup", "EyeGroup", "BrowGroup", "EarGroup", "Jaw", "EyeSocketL", "EyeSocketR",
    "EyeInnerR", "EyeInnerL", "EyeOuterL", "EyeOuterR", "SmilerL", "SmilerR", "Forehead", "Cheeks", "RacialGroup", "cCount",
};

std::string lower_first(std::string s) {
    if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z') s[0] = static_cast<char>(s[0] - 'A' + 'a');
    return s;
}

std::string upper_first(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

/// FName(const char*) as UE5 builds it: a trailing "_<digits>" is stored as digits + 1.
repl::NameValue name_from_string(const std::string& s) {
    size_t digits = 0;
    while (digits < s.size() && s[s.size() - 1 - digits] >= '0' && s[s.size() - 1 - digits] <= '9') ++digits;
    if (digits > 0 && digits < s.size() && s[s.size() - 1 - digits] == '_' && digits <= 10) {
        const char* first = s.data() + s.size() - digits;
        if (first[0] != '0' || digits == 1) {
            int64_t n = 0;
            std::from_chars(first, s.data() + s.size(), n);
            if (n < std::numeric_limits<int32_t>::max()) {
                return repl::NameValue::plain(s.substr(0, s.size() - digits - 1), static_cast<int32_t>(n + 1));
            }
        }
    }
    return repl::NameValue::plain(s, 0);
}

std::optional<std::string> name_to_string(const repl::NameValue& n) {
    if (n.hardcoded) return std::nullopt;
    if (n.number == 0) return n.text;
    return n.text + "_" + std::to_string(static_cast<int64_t>(n.number) - 1);
}

enum class Kind { Float, Int32, Id };

struct FieldSpec {
    repl::CustomizationField field;
    const char*              key;
    Kind                     kind;
};

using F = repl::CustomizationField;
constexpr FieldSpec kFields[repl::kCustomizationFieldCount] = {
    {F::SkinColorHue, "skinColorHue", Kind::Float},
    {F::SkinColorPigmentation, "skinColorPigmentation", Kind::Float},
    {F::NormalDetailStrength01, "normalDetailStrength01", Kind::Float},
    {F::NormalDetailStrength02, "normalDetailStrength02", Kind::Float},
    {F::SkinSet, "skinSet", Kind::Int32},
    {F::EyeColor, "eyeColor", Kind::Id},
    {F::EyeShape, "eyeShape", Kind::Id},
    {F::ScleraShape, "scleraShape", Kind::Id},
    {F::Eyebrows, "eyebrows", Kind::Id},
    {F::HeadHair, "headHair", Kind::Id},
    {F::HeadHairRootColor, "headHairRootColor", Kind::Id},
    {F::HeadHairTipColor, "headHairTipColor", Kind::Id},
    {F::HeadHairLength, "headHairLength", Kind::Float},
    {F::HeadHairContrast, "headHairContrast", Kind::Float},
    {F::HeadHairGradient, "headHairGradient", Kind::Float},
    {F::FacialHairLip, "facialHairLip", Kind::Id},
    {F::FacialHairChin, "facialHairChin", Kind::Id},
    {F::FacialHairCheek, "facialHairCheek", Kind::Id},
    {F::FacialHairLipLength, "facialHairLipLength", Kind::Float},
    {F::FacialHairChinLength, "facialHairChinLength", Kind::Float},
    {F::FacialHairCheekLength, "facialHairCheekLength", Kind::Float},
    {F::EyelashLength, "eyelashLength", Kind::Float},
    {F::FacialHairRootColor, "facialHairRootColor", Kind::Id},
    {F::FacialHairTipColor, "facialHairTipColor", Kind::Id},
    {F::FacialHairContrast, "facialHairContrast", Kind::Float},
    {F::FacialHairGradient, "facialHairGradient", Kind::Float},
    {F::RacialHorns, "racialHorns", Kind::Id},
    {F::RacialHornsLength, "racialHornsLength", Kind::Float},
    {F::NailColor, "nailColor", Kind::Id},
    {F::NailOpacity, "nailOpacity", Kind::Float},
};
static_assert(static_cast<size_t>(F::NailOpacity) + 1 == repl::kCustomizationFieldCount, "kFields covers every CustomizationField");

struct Reader {
    std::string error;

    bool fail(const std::string& path, const char* what) {
        error = "appearance: '" + path + "' " + what;
        return false;
    }

    bool read_float(const json& v, const std::string& path, uint32_t& bits) {
        if (!v.is_number()) return fail(path, "is not a number");
        const float f = static_cast<float>(v.get<double>());
        if (!std::isfinite(f)) return fail(path, "is not a finite float");
        bits = std::bit_cast<uint32_t>(f);
        return true;
    }

    bool read_int32(const json& v, const std::string& path, uint32_t& bits) {
        if (!(v.is_number_integer() || v.is_number_unsigned())) return fail(path, "is not an integer");
        const int64_t n = v.is_number_unsigned() ? (v.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                                                        ? std::numeric_limits<int64_t>::max()
                                                        : static_cast<int64_t>(v.get<uint64_t>()))
                                                 : v.get<int64_t>();
        if (n < std::numeric_limits<int32_t>::min() || n > std::numeric_limits<int32_t>::max()) return fail(path, "does not fit 32 bits");
        bits = static_cast<uint32_t>(static_cast<int32_t>(n));
        return true;
    }

    bool read_u8(const json& v, const std::string& path, uint8_t& out) {
        if (!(v.is_number_integer() || v.is_number_unsigned())) return fail(path, "is not an integer");
        if (v.is_number_integer() && !v.is_number_unsigned() && v.get<int64_t>() < 0) return fail(path, "does not fit a byte");
        if (v.get<uint64_t>() > 255) return fail(path, "does not fit a byte");
        out = static_cast<uint8_t>(v.get<uint64_t>());
        return true;
    }

    bool read_id(const json& v, const std::string& path, uint64_t& bits) {
        if (v.is_number_integer() || v.is_number_unsigned()) {
            bits = v.is_number_unsigned() ? v.get<uint64_t>() : static_cast<uint64_t>(v.get<int64_t>());
            return true;
        }
        if (!v.is_string()) return fail(path, "is not an id (a decimal string)");
        const std::string& s = v.get_ref<const std::string&>();
        if (s.empty()) return fail(path, "is an empty id");
        const char* end = s.data() + s.size();
        int64_t signed_value = 0;
        auto r = std::from_chars(s.data(), end, signed_value);
        if (r.ec == std::errc() && r.ptr == end) {
            bits = static_cast<uint64_t>(signed_value);
            return true;
        }
        uint64_t unsigned_value = 0;
        r = std::from_chars(s.data(), end, unsigned_value);
        if (r.ec == std::errc() && r.ptr == end) {
            bits = unsigned_value;
            return true;
        }
        return fail(path, "is not a 64-bit decimal id");
    }

    bool read_bool(const json& v, const std::string& path, bool& out) {
        if (!v.is_boolean()) return fail(path, "is not true or false");
        out = v.get<bool>();
        return true;
    }

    bool read_double(const json& v, const std::string& path, double& out) {
        if (!v.is_number()) return fail(path, "is not a number");
        out = v.get<double>();
        if (!std::isfinite(out)) return fail(path, "is not finite");
        return true;
    }

    const json* required(const json& obj, const char* key, const std::string& path) {
        const auto it = obj.find(key);
        if (it == obj.end()) {
            fail(path + "." + key, "is missing");
            return nullptr;
        }
        return &*it;
    }

    bool read_decal(const json& e, const std::string& path, repl::CustomizationDecal& d) {
        if (!e.is_object()) return fail(path, "is not an object");
        const auto id = [&](const char* key, uint64_t& out) {
            const json* v = required(e, key, path);
            return v && read_id(*v, path + "." + key, out);
        };
        const auto flt = [&](const char* key, uint32_t& out) {
            const json* v = required(e, key, path);
            return v && read_float(*v, path + "." + key, out);
        };
        const json* slot = required(e, "slotIndex", path);
        return id("guid", d.guid) && flt("strength", d.strength) && flt("scaleU", d.scale_u) && flt("scaleV", d.scale_v) &&
               slot && read_u8(*slot, path + ".slotIndex", d.slot_index) && id("mergeSectionMaterialBase", d.merge_section_material_base) &&
               flt("offsetU", d.offset_u) && flt("offsetV", d.offset_v) && flt("angle", d.angle) && id("color", d.color) &&
               flt("discoloration", d.discoloration);
    }

    bool read_blend_group(const json& e, const std::string& path, repl::CustomizationBlendGroup& g) {
        if (!e.is_object()) return fail(path, "is not an object");
        const json* a = required(e, "decalAppearanceGuid", path);
        const json* c = required(e, "colorGuid", path);
        const json* i = required(e, "intensity", path);
        return a && c && i && read_id(*a, path + ".decalAppearanceGuid", g.decal_appearance_guid) &&
               read_id(*c, path + ".colorGuid", g.color_guid) && read_float(*i, path + ".intensity", g.intensity);
    }
};

bool is_known_key(const std::string& key) {
    static const char* const kOthers[] = {"presetGuid", "randomSeed", "decalData", "decalBlendGroups", "bIsHelmetVisible",
                                          "bIsCapeVisible", "gender", "race", "class", "faceMorphWeightMaps", "sectionsValues"};
    for (const char* k : kOthers)
        if (key == k) return true;
    for (const FieldSpec& f : kFields)
        if (key == f.key) return true;
    return false;
}

bool parse_preset_and_seed(const json& j, Reader& rd, repl::CharacterCustomization& c) {
    if (const auto it = j.find("presetGuid"); it != j.end()) {
        uint64_t preset = 0;
        if (!rd.read_id(*it, "presetGuid", preset)) return false;
        if (preset != 0) {
            rd.error = "appearance: presetGuid " + std::to_string(preset) +
                       " is a preset: whether the client's preset table has it, which decides if Race and Gender are on the wire, is not known";
            return false;
        }
    }
    if (const auto it = j.find("randomSeed"); it != j.end()) {
        uint32_t seed = 0;
        if (!rd.read_int32(*it, "randomSeed", seed)) return false;
        if (seed != 0) c.random_seed = seed;
    }
    return true;
}

bool parse_scalar_fields(const json& j, Reader& rd, repl::CharacterCustomization& c) {
    for (const FieldSpec& f : kFields) {
        const auto it = j.find(f.key);
        if (it == j.end()) continue;
        uint32_t bits32 = 0;
        uint64_t bits64 = 0;
        bool ok = false;
        switch (f.kind) {
        case Kind::Float: ok = rd.read_float(*it, f.key, bits32); bits64 = bits32; break;
        case Kind::Int32: ok = rd.read_int32(*it, f.key, bits32); bits64 = bits32; break;
        case Kind::Id: ok = rd.read_id(*it, f.key, bits64); break;
        }
        if (!ok) return false;
        c.deltas[f.field] = bits64;
    }
    for (const auto& [key, dest] : {std::pair<const char*, std::optional<bool>*>{"bIsHelmetVisible", &c.helmet_visible},
                                    std::pair<const char*, std::optional<bool>*>{"bIsCapeVisible", &c.cape_visible}}) {
        if (const auto it = j.find(key); it != j.end()) {
            bool b = false;
            if (!rd.read_bool(*it, key, b)) return false;
            *dest = b;
        }
    }
    return true;
}

bool parse_decals_and_blends(const json& j, Reader& rd, repl::CharacterCustomization& c) {
    if (const auto it = j.find("decalData"); it != j.end()) {
        if (!it->is_array()) {
            rd.error = "appearance: 'decalData' is not an array";
            return false;
        }
        std::vector<std::optional<repl::CustomizationDecal>> v;
        for (size_t i = 0; i < it->size(); ++i) {
            if ((*it)[i].is_null()) {
                v.push_back(std::nullopt);
                continue;
            }
            repl::CustomizationDecal d;
            if (!rd.read_decal((*it)[i], "decalData[" + std::to_string(i) + "]", d)) return false;
            v.push_back(d);
        }
        c.decals = std::move(v);
    }
    if (const auto it = j.find("decalBlendGroups"); it != j.end()) {
        if (!it->is_array()) {
            rd.error = "appearance: 'decalBlendGroups' is not an array";
            return false;
        }
        std::vector<std::optional<repl::CustomizationBlendGroup>> v;
        for (size_t i = 0; i < it->size(); ++i) {
            if ((*it)[i].is_null()) {
                v.push_back(std::nullopt);
                continue;
            }
            repl::CustomizationBlendGroup g;
            if (!rd.read_blend_group((*it)[i], "decalBlendGroups[" + std::to_string(i) + "]", g)) return false;
            v.push_back(g);
        }
        c.blend_groups = std::move(v);
    }
    return true;
}

bool parse_sections(const json& j, Reader& rd, repl::CharacterCustomization& c) {
    const auto it = j.find("sectionsValues");
    if (it == j.end()) return true;
    if (!it->is_object()) {
        rd.error = "appearance: 'sectionsValues' is not an object";
        return false;
    }
    std::vector<repl::CustomizationSection> v;
    for (const auto& [name, value] : it->items()) {
        const std::string path = "sectionsValues." + name;
        size_t key = kBodySectionCount;
        for (size_t i = 0; i < kBodySectionCount; ++i)
            if (lower_first(kBodySections[i]) == name) key = i;
        if (key == kBodySectionCount) {
            rd.error = "appearance: '" + path + "' is not a body section";
            return false;
        }
        if (!value.is_object()) {
            rd.error = "appearance: '" + path + "' is not an object";
            return false;
        }
        repl::CustomizationSection s;
        s.key = static_cast<uint8_t>(key);
        const json* x = rd.required(value, "x", path);
        const json* y = rd.required(value, "y", path);
        const json* z = rd.required(value, "z", path);
        if (!x || !y || !z || !rd.read_double(*x, path + ".x", s.value.x) || !rd.read_double(*y, path + ".y", s.value.y) ||
            !rd.read_double(*z, path + ".z", s.value.z)) {
            return false;
        }
        v.push_back(s);
    }
    c.sections_values = std::move(v);
    return true;
}

bool parse_face_morphs(const json& j, Reader& rd, repl::CharacterCustomization& c) {
    const auto it = j.find("faceMorphWeightMaps");
    if (it == j.end()) return true;
    if (!it->is_object()) {
        rd.error = "appearance: 'faceMorphWeightMaps' is not an object";
        return false;
    }
    std::vector<repl::CustomizationMorph> v;
    for (const auto& [name, value] : it->items()) {
        repl::CustomizationMorph m;
        m.name = name_from_string(upper_first(name));
        if (!rd.read_float(value, "faceMorphWeightMaps." + name, m.weight)) return false;
        v.push_back(std::move(m));
    }
    c.face_morph_weights = std::move(v);
    return true;
}

bool validate_appearance_identity(const json& j, ClientRace race, ClientGender gender, std::string& error) {
    if (const auto it = j.find("race"); it != j.end()) {
        const auto named = it->is_string() ? client_race_from_name(it->get<std::string>()) : std::nullopt;
        if (!named) {
            error = "appearance: 'race' is not a race name";
            return false;
        }
        if (*named != race) {
            error = std::string("appearance: the appearance is of a ") + client_race_name(*named) + " but the character's race is " + client_race_name(race);
            return false;
        }
    }
    if (const auto it = j.find("gender"); it != j.end()) {
        const auto named = it->is_string() ? client_gender_from_name(it->get<std::string>()) : std::nullopt;
        if (!named) {
            error = "appearance: 'gender' is not a gender name";
            return false;
        }
        if (*named != gender) {
            error = std::string("appearance: the appearance is ") + client_gender_name(*named) + " but the character's gender is " + client_gender_name(gender);
            return false;
        }
    }
    if (const auto it = j.find("class"); it != j.end() && !it->is_string()) {
        error = "appearance: 'class' is not a name";
        return false;
    }
    return true;
}

bool write_scalar_fields(const repl::CharacterCustomization& c, json& j) {
    j["presetGuid"] = c.preset_guid ? std::to_string(static_cast<int64_t>(*c.preset_guid)) : std::string("0");
    j["randomSeed"] = c.random_seed ? static_cast<int64_t>(static_cast<int32_t>(*c.random_seed)) : 0;
    for (const FieldSpec& f : kFields) {
        const auto it = c.deltas.find(f.field);
        if (it == c.deltas.end()) continue;
        switch (f.kind) {
        case Kind::Float: {
            const float v = std::bit_cast<float>(static_cast<uint32_t>(it->second));
            if (!std::isfinite(v)) return false;
            j[f.key] = static_cast<double>(v);
            break;
        }
        case Kind::Int32: j[f.key] = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(it->second))); break;
        case Kind::Id: j[f.key] = std::to_string(static_cast<int64_t>(it->second)); break;
        }
    }
    return true;
}

void write_decals_and_blends(const repl::CharacterCustomization& c, json& j) {
    if (c.decals) {
        json a = json::array();
        for (const auto& d : *c.decals) {
            if (!d) {
                a.push_back(nullptr);
                continue;
            }
            json o = json::object();
            o["guid"] = std::to_string(static_cast<int64_t>(d->guid));
            const auto flt = [&](const char* key, uint32_t bits) { o[key] = static_cast<double>(std::bit_cast<float>(bits)); };
            flt("strength", d->strength);
            flt("scaleU", d->scale_u);
            flt("scaleV", d->scale_v);
            o["slotIndex"] = d->slot_index;
            o["mergeSectionMaterialBase"] = std::to_string(static_cast<int64_t>(d->merge_section_material_base));
            flt("offsetU", d->offset_u);
            flt("offsetV", d->offset_v);
            flt("angle", d->angle);
            o["color"] = std::to_string(static_cast<int64_t>(d->color));
            flt("discoloration", d->discoloration);
            a.push_back(std::move(o));
        }
        j["decalData"] = std::move(a);
    }
    if (c.blend_groups) {
        json a = json::array();
        for (const auto& g : *c.blend_groups) {
            if (!g) {
                a.push_back(nullptr);
                continue;
            }
            json o = json::object();
            o["decalAppearanceGuid"] = std::to_string(static_cast<int64_t>(g->decal_appearance_guid));
            o["colorGuid"] = std::to_string(static_cast<int64_t>(g->color_guid));
            o["intensity"] = static_cast<double>(std::bit_cast<float>(g->intensity));
            a.push_back(std::move(o));
        }
        j["decalBlendGroups"] = std::move(a);
    }
}

bool write_morphs_and_sections(const repl::CharacterCustomization& c, json& j) {
    if (c.face_morph_weights) {
        json o = json::object();
        for (const auto& m : *c.face_morph_weights) {
            const auto name = name_to_string(m.name);
            if (!name) return false;
            const float w = std::bit_cast<float>(m.weight);
            if (!std::isfinite(w)) return false;
            o[*name] = static_cast<double>(w);
        }
        j["faceMorphWeightMaps"] = std::move(o);
    }
    if (c.sections_values) {
        json o = json::object();
        for (const auto& s : *c.sections_values) {
            const char* name = body_section_name(s.key);
            if (!name || !std::isfinite(s.value.x) || !std::isfinite(s.value.y) || !std::isfinite(s.value.z)) return false;
            json v = json::object();
            v["x"] = s.value.x;
            v["y"] = s.value.y;
            v["z"] = s.value.z;
            o[lower_first(name)] = std::move(v);
        }
        j["sectionsValues"] = std::move(o);
    }
    return true;
}

}  // namespace

std::optional<ClientRace> client_race_from_id(int32_t race_id) {
    if (race_id < static_cast<int32_t>(ClientRace::Kaelar) || race_id > static_cast<int32_t>(ClientRace::Nikua)) return std::nullopt;
    return static_cast<ClientRace>(race_id);
}

std::optional<ClientGender> client_gender_from_id(int32_t gender_id) {
    if (gender_id < static_cast<int32_t>(ClientGender::Male) || gender_id > static_cast<int32_t>(ClientGender::Female)) return std::nullopt;
    return static_cast<ClientGender>(gender_id);
}

const char* client_class_name(int32_t class_id) {
    return class_id >= 1 && class_id < static_cast<int32_t>(std::size(kClassNames)) ? kClassNames[class_id] : nullptr;
}

const char* client_race_name(ClientRace race) {
    const size_t i = static_cast<size_t>(race);
    return i < std::size(kRaceNames) ? kRaceNames[i] : nullptr;
}

const char* client_gender_name(ClientGender gender) {
    const size_t i = static_cast<size_t>(gender);
    return i < std::size(kGenderNames) ? kGenderNames[i] : nullptr;
}

std::optional<ClientRace> client_race_from_name(std::string_view name) {
    for (size_t i = 0; i < std::size(kRaceNames); ++i)
        if (name == kRaceNames[i]) return static_cast<ClientRace>(i);
    return std::nullopt;
}

std::optional<ClientGender> client_gender_from_name(std::string_view name) {
    for (size_t i = 0; i < std::size(kGenderNames); ++i)
        if (name == kGenderNames[i]) return static_cast<ClientGender>(i);
    return std::nullopt;
}

const char* body_section_name(uint8_t value) { return value < kBodySectionCount ? kBodySections[value] : nullptr; }

CustomizationFromJson customization_from_json(std::string_view appearance_json, ClientRace race, ClientGender gender) {
    CustomizationFromJson out;
    if (appearance_json.size() > kMaxAppearanceBytes) {
        out.error = "appearance: too large (" + std::to_string(appearance_json.size()) + " bytes; the most is " +
                    std::to_string(kMaxAppearanceBytes) + ")";
        return out;
    }
    const json j = json::parse(appearance_json.begin(), appearance_json.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        out.error = "appearance: the character's appearance is not valid JSON";
        return out;
    }
    if (!j.is_object()) {
        out.error = "appearance: the character's appearance is not a JSON object";
        return out;
    }

    Reader rd;
    repl::CharacterCustomization c;
    c.race_gender = repl::CharacterCustomization::RaceGender{static_cast<uint8_t>(race), static_cast<uint8_t>(gender)};
    if (!parse_preset_and_seed(j, rd, c) || !parse_scalar_fields(j, rd, c) || !parse_decals_and_blends(j, rd, c) ||
        !parse_sections(j, rd, c) || !parse_face_morphs(j, rd, c)) {
        out.error = rd.error;
        return out;
    }
    if (!validate_appearance_identity(j, race, gender, out.error)) return out;

    for (const auto& [key, value] : j.items()) {
        (void)value;
        if (!is_known_key(key)) out.ignored_keys.push_back(key);
    }

    if (!repl::struct_accepts("FCharacterCustomizationSaveData", repl::Value(c))) {
        out.error = "appearance: the customization has a value the wire cannot carry (a morph or section named twice, or a name that is not text)";
        return out;
    }
    out.value = std::move(c);
    return out;
}

std::optional<std::string> customization_to_json(const repl::CharacterCustomization& c) {
    json j = json::object();
    if (!write_scalar_fields(c, j)) return std::nullopt;
    write_decals_and_blends(c, j);
    if (c.helmet_visible) j["bIsHelmetVisible"] = *c.helmet_visible;
    if (c.cape_visible) j["bIsCapeVisible"] = *c.cape_visible;
    if (c.race_gender) {
        if (const char* g = client_gender_name(static_cast<ClientGender>(c.race_gender->gender))) j["gender"] = g;
        if (const char* r = client_race_name(static_cast<ClientRace>(c.race_gender->race))) j["race"] = r;
    }
    if (!write_morphs_and_sections(c, j)) return std::nullopt;
    return j.dump();
}

}  // namespace aoc::worldd::bootstrap
