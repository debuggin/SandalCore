// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "bootstrap/character_setup.h"
#include "spawns/region_index.h"
#include "world/mesh_border.h"
#include "world/zoi_surface.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace aoc::worldd::spawns {

inline constexpr double kDefaultAoiBorderHaloCm = 35000.0;

struct Box {
    double min_x = 0.0, min_y = 0.0, max_x = 0.0, max_y = 0.0;
    [[nodiscard]] bool contains(double x, double y) const noexcept {
        return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
    }
};

struct NodePlugObb {
    uint64_t node_guid = 0; std::string_view name{};
    double cx_cm = 0.0, cy_cm = 0.0, half_extent_cm = 50000.0, yaw_deg = 0.0;
    [[nodiscard]] bool contains(double x_cm, double y_cm) const noexcept {
        const double dx = x_cm - cx_cm, dy = y_cm - cy_cm;
        const double rad = -yaw_deg * (3.14159265358979323846 / 180.0);
        const double c = std::cos(rad), s = std::sin(rad);
        return std::abs(dx * c - dy * s) <= half_extent_cm && std::abs(dx * s + dy * c) <= half_extent_cm;
    }
};

struct DynamicWorkerSeed {
    uint32_t worker_index = 0, current_load = 0, target_capacity = 4000;
    uint64_t parent_zoi_guid = 0;
    bool     is_fixed_zoi = false;
    double   anchor_x_cm = 0.0, anchor_y_cm = 0.0, centroid_x_cm = 0.0, centroid_y_cm = 0.0, effective_radius_cm = 150000.0;

    [[nodiscard]] double evaluate_authority_score(double x_cm, double y_cm) const noexcept {
        const double rx = is_fixed_zoi ? anchor_x_cm : (0.25 * anchor_x_cm + 0.75 * centroid_x_cm);
        const double ry = is_fixed_zoi ? anchor_y_cm : (0.25 * anchor_y_cm + 0.75 * centroid_y_cm);
        const double d = std::hypot(x_cm - rx, y_cm - ry);
        const double load_ratio = target_capacity > 0 ? static_cast<double>(current_load) / target_capacity : 0.0;
        const double shift = std::clamp((load_ratio - 1.0) * 280000.0, 0.0, 450000.0);
        const double eff_r = std::max(25000.0, effective_radius_cm);
        return 0.65 * std::clamp(1.0 - (d + shift) / (eff_r * 2.5), 0.0, 1.0) + 0.35 * std::clamp(1.0 - load_ratio * 0.5, 0.0, 1.0);
    }
};

inline constexpr NodePlugObb kVerraNodePlugs[26] = {
    {108646108518008ULL, "Winstead", -644519.11, 376773.24, 50000.0, 319.20846}, {108646121231993ULL, "Joeva", -1094396.04, 521336.34, 50000.0, 292.625506},
    {108646608033403ULL, "NewAela", -1134183.5439466983, 853387.2772475089, 50000.0, -90.21495968964565}, {108646610851452ULL, "Halcyon", -891024.50, 296919.66, 50000.0, 180.0},
    {108646615701117ULL, "Miraleth", -459889.634548, 599317.092515, 50000.0, -20.0}, {6064632225073725498ULL, "Windansea", -1100624.184211, 246466.992911, 50000.0, 55.41214},
    {6064632734251679746ULL, "Aithanahr", -232617.63, 762558.56, 50000.0, 304.121675}, {6064632734253907971ULL, "Azmaran", -278767.94, 987391.03, 50000.0, 351.156897},
    {6064632734268915716ULL, "Sunhaven", -619803.283992, 873514.222232, 50000.0, -127.073107}, {6064633097318170624ULL, "Djinna", 63370.908474, 861198.365739, 50000.0, -187.137862},
    {6064633097371058177ULL, "SquallsEnd", -37370.0, 1133335.0, 50000.0, -217.228668}, {6064633334652600320ULL, "Korrin", -1764281.284871, 114557.650316, 50000.0, 90.707388},
    {6064633334741794817ULL, "Brinebarrel", -1860980.218817, -171832.947158, 50000.0, -46.566062}, {6064633407845105664ULL, "Seahook", -1764263.887544, 539283.279654, 50000.0, 14.228906},
    {6064633407846088705ULL, "Shorefoot", -1372642.268119, 57374.666533, 50000.0, -66.900158}, {6064633425419042816ULL, "Wildport", -1783982.0, 809682.0, 50000.0, 16.0},
    {6064633425517019141ULL, "Hecribba", -1708500.0, 1053019.0, 50000.0, 246.0}, {6064633425555816457ULL, "Vinebreach", -1476480.0, 1178456.0, 50000.0, 200.0},
    {6064633429404090381ULL, "Arisalon", -1250506.0, 1062377.0, 50000.0, -35.0}, {6064633429423882257ULL, "TangledPost", -1031419.0, 1098848.0, 50000.0, -3.5},
    {6064633429437251605ULL, "Mythbreak", -742705.0, 1087738.0, 50000.0, -159.058146}, {6064633651969589265ULL, "Vexhelm", -1463243.0, -341306.0, 50000.0, 60.0},
    {6064633651985711129ULL, "Duunhold", -1069784.0, -150970.0, 50000.0, 0.0}, {6064633652009173023ULL, "Dhurgrum", -937374.0, -411205.0, 50000.0, -15.0},
    {6064633652023394340ULL, "Vhalgadim", -762862.0, -563881.0, 50000.0, 349.999997}, {6064633652044300331ULL, "KalTorhum", -1271772.0, -462821.0, 50000.0, 5.0},
};

struct ZoiRegionEntry { uint64_t guid; RegionBit bit; std::string_view name; };

