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

static void test_build_flat_and_region(const aoc::worldd::nav::NavFile &flat,
                                       const aoc::worldd::nav::NavFile &reg) {
  const aoc::worldd::nav::NavFile reg1 =
      aoc::worldd::navfix::build_chunk_file(true, 1);
  CHECK(!flat.chunked() && reg.chunked() && reg.chunk_tiles == 8 &&
            reg.region.size() == 1,
        "a region build is chunked (8 tiles a side) and carries its rectangle; "
        "a circle build is flat");
  CHECK(aoc::worldd::nav::serialize_nav_file(reg) ==
            aoc::worldd::nav::serialize_nav_file(reg1),
        "one thread or four: the same chunked bytes");
  CHECK(reg.tiles.size() == 4096, "the footprint's 64 x 64 tiles are all there "
                                  "(flat ground: every tile has a floor)");
  std::map<std::pair<int, int>, const aoc::worldd::nav::NavTileBlob *> by_tile;
  for (const auto &t : reg.tiles) {
    by_tile[{t.tx, t.ty}] = &t;
  }
  size_t common = 0, same = 0;
  for (const auto &t : flat.tiles) {
    const auto it = by_tile.find({t.tx, t.ty});
    if (it == by_tile.end()) {
      continue;
    }
    ++common;
    if (it->second->data == t.data) {
      ++same;
    }
  }
  CHECK(flat.tiles.size() > 800 && common == flat.tiles.size(),
        "every tile of the circle build is in the region build (the circle "
        "lies inside the footprint)");
  CHECK(same == common,
        "and each is the very same bytes: the region mode keeps E1's tiles "
        "(the grid is anchored on the same corner)");
}

static void test_build() {
  std::printf("the chunked build\n");
  const aoc::worldd::nav::NavFile flat =
      aoc::worldd::navfix::build_chunk_file(false, 4);
  const aoc::worldd::nav::NavFile reg =
      aoc::worldd::navfix::build_chunk_file(true, 4);
  test_build_flat_and_region(flat, reg);
  const auto bytes = aoc::worldd::nav::serialize_nav_file(reg);
  aoc::worldd::nav::NavFile back;
  std::string err;
  CHECK(
      aoc::worldd::nav::parse_nav_file(bytes.data(), bytes.size(), &back, &err),
      "the chunked file parses");
  CHECK(back.chunks.size() == 64 && back.tiles.size() == 4096 &&
            back.tile_count_v2 == 4096,
        "64 chunks, 4,096 tiles");
  bool sorted = true, hashes = true, eight = true;
  for (size_t i = 0; i < back.chunks.size(); ++i) {
    if (i &&
        !(aoc::worldd::nav::ChunkKey{back.chunks[i - 1].cx,
                                     back.chunks[i - 1].cy} <
          aoc::worldd::nav::ChunkKey{back.chunks[i].cx, back.chunks[i].cy})) {
      sorted = false;
    }
    if (back.chunks[i].tiles != 64) {
      eight = false;
    }
    if (back.chunks[i].hash == 0) {
      hashes = false;
    }
  }
  CHECK(sorted && hashes && eight,
        "the chunk directory is sorted by (cy, cx), each chunk holds its 8 x 8 "
        "tiles and a hash");
  CHECK(
      back.chunks.front().cx == -1 && back.chunks.front().cy == -1 &&
          back.chunks.back().cx == 6 && back.chunks.back().cy == 6,
      "chunks -1..6 in both directions (tile indices reach into the negative)");
  CHECK(aoc::worldd::nav::serialize_nav_file(back) == bytes,
        "parse and write give the same bytes");
}

