// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/area_loader.cpp
//
//  See area_loader.h.
// ============================================================================
#include "spawns/area_loader.h"

#include "respawn/make_shrines.h"
#include "spawns/gm_garden.h"
#include "spawns/npc_looks.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

namespace aoc::worldd::spawns {

namespace {

namespace repl = ::aoc::protocol::repl;

constexpr double kFoliageCellCm = 25600.0;
constexpr double kFoliageBrushPadCm = 500.0;

struct FoliagePt {
    double x = 0.0, y = 0.0;
};

double cross_2d(FoliagePt a, FoliagePt b, FoliagePt c) noexcept {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

double dist_point_to_seg(double px, double py, FoliagePt a, FoliagePt b) noexcept {
    const double vx = b.x - a.x;
    const double vy = b.y - a.y;
    const double len_sq = vx * vx + vy * vy;
    if (len_sq <= 1.0e-12) return std::hypot(px - a.x, py - a.y);
    const double t = std::clamp(((px - a.x) * vx + (py - a.y) * vy) / len_sq, 0.0, 1.0);
    return std::hypot(px - (a.x + t * vx), py - (a.y + t * vy));
}

std::vector<FoliagePt> deduplicate_sorted_pts(std::vector<FoliagePt> pts) {
    std::sort(pts.begin(), pts.end(), [](FoliagePt a, FoliagePt b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    std::vector<FoliagePt> unique_pts;
    unique_pts.reserve(pts.size());
    for (const FoliagePt p : pts) {
        if (unique_pts.empty() || std::hypot(p.x - unique_pts.back().x, p.y - unique_pts.back().y) > kAreaEpsilonCm) {
            unique_pts.push_back(p);
        }
    }
    return unique_pts;
}

std::vector<FoliagePt> build_convex_hull(std::vector<FoliagePt> pts) {
    const std::vector<FoliagePt> unique_pts = deduplicate_sorted_pts(std::move(pts));
    if (unique_pts.size() <= 2) return unique_pts;
    std::vector<FoliagePt> hull;
    hull.reserve(unique_pts.size() + 1);
    for (const FoliagePt p : unique_pts) {
        while (hull.size() >= 2 && cross_2d(hull[hull.size() - 2], hull.back(), p) <= 0.0) hull.pop_back();
        hull.push_back(p);
    }
    const size_t lower = hull.size();
    for (size_t i = unique_pts.size() - 1; i > 0; --i) {
        const FoliagePt p = unique_pts[i - 1];
        while (hull.size() > lower && cross_2d(hull[hull.size() - 2], hull.back(), p) <= 0.0) hull.pop_back();
        hull.push_back(p);
    }
    hull.pop_back();
    return hull;
}

struct PaintedFoliageSection {
    double min_x = 1e18, max_x = -1e18;
    double min_y = 1e18, max_y = -1e18;
    std::vector<FoliagePt> hull;

    bool contains(double x, double y, double pad_cm) const noexcept {
        if (x < min_x - pad_cm || x > max_x + pad_cm || y < min_y - pad_cm || y > max_y + pad_cm) return false;
        if (hull.empty()) return false;
        if (hull.size() == 1) return std::hypot(x - hull[0].x, y - hull[0].y) <= pad_cm;
        if (hull.size() == 2) return dist_point_to_seg(x, y, hull[0], hull[1]) <= pad_cm;
        bool inside = true;
        for (size_t i = 0; i < hull.size(); ++i) {
            const FoliagePt a = hull[i];
            const FoliagePt b = hull[(i + 1) % hull.size()];
            if (cross_2d(a, b, {x, y}) < 0.0) inside = false;
            if (dist_point_to_seg(x, y, a, b) <= pad_cm) return true;
        }
        return inside;
    }
};

using RawSectionMap = std::unordered_map<std::string, std::pair<std::string, std::vector<FoliagePt>>>;

struct PaintedFoliageIndex {
    std::vector<PaintedFoliageSection> sections;
    std::unordered_map<std::string, std::unordered_map<uint64_t, std::vector<size_t>>> by_def_cell;
};

std::string leaf_of(const std::string& archetype_path) {
    const size_t dot = archetype_path.rfind('.');
    std::string leaf = archetype_path.substr(dot == std::string::npos ? 0 : dot + 1);
    if (leaf.rfind("Default__", 0) == 0) leaf = leaf.substr(9);
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    return leaf;
}

uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::string resource_type_of_def_name(std::string_view name) {
    const size_t first = name.find('_');
    if (first == std::string_view::npos) return std::string(name);
    const size_t second = name.find('_', first + 1);
    return std::string(second == std::string_view::npos ? name : name.substr(0, second));
}

struct AuthenticGatherEntry {
    int64_t def_guid = 0;
    int64_t interaction_guid = 0;
    int cert_tier = 0;
    std::string name;
    std::string profession;
    std::string resource_type;
    std::string tier_name;
};

struct GatherCatalog {
    std::unordered_map<int64_t, AuthenticGatherEntry> by_guid;
    std::unordered_map<std::string, AuthenticGatherEntry> by_name;
    std::unordered_map<std::string, AuthenticGatherEntry> by_resource;

    const AuthenticGatherEntry* find_guid(int64_t guid) const noexcept {
        const auto it = by_guid.find(guid);
        return it == by_guid.end() ? nullptr : &it->second;
    }
    const AuthenticGatherEntry* find_name(const std::string& name) const noexcept {
        if (const auto it = by_name.find(name); it != by_name.end()) return &it->second;
        const auto rit = by_resource.find(resource_type_of_def_name(name));
        return rit == by_resource.end() ? nullptr : &rit->second;
    }
};

GatherCatalog build_gather_catalog(const content::ContentDb& db) {
    GatherCatalog cat;
    std::string err;
    const auto gdata = db.gather_data(err);
    const auto pdata = db.profession_data(err);
    if (!gdata) return cat;
    for (const auto& [guid, def] : gdata->defs) {
        if (def.guid == 0 || def.name.empty()) continue;
        AuthenticGatherEntry e;
        e.def_guid = def.guid;
        e.interaction_guid = def.interaction_guid;
        e.cert_tier = std::clamp(def.cert_tier, 0, 4);
        e.name = def.name;
        e.resource_type = resource_type_of_def_name(def.name);
        if (pdata) {
            for (const auto& pr : pdata->professions) {
                if (pr.guid == def.profession_guid && !pr.name.empty()) { e.profession = pr.name; break; }
            }
            for (const auto& cr : pdata->certifications) {
                if (cr.guid == def.cert_guid && !cr.name.empty()) { e.tier_name = cr.name; break; }
            }
        }
        cat.by_guid[e.def_guid] = e;
        cat.by_name[e.name] = e;
        cat.by_resource.try_emplace(e.resource_type, e);
    }
    return cat;
}

bool file_readable(const std::string& path) noexcept {
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

std::string resolve_content_sibling_path(const std::string& db_path, const char* filename) {
    const size_t sep = db_path.find_last_of("/\\");
    if (sep != std::string::npos) {
        const std::string candidate = db_path.substr(0, sep + 1) + filename;
        if (file_readable(candidate)) return candidate;
    }
    for (const char* dir : {"dist/content/", "deploy/content/", "/app/content/", "/data/"}) {
        const std::string candidate = std::string(dir) + filename;
        if (file_readable(candidate)) return candidate;
    }
    return {};
}

uint64_t pack_foliage_cell(int cx, int cy) noexcept {
    return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy);
}

void parse_painted_foliage_line(RawSectionMap& raw, std::string_view line) {
    if (line.empty() || line[0] == '#') return;
    std::string_view cols[8];
    size_t pos = 0;
    for (size_t i = 0; i < 8; ++i) {
        const size_t tab = line.find('\t', pos);
        if (tab == std::string_view::npos) return;
        cols[i] = line.substr(pos, tab - pos);
        pos = tab + 1;
    }
    if (cols[1].empty() || cols[3].empty() || cols[4].empty()) return;
    char* ex = nullptr;
    char* ey = nullptr;
    const double x = std::strtod(cols[6].data(), &ex);
    const double y = std::strtod(cols[7].data(), &ey);
    if (ex == cols[6].data() || ey == cols[7].data() || !std::isfinite(x) || !std::isfinite(y)) return;
    std::string key;
    key.reserve(cols[1].size() + cols[3].size() + cols[4].size() + 2);
    key.append(cols[1]).push_back('\t');
    key.append(cols[3]).push_back('\t');
    key.append(cols[4]);
    auto& entry = raw[key];
    if (entry.first.empty()) entry.first.assign(cols[1]);
    entry.second.push_back({x, y});
}

void index_painted_section(PaintedFoliageIndex& index, const std::string& def_name, std::vector<FoliagePt> pts) {
    if (pts.empty()) return;
    PaintedFoliageSection sec;
    for (const FoliagePt p : pts) {
        sec.min_x = std::min(sec.min_x, p.x);
        sec.max_x = std::max(sec.max_x, p.x);
        sec.min_y = std::min(sec.min_y, p.y);
        sec.max_y = std::max(sec.max_y, p.y);
    }
    sec.hull = build_convex_hull(std::move(pts));
    const size_t idx = index.sections.size();
    const double pad = kFoliageBrushPadCm + kAreaEpsilonCm;
    const int min_cx = static_cast<int>(std::floor((sec.min_x - pad) / kFoliageCellCm));
    const int max_cx = static_cast<int>(std::floor((sec.max_x + pad) / kFoliageCellCm));
    const int min_cy = static_cast<int>(std::floor((sec.min_y - pad) / kFoliageCellCm));
    const int max_cy = static_cast<int>(std::floor((sec.max_y + pad) / kFoliageCellCm));
    auto& cells = index.by_def_cell[def_name];
    for (int cx = min_cx; cx <= max_cx; ++cx) {
        for (int cy = min_cy; cy <= max_cy; ++cy) {
            cells[pack_foliage_cell(cx, cy)].push_back(idx);
        }
    }
    index.sections.push_back(std::move(sec));
}

const PaintedFoliageIndex* cached_painted_foliage(const std::string& db_path) {
    const std::string path = resolve_content_sibling_path(db_path, "region_foliage.tsv");
    if (path.empty()) return nullptr;
    static std::mutex mu;
    static std::unordered_map<std::string, PaintedFoliageIndex> cache;
    std::lock_guard<std::mutex> lock(mu);
    if (const auto it = cache.find(path); it != cache.end()) return &it->second;
    RawSectionMap raw;
    if (std::FILE* fp = std::fopen(path.c_str(), "rb")) {
        char buf[1024];
        while (std::fgets(buf, sizeof(buf), fp)) parse_painted_foliage_line(raw, buf);
        std::fclose(fp);
    }
    PaintedFoliageIndex index;
    index.sections.reserve(raw.size());
    for (auto& [key, val] : raw) index_painted_section(index, val.first, std::move(val.second));
    return &cache.emplace(path, std::move(index)).first->second;
}

bool is_in_painted_foliage_section(const PaintedFoliageIndex* index, double x, double y, const std::string& def_name) {
    if (!index || def_name.empty()) return false;
    const auto def_it = index->by_def_cell.find(def_name);
    if (def_it == index->by_def_cell.end()) return false;
    const int cx = static_cast<int>(std::floor(x / kFoliageCellCm));
    const int cy = static_cast<int>(std::floor(y / kFoliageCellCm));
    const auto cell_it = def_it->second.find(pack_foliage_cell(cx, cy));
    if (cell_it == def_it->second.end()) return false;
    const double pad = kFoliageBrushPadCm + kAreaEpsilonCm;
    for (const size_t sec_idx : cell_it->second) {
        if (index->sections[sec_idx].contains(x, y, pad)) return true;
    }
    return false;
}

struct Placed {
    double x = 0.0, y = 0.0;
    std::string archetype;
};

struct GatherSpawnSpec {
    double x = 0.0, y = 0.0, z = 0.0;
    uint64_t seed = 0;
    std::vector<world::GatherCandidatePoint> area_points;
};

struct LoadContext {
    world::ActorRegistry& registry;
    const AreaSources& sources;
    const Minter& mint;
    uint32_t server_id = 0;
    double world_time = 0.0;
    AreaReport& report;
    GatherCatalog gather_catalog;
    const PaintedFoliageIndex* painted_foliage = nullptr;
    std::vector<Placed> placed;
    std::vector<Placed> nodes;
};

void note_report(AreaReport& report, std::string n) {
    if (report.notes.size() < 20) report.notes.push_back(std::move(n));
}

bool is_near_placed(const std::vector<Placed>& against, const std::string& archetype, double x, double y, double near_cm) {
    const double limit = near_cm + kAreaEpsilonCm;
    for (const Placed& p : against) {
        if (p.archetype == archetype && std::hypot(p.x - x, p.y - y) <= limit) return true;
    }
    return false;
}

bool spawn_authentic_gatherable(LoadContext& ctx, const OpenTemplate& donor, const AuthenticGatherEntry& entry, GatherSpawnSpec spec) {
    InstanceParams p;
    p.keep_captured_position = false;
    p.x = spec.x;
    p.y = spec.y;
    p.z = spec.z;
    p.yaw_degrees = static_cast<double>((spec.seed >> 16) % 360);
    p.server_id = ctx.server_id;
    p.world_time = ctx.world_time;
    p.actor_def = entry.def_guid;
    p.interact_def = entry.interaction_guid;
    p.name = entry.name;
    p.level = (entry.cert_tier + 1) * 10;
    p.seed = spec.seed;
    std::string why;
    auto w = instantiate(donor, ctx.mint, p, why);
    if (!w) return false;
    w->npc.home_x = spec.x;
    w->npc.home_y = spec.y;
    w->npc.home_z = spec.z;
    for (const world::GatherCandidatePoint& pt : spec.area_points) ctx.nodes.push_back({pt.x, pt.y, donor.archetype_path});
    w->gather.area_points = std::move(spec.area_points);
    w->gather.next_point = 0;
    if (!ctx.registry.add(std::move(*w))) return false;
    ++ctx.report.gatherables;
    return true;
}

void promote_anchor_point(std::vector<world::GatherCandidatePoint>& pts, double cx, double cy) noexcept {
    if (pts.size() <= 1 || !std::isfinite(cx) || !std::isfinite(cy)) return;
    size_t best = 0;
    double best_d = std::hypot(pts[0].x - cx, pts[0].y - cy);
    for (size_t i = 1; i < pts.size(); ++i) {
        const double d = std::hypot(pts[i].x - cx, pts[i].y - cy);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best != 0) std::swap(pts[0], pts[best]);
}

std::vector<world::GatherCandidatePoint> parse_spawner_points(std::string_view json, double cx, double cy, double cz) {
    std::vector<world::GatherCandidatePoint> out;
    size_t pos = 0;
    while (pos < json.size()) {
        const size_t open = json.find('[', pos);
        if (open == std::string_view::npos) break;
        if (open + 1 < json.size() && json[open + 1] == '[') { pos = open + 1; continue; }
        const size_t close = json.find(']', open + 1);
        if (close == std::string_view::npos) break;
        const std::string_view inner = json.substr(open + 1, close - open - 1);
        const size_t c1 = inner.find(',');
        const size_t c2 = c1 == std::string_view::npos ? c1 : inner.find(',', c1 + 1);
        if (c1 != std::string_view::npos && c2 != std::string_view::npos) {
            const double px = std::strtod(inner.data(), nullptr);
            const double py = std::strtod(inner.data() + c1 + 1, nullptr);
            const double pz = std::strtod(inner.data() + c2 + 1, nullptr);
            if (std::isfinite(px) && std::isfinite(py) && std::isfinite(pz)) out.push_back({px, py, pz});
        }
        pos = close + 1;
    }
    if (out.empty() && std::isfinite(cx) && std::isfinite(cy) && std::isfinite(cz)) out.push_back({cx, cy, cz});
    promote_anchor_point(out, cx, cy);
    return out;
}

std::vector<world::GatherCandidatePoint> filter_spawner_points(
    const LoadContext& ctx, const std::string& archetype, const AuthenticGatherEntry& entry,
    const std::vector<world::GatherCandidatePoint>& raw_pts) {
    std::vector<world::GatherCandidatePoint> valid;
    valid.reserve(raw_pts.size());
    for (const world::GatherCandidatePoint& pt : raw_pts) {
        if (!ctx.report.area.contains(pt.x, pt.y) || world::zoi_at(pt.x, pt.y) == 0 || point_in_any_node_town_footprint(pt.x, pt.y)) continue;
        if (is_in_painted_foliage_section(ctx.painted_foliage, pt.x, pt.y, entry.name)) continue;
        if (is_near_placed(ctx.nodes, archetype, pt.x, pt.y, 50.0)) continue;
        valid.push_back(pt);
    }
    return valid;
}

void place_pcap_resource_spawners(LoadContext& ctx, const OpenTemplate& donor) {
    const std::string path = resolve_content_sibling_path(ctx.sources.db->path(), "pcap_resource_spawners.tsv");
    if (path.empty()) return;
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return;
    char buf[8192];
    if (!std::fgets(buf, sizeof(buf), fp)) { std::fclose(fp); return; }
    uint64_t row_idx = 0;
    while (std::fgets(buf, sizeof(buf), fp)) {
        ++row_idx;
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> cols;
        std::stringstream ss(line);
        for (std::string col; std::getline(ss, col, '\t');) cols.push_back(std::move(col));
        if (cols.size() < 12) continue;
        const int64_t def_guid = std::strtoll(cols[1].c_str(), nullptr, 10);
        const AuthenticGatherEntry* entry = ctx.gather_catalog.find_guid(def_guid);
        if (!entry) entry = ctx.gather_catalog.find_name(cols[2]);
        if (!entry) continue;
        const double cx = std::strtod(cols[9].c_str(), nullptr);
        const double cy = std::strtod(cols[10].c_str(), nullptr);
        const double cz = std::strtod(cols[11].c_str(), nullptr);
        const std::string_view pts_json = cols.size() >= 15 ? std::string_view(cols[14]) : std::string_view{};
        const auto raw_pts = parse_spawner_points(pts_json, cx, cy, cz);
        if (raw_pts.empty()) continue;
        auto area_points = filter_spawner_points(ctx, donor.archetype_path, *entry, raw_pts);
        if (area_points.empty()) {
            if (ctx.report.area.contains(cx, cy)) ++ctx.report.gatherable_duplicates;
            continue;
        }
        const world::GatherCandidatePoint first = area_points.front();
        const uint64_t seed = mix(0x50434150ull ^ static_cast<uint64_t>(def_guid) ^ row_idx);
        spawn_authentic_gatherable(ctx, donor, *entry, {first.x, first.y, first.z, seed, std::move(area_points)});
    }
    std::fclose(fp);
}

void place_extracted_resource_spawns(LoadContext& ctx) {
    if (!ctx.sources.db || !ctx.sources.captured || ctx.gather_catalog.by_guid.empty()) return;
    const OpenTemplate* donor = garden_donor(*ctx.sources.captured);
    if (!donor) return;
    place_pcap_resource_spawners(ctx, *donor);
}

void place_captured_spawns(LoadContext& ctx) {
    for (size_t i = 0; i < ctx.sources.captured->spawns().size(); ++i) {
        const CapturedSpawn& c = ctx.sources.captured->spawns()[i];
        if (!ctx.report.area.contains(c.x, c.y)) continue;
        const bool npc = c.open->category == "npc";
        if (is_near_placed(npc ? ctx.placed : ctx.nodes, c.open->archetype_path, c.x, c.y, npc ? kSamePointCm : 50.0)) {
            ++(npc ? ctx.report.duplicates : ctx.report.gatherable_duplicates);
            continue;
        }
        InstanceParams p;
        p.keep_captured_position = true;
        p.server_id = ctx.server_id;
        p.world_time = ctx.world_time;
        p.seed = mix(0xC0FFEEull ^ static_cast<uint64_t>(i));
        std::string why;
        auto w = instantiate(*c.open, ctx.mint, p, why);
        if (!w) {
            ++ctx.report.refused;
            note_report(ctx.report, "a captured spawn (" + leaf_of(c.open->archetype_path) + ") was refused: " + why);
            continue;
        }
        w->x = c.x; w->y = c.y; w->z = c.z;
        w->npc.source = world::Source::Captured;
        w->npc.home_x = c.x; w->npc.home_y = c.y; w->npc.home_z = c.z;
        if (!npc) {
            if (const AuthenticGatherEntry* g = ctx.gather_catalog.find_guid(captured_spawns::gatherable_def_guid(*c.open))) {
                w->label = g->name;
                w->npc.name = g->name;
                w->npc.level = (g->cert_tier + 1) * 10;
            }
        }
        const std::string path = c.open->archetype_path;
        if (!ctx.registry.add(std::move(*w))) {
            ++ctx.report.refused;
            note_report(ctx.report, "a captured spawn could not be added to the registry");
            continue;
        }
        if (npc) {
            ctx.placed.push_back({c.x, c.y, path});
            ++ctx.report.captured_npcs;
            ++ctx.report.npcs_by_archetype[leaf_of(path)];
        } else {
            ctx.nodes.push_back({c.x, c.y, path});
            ++ctx.report.gatherables;
        }
    }
}

void place_gm_vendors(LoadContext& ctx) {
    if (!ctx.sources.gm) return;
    const OpenTemplate* tpl = ctx.sources.captured->template_for(kGmVendorArchetype);
    if (!tpl) {
        note_report(ctx.report, std::string("the vendor archetype ") + kGmVendorArchetype + " has no capture: no GM vendors");
        return;
    }
    const auto& defs = ctx.sources.gm->vendors();
    const std::vector<vendor::GmPlacement> layout = vendor::gm_layout(defs.size(), ctx.report.area, kLoginSpotYaw, kLoginSpotZ);
    for (size_t i = 0; i < defs.size(); ++i) {
        if (!ctx.report.area.contains(layout[i].x, layout[i].y)) continue;
        InstanceParams p;
        p.x = layout[i].x; p.y = layout[i].y; p.z = layout[i].z;
        p.yaw_degrees = layout[i].yaw_degrees;
        p.server_id = ctx.server_id; p.world_time = ctx.world_time;
        p.name = defs[i].label; p.clear_population = true;
        p.seed = mix(0x6D5E11ull ^ static_cast<uint64_t>(defs[i].guid));
        std::string why;
        auto w = instantiate(*tpl, ctx.mint, p, why);
        if (!w) { ++ctx.report.refused; note_report(ctx.report, "the GM vendor " + defs[i].label + " was refused: " + why); continue; }
        if (!world::allow_inputs(w->actor)) {
            ++ctx.report.refused;
            note_report(ctx.report, "the GM vendor " + defs[i].label + " has no interactable component to allow inputs on");
            continue;
        }
        w->npc.source = world::Source::Runtime;
        w->npc.name = defs[i].label;
        w->npc.vendor_guid = defs[i].guid;
        w->npc.is_gm_vendor = true;
        w->npc.home_x = layout[i].x; w->npc.home_y = layout[i].y; w->npc.home_z = layout[i].z;
        if (!ctx.registry.add(std::move(*w))) {
            ++ctx.report.refused;
            note_report(ctx.report, "the GM vendor " + defs[i].label + " could not be added to the registry");
            continue;
        }
        ++ctx.report.gm_vendors;
    }
}

}  // namespace

std::string AreaReport::describe() const {
    std::ostringstream o;
    if (area.region) {
        o << "area: the Riverlands (" << region::kRiverlandsRectCount << " rectangles, " << region::kRiverlandsAreaCells * (region::kCellCm / 100.0) * (region::kCellCm / 100.0) / 1.0e6
          << " km2, " << region::kZoiCount << " zones of influence)";
    } else {
        o << "area: centre (" << static_cast<long long>(area.center_x) << ", " << static_cast<long long>(area.center_y) << ") cm, radius " << area.radius_cm / 100.0 << " m";
    }
    o << ": " << data_npcs << " NPCs from the data (" << population.points_in_area << " placement points and " << population.area_points << " random points of "
      << population.area_sets << " area sets in the area: " << population.generic_spawned << " NPCs made from a generic Blueprint template, " << population.area_spawned
      << " from area points, " << population.no_template << " points without any template, " << population.blacklisted << " blacklisted, " << population.no_asset
      << " without an asset, " << population.filtered_total() << " dropped by the spawn filter), " << captured_npcs << " captured NPCs (" << duplicates << " dropped as the data's), " << gatherables << " resource nodes, " << gm_vendors
      << " GM vendors, " << shrines_made << " Embersprings (" << respawn_points.size() << " respawn points in the area, " << shrines_no_record << " without a respawn definition, " << shrines_refused << " refused)";
    return o.str();
}
// (Cyb) (C.O.A)

std::optional<world::WorldActor> make_data_actor(const DataSpawn& s, const Minter& mint, uint32_t server_id, double world_time, InstanceParams* params, std::string& error,
                                                 const content::ObjectData* objects) {
    InstanceParams p;
    p.x = s.x; p.y = s.y; p.z = s.z;
    p.yaw_degrees = s.yaw;
    p.level = s.level;
    p.server_id = server_id;
    p.world_time = world_time;
    p.pop_instance = s.instance ? s.instance->guid : 0;
    p.pop_asset = s.asset ? s.asset->guid : 0;
    p.seed = s.seed;
    if (s.open && s.open->category == "prop" && s.instance) {
        p.component_def = s.instance->interact_component != 0 ? s.instance->interact_component : (s.asset ? s.asset->interact_component : 0);
        p.interact_def = s.instance->interact_response != 0 ? s.instance->interact_response : (s.asset ? s.asset->interaction_def : 0);
        if (objects) {
            if (const auto it = objects->instance_iad.find(s.instance->guid); it != objects->instance_iad.end()) p.actor_def = it->second;
            if (const auto it = objects->interactables.find(p.component_def); it != objects->interactables.end() && it->second.pid != 0) p.interact_def = it->second.pid;
        }
    }
    if (params) *params = p;
    auto w = instantiate(*s.open, mint, p, error);
    if (!w) return std::nullopt;
    if (s.looks) {
        std::string look_error;
        if (!s.looks->dress(*w, s, look_error)) spdlog::debug("[looks] {}: {}", s.asset ? s.asset->name : std::string("?"), look_error);
    }
    if (std::string idle_error; !apply_idle_animation(*w, s.instance, idle_error)) spdlog::debug("[looks] {}: {}", s.asset ? s.asset->name : std::string("?"), idle_error);
    w->npc.source = world::Source::Data;
    w->npc.ground_by_terrain = s.from_area;
    w->npc.asset_guid = p.pop_asset;
    w->npc.instance_guid = p.pop_instance;
    w->npc.vendor_guid = s.instance ? s.instance->vendor : 0;
    w->npc.dialogue_root = (s.instance && s.instance->dialogue_root != 0) ? s.instance->dialogue_root : (s.asset ? s.asset->dialogue_root : 0);
    w->npc.home_x = s.x; w->npc.home_y = s.y; w->npc.home_z = s.z;
    if (s.asset && s.asset->name.rfind("Hunting_", 0) == 0) {
        if (w->npc.interact_def == 0 && s.asset->interaction_def != 0) w->npc.interact_def = s.asset->interaction_def;
        if (w->npc.name.empty()) w->npc.name = s.asset->name;
        if (w->label.empty()) w->label = w->npc.name;
    }
    if (s.instance && s.instance->override_aggro) {
        if (s.instance->aggro_radius > 0) w->npc.aggro_radius_cm = s.instance->aggro_radius;
        else if (s.asset && s.asset->asset_ref.find("_Timid.") != std::string::npos) w->npc.aggro_off = true;
    } else if (s.asset && s.asset->aggro_radius > 0) {
        w->npc.aggro_radius_cm = s.asset->aggro_radius;
    }
    if (s.asset && s.asset->leash_radius > 0) w->npc.leash_radius_cm = s.asset->leash_radius;
    if (s.instance) {
        w->npc.respawn_seconds = s.instance->respawn_time;
        w->npc.respawn_variance = s.instance->respawn_variance;
    }
    if (s.asset) w->npc.loot_tables.insert(w->npc.loot_tables.end(), s.asset->loot_tables.begin(), s.asset->loot_tables.end());
    if (s.instance) {
        w->npc.loot_tables.insert(w->npc.loot_tables.end(), s.instance->loot_tables.begin(), s.instance->loot_tables.end());
        w->npc.respawn_rule = s.instance->respawn_rule;
    }
    return w;
}

void place_data_spawns(LoadContext& ctx, const content::PopulationData& population, const std::vector<DataSpawn>& data) {
    for (const DataSpawn& s : data) {
        InstanceParams p;
        std::string why;
        auto w = make_data_actor(s, ctx.mint, ctx.server_id, ctx.world_time, &p, why, ctx.sources.objects);
        if (!w) { ++ctx.report.refused; note_report(ctx.report, "a data NPC (" + leaf_of(s.open->archetype_path) + ") was refused: " + why); continue; }
        if (s.instance) {
            ctx.report.instances.emplace(s.instance->guid, *s.instance);
            if (s.instance->patrol_route != 0) {
                if (const auto r = population.routes.find(s.instance->patrol_route); r != population.routes.end()) ctx.report.routes.emplace(r->first, r->second);
            }
            ctx.report.recipes.push_back({s.open, p, w->npc, s});
            w->npc.spawn_ref = static_cast<uint32_t>(ctx.report.recipes.size());
            ctx.report.recipes.back().info.spawn_ref = w->npc.spawn_ref;
        }
        const std::string path = s.open->archetype_path;
        const double x = w->x, y = w->y;
        const bool prop = w->kind == world::ActorKind::Prop;
        if (prop) {
            ctx.report.props.push_back(PropRecipe{s.open, p, s.instance ? s.instance->guid : 0, s.instance ? s.instance->respawn_time : 0, s.instance ? s.instance->respawn_variance : 0, w->id});
            w->prop.slot = static_cast<uint32_t>(ctx.report.props.size());
        }
        if (!ctx.registry.add(std::move(*w))) {
            ++ctx.report.refused; note_report(ctx.report, "a data NPC could not be added to the registry");
            if (prop) ctx.report.props.pop_back();
            continue;
        }
        if (prop) continue;
        ctx.placed.push_back({x, y, path});
        ++ctx.report.data_npcs;
        ++ctx.report.npcs_by_archetype[leaf_of(path)];
    }
}

SpawnContext build_filter_context(const AreaConfig& area, SpawnContext fctx) {
    if (!area.region && area.zoi_guids.empty() && !area.whole_world) {
        if (!area.include_node_plugs.empty() || !area.polygon_cutout_cm.empty()) fctx.biome = "*";
    } else if (!area.include_node_plugs.empty() || !area.polygon_cutout_cm.empty() ||
               (area.region && area.region_mask != static_cast<uint8_t>(RegionBit::Riverlands)) || !area.zoi_guids.empty()) {
        if (fctx.biome.empty() || fctx.biome == "Riverlands") fctx.biome = "*";
    }
    return fctx;
}

std::optional<AreaReport> load_area(world::ActorRegistry& registry, const AreaSources& sources, const AreaConfig& area, const Minter& mint, uint32_t server_id,
                                    double world_time, std::string& error) {
    if (!sources.db || !sources.captured) {
        error = "the area needs content.db and the captured spawns";
        return std::nullopt;
    }
    AreaConfig::active_worker_area(&area);
    (void)build_filter_context(area, sources.filter ? sources.filter->context() : SpawnContext{});
    AreaReport report;
    report.area = area;
    auto loaded = sources.db->population_data(error);
    if (!loaded) return std::nullopt;
    report.data = std::make_shared<const content::PopulationData>(std::move(*loaded));
    const content::PopulationData* population = report.data.get();

    report.respawn_points = respawn_points(*population, area);
    const TemplateSource templates{sources.captured, sources.generic, sources.looks};
    const std::vector<DataSpawn> data = select_data_spawns(*population, templates, area, report.population, sources.filter);
    const auto note = [&](std::string n) { note_report(report, std::move(n)); };

    LoadContext ctx{registry, sources, mint, server_id, world_time, report, build_gather_catalog(*sources.db), cached_painted_foliage(sources.db->path()), {}, {}};
    place_data_spawns(ctx, *population, data);
    place_captured_spawns(ctx);
    place_extracted_resource_spawns(ctx);
    place_gm_vendors(ctx);

    if (sources.db && sources.captured) {
        std::string respawn_error;
        if (const auto rdata = sources.db->respawn_data(respawn_error)) {
            respawn::MadeShrines made = respawn::make_shrines(registry, *sources.captured, *rdata, report.respawn_points, mint, server_id, world_time, note);
            report.shrines_made = made.made;
            report.shrines_no_record = made.no_record;
            report.shrines_refused = made.refused;
            if (made.no_number > 0) note(std::to_string(made.no_number) + " Emberspring(s) stand for a place with no number in the master list: they can be seen and used, not discovered");
            report.shrines = std::move(made.book);
        } else {
            note("no Embersprings: " + respawn_error);
        }
    }
    return report;
}

}  // namespace aoc::worldd::spawns