inline constexpr ZoiRegionEntry kZoiRegionTable[] = {
    {6064629126805651478ULL, RegionBit::Riverlands, "Verra_RVR_Node_Miraleth"}, {6064629126900678679ULL, RegionBit::Riverlands, "Verra_RVR_Node_Winstead"},
    {6064629126902120472ULL, RegionBit::Riverlands, "Verra_RVR_Node_Halcyon"}, {6064629126905528345ULL, RegionBit::Riverlands, "Verra_RVR_Node_Joeva"},
    {6064629126906904602ULL, RegionBit::Riverlands, "Verra_RVR_Node_NewAela"}, {6064629127133134848ULL, RegionBit::Riverlands, "Verra_RVR_Dungeon_BefallenForge"},
    {6064629127135166465ULL, RegionBit::Riverlands, "Verra_RVR_StartingArea_RuinsOfAela"}, {6064629127486832640ULL, RegionBit::Riverlands, "Verra_RVR_Dungeon_Harth"},
    {6064632150768680960ULL, RegionBit::TurquoiseSea, "Verra_TUR_Node_Windansea"}, {6064632218459766784ULL, RegionBit::TurquoiseSea, "Verra_TUR_Palette10"},
    {6064632501420490752ULL, RegionBit::Riverlands, "Verra_RVR_StartingArea_Samia"}, {6064632734163927040ULL, RegionBit::Sandsquall, "Verra_SAN_Lawless_Desert"},
    {6064632734314856453ULL, RegionBit::Sandsquall, "Verra_SAN_Node_Sunhaven"}, {6064632734318067718ULL, RegionBit::Sandsquall, "Verra_SAN_Node_Azmaran"},
    {6064632751844032513ULL, RegionBit::Sandsquall, "Verra_SAN_Node_Aithanahr"}, {6064633097376432130ULL, RegionBit::Sandsquall, "Verra_SAN_Node_Djinna"},
    {6064633097387311107ULL, RegionBit::Sandsquall, "Verra_SAN_Node_SquallsEnd"}, {6064633253929484288ULL, RegionBit::TurquoiseSea, "Verra_TUR_Node_Brinebarrel"},
    {6064633253929615361ULL, RegionBit::TurquoiseSea, "Verra_TUR_Node_Shorefoot"}, {6064633253929680898ULL, RegionBit::TurquoiseSea, "Verra_TUR_Node_Korrin"},
    {6064633253929811971ULL, RegionBit::TurquoiseSea, "Verra_TUR_Node_Seahook"}, {6064633253931057156ULL, RegionBit::TurquoiseSea, "Verra_TUR_HARBOR_Eastern"},
    {6064633253931122693ULL, RegionBit::TurquoiseSea, "Verra_TUR_HARBOR_Southern"}, {6064633253931188230ULL, RegionBit::TurquoiseSea, "Verra_TUR_Harbor_TurqioseSeaTradingPost"},
    {6064633430628630549ULL, RegionBit::Jundark, "Verra_JUN_Lawless_Jungle"}, {6064633430643441687ULL, RegionBit::Jundark, "Verra_JUN_Node_Hecribba"},
    {6064633430683156504ULL, RegionBit::Jundark, "Verra_JUN_Node_Vinebreach"}, {6064633430691414041ULL, RegionBit::Jundark, "Verra_JUN_Node_Arisalon"},
    {6064633430733291549ULL, RegionBit::Jundark, "Verra_JUN_Node_TangledPost"}, {6064633430746988577ULL, RegionBit::Jundark, "Verra_JUN_Node_Mythbreak"},
    {6064633448010220515ULL, RegionBit::Jundark, "Verra_JUN_HARBOR_West"}, {6064633448024376293ULL, RegionBit::Jundark, "Verra_JUN_HARBOR_South"},
    {6064633448221772947ULL, RegionBit::Jundark, "Verra_JUN_Node_Wildport"}, {6064633448370277617ULL, RegionBit::Riverlands, "Verra_RVR_HARBOR_BreezewaterBay"},
    {6064633463613227008ULL, RegionBit::Sandsquall, "Verra_SAN_HARBOR_SandsquallRiver"}, {6064633463714283523ULL, RegionBit::Sandsquall, "Verra_SAN_HARBOR_SandsquallEastern"},
    {6064633486589362176ULL, RegionBit::TurquoiseSea, "Verra_Lawless_Ocean"}, {6064633563092353291ULL, RegionBit::TurquoiseSea, "Verra_TUR_Lawless_Brinebarrel"},
    {6064633563104805132ULL, RegionBit::TurquoiseSea, "Verra_TUR_Lawless_Korrin"}, {6064633563110965517ULL, RegionBit::TurquoiseSea, "Verra_TUR_Lawless_Seahook"},
    {6064633563124793614ULL, RegionBit::TurquoiseSea, "Verra_TUR_Lawless_Shorefoot"}, {6064633563130298639ULL, RegionBit::TurquoiseSea, "Verra_TUR_Lawless_Windansea"},
    {6064633651517784064ULL, RegionBit::Anvils, "Verra_ANV_StartingArea_Dunir"}, {6064633651853983746ULL, RegionBit::Anvils, "Verra_ANV_Node_Duunhold"},
    {6064633651868794883ULL, RegionBit::Anvils, "Verra_ANV_StartingArea_Nikua"}, {6064633651878100996ULL, RegionBit::Anvils, "Verra_ANV_Node_Dhurgrum"},
    {6064633651881115654ULL, RegionBit::Anvils, "Verra_ANV_Node_Vhalgadim"}, {6064633651889766408ULL, RegionBit::Anvils, "Verra_ANV_Node_KalTorhum"},
    {6064633651916963850ULL, RegionBit::Anvils, "Verra_ANV_Node_Vexhelm"}, {6064633651926335500ULL, RegionBit::Anvils, "Verra_ANV_Harbor_Western"},
    {6064633663404507136ULL, RegionBit::Anvils, "Verra_ANV_Castle_Terravolgen"}, {6064633690992410624ULL, RegionBit::Anvils, "Verra_ANV_StartingArea_DivineGate"},
    {6064633691224408065ULL, RegionBit::Anvils, "Verra_ANV_Lawless_TabletopMts"}, {6064634256757686288ULL, RegionBit::TurquoiseSea, "Verra_TUR_Palette52"},
    {6064634529098825728ULL, RegionBit::Anvils, "Verra_ANV_StartingArea_Nikua_ArtisanPOI"}, {6064634529147977729ULL, RegionBit::Anvils, "Verra_ANV_StartingArea_Dunir_ArtisanPOI"},
    {6064634529259913218ULL, RegionBit::Riverlands, "Verra_RVR_StartingArea_RuinsOfAela_ArtisanPOI"}, {6064634529394065411ULL, RegionBit::Riverlands, "Verra_RVR_StartingArea_Samia_ArtisanPOI"},
    {6064634760428585000ULL, RegionBit::Anvils, "Verra_ANV_Harbor_Eastern"},
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
    const RegionBit bit = region_of_zoi_guid(world::zoi_at(x_cm, y_cm));
    if (bit != RegionBit::None) return bit;
    if (region::Index::get().contains(x_cm, y_cm)) return RegionBit::Riverlands;
    size_t best_idx = 0; double best_d2 = 1.0e30;
    for (size_t i = 0; i < (sizeof(kVerraNodePlugs) / sizeof(kVerraNodePlugs[0])); ++i) {
        const double dx = x_cm - kVerraNodePlugs[i].cx_cm, dy = y_cm - kVerraNodePlugs[i].cy_cm;
        if (const double d2 = dx * dx + dy * dy; d2 < best_d2) { best_d2 = d2; best_idx = i; }
    }
    return node_plug_region_bit(best_idx);
}