static void test_file_disk_corrupt(const std::string &path,
                                   const aoc::worldd::nav::NavFile &idx) {
  std::string err;
  std::vector<aoc::worldd::nav::NavTileBlob> t;
  {
    std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
    io.seekp(static_cast<std::streamoff>(idx.chunks[20].offset + 40));
    const char x = 0x7f;
    io.write(&x, 1);
  }
  aoc::worldd::nav::NavFile idx2;
  std::ifstream in2(path, std::ios::binary);
  CHECK(aoc::worldd::nav::read_nav_index(path, &idx2, &err) &&
            !aoc::worldd::nav::read_nav_chunk(in2, idx2.chunks[20], &t, &err),
        "a chunk damaged on disk: the index reads, that chunk is refused");
  std::string lerr;
  auto n = aoc::worldd::nav::AreaNav::load(path, &lerr);
  CHECK(n != nullptr && n->windowed(),
        "the window opens on a file with a damaged chunk (it fails only when "
        "that chunk is wanted)");
  if (n) {
    const aoc::worldd::nav::ChunkKey bad_key{idx.chunks[20].cx,
                                             idx.chunks[20].cy};
    n->load_around(cx_of(bad_key.x), cx_of(bad_key.y), 0, 5);
    CHECK(n->window_stats().load_failures == 1 && !n->chunk_resident(bad_key),
          "the damaged chunk is a counted failure and stays out");
    n->set_interest({{cx_of(bad_key.x), cx_of(bad_key.y)}});
    n->pump(50);
    settle(*n);
    CHECK(n->window_stats().load_failures == 1,
          "and is not asked again and again");
  }
  n.reset();
  in2.close();
}

static void test_file_disk(const aoc::worldd::nav::NavFile &reg,
                           const std::vector<uint8_t> &bytes) {
  std::string err;
  const std::string path = "nav_window_roundtrip.bin";
  CHECK(aoc::worldd::nav::write_nav_file(reg, path, &err),
        "the file is written to disk");
  CHECK(aoc::worldd::nav::nav_file_version(path) == 2 &&
            aoc::worldd::nav::nav_file_version("no/such/file.bin") == 0,
        "nav_file_version: 2 for it, 0 for a missing file");
  aoc::worldd::nav::NavFile idx;
  CHECK(aoc::worldd::nav::read_nav_index(path, &idx, &err) &&
            idx.tiles.empty() && idx.chunks.size() == 64 &&
            idx.terrain.size() == 36 && idx.region.size() == 1,
        "read_nav_index: the directory, the terrain and the region, no tile");
  std::ifstream in(path, std::ios::binary);
  std::vector<aoc::worldd::nav::NavTileBlob> t;
  CHECK(aoc::worldd::nav::read_nav_chunk(in, idx.chunks[10], &t, &err) &&
            t.size() == 64,
        "read_nav_chunk: the 64 tiles of one chunk");
  aoc::worldd::nav::NavChunkEntry wrong = idx.chunks[10];
  wrong.hash ^= 1;
  CHECK(!aoc::worldd::nav::read_nav_chunk(in, wrong, &t, &err) &&
            err.find("hash") != std::string::npos,
        "a directory row with a wrong hash is refused");
  CHECK(aoc::worldd::nav::read_nav_chunk(in, idx.chunks[10], &t, &err,
                                         idx.chunk_tiles) &&
            t.size() == 64,
        "a chunk is read with its tiles checked to lie in it (the file's 8 "
        "tiles a side)");
  CHECK(!aoc::worldd::nav::read_nav_chunk(in, idx.chunks[10], &t, &err, 4) &&
            err.find("another chunk") != std::string::npos,
        "and a chunk read as if chunks were 4 tiles a side is refused: its "
        "tiles lie in other chunks");
  aoc::worldd::nav::NavFile whole;
  CHECK(aoc::worldd::nav::read_nav_file(path, &whole, &err) &&
            aoc::worldd::nav::serialize_nav_file(whole) == bytes,
        "read_nav_file reads every chunk and writes back the same bytes");
  in.close();
  test_file_disk_corrupt(path, idx);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

static void test_file_offset_bounds(const std::vector<uint8_t> &bytes) {
  aoc::worldd::nav::NavFile f;
  std::string err;
  uint64_t terrain_dir = 0, chunk_dir = 0;
  std::memcpy(&terrain_dir, bytes.data() + 128, sizeof terrain_dir);
  std::memcpy(&chunk_dir, bytes.data() + 136, sizeof chunk_dir);
  const auto with_offset = [&](size_t at, uint64_t value) {
    auto malformed = bytes;
    std::memcpy(malformed.data() + at, &value, sizeof value);
    return malformed;
  };
  for (const uint64_t offset :
       {std::numeric_limits<uint64_t>::max() - 3, uint64_t{0}}) {
    const auto malformed =
        with_offset(static_cast<size_t>(terrain_dir) + 56, offset);
    CHECK(!aoc::worldd::nav::parse_nav_file(malformed.data(), malformed.size(),
                                            &f, &err),
          "terrain samples outside their section are refused before reading "
          "them");
  }
  const auto overflow_chunk =
      with_offset(static_cast<size_t>(chunk_dir) + 16,
                  std::numeric_limits<uint64_t>::max() - 3);
  CHECK(!aoc::worldd::nav::parse_nav_file(overflow_chunk.data(),
                                          overflow_chunk.size(), &f, &err),
        "a chunk offset that would wrap around is refused");
  auto overflow_dir =
      with_offset(136, std::numeric_limits<uint64_t>::max() - 15);
  uint32_t chunk_count = 0;
  std::memcpy(&chunk_count, bytes.data() + 120, sizeof chunk_count);
  const uint64_t blob_offset =
      std::numeric_limits<uint64_t>::max() - 15 + 32ull * chunk_count;
  std::memcpy(overflow_dir.data() + 144, &blob_offset, sizeof blob_offset);
  CHECK(!aoc::worldd::nav::parse_nav_file(overflow_dir.data(),
                                          overflow_dir.size(), &f, &err),
        "a chunk directory whose end wraps around is refused");
}

static void test_file() {
  std::printf("the chunked file\n");
  const aoc::worldd::nav::NavFile reg =
      aoc::worldd::navfix::build_chunk_file(true, 4);
  const auto bytes = aoc::worldd::nav::serialize_nav_file(reg);
  aoc::worldd::nav::NavFile f;
  std::string err;
  std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 5);
  CHECK(!aoc::worldd::nav::parse_nav_file(cut.data(), cut.size(), &f, &err) &&
            !err.empty(),
        "a cut-short file is refused");
  std::vector<uint8_t> bad = bytes;
  bad[7] = '9';
  CHECK(!aoc::worldd::nav::parse_nav_file(bad.data(), bad.size(), &f, &err),
        "a wrong magic is refused");
  std::vector<uint8_t> ver = bytes;
  ver[8] = 7;
  CHECK(!aoc::worldd::nav::parse_nav_file(ver.data(), ver.size(), &f, &err) &&
            err.find("version") != std::string::npos,
        "a wrong version is refused");
  std::vector<uint8_t> flip = bytes;
  flip[bytes.size() - 100] ^= 0x55;
  CHECK(!aoc::worldd::nav::parse_nav_file(flip.data(), flip.size(), &f, &err) &&
            err.find("hash") != std::string::npos,
        "a damaged chunk is refused by its hash");
  std::vector<uint8_t> pad = bytes;
  pad.push_back(0);
  CHECK(!aoc::worldd::nav::parse_nav_file(pad.data(), pad.size(), &f, &err),
        "a trailing byte is refused");
  test_file_offset_bounds(bytes);
  test_file_disk(reg, bytes);
}

