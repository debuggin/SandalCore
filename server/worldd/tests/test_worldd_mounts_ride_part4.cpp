// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_mounts_ride_common.h"

using namespace aoc::worldd;
using namespace aoc::protocol;

namespace {

struct RealRoad {
    int tier = 0;
    double half = 0;
    std::vector<std::pair<double, double>> xy;
};

void parse_real_road_line(const std::string& line, std::vector<RealRoad>& out) {
    if (line.empty() || line[0] == '#') return;
    std::vector<std::string> cells;
    size_t a = 0;
    while (a <= line.size()) {
        size_t b = line.find('\t', a);
        if (b == std::string::npos) b = line.size();
        cells.push_back(line.substr(a, b - a));
        a = b + 1;
    }
    if (cells.size() < 5 || cells[0] != "road") return;
    RealRoad r;
    r.tier = std::atoi(cells[1].c_str());
    r.half = std::atof(cells[2].c_str());
    for (size_t i = 3; i < cells.size(); ++i) {
        const size_t comma = cells[i].find(',');
        if (comma == std::string::npos) continue;
        r.xy.emplace_back(std::atof(cells[i].substr(0, comma).c_str()), std::atof(cells[i].substr(comma + 1).c_str()));
    }
    out.push_back(std::move(r));
}

std::vector<RealRoad> read_real_roads(const std::string& path) {
    std::vector<RealRoad> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::string text;
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        parse_real_road_line(text.substr(pos, eol - pos), out);
        pos = eol + 1;
    }
    return out;
}

void verify_real_roads_map(const std::vector<RealRoad>& roads, const world::RoadMap& map) {
    size_t t1 = 0, t2 = 0, other = 0;
    double metres = 0, narrow = 1e9;
    bool self = true;
    for (const RealRoad& r : roads) {
        (r.tier == 1 ? t1 : r.tier == 2 ? t2 : other)++;
        narrow = std::min(narrow, r.half);
        for (size_t i = 1; i < r.xy.size(); ++i) metres += std::hypot(r.xy[i].first - r.xy[i - 1].first, r.xy[i].second - r.xy[i - 1].second) / 100.0;
        for (const auto& [x, y] : r.xy) self = self && map.tier_at(x, y) >= r.tier;
    }
    CHECK(t1 >= 5 && t2 >= 20 && other == 0, "the area's roads are tier 1 (the Riverlands Artery at level 0) and tier 2 (Lions Hold's)");
    CHECK(metres > 25000.0 && metres < 45000.0, "about 34 km of road in all (the region's 91 roads at Wilderness: 24.7 km of tier 1, 9.1 km of tier 2)");
    CHECK(narrow >= 500.0, "no road is narrower than half of max(RoadWidth, 1000): the overlap's half width is at least 500 cm");
    CHECK(self, "every sample of every road lies on a road of at least its tier");
    CHECK(map.tier_at(map.bounds().max_x + 300000.0, map.bounds().max_y + 300000.0) == -1, "a point 3 km outside the roads' extent is on none");
}

void ride_to_point(Harness& h, Rider& r, double tx, double ty) {
    int guard = 0;
    while (std::hypot(tx - r.x, ty - r.y) > 1.0 && guard++ < 400) {
        const double dist = std::hypot(tx - r.x, ty - r.y);
        const double step = std::min(dist, 99.0);
        ride_step(h, r, (tx - r.x) / dist * step, (ty - r.y) / dist * step);
    }
}

const RealRoad* find_nearest_tier2_road(const std::vector<RealRoad>& roads, size_t& best_i) {
    const double cx = -777664.5, cy = 616578.69;
    const RealRoad* best = nullptr;
    double best_d = 1e18;
    for (const RealRoad& r : roads) {
        if (r.tier != 2 || r.xy.size() < 40) continue;
        for (size_t i = 0; i + 30 < r.xy.size(); ++i) {
            const double d = std::hypot(r.xy[i].first - cx, r.xy[i].second - cy);
            if (d < best_d) {
                best_d = d;
                best = &r;
                best_i = i;
            }
        }
    }
    return best;
}

bool find_off_road_point(const world::RoadMap& map, const Rider& r, double nx, double ny, std::pair<double, double>& off) {
    for (const double away : {2500.0, 4000.0, 6000.0, 9000.0}) {
        for (const double side : {1.0, -1.0}) {
            if (map.tier_at(r.x + nx * away * side, r.y + ny * away * side) < 0) {
                off = {r.x + nx * away * side, r.y + ny * away * side};
                return true;
            }
        }
    }
    return false;
}

