// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.

#pragma once

#include "nav_chunk_fixture.h"
#include "protocol/repl/codecs.h"
#include "protocol/schema/generated/index.h"
#include "session/player_world.h"

#include <filesystem>
#include <fstream>
#include <thread>

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg)                 \
  do {                                   \
    if (cond) {                          \
      std::printf("  [ok ] %s\n", msg);  \
      g_pass++;                          \
    } else {                             \
      std::printf("  [FAIL] %s\n", msg); \
      g_fail++;                          \
    }                                    \
  } while (0)

using Clock = std::chrono::steady_clock;

inline double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

inline double cx_of(int j) {
  return aoc::worldd::navfix::chunk_x0(j) +
         aoc::worldd::navfix::kChunkSpan * 0.5;
}

inline void settle(aoc::worldd::nav::AreaNav &n, double seconds = 10) {
  const auto t0 = Clock::now();
  while (n.loading() && ms_since(t0) < seconds * 1000) {
    n.pump(50.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  n.pump(50.0);
}

inline size_t tiles_of(const aoc::worldd::nav::NavFile &f,
                       const std::vector<aoc::worldd::nav::ChunkKey> &keys) {
  size_t n = 0;
  for (const auto &c : f.chunks) {
    for (const auto &k : keys) {
      if (c.cx == k.x && c.cy == k.y) {
        n += c.tiles;
      }
    }
  }
  return n;
}

inline void run_window(aoc::worldd::nav::AreaNav &n,
                       const std::vector<std::pair<double, double>> &players,
                       double seconds = 8) {
  const auto t0 = Clock::now();
  int quiet = 0;
  while (ms_since(t0) < seconds * 1000) {
    const auto before = n.window_stats();
    n.set_interest(players);
    n.pump(20.0);
    const auto after = n.window_stats();
    const bool moved = after.chunk_loads != before.chunk_loads ||
                       after.chunk_unloads != before.chunk_unloads ||
                       after.committed_tiles != before.committed_tiles ||
                       after.resident_tiles != before.resident_tiles;
    quiet = (!n.loading() && !moved) ? quiet + 1 : 0;
    if (quiet >= 30) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

inline const aoc::protocol::schema::ClassSchema &
schema_named(const char *name) {
  const aoc::protocol::schema::ClassSchema *s =
      aoc::protocol::schema::gen::find_class_schema(name);
  if (!s) {
    std::printf("  [FAIL] no schema %s\n", name);
    std::exit(2);
  }
  return *s;
}

inline aoc::worldd::world::WorldActor make_npc(uint64_t id, double x, double y,
                                               double z) {
  aoc::worldd::world::WorldActor w;
  w.id = id;
  w.kind = aoc::worldd::world::ActorKind::Npc;
  w.x = x;
  w.y = y;
  w.z = z;
  aoc::protocol::repl::Actor &a = w.actor;
  aoc::protocol::emit::FIntrepidNetworkGUID g;
  g.ObjectId = id;
  g.ServerId = 60;
  g.Randomizer = 0x6ee87ee4u;
  a.guid = g;
  a.obj.schema = &schema_named("ANPCCharacter");
  aoc::protocol::repl::RepMovement m;
  m.location = aoc::protocol::repl::quantize_location(a.obj.opts, x, y, z);
  m.rotation = aoc::protocol::repl::Rot{0, 64, 0};
  m.linear_velocity = aoc::protocol::repl::QuantVec{
      aoc::protocol::repl::QuantVec::Mode::Scaled, 0, 0, 0};
  a.obj.state.set(*a.obj.schema, "ReplicatedMovement",
                  aoc::protocol::repl::Value(m));
  a.obj.state.set(*a.obj.schema, "ReplicatedServerLastTransformUpdateTimeStamp",
                  aoc::protocol::repl::Value::f32(0.f));
  a.obj.state.set(
      *a.obj.schema, "CurrentMovementInput",
      aoc::protocol::repl::Value(aoc::protocol::repl::PlayerMovementInput{0}));
  a.open.spawn.location = aoc::protocol::repl::quantize_vector(10, x, y, z);
  a.open.spawn.rotation = aoc::protocol::repl::Rot{0, 11000, 0};
  w.npc.name = "test npc";
  return w;
}

inline void test_build_flat_and_region(const aoc::worldd::nav::NavFile &flat,
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

inline void test_build() {
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

inline void test_file_disk_corrupt(const std::string &path,
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

inline void test_file_disk(const aoc::worldd::nav::NavFile &reg,
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

inline void test_file_offset_bounds(const std::vector<uint8_t> &bytes) {
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

inline void test_file() {
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

inline void test_window_around(aoc::worldd::nav::AreaNav &n) {
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

inline void test_window() {
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
