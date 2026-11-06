// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/area.h  (plan E1, stage 1; G1, stage 1, Part B)
//
//  The area world fills with actors.  It is one of two shapes:
//
//    the Riverlands   (G1 stage 1: `AreaConfig::riverlands()`, world's default) the 12 zones of influence whose biome is Riverlands, 49.12 km^2 as 401 exact
//                     rectangles (spawns/region_data.h, the lookup in spawns/region_index.h).  Every point also has a zone of influence (ZOI), the guid of the
//                     record that owns its cell: the spawn filter's zone names (11 PopulationFilters name Aela, Winstead or NewAela) are answered by it.
//    a circle         (E1's: `AreaConfig{}`, and world's `--area-radius METRES` > 0) a disc around the login spot, 1 km by default.  It has no zones: a point's
//                     ZOI is 0 ("none") and a filter that names a zone does not spawn there, exactly as in E1, so E1's golden numbers stay E1's.
//
//  `--area-radius` / `AOC_AREA_RADIUS_M`: -1 (the default) the Riverlands, 0 no other actors, > 0 the circle of that radius.  This is the one place the area's
//  shape is set: nothing else reads a number of its own.
//
//  The centre is where a character with no saved position appears: the captured character's spawn point (bootstrap/character_setup.h kDefaultSpawn); the GM
//  vendors and the GM garden stand around it in either shape.
// ============================================================================
#pragma once

#include "bootstrap/character_setup.h"
#include "spawns/region_index.h"
#include "world/mesh_border.h"
#include "world/zoi_surface.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace aoc::worldd::spawns {

struct Box {
    double min_x = 0.0, min_y = 0.0, max_x = 0.0, max_y = 0.0;
    [[nodiscard]] bool contains(double x, double y) const noexcept {
        return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
    }
};

struct NodePlugObb {
    uint64_t         node_guid = 0;
    std::string_view name{};
    double           cx_cm = 0.0;            // CityNodeRecordBase.location.x
    double           cy_cm = 0.0;            // CityNodeRecordBase.location.y
    double           half_extent_cm = 50000.0; // 100,000 cm / 2
    double           yaw_deg = 0.0;

    [[nodiscard]] bool contains(double x_cm, double y_cm) const noexcept {
        const double dx = x_cm - cx_cm;
        const double dy = y_cm - cy_cm;
        const double rad = -yaw_deg * (3.14159265358979323846 / 180.0);
        const double c = std::cos(rad), s = std::sin(rad);
        const double lx = dx * c - dy * s;
        const double ly = dx * s + dy * c;
        return std::abs(lx) <= half_extent_cm && std::abs(ly) <= half_extent_cm;
    }
};

struct DynamicWorkerSeed {
    uint32_t worker_index = 0;
    uint64_t parent_zoi_guid = 0;
    bool     is_fixed_zoi = false;
    double   anchor_x_cm = 0.0;
    double   anchor_y_cm = 0.0;
    double   centroid_x_cm = 0.0;
    double   centroid_y_cm = 0.0;
    double   effective_radius_cm = 150000.0;
    uint32_t current_load = 0;
    uint32_t target_capacity = 4000;

    [[nodiscard]] double evaluate_authority_score(double x_cm, double y_cm) const noexcept {
        const double rx = is_fixed_zoi ? anchor_x_cm : (0.25 * anchor_x_cm + 0.75 * centroid_x_cm);
        const double ry = is_fixed_zoi ? anchor_y_cm : (0.25 * anchor_y_cm + 0.75 * centroid_y_cm);
        const double d = std::hypot(x_cm - rx, y_cm - ry);
        const double load_ratio = target_capacity > 0 ? static_cast<double>(current_load) / target_capacity : 0.0;
        const double shift = std::clamp((load_ratio - 1.0) * 280000.0, 0.0, 450000.0);
        const double eff_r = std::max(25000.0, effective_radius_cm);
        const double s_dist = std::clamp(1.0 - (d + shift) / (eff_r * 2.5), 0.0, 1.0);
        const double s_load = std::clamp(1.0 - load_ratio * 0.5, 0.0, 1.0);
        return 0.65 * s_dist + 0.35 * s_load;
    }
};