void verify_real_road_on_and_off(Harness& h, Rider& r, const world::RoadMap& map, const RealRoad& best, size_t from_vertex) {
    const uint32_t mch = h.mount_ch();
    const content::MountData& data = h.fx.world->mounts()->data();
    ride_to_point(h, r, best.xy[from_vertex].first, best.xy[from_vertex].second);
    CHECK(h.s->ride()->road_tier() == 2 && std::fabs(mult_of(*h.mount()).value_or(0) - 1.2) < 1e-6, "on the real road the mount has Road_T2_Speed_Mount: MoveSpeedMult 1.2");
    const auto fx = items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects");
    CHECK(fx.size() == 1 && data.road(2) && fx[0].values.at(2).get_if<int64_t>() && *fx[0].values.at(2).get_if<int64_t>() == data.road(2)->effect, "the effect is the road's tier-2 effect, one item");
    const size_t refused = h.s->moves_refused();
    for (size_t i = from_vertex + 1; i < std::min(best.xy.size(), from_vertex + 25); ++i) ride_to_point(h, r, best.xy[i].first, best.xy[i].second);
    CHECK(h.s->moves_refused() == refused && h.s->ride()->road_tier() >= 1, "riding along 25 samples of the road (5 km... 50 m each 200 cm... 48 m): no move refused, the effect stays on");
    size_t effect_updates = 0;
    for (const fake::Delivered* d : chains_on(h, mch, 0)) {
        for (const auto& [id, o] : states_of(*d, h.mount()->actor)) {
            const schema::NetField* fe = o.schema ? o.schema->field("CurrentEffects") : nullptr;
            if (fe && o.arrays.find(fe->index)) ++effect_updates;
        }
    }
    CHECK(effect_updates >= 1 && effect_updates <= 3, "the effect was added once (at the mount-up, the window starts there) and not re-sent as the mount rode along the road (no flapping at the samples)");
    const auto& q0 = best.xy[std::min(best.xy.size() - 2, from_vertex + 24)];
    const auto& q1 = best.xy[std::min(best.xy.size() - 1, from_vertex + 25)];
    const double qlen = std::hypot(q1.first - q0.first, q1.second - q0.second);
    const double nx = -(q1.second - q0.second) / qlen, ny = (q1.first - q0.first) / qlen;
    std::pair<double, double> off{0.0, 0.0};
    const bool found_off = find_off_road_point(map, r, nx, ny, off);
    CHECK(found_off, "(a point within 90 m of the road that is on no road)");
    if (!found_off) return;
    ride_to_point(h, r, off.first, off.second);
    CHECK(map.tier_at(off.first, off.second) < 0 && h.s->ride()->road_tier() == -1 && mult_of(*h.mount()).value_or(0) == 1.0 && items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects").empty(),
          "off every road the effect is removed, MoveSpeedMult is back to 1");
    ride_to_point(h, r, q1.first, q1.second);
    CHECK(h.s->ride()->road_tier() >= 1, "back on the road it is added again");
}

void test_real_roads() {
    std::printf("stage 4: the roads of the area (mapc roads, contentc roads): the real file, and a mount riding along one\n");
    const std::string path = MOUNT_ROADS_PATH;
    const std::vector<RealRoad> roads = read_real_roads(path);
    CHECK(roads.size() >= 30, "dist/content/roads.tsv holds the area's roads (91 at Wilderness over the whole region: 54 tier 1, 37 tier 2)");
    if (roads.empty()) return;
    world::RoadMap map;
    std::string error;
    CHECK(map.load(path, error) && map.roads() == roads.size() && map.node_level() == 0, "worldd's loader reads it, and the header says the tiers were resolved at node level 0");
    verify_real_roads_map(roads, map);
    size_t best_i = 0;
    const RealRoad* best = find_nearest_tier2_road(roads, best_i);
    CHECK(best != nullptr, "(a tier-2 road with 30 samples ahead of its nearest point)");
    if (!best) return;
    const auto& p0 = best->xy[best_i];
    const auto& p1 = best->xy[best_i + 1];
    const double yaw = std::atan2(p1.second - p0.second, p1.first - p0.first) * 180.0 / 3.14159265358979;
    svc::Character rec = cyb_record();
    rec.has_position = true;
    rec.position = {p0.first, p0.second, 3000.0, static_cast<float>(yaw)};
    Harness h(rec, [&](PlayerWorld& w) {
        std::string e;
        if (!w.load_roads(path, e)) std::printf("  [FAIL] the roads do not load into the world: %s\n", e.c_str());
    });
    h.play();
    CHECK(h.fx.world->roads().roads() == roads.size(), "(the world holds the real roads)");
    CHECK(ride_on(h, kMountSlot), "(the player rides the terror bird, summoned in front of it on the road)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    size_t from_vertex = best_i;
    double nearest = 1e18;
    for (size_t i = best_i; i < best->xy.size(); ++i) {
        const double d = std::hypot(best->xy[i].first - r.x, best->xy[i].second - r.y);
        if (d < nearest) {
            nearest = d;
            from_vertex = i;
        }
    }
    verify_real_road_on_and_off(h, r, map, *best, from_vertex);
}

} // namespace

void run_mounts_ride_part4() {
    test_real_roads();
}