static void test_window_around(aoc::worldd::nav::AreaNav &n) {
  const aoc::worldd::nav::ChunkKey c0 = n.chunk_at(0, 0);
  CHECK(c0.x == 2 && c0.y == 2, "the centre is in chunk (2, 2)");
  CHECK(n.chunk_at(-23000, -23000) == (aoc::worldd::nav::ChunkKey{0, 0}) &&
            n.chunk_at(-23000.01, -23000.01) ==
                (aoc::worldd::nav::ChunkKey{-1, -1}) &&
            n.chunk_at(1000, 1000) == (aoc::worldd::nav::ChunkKey{3, 3}) &&
            n.chunk_at(999.99, 999.99) == (aoc::worldd::nav::ChunkKey{2, 2}),
        "chunk_at: the borders fall on the tile grid (floor, also for the "
        "negative tiles)");
  CHECK(n.load_around(0, 0, 1),
        "load_around the centre, one chunk each way, returns true");
  CHECK(n.window_stats().resident_chunks == 9 && n.tile_count() == 9 * 64,
        "9 chunks, 576 tiles are in");
  CHECK(n.ready_at(0, 0) && n.ready_at(cx_of(3), cx_of(1)) &&
            !n.ready_at(cx_of(4), 0),
        "ready inside the 3 x 3, not outside");
  CHECK(n.on_navmesh({0, 0, 1000}), "and the navmesh answers there");
  CHECK(n.polygon_count() > 9, "polygons are counted");
  const auto stats = n.window_stats();
  CHECK(stats.chunk_loads == 9 && stats.peak_resident_tiles == 576,
        "9 loads, a peak of 576 tiles");
}