inline constexpr NodePlugObb kVerraNodePlugs[26] = {
    {108646108518008ULL,     "Winstead",    -644519.11,         376773.24,         50000.0,  319.20846},
    {108646121231993ULL,     "Joeva",       -1094396.04,        521336.34,         50000.0,  292.625506},
    {108646608033403ULL,     "NewAela",     -1134183.5439466983, 853387.2772475089, 50000.0, -90.21495968964565},
    {108646610851452ULL,     "Halcyon",     -891024.50,         296919.66,         50000.0,  180.0},
    {108646615701117ULL,     "Miraleth",    -459889.634548,     599317.092515,     50000.0, -20.0},
    {6064632225073725498ULL, "Windansea",   -1100624.184211,    246466.992911,     50000.0,  55.41214},
    {6064632734251679746ULL, "Aithanahr",   -232617.63,         762558.56,         50000.0,  304.121675},
    {6064632734253907971ULL, "Azmaran",     -278767.94,         987391.03,         50000.0,  351.156897},
    {6064632734268915716ULL, "Sunhaven",    -619803.283992,     873514.222232,     50000.0, -127.073107},
    {6064633097318170624ULL, "Djinna",       63370.908474,      861198.365739,     50000.0, -187.137862},
    {6064633097371058177ULL, "SquallsEnd",  -37370.0,           1133335.0,         50000.0, -217.228668},
    {6064633334652600320ULL, "Korrin",      -1764281.284871,    114557.650316,     50000.0,  90.707388},
    {6064633334741794817ULL, "Brinebarrel", -1860980.218817,   -171832.947158,     50000.0, -46.566062},
    {6064633407845105664ULL, "Seahook",     -1764263.887544,    539283.279654,     50000.0,  14.228906},
    {6064633407846088705ULL, "Shorefoot",   -1372642.268119,    57374.666533,      50000.0, -66.900158},
    {6064633425419042816ULL, "Wildport",    -1783982.0,         809682.0,          50000.0,  16.0},
    {6064633425517019141ULL, "Hecribba",    -1708500.0,         1053019.0,         50000.0,  246.0},
    {6064633425555816457ULL, "Vinebreach",  -1476480.0,         1178456.0,         50000.0,  200.0},
    {6064633429404090381ULL, "Arisalon",    -1250506.0,         1062377.0,         50000.0, -35.0},
    {6064633429423882257ULL, "TangledPost", -1031419.0,         1098848.0,         50000.0, -3.5},
    {6064633429437251605ULL, "Mythbreak",   -742705.0,          1087738.0,         50000.0, -159.058146},
    {6064633651969589265ULL, "Vexhelm",     -1463243.0,        -341306.0,          50000.0,  60.0},
    {6064633651985711129ULL, "Duunhold",    -1069784.0,        -150970.0,          50000.0,  0.0},
    {6064633652009173023ULL, "Dhurgrum",    -937374.0,         -411205.0,          50000.0, -15.0},
    {6064633652023394340ULL, "Vhalgadim",   -762862.0,         -563881.0,          50000.0,  349.999997},
    {6064633652044300331ULL, "KalTorhum",   -1271772.0,        -462821.0,          50000.0,  5.0},
};

struct ZoiRegionEntry {
    uint64_t         guid;
    RegionBit        bit;
    std::string_view name;
};