template <typename Pred>
[[nodiscard]] inline float signed_border_distance_for_predicate(double x_cm, double y_cm, Pred&& in_cur) noexcept {
    if (!std::isfinite(x_cm) || !std::isfinite(y_cm)) return 0.0f;
    const bool inside_now = in_cur(x_cm, y_cm);
    static constexpr double kDirs[16][2] = {
        {1.0, 0.0}, {-1.0, 0.0}, {0.0, 1.0}, {0.0, -1.0},
        {0.70710678, 0.70710678}, {-0.70710678, 0.70710678}, {0.70710678, -0.70710678}, {-0.70710678, -0.70710678},
        {0.92387953, 0.38268343}, {-0.92387953, 0.38268343}, {0.92387953, -0.38268343}, {-0.92387953, -0.38268343},
        {0.38268343, 0.92387953}, {-0.38268343, 0.92387953}, {0.38268343, -0.92387953}, {-0.38268343, -0.92387953},
    };
    for (double r = 500.0; r <= 40000.0; r += 500.0) {
        for (const auto& d : kDirs) {
            if (in_cur(x_cm + d[0] * r, y_cm + d[1] * r) != inside_now) {
                double lo = r - 500.0, hi = r;
                for (int iter = 0; iter < 5; ++iter) {
                    const double mid = 0.5 * (lo + hi);
                    if (in_cur(x_cm + d[0] * mid, y_cm + d[1] * mid) != inside_now) hi = mid; else lo = mid;
                }
                const float dist = static_cast<float>(0.5 * (lo + hi));
                return inside_now ? -dist : dist;
            }
        }
    }
    return inside_now ? -40000.0f : 40000.0f;
}

[[nodiscard]] inline float signed_region_border_distance_cm(double x_cm, double y_cm, RegionBit current_region) noexcept {
    const uint8_t cur_mask = static_cast<uint8_t>(current_region);
    if (cur_mask == 0) return 0.0f;
    return signed_border_distance_for_predicate(x_cm, y_cm, [cur_mask](double px, double py) noexcept {
        return (static_cast<uint8_t>(resolve_point_region_bit(px, py)) & cur_mask) != 0;
    });
}

[[nodiscard]] inline std::string normalize_area_token(std::string_view s) {
    std::string out; out.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] inline const NodePlugObb* find_node_plug(std::string_view name_or_guid) {
    const std::string key = normalize_area_token(name_or_guid);
    if (key.empty()) return nullptr;
    std::string raw(name_or_guid); char* endp = nullptr;
    const unsigned long long g = std::strtoull(raw.c_str(), &endp, 10);
    for (const NodePlugObb& p : kVerraNodePlugs) {
        if (endp != raw.c_str() && g != 0 && p.node_guid == static_cast<uint64_t>(g)) return &p;
        const std::string pkey = normalize_area_token(p.name);
        if (key == pkey || (key.size() > pkey.size() && key.compare(key.size() - pkey.size(), pkey.size(), pkey) == 0)) return &p;
    }
    return nullptr;
}

[[nodiscard]] inline uint64_t find_zoi_guid(std::string_view name_or_guid) {
    std::string raw(name_or_guid); char* endp = nullptr;
    const unsigned long long g = std::strtoull(raw.c_str(), &endp, 10);
    if (endp != raw.c_str() && g != 0) return static_cast<uint64_t>(g);
    const std::string key = normalize_area_token(name_or_guid);
    if (key.empty()) return 0;
    for (const ZoiRegionEntry& e : kZoiRegionTable) {
        const std::string ekey = normalize_area_token(e.name);
        if (key == ekey || (ekey.size() > key.size() && ekey.compare(ekey.size() - key.size(), key.size(), key) == 0)) return e.guid;
    }
    return 0;
}

[[nodiscard]] inline bool point_in_convex_polygon_cm(
    double x_cm, double y_cm, const std::vector<std::pair<float, float>>& poly) noexcept {
    if (poly.size() < 3 || !std::isfinite(x_cm) || !std::isfinite(y_cm)) return false;
    int sign = 0;
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const auto [ax, ay] = poly[i];
        const auto [bx, by] = poly[(i + 1) % n];
        const double cross = (static_cast<double>(bx) - ax) * (y_cm - ay) - (static_cast<double>(by) - ay) * (x_cm - ax);
        if (std::abs(cross) <= 1e-3) continue;
        const int cur = cross > 0.0 ? 1 : -1;
        if (sign == 0) sign = cur;
        else if (sign != cur) return false;
    }
    return true;
}

[[nodiscard]] inline std::string slugify_worker_token(std::string_view raw) {
    std::string s(raw);
    for (const char* pfx : {"Verra_RVR_Node_", "Verra_SAN_Node_", "Verra_JUN_Node_", "Verra_ANV_Node_", "Verra_TUR_Node_", "Verra_"}) {
        const size_t n = std::strlen(pfx);
        if (s.size() > n && s.compare(0, n, pfx) == 0) { s.erase(0, n); break; }
    }
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        else if ((c == '_' || c == '-' || c == ' ') && !out.empty() && out.back() != '-') out.push_back('-');
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "world" : out;
}

inline constexpr size_t kMaxDynamicWorkersPerScope = 26;
inline constexpr std::string_view kDynamicWorkerNatoSuffixes[kMaxDynamicWorkersPerScope] = {
    "alpha", "beta", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india",
    "juliett", "kilo", "lima", "mike", "november", "oscar", "papa", "quebec", "romeo",
    "sierra", "tango", "uniform", "victor", "whiskey", "xray", "yankee", "zulu"
};

