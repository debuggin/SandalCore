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
  test_npcs();
  test_edge();
  test_real();
  std::printf("test_worldd_nav_window: %d passed, %d failed (%.1f s)\n", g_pass, g_fail, ms_since(t0) / 1000.0);
  return g_fail == 0 ? 0 : 1;
}