inline constexpr ZoiRegionEntry kZoiRegionTable[] = {
    {6064629126805651478ULL, RegionBit::Riverlands,   "Verra_RVR_Node_Miraleth"},
    {6064629126900678679ULL, RegionBit::Riverlands,   "Verra_RVR_Node_Winstead"},
    {6064629126902120472ULL, RegionBit::Riverlands,   "Verra_RVR_Node_Halcyon"},
    {6064629126905528345ULL, RegionBit::Riverlands,   "Verra_RVR_Node_Joeva"},
    {6064629126906904602ULL, RegionBit::Riverlands,   "Verra_RVR_Node_NewAela"},
    {6064629127133134848ULL, RegionBit::Riverlands,   "Verra_RVR_Dungeon_BefallenForge"},
    {6064629127135166465ULL, RegionBit::Riverlands,   "Verra_RVR_StartingArea_RuinsOfAela"},
    {6064629127486832640ULL, RegionBit::Riverlands,   "Verra_RVR_Dungeon_Harth"},
    {6064632150768680960ULL, RegionBit::TurquoiseSea, "Verra_TUR_Node_Windansea"},
    {6064632218459766784ULL, RegionBit::TurquoiseSea, "Verra_TUR_Palette10"},
    {6064632501420490752ULL, RegionBit::Riverlands,   "Verra_RVR_StartingArea_Samia"},
    {6064632734163927040ULL, RegionBit::Sandsquall,   "Verra_SAN_Lawless_Desert"},
    {6064632734314856453ULL, RegionBit::Sandsquall,   "Verra_SAN_Node_Sunhaven"},
    {6064632734318067718ULL, RegionBit::Sandsquall,   "Verra_SAN_Node_Azmaran"},
    {6064632751844032513ULL, RegionBit::Sandsquall,   "Verra_SAN_Node_Aithanahr"},
    {6064633097376432130ULL, RegionBit::Sandsquall,   "Verra_SAN_Node_Djinna"},
    {6064633097387311107ULL, RegionBit::Sandsquall,   "Verra_SAN_Node_SquallsEnd"},
    {6064633253929484288ULL, RegionBit::TurquoiseSea, "Verra_TUR_Node_Brinebarrel"},
    {6064633253929615361ULL, RegionBit::TurquoiseSea, "Verra_TUR_Node_Shorefoot"},
    {6064633253929680898ULL, RegionBit::TurquoiseSea, "Verra_TUR_Node_Korrin"},
    {6064633253929811971ULL, RegionBit::TurquoiseSea, "Verra_TUR_Node_Seahook"},
    {6064633253931057156ULL, RegionBit::TurquoiseSea, "Verra_TUR_HARBOR_Eastern"},
    {6064633253931122693ULL, RegionBit::TurquoiseSea, "Verra_TUR_HARBOR_Southern"},
    {6064633253931188230ULL, RegionBit::TurquoiseSea, "Verra_TUR_Harbor_TurqioseSeaTradingPost"},
    {6064633430628630549ULL, RegionBit::Jundark,      "Verra_JUN_Lawless_Jungle"},
    {6064633430643441687ULL, RegionBit::Jundark,      "Verra_JUN_Node_Hecribba"},
    {6064633430683156504ULL, RegionBit::Jundark,      "Verra_JUN_Node_Vinebreach"},
    {6064633430691414041ULL, RegionBit::Jundark,      "Verra_JUN_Node_Arisalon"},
    {6064633430733291549ULL, RegionBit::Jundark,      "Verra_JUN_Node_TangledPost"},
    {6064633430746988577ULL, RegionBit::Jundark,      "Verra_JUN_Node_Mythbreak"},
    {6064633448010220515ULL, RegionBit::Jundark,      "Verra_JUN_HARBOR_West"},
    {6064633448024376293ULL, RegionBit::Jundark,      "Verra_JUN_HARBOR_South"},
    {6064633448221772947ULL, RegionBit::Jundark,      "Verra_JUN_Node_Wildport"},
    {6064633448370277617ULL, RegionBit::Riverlands,   "Verra_RVR_HARBOR_BreezewaterBay"},
    {6064633463613227008ULL, RegionBit::Sandsquall,   "Verra_SAN_HARBOR_SandsquallRiver"},
    {6064633463714283523ULL, RegionBit::Sandsquall,   "Verra_SAN_HARBOR_SandsquallEastern"},
    {6064633486589362176ULL, RegionBit::TurquoiseSea, "Verra_Lawless_Ocean"},
    {6064633563092353291ULL, RegionBit::TurquoiseSea, "Verra_TUR_Lawless_Brinebarrel"},
    {6064633563104805132ULL, RegionBit::TurquoiseSea, "Verra_TUR_Lawless_Korrin"},
    {6064633563110965517ULL, RegionBit::TurquoiseSea, "Verra_TUR_Lawless_Seahook"},
    {6064633563124793614ULL, RegionBit::TurquoiseSea, "Verra_TUR_Lawless_Shorefoot"},
    {6064633563130298639ULL, RegionBit::TurquoiseSea, "Verra_TUR_Lawless_Windansea"},
    {6064633651517784064ULL, RegionBit::Anvils,       "Verra_ANV_StartingArea_Dunir"},
    {6064633651853983746ULL, RegionBit::Anvils,       "Verra_ANV_Node_Duunhold"},
    {6064633651868794883ULL, RegionBit::Anvils,       "Verra_ANV_StartingArea_Nikua"},
    {6064633651878100996ULL, RegionBit::Anvils,       "Verra_ANV_Node_Dhurgrum"},
    {6064633651881115654ULL, RegionBit::Anvils,       "Verra_ANV_Node_Vhalgadim"},
    {6064633651889766408ULL, RegionBit::Anvils,       "Verra_ANV_Node_KalTorhum"},
    {6064633651916963850ULL, RegionBit::Anvils,       "Verra_ANV_Node_Vexhelm"},
    {6064633651926335500ULL, RegionBit::Anvils,       "Verra_ANV_Harbor_Western"},
    {6064633663404507136ULL, RegionBit::Anvils,       "Verra_ANV_Castle_Terravolgen"},
    {6064633690992410624ULL, RegionBit::Anvils,       "Verra_ANV_StartingArea_DivineGate"},
    {6064633691224408065ULL, RegionBit::Anvils,       "Verra_ANV_Lawless_TabletopMts"},
    {6064634256757686288ULL, RegionBit::TurquoiseSea, "Verra_TUR_Palette52"},
    {6064634529098825728ULL, RegionBit::Anvils,       "Verra_ANV_StartingArea_Nikua_ArtisanPOI"},
    {6064634529147977729ULL, RegionBit::Anvils,       "Verra_ANV_StartingArea_Dunir_ArtisanPOI"},
    {6064634529259913218ULL, RegionBit::Riverlands,   "Verra_RVR_StartingArea_RuinsOfAela_ArtisanPOI"},
    {6064634529394065411ULL, RegionBit::Riverlands,   "Verra_RVR_StartingArea_Samia_ArtisanPOI"},
    {6064634760428585000ULL, RegionBit::Anvils,       "Verra_ANV_Harbor_Eastern"},
};

