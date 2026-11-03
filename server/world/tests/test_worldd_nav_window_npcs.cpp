// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "test_worldd_nav_window_common.h"

#include "spawns/region_data.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <limits>
#include <random>

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace emit = aoc::protocol::emit;
using nav::AreaNav;
using nav::ChunkKey;
using nav::Vec3;

static void test_npcs_arrival(AreaNav &n, world::ActorRegistry &reg, npc::NpcWorld &w, double border) {
  n.set_interest({{border + 3000, 3000}});
  settle(n);
  std::vector<ChunkKey> keys = {n.chunk_at(border + 2000, 3000), n.chunk_at(border + 400, 6000), n.chunk_at(border + 3000, -3000)};
  const uint32_t reliable_platform = reg.find(1004)->reliable_version, reliable_ground = reg.find(1002)->reliable_version;
  w.chunks_resident(keys);
  CHECK(w.pending_ground() == 0, "the chunks are reported: nothing waits");
  CHECK(reg.find(1004)->reliable_version > reliable_platform, "the NPC lifted 540 cm onto its platform is a reliable change of the actor (a dormant view of it is sent again)");
  CHECK(reg.find(1002)->reliable_version == reliable_ground, "the one that stays on the terrain is not (nothing to send again)");
  const world::WorldActor *a3 = reg.find(1004);
  CHECK(a3 && std::fabs(a3->z - (1540.0 + 88.0)) < 20.0, "the NPC over the platform is lifted onto its floor (1,540 + 88) when its chunk arrives");
  const world::WorldActor *a2 = reg.find(1002);
  CHECK(a2 && std::fabs(a2->z - 1088.0) < 0.5, "the other stands on the terrain");
  double moved = 0;
  for (int i = 0; i < 600; ++i) {
    w.note_player(1, border + 3000, -3000, 10.0 + i / 15.0);
    w.tick(10.0 + i / 15.0);
    if (const auto *a4 = reg.find(1006)) {
      moved = std::max(moved, std::hypot(a4->x - (border + 3000), a4->y + 3000));
    }
  }
  CHECK(moved > 50.0, "and now the wanderer walks");
  n.set_interest({{cx_of(6), cx_of(6)}});
  for (int i = 0; i < 200; ++i) {
    n.pump(50);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(!n.ready_at(border + 3000, -3000), "the chunk is gone");
  const auto before = *reg.find(1006);
  for (int i = 0; i < 150; ++i) {
    w.note_player(1, border + 3000, -3000, 50.0 + i / 15.0);
    w.tick(50.0 + i / 15.0);
  }
  const auto &after = *reg.find(1006);
  CHECK(std::fabs(before.x - after.x) < 1e-6 && std::fabs(before.y - after.y) < 1e-6, "an awake-range NPC whose chunk is not resident is frozen");
  w.forget_player(1);
}

static void test_npcs_pending_terrain(world::ActorRegistry &reg, npc::NpcWorld &w, double border) {
  const world::WorldActor *a2 = reg.find(1002);
  CHECK(a2 && std::fabs(a2->z - 1088.0) < 0.5, "the waiting NPC stands on the terrain meanwhile (1,000 + 88), not at the 900 the data gave it");
  const world::WorldActor *a3 = reg.find(1004);
  CHECK(a3 && std::fabs(a3->z - 1088.0) < 0.5, "the one over the platform is on the terrain too, until the floor is here");
  w.note_player(1, border + 3000, -3000, 0.0);
  w.tick(0.0);
  for (int i = 1; i <= 150; ++i) {
    w.note_player(1, border + 3000, -3000, i / 15.0);
    w.tick(i / 15.0);
  }
  const world::WorldActor *a4 = reg.find(1006);
  CHECK(a4 && std::fabs(a4->x - (border + 3000)) < 1e-6 && std::fabs(a4->y + 3000) < 1e-6, "a player near, but its chunk not here: the pending wanderer sleeps where it stands");
}

static void test_npcs() {
  std::printf("NPCs and the window\n");
  const std::string path = navfix::write_chunk_file("npcs");
  std::string err;
  auto n = AreaNav::load(path, &err);
  if (!n) {
    CHECK(false, "the fixture opens");
    return;
  }
  n->load_around(0, 0, 0);
  world::ActorRegistry reg;
  npc::NpcWorld w(reg, n.get());
  npc::NpcMotionSetup stand;
  stand.foot_offset = 88;
  npc::NpcMotionSetup wander = stand;
  wander.ambient = npc::Ambient::Wander;
  wander.wander_min = 200;
  wander.wander_max = 1500;
  wander.walk_speed = 150;
  const double border = navfix::chunk_x0(3);
  reg.add(make_npc(1000, 500, 500, 1088));
  reg.add(make_npc(1002, border + 2000, 3000, 900));
  reg.add(make_npc(1004, border + 400, 6000, 1700));
  reg.add(make_npc(1006, border + 3000, -3000, 1088));
  bool found = false, pending = false;
  const bool added1 = w.add(1000, wander, &found, &pending);
  std::printf("  (npc 1: added %d found %d pending %d, pending_ground %zu, ready_at %d)\n", added1, found, pending, w.pending_ground(), n->ready_at(500, 500));
  CHECK(added1 && found && !pending && w.pending_ground() == 0, "an NPC in the loaded chunk is grounded when it is added");
  CHECK(w.add(1002, stand, &found, &pending) && !found && pending && w.pending_ground() == 1, "an NPC in a chunk that is not here is pending");
  CHECK(w.add(1004, stand, &found, &pending) && pending && w.add(1006, wander, &found, &pending) && pending && w.pending_ground() == 3, "three NPCs wait for their chunks");
  reg.add(make_npc(1008, border + 2500, 3500, 900));
  CHECK(w.add(1008, stand, &found, &pending) && pending && w.pending_ground() == 4, "a fourth waits");
  reg.remove(1008);
  CHECK(w.pending_ground() == 3 && !w.has(1008), "and the registry's removal of it takes it off the count");
  test_npcs_pending_terrain(reg, w, border);
  test_npcs_arrival(*n, reg, w, border);
  n.reset();
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

static void test_edge_spot_height(PlayerWorld &w, AreaNav *ground) {
  bool grounded = true;
  const double before = PlayerWorld::spot_height(w.nav(), 2000, 6000, 1540.0, &grounded);
  CHECK(!ground->ready_at(2000, 6000) && !grounded && std::fabs(before - 1540.0) < 1e-9, "a spot over a platform in a chunk that is not here keeps the data's height (not the terrain's 1,000 + 98)");
  ground->load_around(2000, 6000, 0);
  const double after = PlayerWorld::spot_height(w.nav(), 2000, 6000, 1540.0, &grounded);
  CHECK(ground->ready_at(2000, 6000) && grounded && std::fabs(after - (1540.0 + 98.0)) < 15.0, "and once the chunk is here it is the platform's top + 98 (1,638)");
  const double flat = PlayerWorld::spot_height(w.nav(), 6000, 3000, 1000.0, &grounded);
  CHECK(grounded && std::fabs(flat - 1098.0) < 1.0, "on open ground in a resident chunk: the terrain + 98");
  CHECK(PlayerWorld::spot_height(nullptr, 0, 0, 1234.0) == 1234.0, "a world without ground gives the data's height");
}

static void test_edge() {
  std::printf("the region's edge\n");
  const std::string path = navfix::write_chunk_file("edge");
  std::string err;
  auto n = AreaNav::load(path, &err);
  if (!n) {
    CHECK(false, "the fixture opens");
    return;
  }
  {
    aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
    PlayerWorld w(nullptr, "no content.db in this test", bootstrap::GameStateMode::Tier2, 60, guids);
    spdlog::set_level(spdlog::level::off);
    AreaNav *ground = n.get();
    CHECK(w.adopt_ground(std::move(n), err), "the world adopts the chunked ground");
    test_edge_spot_height(w, ground);
    CHECK(w.region_exits() == 0 && !w.outside_region(7), "nobody has left");
    w.note_player(7, 0, 0);
    w.note_player(7, 25000, 25000);
    CHECK(w.region_exits() == 0 && !w.outside_region(7), "inside the footprint: no exit");
    w.note_player(7, 40000, 0);
    CHECK(w.region_exits() == 1 && w.outside_region(7), "beyond the edge: one exit, outside");
    for (int i = 0; i < 100; ++i) {
      w.note_player(7, 40000 + i * 100, 50);
    }
    CHECK(w.region_exits() == 1, "a hundred more positions beyond the edge: still one exit (logged once)");
    const nav::AreaNav *nv = w.nav();
    double z = 0;
    CHECK(nv && !nv->ground_height(45000, 0, 1000, &z) && !nv->on_navmesh({45000, 0, 1000}), "beyond the edge the ground answers nothing (no terrain, no navmesh: the callers' no-ground branch)");
    w.note_player(7, 10000, 0);
    CHECK(!w.outside_region(7) && w.region_exits() == 1, "back inside: noted, not an exit");
    w.note_player(7, 0, -45000);
    CHECK(w.region_exits() == 2, "out again: the second exit");
    w.forget_player(7);
    CHECK(!w.outside_region(7), "a player who left the world is forgotten");
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

static const nav::NavTileBlob *find_tile_in_chunk(const std::vector<nav::NavTileBlob> &tiles, int tx, int ty) {
  for (const auto &u : tiles) {
    if (u.tx == tx && u.ty == ty) {
      return &u;
    }
  }
  return nullptr;
}

struct E1CompareCounts {
  size_t common = 0;
  size_t same_tiles = 0;
  size_t outside = 0;
  std::vector<std::pair<int, int>> diff;
};

static void record_e1_tile_match(const nav::NavTileBlob &t, const std::vector<nav::NavTileBlob> &tiles,
                                 E1CompareCounts &counts) {
  const nav::NavTileBlob *match = find_tile_in_chunk(tiles, t.tx, t.ty);
  if (!match) {
    ++counts.outside;
    return;
  }
  ++counts.common;
  if (match->data == t.data) {
    ++counts.same_tiles;
  } else {
    counts.diff.push_back({t.tx, t.ty});
  }
}

static void test_real_e1_tiles(const std::string &path, const nav::NavFile &idx, const nav::NavFile &e1) {
  std::string err;
  std::ifstream in(path, std::ios::binary);
  std::map<std::pair<int, int>, std::vector<nav::NavTileBlob>> cache;
  std::map<std::pair<int, int>, const nav::NavChunkEntry *> dir;
  for (const auto &c : idx.chunks) {
    dir[{c.cx, c.cy}] = &c;
  }
  E1CompareCounts counts;
  for (const auto &t : e1.tiles) {
    const ChunkKey k = nav::chunk_of_tile(t.tx, t.ty);
    const auto d = dir.find({k.x, k.y});
    if (d == dir.end()) {
      ++counts.outside;
      continue;
    }
    auto &tiles = cache[{k.x, k.y}];
    if (tiles.empty() && !nav::read_nav_chunk(in, *d->second, &tiles, &err)) {
      CHECK(false, "a chunk of the real file reads");
      return;
    }
    record_e1_tile_match(t, tiles, counts);
  }
  std::printf("  E1's %zu tiles: %zu are not in the region (the circle reaches past the Riverlands), %zu are in both: %zu byte for byte the same, %zu differ\n", e1.tiles.size(), counts.outside, counts.common, counts.same_tiles, counts.diff.size());
  for (size_t i = 0; i < counts.diff.size() && i < 12; ++i) {
    std::printf("    differs: tile (%d, %d)\n", counts.diff[i].first, counts.diff[i].second);
  }
  CHECK(counts.common > 2000, "most of E1's tiles are in the region");
  CHECK(counts.same_tiles + 12 >= counts.common, "and all but a dozen (the tiles near the 19 meshes whose short names two assets share: E1's read merged them) are byte for byte E1's");
}

static void test_real_index_and_e1(const std::string &path, const nav::NavFile &idx) {
  namespace rg = aoc::worldd::spawns::region;
  bool same = rg::kRiverlandsRectCount == idx.region.size();
  for (size_t i = 0; same && i < idx.region.size(); ++i) {
    same = idx.region[i].x0 == rg::kRiverlandsRects[i].x0 && idx.region[i].y0 == rg::kRiverlandsRects[i].y0 && idx.region[i].x1 == rg::kRiverlandsRects[i].x1 && idx.region[i].y1 == rg::kRiverlandsRects[i].y1;
  }
  CHECK(same, "the nav file's region is the header's 401 rectangles (the same generator)");
  uint64_t cells = 0, zoi_cells = 0, largest = 0;
  for (const auto &r : rg::kRiverlandsRects) {
    cells += static_cast<uint64_t>((r.x1 - r.x0) / rg::kCellCm) * static_cast<uint64_t>((r.y1 - r.y0) / rg::kCellCm);
  }
  for (const auto &z : rg::kZois) {
    zoi_cells += z.cells;
  }
  CHECK(cells == rg::kRiverlandsAreaCells && zoi_cells == cells && rg::kZoiCount == 12, "the header's 12 ZOIs hold the footprint's 1,873,943 cells (49.12 km2)");
  for (const auto &c : idx.chunks) {
    largest = std::max<uint64_t>(largest, c.tiles);
  }
  CHECK(largest <= 64, "no chunk holds more than 64 tiles");

  const char *e1env = std::getenv("AOC_E1_NAV");
  const std::string e1path = e1env && *e1env ? e1env : "dist/content/e1/area_nav.bin";
  nav::NavFile e1;
  std::string err;
  if (nav::nav_file_version(e1path) == 1 && nav::read_nav_file(e1path, &e1, &err)) {
    test_real_e1_tiles(path, idx, e1);
  } else {
    std::printf("  [skip] %s: E1's flat file is not there (the byte-for-byte check against it)\n", e1path.c_str());
  }
}

static void test_real_walk(AreaNav &n, double login_x, double login_y) {
  std::vector<std::pair<double, double>> legs = {{login_x, login_y}, {-350000, 880000}, {-1300000, 300000}, {-600000, 330000}, {login_x, login_y}};
  const auto t0 = Clock::now();
  size_t steps = 0, peak_seen = 0;
  double sum_pump = 0, walked_cm = 0;
  for (size_t l = 0; l + 1 < legs.size(); ++l) {
    const double dx = legs[l + 1].first - legs[l].first, dy = legs[l + 1].second - legs[l].second;
    const double len = std::hypot(dx, dy);
    walked_cm += len;
    const int count = static_cast<int>(len / 1500.0);
    for (int i = 0; i <= count; ++i) {
      const double f = static_cast<double>(i) / std::max(1, count);
      n.set_interest({{legs[l].first + dx * f, legs[l].second + dy * f}});
      const auto tp = Clock::now();
      n.pump(3.0);
      sum_pump += ms_since(tp);
      peak_seen = std::max(peak_seen, n.tile_count());
      ++steps;
      if (steps % 6 == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }
  settle(n);
  const auto st = n.window_stats();
  std::printf("  walk of %zu steps (%.0f km) in %.1f s: peak %zu tiles resident (%zu committed), %llu chunk loads, %llu unloads, %llu failures, %llu deferred by the cap\n", steps, walked_cm / 100000.0, ms_since(t0) / 1000.0,
              st.peak_resident_tiles, st.peak_committed_tiles, static_cast<unsigned long long>(st.chunk_loads), static_cast<unsigned long long>(st.chunk_unloads),
              static_cast<unsigned long long>(st.load_failures), static_cast<unsigned long long>(st.deferred_by_cap));
  std::printf("  a chunk: read and checked in %.2f ms at most (worker), added in %.2f ms at most, removed in %.2f ms at most (net thread); the longest pump %.2f ms; pumps %.3f ms on average\n", st.max_chunk_read_ms,
              st.max_chunk_add_ms, st.max_chunk_remove_ms, st.max_pump_ms, sum_pump / std::max<size_t>(1, steps));
  CHECK(st.peak_resident_tiles <= 4096 && st.peak_committed_tiles <= 4096 && peak_seen <= 4096, "a walk across the Riverlands never has more than 4,096 tiles resident");
  CHECK(st.load_failures == 0, "no chunk failed to load or to add");
  CHECK(st.chunk_loads > 100 && st.chunk_unloads > 50, "and the window followed the walk");
  CHECK(st.max_pump_ms <= 15.0, "no pump (the window's whole share of a tick, budget 5 ms) takes more than 15 ms on the net thread: a chunk's tiles are added a few at a time");
  CHECK(st.max_chunk_add_ms <= 36.0 && st.max_chunk_remove_ms <= 18.0, "and the densest chunk's 64 tiles cost at most 36 ms to add (budget 12, over two or three pumps) and 18 ms to remove (budget 6)");
}

struct PatrolBand {
  double lo;
  double hi;
  size_t pairs = 0;
  size_t old_cap = 0;
  size_t scaled = 0;
  size_t incomplete = 0;
  long long used = 0;
};

static PatrolBand *find_open_patrol_band(std::vector<PatrolBand> &bands, double straight) {
  PatrolBand *band = nullptr;
  for (PatrolBand &bd : bands) {
    if (straight >= bd.lo && straight < bd.hi && bd.pairs + bd.incomplete < 60) {
      band = &bd;
    }
  }
  return band;
}

static void sample_patrol_band(AreaNav &n, const Vec3 &a, const Vec3 &b, std::vector<PatrolBand> &bands) {
  const double straight = std::hypot(a.x - b.x, a.y - b.y);
  PatrolBand *band = find_open_patrol_band(bands, straight);
  if (!band) {
    return;
  }
  const nav::PathResult full = n.find_path(a, b, 200, 400, 0);
  if (!full.found || !full.complete) {
    ++band->incomplete;
    return;
  }
  ++band->pairs;
  const nav::PathResult o = n.find_path(a, b, 200, 400, 2500);
  if (o.found && o.complete) {
    ++band->old_cap;
  }
  const nav::PathResult sc = n.find_path(a, b, 200, 400, npc::Mover::search_iterations(straight));
  if (sc.found && sc.complete) {
    ++band->scaled;
  }
  band->used += sc.iterations;
}

static void test_real_patrol_bands(AreaNav &n, double login_x, double login_y) {
  n.load_around(login_x, login_y, 2);
  std::mt19937 rng(77);
  const auto unit = [&] {
    return static_cast<double>(rng() >> 8) / 16777216.0;
  };
  double login_z = 0;
  n.terrain_height(login_x, login_y, &login_z);
  const Vec3 centre{login_x, login_y, login_z};
  std::vector<PatrolBand> bands = {{3000, 10000}, {10000, 20000}, {20000, 40000}, {40000, 80000}};
  for (int attempt = 0; attempt < 4000; ++attempt) {
    Vec3 a, b;
    if (n.random_point(centre, 50000, unit(), unit(), &a) && n.random_point(centre, 50000, unit(), unit(), &b)) {
      sample_patrol_band(n, a, b, bands);
    }
  }
  for (const PatrolBand &bd : bands) {
    std::printf("  legs of %3.0f to %3.0f m straight on the real navmesh: %2zu found unbounded (%zu not found at all); with 2,500 expansions: %2zu; with the cap scaled by the distance: %2zu (%lld expansions on average)\n",
                bd.lo / 100.0, bd.hi / 100.0, bd.pairs, bd.incomplete, bd.old_cap, bd.scaled, bd.pairs ? bd.used / static_cast<long long>(bd.pairs) : 0);
  }
  bool never_worse = true;
  for (const PatrolBand &bd : bands) {
    never_worse = never_worse && bd.scaled >= bd.old_cap;
  }
  CHECK(bands[0].pairs >= 20 && bands[1].pairs >= 20, "enough legs to judge by in the two bands that hold 99 % of the data's patrol legs (30 to 200 m)");
  CHECK(bands[0].scaled * 100 >= bands[0].pairs * 75 && bands[1].scaled * 100 >= bands[1].pairs * 75, "three in four of the legs of 30 to 200 m that the unbounded search finds are found within the cap (the rest take the long way round)");
  CHECK(never_worse, "and in every band the scaled cap finds at least what the cap of 2,500 found");
}

static void test_real() {
  std::printf("the real ground\n");
  const char *env = std::getenv("AOC_AREA_NAV");
  const std::string path = env && *env ? env : "dist/content/area_nav.bin";
  if (nav::nav_file_version(path) != 2) {
    std::printf("  [skip] %s is not a chunked area_nav.bin (run tools/mapc: mapc build)\n", path.c_str());
    return;
  }
  std::string err;
  nav::NavFile idx;
  CHECK(nav::read_nav_index(path, &idx, &err), "the index of the real file reads");
  std::printf("  %zu terrain grids, %zu chunks, %llu tiles, %zu region rectangles\n", idx.terrain.size(), idx.chunks.size(), static_cast<unsigned long long>(idx.tile_count_v2), idx.region.size());
  CHECK(idx.region.size() == 401 && idx.chunk_tiles == 8 && idx.settings.max_tiles == 4096 && idx.settings.max_polys == 1024, "401 rectangles, chunks of 8 x 8 tiles, a mesh of 4,096 tiles and 1,024 polygons");
  CHECK(std::fabs(idx.center_x - (-777664.5)) < 1e-6 && std::fabs(idx.center_y - 616578.6875) < 1e-6, "the grid is anchored on E1's corner (the login spot - 112,000)");
  test_real_index_and_e1(path, idx);
  auto n = AreaNav::load(path, &err);
  if (!n) {
    CHECK(false, "the real file opens");
    return;
  }
  CHECK(n->windowed() && n->tile_count() == 0, "it opens as a window with no tile resident");
  const double login_x = n->center_x(), login_y = n->center_y();
  test_real_walk(*n, login_x, login_y);
  test_real_patrol_bands(*n, login_x, login_y);
}

void run_nav_window_npcs_tests() {
  test_npcs();
  test_edge();
  test_real();
}