[[nodiscard]] constexpr std::string_view dynamic_worker_nato_suffix(size_t ordinal) noexcept {
    return kDynamicWorkerNatoSuffixes[ordinal < kMaxDynamicWorkersPerScope ? ordinal : (kMaxDynamicWorkersPerScope - 1)];
}

[[nodiscard]] inline int parse_dynamic_worker_nato_ordinal(std::string_view token) {
    const std::string key = normalize_area_token(token);
    if (key == "bravo") return 1;
    for (size_t i = 0; i < kMaxDynamicWorkersPerScope; ++i) {
        if (key == kDynamicWorkerNatoSuffixes[i]) return static_cast<int>(i);
    }
    return -1;
}

struct PoiCutoutEntry {
    uint64_t poi_guid = 0; RegionBit region = RegionBit::Riverlands;
    std::string_view slug{}; double cx_cm = 0.0, cy_cm = 0.0, radius_cm = 15000.0;
};

inline constexpr PoiCutoutEntry kPoiCutoutTable[] = {
    {900101ULL, RegionBit::Riverlands, "lionhold", -741157.0, 560319.0, 15000.0}, {900102ULL, RegionBit::Riverlands, "samias-hope", -549519.0, 376773.0, 15000.0},
    {900103ULL, RegionBit::Riverlands, "ashes-vigil", -999396.0, 521336.0, 15000.0}, {900104ULL, RegionBit::Riverlands, "carphin", -796024.0, 296920.0, 15000.0},
    {900105ULL, RegionBit::Riverlands, "ursine-caves", -1039184.0, 853387.0, 15000.0}, {900106ULL, RegionBit::Riverlands, "highwaymen-hills", -554890.0, 599317.0, 15000.0},
    {900107ULL, RegionBit::Riverlands, "bandit-camp", -644519.0, 471773.0, 15000.0}, {900108ULL, RegionBit::Riverlands, "oota-shrine", -1094396.0, 616336.0, 15000.0},
    {900109ULL, RegionBit::Riverlands, "befallen-forge", -739519.0, 376773.0, 15000.0}, {900110ULL, RegionBit::Riverlands, "gravepeak", -891024.0, 391920.0, 15000.0},
    {900111ULL, RegionBit::Riverlands, "steelbloom", -459890.0, 504317.0, 15000.0}, {900201ULL, RegionBit::Sandsquall, "sandsquall-oasis", -137618.0, 762559.0, 15000.0},
    {900202ULL, RegionBit::Sandsquall, "sunhaven-ruins", -524803.0, 873514.0, 15000.0}, {900203ULL, RegionBit::Sandsquall, "djinna-spire", -183768.0, 987391.0, 15000.0},
    {900301ULL, RegionBit::Jundark, "jundark-temple", -1613500.0, 1053019.0, 15000.0}, {900302ULL, RegionBit::Jundark, "vinebreach-falls", -1381480.0, 1178456.0, 15000.0},
    {900303ULL, RegionBit::Jundark, "arisalon-grove", -1155506.0, 1062377.0, 15000.0}, {900401ULL, RegionBit::Anvils, "kal-torhum", -1176772.0, -462821.0, 15000.0},
    {900402ULL, RegionBit::Anvils, "duunhold-depths", -974784.0, -150970.0, 15000.0}, {900403ULL, RegionBit::Anvils, "vhalgadim-keep", -842374.0, -411205.0, 15000.0},
    {900501ULL, RegionBit::TurquoiseSea, "turquoise-reef", -1195624.0, 246467.0, 15000.0}, {900502ULL, RegionBit::TurquoiseSea, "seahook-cove", -1669264.0, 539283.0, 15000.0},
    {900503ULL, RegionBit::TurquoiseSea, "coral-atoll", -1669281.0, 114558.0, 15000.0},
};

[[nodiscard]] inline std::vector<std::pair<float, float>> make_octagon_polygon_cm(double cx_cm, double cy_cm, double radius_cm) {
    std::vector<std::pair<float, float>> poly; poly.reserve(8);
    const double r = std::max(500.0, radius_cm);
    for (int i = 0; i < 8; ++i) {
        const double ang = static_cast<double>(i) * (3.14159265358979323846 / 4.0);
        poly.emplace_back(static_cast<float>(cx_cm + std::cos(ang) * r), static_cast<float>(cy_cm + std::sin(ang) * r));
    }
    return poly;
}

[[nodiscard]] inline const PoiCutoutEntry* find_poi_cutout(std::string_view token) {
    const std::string key = normalize_area_token(token);
    if (key.empty()) return nullptr;
    std::string raw(token); char* endp = nullptr;
    const unsigned long long g = std::strtoull(raw.c_str(), &endp, 10);
    for (const PoiCutoutEntry& e : kPoiCutoutTable) {
        if (endp != raw.c_str() && g != 0 && e.poi_guid == static_cast<uint64_t>(g)) return &e;
        const std::string ekey = normalize_area_token(e.slug);
        if (key == ekey || (key.size() > ekey.size() && key.compare(key.size() - ekey.size(), ekey.size(), ekey) == 0)) return &e;
    }
    return nullptr;
}

[[nodiscard]] inline std::vector<std::pair<float, float>> resolve_poi_polygon_cm(
    std::string_view token, std::string* resolved_name = nullptr) {
    if (const size_t at = token.find('@'); at != std::string_view::npos) {
        if (resolved_name) *resolved_name = slugify_worker_token(token.substr(0, at));
        const std::string_view coords = token.substr(at + 1);
        if (const size_t c1 = coords.find(':'); c1 != std::string_view::npos) {
            const size_t c2 = coords.find(':', c1 + 1);
            const double cx = std::strtod(std::string(coords.substr(0, c1)).c_str(), nullptr);
            const double cy = std::strtod(std::string(coords.substr(c1 + 1, c2 == std::string_view::npos ? std::string_view::npos : c2 - c1 - 1)).c_str(), nullptr);
            const double rad = (c2 != std::string_view::npos) ? std::strtod(std::string(coords.substr(c2 + 1)).c_str(), nullptr) : 15000.0;
            if (std::isfinite(cx) && std::isfinite(cy)) return make_octagon_polygon_cm(cx, cy, rad > 0.0 ? rad : 15000.0);
        }
    }
    if (const PoiCutoutEntry* e = find_poi_cutout(token)) {
        if (resolved_name) *resolved_name = std::string(e->slug);
        return make_octagon_polygon_cm(e->cx_cm, e->cy_cm, e->radius_cm);
    }
    return {};
}