[[nodiscard]] inline RegionBit region_of_zoi_guid(uint64_t zoi_guid) noexcept {
    if (zoi_guid == 0) return RegionBit::None;
    const auto* end = kZoiRegionTable + (sizeof(kZoiRegionTable) / sizeof(kZoiRegionTable[0]));
    const auto* it = std::lower_bound(kZoiRegionTable, end, zoi_guid,
                                      [](const ZoiRegionEntry& e, uint64_t g) { return e.guid < g; });
    return (it != end && it->guid == zoi_guid) ? it->bit : RegionBit::None;
}

[[nodiscard]] constexpr std::string_view region_bit_slug(RegionBit bit) noexcept {
    switch (bit) {
        case RegionBit::Riverlands:   return "riverlands";
        case RegionBit::Sandsquall:   return "sandsquall";
        case RegionBit::Jundark:      return "jundark";
        case RegionBit::Anvils:       return "anvils";
        case RegionBit::TurquoiseSea: return "turquoise_sea";
        case RegionBit::All:          return "continent";
        default:                      return "none";
    }
}

[[nodiscard]] inline constexpr RegionBit node_plug_region_bit(size_t idx) noexcept {
    if (idx <= 4) return RegionBit::Riverlands;
    if (idx == 5 || (idx >= 11 && idx <= 14)) return RegionBit::TurquoiseSea;
    if (idx >= 6 && idx <= 10) return RegionBit::Sandsquall;
    if (idx >= 15 && idx <= 20) return RegionBit::Jundark;
    return RegionBit::Anvils;
}