static void test_window() {
  std::printf("the window\n");
  const std::string path = aoc::worldd::navfix::write_chunk_file("window");
  std::string err;
  auto n = aoc::worldd::nav::AreaNav::load(path, &err);
  CHECK(n != nullptr && n->windowed() && n->chunk_count() == 64,
        "the chunked file opens as a window of 64 chunks");
  if (!n) {
    return;
  }
  CHECK(n->tile_count() == 0 && n->total_tiles() == 4096 &&
            n->terrain_grids() == 36,
        "no tile is in the mesh; the terrain (36 grids) is");
  double z = 0;
  CHECK(n->terrain_height(100, 100, &z) && std::fabs(z - 1000.0) < 1e-6,
        "the terrain answers at once");
  CHECK(!n->ready_at(0, 0) && !n->chunk_resident(n->chunk_at(0, 0)),
        "the ground at the centre is not ready");
  CHECK(!n->on_navmesh({0, 0, 1000}) &&
            !n->find_path({-500, 0, 1000}, {500, 0, 1000}).found,
        "no navmesh queries are answered before the chunk is here");
  CHECK(n->in_region(0, 0) && n->has_region() && !n->in_region(40000, 0) &&
            !n->in_region(0, -40000) && n->in_region(32999, -30999),
        "in_region: the rectangle [-31,000, 33,000) squared");
  test_window_around(*n);
  n.reset();
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

static void test_borders_west_only(AreaNav &n, double border) {
  n.load_around(-3000, 0, 0);
  Vec3 hit;
  CHECK(n.ready_at(-3000, 0) && !n.ready_at(border + 3000, 0), "the chunk west of the border (the centre's) is here, the east one is not");
  const bool reaches = n.raycast({border - 3000, 0, 1000}, {border + 3000, 0, 1000}, &hit);
  CHECK(!reaches && hit.x > border - 150 && hit.x < border + 50, "a ray along the ground towards the unloaded chunk stops at the border");
  CHECK(!n.find_path({border - 3000, 0, 1000}, {border + 3000, 0, 1000}).found, "a path to a point in the unloaded chunk is not found (no polygon there)");
  auto partial = n.find_path({border - 3000, 0, 1000}, {border - 500, 0, 1000});
  CHECK(partial.found && partial.complete, "a path inside the loaded chunk is fine");
  double z0 = 0;
  CHECK(n.ground_height(border + 3000, 0, 1000, &z0) && std::fabs(z0 - 1000) < 1e-6, "the ground in the unloaded chunk is the terrain, exact (it never waits for the disk)");
  (void)z0;
}

static void test_borders_across(AreaNav &n, double border) {
  Vec3 hit;
  n.set_interest({{border - 500, 3000}, {border + 500, 3000}});
  settle(n);
  CHECK(n.ready_at(border - 3000, 0) && n.ready_at(border + 3000, 0) && n.ready_at(border - 3000, 3000) && n.ready_at(border + 3000, 3000), "both chunks of the border are here (in both rows)");
  CHECK(n.raycast({border - 3000, 0, 1000}, {border + 3000, 0, 1000}, &hit), "a ray along the ground now crosses the border");
  auto across = n.find_path({border - 3000, 0, 1000}, {border + 3000, 0, 1000});
  CHECK(across.found && across.complete && std::fabs(across.length - 6000) < 100, "and a path across it is complete and straight (6,000 cm)");
  bool crosses = false;
  for (size_t i = 0; i + 1 < across.points.size(); ++i) {
    if ((across.points[i].x < border) != (across.points[i + 1].x < border)) {
      crosses = true;
    }
  }
  CHECK(crosses, "its points are on both sides of the border");
  auto round_wall = n.find_path({border - 3000, -3500, 1000}, {border + 3000, -3500, 1000});
  CHECK(round_wall.found && round_wall.complete && round_wall.length > 6000 + 1500, "a path across the border where a wall stands on it goes round the wall's end");
  CHECK(!n.raycast({border - 3000, -3500, 1000}, {border + 3000, -3500, 1000}, &hit) && hit.x > border - 700 && hit.x < border, "and a ray into that wall stops before it");
}

static void test_borders_ground_and_platform(AreaNav &n, double border) {
  double z = 0;
  CHECK(n.ground_height(border - 0.01, 3000, 1000, &z) && std::fabs(z - 1000) < 1e-6 && n.ground_height(border + 0.01, 3000, 1000, &z) && std::fabs(z - 1000) < 1e-6 &&
            n.ground_height(border, 3000, 1000, &z) && std::fabs(z - 1000) < 1e-6,
        "ground_height at the border's two sides and on it: the terrain, exact");
  CHECK(n.ground_height(border - 400, 6000, 1560, &z) && std::fabs(z - 1540) < 15 && n.ground_height(border + 400, 6000, 1560, &z) && std::fabs(z - 1540) < 15 &&
            n.ground_height(border, 6000, 1560, &z) && std::fabs(z - 1540) < 15,
        "the platform across the border is a surface on both sides and on the line (1,540)");
  CHECK(n.on_navmesh({border - 100, 6000, 1540}, 40, 60) && n.on_navmesh({border + 100, 6000, 1540}, 40, 60), "its top is on the navmesh on both sides");
}

static void test_borders() {
  std::printf("borders\n");
  const std::string path = navfix::write_chunk_file("borders");
  std::string err;
  auto n = AreaNav::load(path, &err);
  if (!n) {
    CHECK(false, "the fixture opens");
    return;
  }
  const double border = navfix::chunk_x0(3);
  test_borders_west_only(*n, border);
  test_borders_across(*n, border);
  test_borders_ground_and_platform(*n, border);

  n->set_interest({{cx_of(6), cx_of(6)}});
  for (int i = 0; i < 100; ++i) {
    n->pump(50);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(!n->ready_at(border - 3000, 3000) && !n->on_navmesh({border - 3000, 3000, 1000}) && !n->find_path({border - 3000, 3000, 1000}, {border - 2000, 3000, 1000}).found,
        "after the players left, an unloaded chunk answers nothing (a path there is not found: a stale reference is never kept, the caller asks again)");
  n->set_interest({{border - 500, 3000}});
  settle(*n);
  CHECK(n->ready_at(border - 3000, 3000) && n->find_path({border - 3000, 3000, 1000}, {border + 1000, 3000, 1000}).complete, "and when the chunk comes back the path is found again");
  n.reset();
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

static void test_policy_walk_and_two_players(AreaNav &n) {
  const auto before = n.window_stats();
  for (int i = 0; i < 40; ++i) {
    n.set_interest({{navfix::chunk_x0(4) + (i % 2 ? 100 : -100), cx_of(1)}});
    for (int k = 0; k < 5; ++k) {
      n.pump(20);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  settle(n);
  const auto after = n.window_stats();
  CHECK(after.chunk_loads - before.chunk_loads <= 10 && after.chunk_unloads - before.chunk_unloads <= 5, "40 steps back and forth over a border: at most the new column's loads, nothing dropped and re-read");
  n.set_interest({{cx_of(0), cx_of(0)}, {cx_of(5), cx_of(5)}});
  for (int i = 0; i < 300; ++i) {
    n.pump(20);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  settle(n);
  CHECK(n.chunk_resident({0, 0}) && n.chunk_resident({5, 5}) && n.chunk_resident({2, 2}) && n.chunk_resident({3, 3}) && n.chunk_resident({7 - 1, 6}), "two players: the union of their windows is resident");
  CHECK(n.ready_at(cx_of(0), cx_of(0)) && n.ready_at(cx_of(5), cx_of(5)), "ready at both players");
  const size_t tiles = n.tile_count();
  n.set_interest({});
  n.pump(50);
  CHECK(n.tile_count() == tiles, "no player at all: the ground stays as it is");
  n.set_interest({{std::nan(""), 0}, {1e300, -1e300}});
  n.pump(50);
  CHECK(true, "NaN and enormous interest points do nothing (no crash)");
}

static void test_policy_zero_budget_pump(const std::string &path) {
  std::string err;
  auto n = AreaNav::load(path, &err);
  n->set_interest({{cx_of(2), cx_of(2)}});
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  int pumps = 0;
  size_t last = 0;
  bool progress = true;
  while (n->window_stats().resident_chunks < 25 && pumps < 5000) {
    n->pump(0.0);
    ++pumps;
    if (n->tile_count() < last) {
      progress = false;
    }
    last = n->tile_count();
  }
  CHECK(progress && n->window_stats().resident_chunks == 25 && pumps > 25, "pump(0) adds at least a tile each call: a chunk is spread over several pumps and all arrive");
  n.reset();
}

static void test_policy() {
  std::printf("the window's policy\n");
  const std::string path = navfix::write_chunk_file("policy");
  std::string err;
  auto n = AreaNav::load(path, &err);
  if (!n) {
    CHECK(false, "the fixture opens");
    return;
  }
  n->set_interest({{cx_of(1), cx_of(1)}});
  settle(*n);
  auto st = n->window_stats();
  CHECK(st.resident_chunks == 25 && n->tile_count() == 25 * 64, "one player: 5 x 5 chunks (1,600 tiles) are resident");
  CHECK(n->chunk_resident({-1, -1}) && n->chunk_resident({3, 3}) && !n->chunk_resident({4, 1}), "the ones within two chunks, not the third");
  n->set_interest({{cx_of(2), cx_of(1)}});
  settle(*n);
  st = n->window_stats();
  CHECK(st.resident_chunks == 30 && st.chunk_unloads == 0, "one chunk east: the western column (now 3 away) stays, a new column comes: 30 chunks, none dropped");
  n->set_interest({{cx_of(3), cx_of(1)}});
  settle(*n);
  st = n->window_stats();
  CHECK(st.resident_chunks == 30 && st.chunk_unloads == 5 && !n->chunk_resident({-1, 1}) && n->chunk_resident({0, 1}), "two chunks east: the column 4 away is dropped (5 chunks)");
  test_policy_walk_and_two_players(*n);
  n.reset();
  test_policy_zero_budget_pump(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

static void test_cap_walk(AreaNav &n, int cap) {
  int own_ready = 0, stops = 0;
  for (int step = 0; step < 400; ++step) {
    const double f = step / 399.0;
    const double px = navfix::chunk_x0(-1) + f * 8 * navfix::kChunkSpan - 1, py = navfix::chunk_x0(-1) + f * 8 * navfix::kChunkSpan - 1;
    n.set_interest({{px, py}});
    n.pump(2.0);
    if (step % 4 == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (step % 50 == 49) {
      settle(n);
      ++stops;
      if (n.ready_at(px, py)) {
        ++own_ready;
      }
    }
  }
  settle(n);
  const auto st = n.window_stats();
  CHECK(stops == 8 && own_ready == stops, "at each of 8 stops of the walk the chunk under the player is resident (the nearest chunks win the room)");
  CHECK(st.evicted_for_room > 0, "and chunks that were only kept were dropped to make room for nearer ones");
  CHECK(st.peak_resident_tiles <= static_cast<size_t>(cap) && st.peak_committed_tiles <= static_cast<size_t>(cap) && st.load_failures == 0, "a diagonal walk across the world: never more than 1,000 tiles resident or on the way, and Detour never refused a tile");
  CHECK(st.chunk_loads > 30 && st.chunk_unloads > 20, "and the window followed (chunks came and went)");
}

static void test_cap() {
  std::printf("the tile cap\n");
  const int cap = 1000;
  const std::string path = navfix::write_chunk_file("cap", cap);
  std::string err;
  auto n = AreaNav::load(path, &err);
  if (!n) {
    CHECK(false, "the fixture opens");
    return;
  }
  CHECK(n->file().settings.max_tiles == cap, "the file's mesh holds 1,000 tiles");
  n->set_interest({{cx_of(2), cx_of(2)}});
  settle(*n);
  auto st = n->window_stats();
  CHECK(st.peak_committed_tiles <= static_cast<size_t>(cap) && st.peak_resident_tiles <= static_cast<size_t>(cap) && st.deferred_by_cap > 0, "a window that would hold 1,600 tiles stops at the cap and says so");
  CHECK(n->chunk_resident({2, 2}) && n->chunk_resident({3, 2}) && n->chunk_resident({2, 3}), "the nearest chunks are the ones that came (the player's own first)");
  test_cap_walk(*n, cap);
  n.reset();
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

static void test_starvation_verify(AreaNav &n, std::vector<std::pair<double, double>> &players, int cap, size_t arrivals_ok, size_t starved) {
  const auto st = n.window_stats();
  std::printf("    8 players one after another on a mesh of %d tiles (15 chunks): %zu arrived with their own chunk resident, %zu starved; %llu chunks dropped for a nearer one, %llu deferred, peak %zu tiles\n", cap, arrivals_ok, starved,
              static_cast<unsigned long long>(st.evicted_for_room), static_cast<unsigned long long>(st.deferred_by_cap), st.peak_committed_tiles);
  CHECK(starved == 0, "every newcomer got the chunk it stands in, whatever the first players' windows held");
  bool all = true;
  for (const auto &p : players) {
    all = all && n.ready_at(p.first, p.second);
  }
  CHECK(all, "and at the end each of the eight players stands in a resident chunk");
  CHECK(st.peak_committed_tiles <= static_cast<size_t>(cap) && st.peak_resident_tiles <= static_cast<size_t>(cap) && st.load_failures == 0, "the cap held and Detour refused nothing");
  CHECK(st.resident_chunks >= 14 && st.resident_chunks <= 15, "and the mesh is full: the eight own chunks and the nearest of the rest, 15 chunks of 64 tiles");
  const size_t resident_before = n.window_stats().resident_chunks;
  players.pop_back();
  players.pop_back();
  run_window(n, players);
  CHECK(n.window_stats().resident_chunks >= resident_before - 2 && n.window_stats().resident_tiles <= static_cast<size_t>(cap), "two players leaving: the others' windows grow into the room (or the chunks stay within the hysteresis)");
}

static void test_starvation() {
  std::printf("the tile cap, shared by distance\n");
  const int cap = 1000;
  const std::string path = navfix::write_chunk_file("starve", cap);
  std::string err;
  auto n = AreaNav::load(path, &err);
  if (!n) {
    CHECK(false, "the fixture opens");
    return;
  }
  const int spots[8][2] = {{0, 0}, {6, 0}, {0, 6}, {6, 6}, {3, 0}, {0, 3}, {6, 3}, {3, 6}};
  std::vector<std::pair<double, double>> players;
  size_t starved = 0, arrivals_ok = 0;
  for (int k = 0; k < 8; ++k) {
    players.emplace_back(cx_of(spots[k][0]), cx_of(spots[k][1]));
    run_window(*n, players);
    bool all = true;
    for (const auto &p : players) {
      all = all && n->ready_at(p.first, p.second);
    }
    if (n->ready_at(players.back().first, players.back().second)) {
      ++arrivals_ok;
    } else {
      ++starved;
    }
    if (!all) {
      std::printf("    (after player %d: not every player's own chunk is resident)\n", k + 1);
    }
  }
  test_starvation_verify(*n, players, cap, arrivals_ok, starved);
  n.reset();
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

static void test_interest_gating() {
  std::printf("set_interest and the plan\n");
  const std::string path = navfix::write_chunk_file("gating");
  std::string err;
  auto n = AreaNav::load(path, &err);
  if (!n) {
    CHECK(false, "the fixture opens");
    return;
  }
  const std::vector<std::pair<double, double>> one = {{cx_of(1), cx_of(1)}};
  run_window(*n, one);
  CHECK(n->window_stats().resident_chunks == 25, "one player: its 25 chunks are in");
  const auto before = n->window_stats();
  for (int i = 0; i < 200; ++i) {
    n->set_interest({{cx_of(1) + (i % 7), cx_of(1) - (i % 5)}});
  }
  const auto after = n->window_stats();
  CHECK(after.plans == before.plans && after.plans_skipped >= before.plans_skipped + 200, "200 calls with the same chunk and nothing waiting: no plan (all skipped)");
  n->set_interest({{cx_of(2), cx_of(1)}});
  CHECK(n->window_stats().plans == after.plans + 1, "a step into the next chunk plans once");
  run_window(*n, {{cx_of(2), cx_of(1)}});
  const size_t chunks = n->window_stats().resident_chunks;
  n->set_interest({{std::nan(""), std::nan("")}});
  n->pump(50);
  CHECK(n->window_stats().resident_chunks == chunks && !n->loading(), "a point that is not a number is no player: the ground stays as it is");
  n->load_around(cx_of(5), cx_of(5), 0);
  const auto p0 = n->window_stats().plans;
  n->set_interest({{cx_of(2), cx_of(1)}});
  CHECK(n->window_stats().plans == p0 + 1, "after a load_around the next set_interest plans again (it used other radii)");
  n.reset();
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

static void test_read_failure_recover(AreaNav &n, const std::string &path, uintmax_t full, ChunkKey last_key) {
  std::error_code ec;
  {
    std::string werr;
    CHECK(nav::write_nav_file(navfix::build_chunk_file(true, 4, 4096), path, &werr) && std::filesystem::file_size(path, ec) == full, "(the file is written whole again)");
  }
  const bool other = n.load_around(cx_of(0), cx_of(0), 0, 5);
  CHECK(other && n.chunk_resident({0, 0}), "a chunk read after the failed one loads: the stream's fail bit does not outlive the failure");
  n.set_interest({{cx_of(last_key.x), cx_of(last_key.y)}});
  const auto t0 = Clock::now();
  while (!n.chunk_resident(last_key) && ms_since(t0) < 5000) {
    n.set_interest({{cx_of(last_key.x), cx_of(last_key.y)}});
    n.pump(20.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(n.chunk_resident(last_key) && n.window_stats().retries >= 1, "and the chunk that failed is asked for again after the retry delay, and comes");
}

static void test_read_failure() {
  std::printf("a failed read\n");
  const std::string path = navfix::write_chunk_file("readfail");
  std::string err;
  auto n = AreaNav::load(path, &err);
  if (!n) {
    CHECK(false, "the fixture opens");
    return;
  }
  n->set_retry_delay(0.05);
  const auto &dir = n->file().chunks;
  nav::NavChunkEntry last = dir.front();
  for (const auto &c : dir) {
    if (c.offset > last.offset) {
      last = c;
    }
  }
  const ChunkKey last_key{last.cx, last.cy};
  std::error_code ec;
  const uintmax_t full = std::filesystem::file_size(path, ec);
  std::filesystem::resize_file(path, last.offset + 10, ec);
  CHECK(!ec && std::filesystem::file_size(path) == last.offset + 10, "the file is cut short under the open worker (the last chunk's blob loses its tail)");
  if (ec) {
    n.reset();
    return;
  }
  n->load_around(cx_of(last_key.x), cx_of(last_key.y), 0, 5);
  CHECK(n->window_stats().load_failures == 1 && !n->chunk_resident(last_key), "the chunk whose blob is cut is a failed read");
  const auto errors = n->take_load_errors();
  CHECK(errors.size() == 1 && errors[0].find("chunk (" + std::to_string(last_key.x) + ", " + std::to_string(last_key.y) + ")") != std::string::npos, "and the failure is named (the owner logs it)");
  CHECK(n->take_load_errors().empty(), "(taken once)");
  test_read_failure_recover(*n, path, full, last_key);
  n.reset();
  std::filesystem::remove(path, ec);
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const auto t0 = Clock::now();
  test_build();
  test_file();
  test_window();
  test_borders();
  test_policy();
  test_cap();
  test_starvation();
  test_interest_gating();
  test_read_failure();
  run_nav_window_npcs_tests();
  std::printf("test_worldd_nav_window: %d passed, %d failed (%.1f s)\n", g_pass, g_fail, ms_since(t0) / 1000.0);
  return g_fail == 0 ? 0 : 1;
}