[[nodiscard]] inline std::vector<uint64_t> zoi_guids_for_region(RegionBit region) {
    std::vector<uint64_t> out;
    for (const ZoiRegionEntry& e : kZoiRegionTable) if (e.bit == region) out.push_back(e.guid);
    return out;
}
[[nodiscard]] inline std::vector<uint64_t> riverlands_zoi_guids() { return zoi_guids_for_region(RegionBit::Riverlands); }
[[nodiscard]] inline std::vector<NodePlugObb> node_plugs_for_region(RegionBit region) {
    std::vector<NodePlugObb> out;
    for (size_t i = 0; i < (sizeof(kVerraNodePlugs) / sizeof(kVerraNodePlugs[0])); ++i) if (node_plug_region_bit(i) == region) out.push_back(kVerraNodePlugs[i]);
    return out;
}
[[nodiscard]] inline std::vector<PoiCutoutEntry> poi_cutouts_for_region(RegionBit region) {
    std::vector<PoiCutoutEntry> out;
    for (const PoiCutoutEntry& e : kPoiCutoutTable) if (e.region == region) out.push_back(e);
    return out;
}

[[nodiscard]] constexpr RegionBit pick_random_region(uint64_t seed) noexcept {
    constexpr RegionBit kRegions[5] = {RegionBit::Riverlands, RegionBit::Sandsquall, RegionBit::Anvils, RegionBit::TableMountains, RegionBit::TurquoiseSea};
    uint64_t z = seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return kRegions[(z ^ (z >> 31)) % 5];
}

struct WorkerAreaSpec {
    double area_radius_m = -1.0;
    std::string_view area_region{}, area_zoi{}, exclude_zoi{}, node_plug{}, exclude_node_plug{};
    bool no_zone_filter = false;
    std::string_view polygon_cutout{}, exclude_polygon{}, poi_cutout{}, exclude_poi_cutout{};
    int worker_index = -1;
};

struct AreaConfig {
    double center_x = bootstrap::kDefaultSpawn.x, center_y = bootstrap::kDefaultSpawn.y, radius_cm = 100000.0, aoi_border_halo_cm = kDefaultAoiBorderHaloCm;
    float handover_hysteresis_cm = world::kRegionHandoverHysteresisCm;
    bool region = false, whole_world = false, zones = true, dynamic_scaling_stub = false;
    uint8_t region_mask = static_cast<uint8_t>(RegionBit::Riverlands);
    int dynamic_ordinal = 0, worker_index = 0;
    std::string region_label = "riverlands", worker_name = "aoc-riverlands", topology_mode = "region", poi_cutout_name{};
    std::vector<uint64_t> zoi_guids, exclude_zoi_guids;
    std::vector<NodePlugObb> include_node_plugs, exclude_node_plugs;
    std::vector<Box> node_boxes;
    std::vector<std::pair<float, float>> polygon_cutout_cm;
    std::vector<std::vector<std::pair<float, float>>> excluded_polygons_cm;
    std::optional<uint32_t> active_dynamic_worker_index;
    std::vector<DynamicWorkerSeed> dynamic_peers;

    static AreaConfig riverlands() {
        AreaConfig a; a.region = true; a.whole_world = false;
        a.region_mask = static_cast<uint8_t>(RegionBit::Riverlands); a.region_label = "riverlands";
        return a;
    }

    static AreaConfig continent() {
        AreaConfig a; a.region = true; a.whole_world = true;
        a.region_mask = static_cast<uint8_t>(RegionBit::All); a.region_label = "continent";
        return a;
    }

    static AreaConfig for_region(std::string_view name) {
        const std::string key = normalize_area_token(name);
        AreaConfig a = continent();
        if (key == "riverlands" || key == "rvr") { a.region_mask = static_cast<uint8_t>(RegionBit::Riverlands); a.region_label = "riverlands"; }
        else if (key == "sandsquall" || key == "desert" || key == "san") { a.region_mask = static_cast<uint8_t>(RegionBit::Sandsquall); a.region_label = "sandsquall"; }
        else if (key == "jundark" || key == "jungle" || key == "jun" || key == "tablemountains") { a.region_mask = static_cast<uint8_t>(RegionBit::Jundark); a.region_label = "jundark"; }
        else if (key == "anvils" || key == "north" || key == "tabletop" || key == "anv") { a.region_mask = static_cast<uint8_t>(RegionBit::Anvils); a.region_label = "anvils"; }
        else if (key == "turquoisesea" || key == "tropics" || key == "tur") { a.region_mask = static_cast<uint8_t>(RegionBit::TurquoiseSea); a.region_label = "turquoise_sea"; }
        return a;
    }

    static std::vector<uint64_t> parse_zoi_csv(std::string_view csv) {
        std::vector<uint64_t> out;
        for (size_t start = 0; start <= csv.size();) {
            const size_t comma = csv.find(',', start);
            const std::string_view tok = csv.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            if (const uint64_t g = find_zoi_guid(tok); g != 0) out.push_back(g);
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

    static std::vector<std::pair<float, float>> parse_polygon_cm(std::string_view spec) {
        std::vector<std::pair<float, float>> out;
        for (size_t start = 0; start <= spec.size();) {
            const size_t sep = spec.find(';', start);
            const std::string_view pt = spec.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start);
            if (const size_t comma = pt.find(','); comma != std::string_view::npos) {
                const std::string sx(pt.substr(0, comma)), sy(pt.substr(comma + 1));
                char *ex = nullptr, *ey = nullptr;
                const float vx = std::strtof(sx.c_str(), &ex), vy = std::strtof(sy.c_str(), &ey);
                if (ex != sx.c_str() && ey != sy.c_str() && std::isfinite(vx) && std::isfinite(vy)) out.emplace_back(vx, vy);
            }
            if (sep == std::string_view::npos) break;
            start = sep + 1;
        }
        return out;
    }

    static std::vector<std::vector<std::pair<float, float>>> parse_polygon_list_cm(std::string_view spec) {
        std::vector<std::vector<std::pair<float, float>>> out;
        for (size_t start = 0; start <= spec.size();) {
            const size_t bar = spec.find('|', start);
            const std::string_view one = spec.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start);
            if (auto poly = parse_polygon_cm(one); poly.size() >= 3) out.push_back(std::move(poly));
            if (bar == std::string_view::npos) break;
            start = bar + 1;
        }
        return out;
    }