[[nodiscard]] inline RegionBit resolve_point_region_bit(double x_cm, double y_cm) noexcept {
    if (!std::isfinite(x_cm) || !std::isfinite(y_cm)) return RegionBit::None;
    const uint64_t g = world::zoi_at(x_cm, y_cm);
    const RegionBit bit = region_of_zoi_guid(g);
    if (bit != RegionBit::None) return bit;
    if (region::Index::get().contains(x_cm, y_cm)) return RegionBit::Riverlands;
    size_t best_idx = 0;
    double best_d2 = 1.0e30;
    for (size_t i = 0; i < (sizeof(kVerraNodePlugs) / sizeof(kVerraNodePlugs[0])); ++i) {
        const double dx = x_cm - kVerraNodePlugs[i].cx_cm;
        const double dy = y_cm - kVerraNodePlugs[i].cy_cm;
        const double d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best_idx = i;
        }
    }
    return node_plug_region_bit(best_idx);
}

[[nodiscard]] inline float signed_region_border_distance_cm(
    double x_cm, double y_cm, RegionBit current_region) noexcept {
    if (!std::isfinite(x_cm) || !std::isfinite(y_cm)) return 0.0f;
    const uint8_t cur_mask = static_cast<uint8_t>(current_region);
    if (cur_mask == 0) return 0.0f;
    const auto in_cur = [cur_mask](double px, double py) noexcept {
        return (static_cast<uint8_t>(resolve_point_region_bit(px, py)) & cur_mask) != 0;
    };
    const bool inside_now = in_cur(x_cm, y_cm);
    static constexpr double kDirs[16][2] = {
        {1.0, 0.0}, {-1.0, 0.0}, {0.0, 1.0}, {0.0, -1.0},
        {0.70710678, 0.70710678}, {-0.70710678, 0.70710678},
        {0.70710678, -0.70710678}, {-0.70710678, -0.70710678},
        {0.92387953, 0.38268343}, {-0.92387953, 0.38268343},
        {0.92387953, -0.38268343}, {-0.92387953, -0.38268343},
        {0.38268343, 0.92387953}, {-0.38268343, 0.92387953},
        {0.38268343, -0.92387953}, {-0.38268343, -0.92387953},
    };
    for (double r = 500.0; r <= 20000.0; r += 500.0) {
        for (const auto& d : kDirs) {
            if (in_cur(x_cm + d[0] * r, y_cm + d[1] * r) != inside_now) {
                double lo = r - 500.0;
                double hi = r;
                for (int iter = 0; iter < 5; ++iter) {
                    const double mid = 0.5 * (lo + hi);
                    if (in_cur(x_cm + d[0] * mid, y_cm + d[1] * mid) != inside_now) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                const float dist = static_cast<float>(0.5 * (lo + hi));
                return inside_now ? -dist : dist;
            }
        }
    }
    return inside_now ? -20000.0f : 20000.0f;
}

[[nodiscard]] inline std::string normalize_area_token(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

[[nodiscard]] inline const NodePlugObb* find_node_plug(std::string_view name_or_guid) {
    const std::string key = normalize_area_token(name_or_guid);
    if (key.empty()) return nullptr;
    std::string raw(name_or_guid);
    char* endp = nullptr;
    const unsigned long long g = std::strtoull(raw.c_str(), &endp, 10);
    for (const NodePlugObb& p : kVerraNodePlugs) {
        if (endp != raw.c_str() && g != 0 && p.node_guid == static_cast<uint64_t>(g)) return &p;
        const std::string pkey = normalize_area_token(p.name);
        if (key == pkey || (key.size() > pkey.size() && key.compare(key.size() - pkey.size(), pkey.size(), pkey) == 0)) {
            return &p;
        }
    }
    return nullptr;
}

[[nodiscard]] inline uint64_t find_zoi_guid(std::string_view name_or_guid) {
    std::string raw(name_or_guid);
    char* endp = nullptr;
    const unsigned long long g = std::strtoull(raw.c_str(), &endp, 10);
    if (endp != raw.c_str() && g != 0) return static_cast<uint64_t>(g);
    const std::string key = normalize_area_token(name_or_guid);
    if (key.empty()) return 0;
    for (const ZoiRegionEntry& e : kZoiRegionTable) {
        const std::string ekey = normalize_area_token(e.name);
        if (key == ekey || (ekey.size() > key.size() && ekey.compare(ekey.size() - key.size(), key.size(), key) == 0)) {
            return e.guid;
        }
    }
    return 0;
}

struct WorkerAreaSpec {
    double           area_radius_m = -1.0;
    std::string_view area_region{};
    std::string_view area_zoi{};
    std::string_view exclude_zoi{};
    std::string_view node_plug{};
    std::string_view exclude_node_plug{};
    bool             no_zone_filter = false;
};

struct AreaConfig {
    double                           center_x = bootstrap::kDefaultSpawn.x;   // cm: the login spot
    double                           center_y = bootstrap::kDefaultSpawn.y;
    double                           radius_cm = 100000.0;                    // the circle's: 1 km
    bool                             region = false;                          // true: the Riverlands or whole continent (radius_cm is then only a hint for tools)
    bool                             whole_world = false;                     // true: 55-ZOI surface table lookup enabled
    bool                             zones = true;                            // a point has a zone of influence (false: none, so a filter that names zones never passes: E1's rule)
    uint8_t                          region_mask = static_cast<uint8_t>(RegionBit::Riverlands);
    std::string                      region_label = "riverlands";
    std::vector<uint64_t>            zoi_guids;
    std::vector<uint64_t>            exclude_zoi_guids;
    std::vector<NodePlugObb>         include_node_plugs;
    std::vector<NodePlugObb>         exclude_node_plugs;
    std::vector<Box>                 node_boxes;
    std::optional<uint32_t>          active_dynamic_worker_index;
    std::vector<DynamicWorkerSeed>   dynamic_peers;
// (C.O.A + Cyb)

    /// The Riverlands (exact 401-rectangle footprint for G1 stage 1 unit tests).
    static AreaConfig riverlands() {
        AreaConfig a;
        a.region = true;
        a.whole_world = false;
        a.region_mask = static_cast<uint8_t>(RegionBit::Riverlands);
        a.region_label = "riverlands";
        return a;
    }

    /// The whole Verra continent (all 55 zones of influence across Riverlands, Sandsquall, Jundark, Anvils, and Turquoise Sea).
    static AreaConfig continent() {
        AreaConfig a;
        a.region = true;
        a.whole_world = true;
        a.region_mask = static_cast<uint8_t>(RegionBit::All);
        a.region_label = "continent";
        return a;
    }

    /// Worker region partition factory (`riverlands`, `sandsquall`, `jundark`, `anvils`, `turquoise_sea`, or `continent`).
    static AreaConfig for_region(std::string_view name) {
        std::string key;
        key.reserve(name.size());
        for (char c : name) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        AreaConfig a = continent();
        if (key == "riverlands" || key == "rvr") {
            a.region_mask = static_cast<uint8_t>(RegionBit::Riverlands);
            a.region_label = "riverlands";
        } else if (key == "sandsquall" || key == "desert" || key == "san") {
            a.region_mask = static_cast<uint8_t>(RegionBit::Sandsquall);
            a.region_label = "sandsquall";
        } else if (key == "jundark" || key == "jungle" || key == "jun") {
            a.region_mask = static_cast<uint8_t>(RegionBit::Jundark);
            a.region_label = "jundark";
        } else if (key == "anvils" || key == "north" || key == "tabletop" || key == "anv") {
            a.region_mask = static_cast<uint8_t>(RegionBit::Anvils);
            a.region_label = "anvils";
        } else if (key == "turquoise_sea" || key == "turquoise-sea" || key == "tropics" || key == "tur") {
            a.region_mask = static_cast<uint8_t>(RegionBit::TurquoiseSea);
            a.region_label = "turquoise_sea";
        }
        return a;
    }

    static std::vector<uint64_t> parse_zoi_csv(std::string_view csv) {
        std::vector<uint64_t> out;
        for (size_t start = 0; start <= csv.size();) {
            const size_t comma = csv.find(',', start);
            const std::string_view tok = csv.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            const uint64_t g = find_zoi_guid(tok);
            if (g != 0) out.push_back(g);
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        return out;
    }

    static std::vector<NodePlugObb> parse_node_plug_csv(std::string_view csv) {
        std::vector<NodePlugObb> out;
        for (size_t start = 0; start <= csv.size();) {
            const size_t comma = csv.find(',', start);
            const std::string_view tok = csv.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            if (const NodePlugObb* p = find_node_plug(tok)) out.push_back(*p);
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        return out;
    }

    static std::string_view env_or(std::string_view cli, const char* env1, const char* env2) {
        if (!cli.empty()) return cli;
        if (const char* v1 = std::getenv(env1); v1 && *v1) return v1;
        if (const char* v2 = std::getenv(env2); v2 && *v2) return v2;
        return {};
    }

    static AreaConfig resolve_worker_area(const WorkerAreaSpec& spec) {
        if (spec.area_radius_m >= 0.0) {
            AreaConfig a;
            a.radius_cm = spec.area_radius_m * 100.0;
            a.region_label = spec.area_radius_m == 0.0 ? "empty" : "circle";
            return a;
        }
        const std::string_view region_sv = env_or(spec.area_region, "AOC_WORLD_REGION", "WORLD_REGION");
        const std::string_view zoi_sv = env_or(spec.area_zoi, "AOC_WORLD_ZOI", "WORLD_ZOI");
        const std::string_view ex_zoi_sv = env_or(spec.exclude_zoi, "AOC_WORLD_EXCLUDE_ZOI", "WORLD_EXCLUDE_ZOI");
        const std::string_view plug_sv = env_or(spec.node_plug, "AOC_WORLD_NODE_PLUG", "WORLD_NODE_PLUG");
        const std::string_view ex_plug_sv = env_or(spec.exclude_node_plug, "AOC_WORLD_EXCLUDE_NODE_PLUG", "WORLD_EXCLUDE_NODE_PLUG");

        AreaConfig a = !zoi_sv.empty() ? continent()
                     : (!region_sv.empty() ? for_region(region_sv) : continent());
        if (!zoi_sv.empty()) {
            a.zoi_guids = parse_zoi_csv(zoi_sv);
            a.region_label = "zoi";
        } else if (region_sv.empty() && !plug_sv.empty()) {
            a.region_mask = static_cast<uint8_t>(RegionBit::None);
            a.region_label = "node_plug";
        }
        if (!ex_zoi_sv.empty()) a.exclude_zoi_guids = parse_zoi_csv(ex_zoi_sv);
        if (!plug_sv.empty()) a.include_node_plugs = parse_node_plug_csv(plug_sv);
        if (!ex_plug_sv.empty()) a.exclude_node_plugs = parse_node_plug_csv(ex_plug_sv);
        a.zones = !spec.no_zone_filter;
        return a;
    }

    [[nodiscard]] bool owns_dynamic_cell(double x, double y) const {
        const DynamicWorkerSeed* self = nullptr;
        for (const DynamicWorkerSeed& p : dynamic_peers) {
            if (p.worker_index == *active_dynamic_worker_index) { self = &p; break; }
        }
        if (!self) return false;
        if (self->parent_zoi_guid != 0 && world::zoi_at(x, y) != self->parent_zoi_guid) return false;
        const double my_score = self->evaluate_authority_score(x, y);
        for (const DynamicWorkerSeed& p : dynamic_peers) {
            if (p.worker_index == self->worker_index) continue;
            if (self->parent_zoi_guid != 0 && p.parent_zoi_guid != 0 && p.parent_zoi_guid != self->parent_zoi_guid) continue;
            const double s = p.evaluate_authority_score(x, y);
            if (s > my_score || (s == my_score && p.worker_index < self->worker_index)) return false;
        }
        return true;
    }

    [[nodiscard]] bool has_carve_out(double x, double y) const {
        for (const NodePlugObb& plug : exclude_node_plugs) {
            if (plug.contains(x, y)) return true;
        }
        if (!exclude_zoi_guids.empty()) {
            const uint64_t g = world::zoi_at(x, y);
            if (g != 0 && std::find(exclude_zoi_guids.begin(), exclude_zoi_guids.end(), g) != exclude_zoi_guids.end()) return true;
        }
        return false;
    }

    bool contains(double x, double y) const {
        if (has_carve_out(x, y)) return false;
        for (const NodePlugObb& plug : include_node_plugs) {
            if (plug.contains(x, y)) return true;
        }
        if (active_dynamic_worker_index && !dynamic_peers.empty()) return owns_dynamic_cell(x, y);
        if (!node_boxes.empty()) {
            for (const Box& b : node_boxes) {
                if (b.contains(x, y)) return true;
            }
            return false;
        }
        if (!zoi_guids.empty()) {
            const uint64_t g = world::zoi_at(x, y);
            return g != 0 && std::find(zoi_guids.begin(), zoi_guids.end(), g) != zoi_guids.end();
        }
        if (region) {
            if (whole_world) {
                if (region_mask == static_cast<uint8_t>(RegionBit::None)) return false;
                const uint64_t g = world::zoi_at(x, y);
                if ((static_cast<uint8_t>(region_of_zoi_guid(g)) & region_mask) != 0) return true;
                return (region_mask & static_cast<uint8_t>(RegionBit::Riverlands)) != 0 && region::Index::get().contains(x, y);
            }
            return region::Index::get().contains(x, y);
        }
        return std::hypot(x - center_x, y - center_y) <= radius_cm;
    }

    [[nodiscard]] float signed_border_distance_cm(double x, double y) const noexcept {
        if (region_mask != 0 && region_mask != static_cast<uint8_t>(RegionBit::All)) {
            return signed_region_border_distance_cm(x, y, static_cast<RegionBit>(region_mask));
        }
        return static_cast<float>(std::hypot(x - center_x, y - center_y) - radius_cm);
    }

    [[nodiscard]] world::AuthorityState evaluate_handover(
        double x, double y, world::AuthorityState current_state,
        float hysteresis_cm = world::kRegionHandoverHysteresisCm) const noexcept {
        return world::evaluate_border_handover(signed_border_distance_cm(x, y), current_state, hysteresis_cm);
    }

    /// The guid of the zone of influence that owns (x, y); 0 when the area has no zones (a circle) or the point is outside every zone.
    uint64_t zoi_at(double x, double y) const {
        if (!region || !zones) return 0;
        if (whole_world) {
            const uint64_t g = world::zoi_at(x, y);
            if (g != 0) return g;
            return region::Index::get().zoi_guid(x, y);
        }
        return region::Index::get().zoi_guid(x, y);
    }
    /// The zone's record name ("Verra_RVR_StartingArea_RuinsOfAela"); nullptr as for zoi_at.
    const char* zoi_name_at(double x, double y) const { return region && zones ? region::Index::get().zoi_name(x, y) : nullptr; }
};

/// The login spot's height (cm): the captured character's spawn point.
inline constexpr double kLoginSpotZ = bootstrap::kDefaultSpawn.z;
/// The yaw the captured character faces at the login spot, degrees.
inline constexpr double kLoginSpotYaw = bootstrap::kDefaultSpawn.yaw_degrees;

}  // namespace aoc::worldd::spawns