    static std::vector<std::vector<std::pair<float, float>>> parse_poi_polygon_list_cm(std::string_view csv) {
        std::vector<std::vector<std::pair<float, float>>> out;
        for (size_t start = 0; start <= csv.size();) {
            const size_t comma = csv.find(',', start);
            const std::string_view tok = csv.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            if (auto poly = resolve_poi_polygon_cm(tok); poly.size() >= 3) out.push_back(std::move(poly));
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

    void resolve_canonical_worker_name() {
        const std::string_view mode_sv = env_or({}, "AOC_WORLD_TOPOLOGY_MODE", "AOC_WORLD_SHARD_MODE");
        if (!mode_sv.empty()) topology_mode = std::string(mode_sv);
        dynamic_scaling_stub = (topology_mode == "dynamic_node_plug_zoi" || topology_mode == "dynamic_zoi");
        if (const std::string_view ord_sv = env_or({}, "AOC_WORLD_DYNAMIC_ORDINAL", "WORLD_DYNAMIC_ORDINAL"); !ord_sv.empty()) {
            const int by_name = parse_dynamic_worker_nato_ordinal(ord_sv);
            dynamic_ordinal = (by_name >= 0) ? by_name : std::clamp(static_cast<int>(std::strtol(std::string(ord_sv).c_str(), nullptr, 10)), 0, static_cast<int>(kMaxDynamicWorkersPerScope - 1));
        }
        if (const std::string_view wname = env_or({}, "AOC_WORLD_WORKER_NAME", "AOC_WORLD_WORKER_ID"); !wname.empty()) {
            const std::string slug = slugify_worker_token(wname);
            worker_name = slug.rfind("aoc-", 0) == 0 ? slug : ("aoc-" + slug);
            return;
        }
        const std::string_view kind = scope_kind();
        const std::string dyn_suffix = (dynamic_scaling_stub && (kind == "node_plug" || kind == "zoi"))
                                     ? ("-" + std::string(dynamic_worker_nato_suffix(static_cast<size_t>(dynamic_ordinal)))) : std::string{};
        if (kind == "world") {
            worker_name = "aoc-world";
        } else if (kind == "node_plug" && !include_node_plugs.empty()) {
            worker_name = "aoc-" + slugify_worker_token(include_node_plugs.front().name) + dyn_suffix;
        } else if (kind == "zoi" && !zoi_guids.empty()) {
            std::string_view zname = "zoi";
            for (const ZoiRegionEntry& e : kZoiRegionTable) {
                if (e.guid == zoi_guids.front()) { zname = e.name; break; }
            }
            worker_name = "aoc-" + slugify_worker_token(zname) + dyn_suffix;
        } else if (kind == "polygon_cutout") {
            const std::string_view pname = !poi_cutout_name.empty() ? std::string_view(poi_cutout_name) : env_or({}, "AOC_WORLD_POLYGON_NAME", "WORLD_POLYGON_NAME");
            worker_name = !pname.empty() ? ("aoc-poly-" + slugify_worker_token(pname)) : ("aoc-poly-" + std::to_string(worker_index));
        } else {
            worker_name = "aoc-" + slugify_worker_token(region_label);
        }
    }

    void apply_boundary_env_overrides() {
        if (const std::string_view h_sv = env_or({}, "AOC_WORLD_AOI_HALO_CM", "WORLD_AOI_HALO_CM"); !h_sv.empty()) {
            const double h = std::strtod(std::string(h_sv).c_str(), nullptr);
            if (std::isfinite(h) && h >= 0.0) aoi_border_halo_cm = std::clamp(h, 0.0, 200000.0);
        }
        handover_hysteresis_cm = world::runtime_handover_hysteresis_cm();
    }

    static AreaConfig resolve_worker_area(const WorkerAreaSpec& spec) {
        if (spec.area_radius_m >= 0.0) {
            AreaConfig a;
            a.radius_cm = spec.area_radius_m * 100.0;
            a.region_label = spec.area_radius_m == 0.0 ? "empty" : "circle";
            a.worker_index = std::max(0, spec.worker_index);
            a.apply_boundary_env_overrides();
            a.resolve_canonical_worker_name();
            return a;
        }
        const std::string_view region_sv = env_or(spec.area_region, "AOC_WORLD_REGION", "WORLD_REGION");
        const std::string_view zoi_sv = env_or(spec.area_zoi, "AOC_WORLD_ZOI", "WORLD_ZOI");
        const std::string_view ex_zoi_sv = env_or(spec.exclude_zoi, "AOC_WORLD_EXCLUDE_ZOI", "WORLD_EXCLUDE_ZOI");
        const std::string_view plug_sv = env_or(spec.node_plug, "AOC_WORLD_NODE_PLUG", "WORLD_NODE_PLUG");
        const std::string_view ex_plug_sv = env_or(spec.exclude_node_plug, "AOC_WORLD_EXCLUDE_NODE_PLUG", "WORLD_EXCLUDE_NODE_PLUG");
        const std::string_view poly_sv = env_or(spec.polygon_cutout, "AOC_WORLD_POLYGON_CUTOUT", "WORLD_POLYGON_CUTOUT");
        const std::string_view ex_poly_sv = env_or(spec.exclude_polygon, "AOC_WORLD_EXCLUDE_POLYGON_CUTOUT", "WORLD_EXCLUDE_POLYGON_CUTOUT");
        const std::string_view poi_sv = env_or(spec.poi_cutout, "AOC_WORLD_POI_CUTOUT", "WORLD_POI_CUTOUT");
        const std::string_view ex_poi_sv = env_or(spec.exclude_poi_cutout, "AOC_WORLD_EXCLUDE_POI_CUTOUT", "WORLD_EXCLUDE_POI_CUTOUT");

        AreaConfig a = !zoi_sv.empty() ? continent() : (!region_sv.empty() ? for_region(region_sv) : continent());
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
        if (!poly_sv.empty()) a.polygon_cutout_cm = parse_polygon_cm(poly_sv);
        if (!poi_sv.empty()) {
            if (auto poly = resolve_poi_polygon_cm(poi_sv, &a.poi_cutout_name); poly.size() >= 3) a.polygon_cutout_cm = std::move(poly);
        }
        if (!ex_poly_sv.empty()) a.excluded_polygons_cm = parse_polygon_list_cm(ex_poly_sv);
        if (!ex_poi_sv.empty()) {
            for (auto& p : parse_poi_polygon_list_cm(ex_poi_sv)) a.excluded_polygons_cm.push_back(std::move(p));
        }
        if (spec.worker_index >= 0) a.worker_index = spec.worker_index;
        else if (const std::string_view idx_sv = env_or({}, "AOC_WORLD_WORKER_INDEX", "WORLD_WORKER_INDEX"); !idx_sv.empty()) {
            a.worker_index = std::max(0, static_cast<int>(std::strtol(std::string(idx_sv).c_str(), nullptr, 10)));
        }
        a.zones = !spec.no_zone_filter;
        a.apply_boundary_env_overrides();
        a.resolve_canonical_worker_name();
        return a;
    }

    [[nodiscard]] std::string_view scope_kind() const noexcept {
        if (polygon_cutout_cm.size() >= 3) return "polygon_cutout";
        if (!include_node_plugs.empty()) return "node_plug";
        if (!zoi_guids.empty()) return "zoi";
        if (region_mask != static_cast<uint8_t>(RegionBit::All)) return "region";
        return "world";
    }

    [[nodiscard]] int default_tick_hz_for_scope() const noexcept {
        const std::string_view kind = scope_kind();
        if (kind == "polygon_cutout" || kind == "node_plug") {
            const char* primary = (kind == "polygon_cutout") ? "AOC_WORLD_TICK_HZ_POI" : "AOC_WORLD_TICK_HZ_NODE";
            for (const char* k : {primary, "AOC_WORLD_TICK_HZ_NODE", "AOC_WORLD_TICK_HZ_ZOI"}) {
                if (const char* v = std::getenv(k); v && *v) {
                    if (const long p = std::strtol(v, nullptr, 10); p >= 1 && p <= 120) return static_cast<int>(p);
                }
            }
            return 100;
        }
        const char* env_key = (kind == "zoi") ? "AOC_WORLD_TICK_HZ_ZOI"
                            : (kind == "region" ? "AOC_WORLD_TICK_HZ_REGION" : "AOC_WORLD_TICK_HZ_WORLD");
        const int fallback = (kind == "zoi") ? 60 : (kind == "region" ? 45 : 30);
        if (const char* v = std::getenv(env_key); v && *v) {
            if (const long p = std::strtol(v, nullptr, 10); p >= 1 && p <= 120) return static_cast<int>(p);
        }
        return fallback;
    }

    [[nodiscard]] size_t default_npc_sim_step_cap_for_scope() const noexcept {
        const std::string_view kind = scope_kind();
        const char* scope_env = (kind == "polygon_cutout") ? "AOC_WORLD_NPC_SIM_STEP_CAP_POI"
                              : (kind == "node_plug" ? "AOC_WORLD_NPC_SIM_STEP_CAP_NODE"
                              : (kind == "zoi" ? "AOC_WORLD_NPC_SIM_STEP_CAP_ZOI"
                              : (kind == "region" ? "AOC_WORLD_NPC_SIM_STEP_CAP_REGION" : "AOC_WORLD_NPC_SIM_STEP_CAP_WORLD")));
        for (const char* k : {scope_env, "AOC_WORLD_NPC_SIM_STEP_CAP"}) {
            if (const char* v = std::getenv(k); v && *v) {
                if (const long p = std::strtol(v, nullptr, 10); p >= 64 && p <= 50000) return static_cast<size_t>(p);
            }
        }
        if (kind == "polygon_cutout" || kind == "node_plug") return 1500;
        if (kind == "zoi") return 2600;
        return (kind == "region") ? 3600 : 5200;
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
        for (const auto& poly : excluded_polygons_cm) {
            if (point_in_convex_polygon_cm(x, y, poly)) return true;
        }
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
        if (polygon_cutout_cm.size() >= 3) return point_in_convex_polygon_cm(x, y, polygon_cutout_cm);
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
        if (!zoi_guids.empty() || !exclude_zoi_guids.empty() || !include_node_plugs.empty() || !exclude_node_plugs.empty() ||
            polygon_cutout_cm.size() >= 3 || !excluded_polygons_cm.empty() || (active_dynamic_worker_index && !dynamic_peers.empty())) {
            return signed_border_distance_for_predicate(x, y, [this](double px, double py) noexcept { return this->contains(px, py); });
        }
        if (region) {
            if (region_mask != 0 && region_mask != static_cast<uint8_t>(RegionBit::All)) {
                return spawns::signed_region_border_distance_cm(x, y, static_cast<RegionBit>(region_mask));
            }
            return -40000.0f;
        }
        return static_cast<float>(std::hypot(x - center_x, y - center_y) - radius_cm);
    }

    [[nodiscard]] float signed_region_border_distance_cm(double x, double y) const noexcept {
        return signed_border_distance_cm(x, y);
    }

    [[nodiscard]] bool uses_boundary_halo_for_spawns() const noexcept {
        return whole_world || aoi_border_halo_cm != kDefaultAoiBorderHaloCm || !zoi_guids.empty() ||
               !exclude_zoi_guids.empty() || !include_node_plugs.empty() || !exclude_node_plugs.empty() ||
               polygon_cutout_cm.size() >= 3 || !excluded_polygons_cm.empty();
    }

    [[nodiscard]] bool contains_with_halo(double x, double y, double halo_cm = -1.0) const noexcept {
        if (contains(x, y)) return true;
        const double h = (halo_cm >= 0.0) ? halo_cm : aoi_border_halo_cm;
        if (h <= 0.0 || !std::isfinite(x) || !std::isfinite(y)) return false;
        if (!region && polygon_cutout_cm.size() < 3 && include_node_plugs.empty() && zoi_guids.empty()) {
            return std::hypot(x - center_x, y - center_y) <= (radius_cm + h);
        }
        return static_cast<double>(signed_border_distance_cm(x, y)) <= h;
    }

    [[nodiscard]] world::AuthorityState evaluate_handover(
        double x, double y, world::AuthorityState current_state,
        float hysteresis_cm = world::kRegionHandoverHysteresisCm) const noexcept {
        return world::evaluate_border_handover(signed_border_distance_cm(x, y), current_state, hysteresis_cm);
    }
    uint64_t zoi_at(double x, double y) const {
        if (!region || !zones) return 0;
        if (whole_world) { const uint64_t g = world::zoi_at(x, y); return g != 0 ? g : region::Index::get().zoi_guid(x, y); }
        return region::Index::get().zoi_guid(x, y);
    }
    const char* zoi_name_at(double x, double y) const { return region && zones ? region::Index::get().zoi_name(x, y) : nullptr; }
};

enum class PartitionTopologyMode : uint8_t {
    SingleRiverlands = 0, SingleWorld = 1, PerRegion = 2,
    PerZoiInRegion = 3, PerZoiAndNodePlugsInRegion = 4, PerZoiNodePlugsAndPoisInRegion = 5,
};

[[nodiscard]] inline PartitionTopologyMode parse_partition_topology_mode_from_env(
    PartitionTopologyMode fallback = PartitionTopologyMode::SingleRiverlands) noexcept {
    for (const char* k : {"AOC_WORLD_PARTITION_MODE", "WORLD_PARTITION_MODE", "AOC_WORLD_TOPOLOGY_MODE"}) {
        if (const char* v = std::getenv(k); v && *v) {
            const std::string key = normalize_area_token(v);
            if (key == "singleriverlands") return PartitionTopologyMode::SingleRiverlands;
            if (key == "singleworld" || key == "world" || key == "continent") return PartitionTopologyMode::SingleWorld;
            if (key == "perregion" || key == "region") return PartitionTopologyMode::PerRegion;
            if (key == "perzoi" || key == "zoi" || key == "perzoiinregion") return PartitionTopologyMode::PerZoiInRegion;
            if (key == "perzoiandnodes" || key == "perzoiandnodeplugsinregion") return PartitionTopologyMode::PerZoiAndNodePlugsInRegion;
            if (key == "perzoinodesandpois" || key == "perzoinodeplugsandpoisinregion" || key == "mixed") return PartitionTopologyMode::PerZoiNodePlugsAndPoisInRegion;
        }
    }
    return fallback;
}

[[nodiscard]] inline RegionBit resolve_partition_region_from_env(uint64_t fallback_seed = 0x40A1B2C3ULL) noexcept {
    for (const char* k : {"AOC_WORLD_PARTITION_REGION", "WORLD_PARTITION_REGION", "AOC_WORLD_REGION"}) {
        if (const char* v = std::getenv(k); v && *v) {
            const std::string key = normalize_area_token(v);
            if (key == "random") return pick_random_region(fallback_seed);
            if (key == "riverlands" || key == "rvr") return RegionBit::Riverlands;
            if (key == "sandsquall" || key == "san") return RegionBit::Sandsquall;
            if (key == "anvils" || key == "anv") return RegionBit::Anvils;
            if (key == "tablemountains" || key == "jundark" || key == "jun") return RegionBit::TableMountains;
            if (key == "turquoisesea" || key == "tur") return RegionBit::TurquoiseSea;
        }
    }
    return pick_random_region(fallback_seed);
}

[[nodiscard]] inline std::vector<AreaConfig> build_partition_topology(
    PartitionTopologyMode mode, RegionBit target_region = RegionBit::Riverlands) {
    std::vector<AreaConfig> out;
    if (mode == PartitionTopologyMode::SingleRiverlands) {
        AreaConfig a = AreaConfig::for_region("riverlands"); a.worker_name = "aoc-riverlands";
        out.push_back(std::move(a)); return out;
    }
    if (mode == PartitionTopologyMode::SingleWorld) {
        AreaConfig a = AreaConfig::continent(); a.worker_name = "aoc-world";
        out.push_back(std::move(a)); return out;
    }
    if (mode == PartitionTopologyMode::PerRegion) {
        for (const char* r : {"riverlands", "sandsquall", "jundark", "anvils", "turquoise_sea"}) {
            AreaConfig a = AreaConfig::for_region(r); a.worker_index = static_cast<int>(out.size());
            a.resolve_canonical_worker_name(); out.push_back(std::move(a));
        }
        return out;
    }
    const auto zois = zoi_guids_for_region(target_region);
    const auto plugs = (mode != PartitionTopologyMode::PerZoiInRegion) ? node_plugs_for_region(target_region) : std::vector<NodePlugObb>{};
    const auto pois = (mode == PartitionTopologyMode::PerZoiNodePlugsAndPoisInRegion) ? poi_cutouts_for_region(target_region) : std::vector<PoiCutoutEntry>{};
    std::vector<std::vector<std::pair<float, float>>> poi_polys;
    for (const PoiCutoutEntry& pe : pois) poi_polys.push_back(make_octagon_polygon_cm(pe.cx_cm, pe.cy_cm, pe.radius_cm));
    for (uint64_t g : zois) {
        AreaConfig a = AreaConfig::continent(); a.zoi_guids = {g}; a.region_label = "zoi";
        a.exclude_node_plugs = plugs; a.excluded_polygons_cm = poi_polys; a.worker_index = static_cast<int>(out.size());
        a.resolve_canonical_worker_name(); out.push_back(std::move(a));
    }
    for (const NodePlugObb& p : plugs) {
        AreaConfig a = AreaConfig::continent(); a.region_mask = static_cast<uint8_t>(RegionBit::None);
        a.region_label = "node_plug"; a.include_node_plugs = {p}; a.worker_index = static_cast<int>(out.size());
        a.resolve_canonical_worker_name(); out.push_back(std::move(a));
    }
    for (size_t i = 0; i < pois.size(); ++i) {
        AreaConfig a = AreaConfig::continent(); a.polygon_cutout_cm = poi_polys[i];
        a.poi_cutout_name = std::string(pois[i].slug); a.worker_index = static_cast<int>(out.size());
        a.resolve_canonical_worker_name(); out.push_back(std::move(a));
    }
    return out;
}

inline constexpr double kLoginSpotZ = bootstrap::kDefaultSpawn.z;
inline constexpr double kLoginSpotYaw = bootstrap::kDefaultSpawn.yaw_degrees;

}  // namespace aoc::worldd::spawns
