// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
//
// test_worldd_mounts_fly_common.h: shared fixtures and helpers for
// test_worldd_mounts_fly.cpp
#pragma once

#include "capture_mount_fly.h"
#include "protocol/repl/move_packed.h"
#include "session/riding.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#ifndef MOUNT_CONTENT_DB_PATH
#define MOUNT_CONTENT_DB_PATH "content.db"
#endif
#ifndef MOUNT_SPEEDS_PATH
#define MOUNT_SPEEDS_PATH "mount_speeds.tsv"
#endif

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
inline const Clock::time_point t0{std::chrono::seconds(5000)};

inline Clock::time_point at_ms(double ms) {
  return t0 + std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<double, std::milli>(ms));
}

inline std::string real_db_path() {
  const char *env = std::getenv("AOC_CONTENT_DB");
  return env && *env ? env : MOUNT_CONTENT_DB_PATH;
}

struct Mounts {
  std::optional<aoc::worldd::content::ContentDb> db;
  std::optional<aoc::worldd::spawns::MountBook> book;
  Mounts() {
    std::string error;
    db = aoc::worldd::content::ContentDb::open(real_db_path(), error);
    if (!db) {
      std::printf("  [FAIL] the real content.db does not open (%s): %s\n",
                  real_db_path().c_str(), error.c_str());
      std::exit(2);
    }
    book = aoc::worldd::spawns::MountBook::load(*db, error);
    if (!book || !book->load_speeds(MOUNT_SPEEDS_PATH, error)) {
      std::printf("  [FAIL] the mounts do not load: %s\n", error.c_str());
      std::exit(2);
    }
  }
  const aoc::worldd::content::MountRow *
  by_blueprint_prefix(const std::string &prefix) const {
    for (const aoc::worldd::content::MountRow &r : book->data().mounts) {
      if (r.blueprint.rfind(prefix, 0) == 0) {
        return &r;
      }
    }
    return nullptr;
  }
  const aoc::worldd::content::MountRow *by_item(const std::string &name) const {
    for (const aoc::worldd::content::MountRow &r : book->data().mounts) {
      if (r.item_name == name) {
        return &r;
      }
    }
    return nullptr;
  }
};

struct Rig {
  aoc::worldd::Ride ride;
  explicit Rig(const Mounts &m, const aoc::worldd::content::MountRow &row)
      : ride(setup_of(m, row)) {
  }
  static aoc::worldd::Ride::Setup
  setup_of(const Mounts &m, const aoc::worldd::content::MountRow &row) {
    aoc::worldd::Ride::Setup s;
    s.row = &row;
    const aoc::worldd::spawns::MountBook::Speeds *sp =
        m.book->speeds_of(row.blueprint);
    s.run_speed = sp && sp->run > 0 ? sp->run : 600.0 * row.ground_pct / 100.0;
    aoc::worldd::Ride::fill_speeds(s, *m.book, row);
    return s;
  }
  aoc::worldd::MoveChecker checker(double x, double y, double z) const {
    aoc::worldd::GoodPosition start;
    start.x = x;
    start.y = y;
    start.z = z;
    return aoc::worldd::MoveChecker(ride.limits(aoc::worldd::MoveLimits{}, 0),
                                    start);
  }
};

inline aoc::protocol::repl::NetworkMove move_at(float ts,
                                                aoc::protocol::repl::Vec3d pos,
                                                uint8_t mode,
                                                double pitch_degrees = 0) {
  aoc::protocol::repl::NetworkMove m;
  m.time_stamp = ts;
  m.acceleration = aoc::protocol::repl::quantize_vector(10, 0, 0, 0);
  m.location = aoc::protocol::repl::quantize_vector(100, pos.x, pos.y, pos.z);
  m.control_rotation.pitch = static_cast<uint16_t>(
      static_cast<int>(std::lround(pitch_degrees * 65536.0 / 360.0)) & 0xFFFF);
  m.control_rotation.yaw = 0;
  m.movement_mode = mode;
  return m;
}

inline std::vector<uint8_t> bytes_of(const char *hex) {
  const auto nibble = [](char c) {
    return static_cast<uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
  };
  std::vector<uint8_t> out;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    out.push_back(
        static_cast<uint8_t>(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
  }
  return out;
}

inline double mult_at(const capture_mount_fly::Case &c, double ms) {
  const auto value_at = [&](double t) {
    double m = 1.0;
    for (size_t i = 0; i < c.speeds; ++i) {
      const capture_mount_fly::Speed &s =
          capture_mount_fly::kSpeeds[c.first_speed + i];
      if (s.ms <= t) {
        m = s.mult;
      }
    }
    return m;
  };
  double best = std::max(value_at(ms), value_at(ms - 1000.0));
  for (size_t i = 0; i < c.speeds; ++i) {
    const capture_mount_fly::Speed &s =
        capture_mount_fly::kSpeeds[c.first_speed + i];
    if (s.ms <= ms && s.ms >= ms - 1000.0) {
      best = std::max(best, static_cast<double>(s.mult));
    }
  }
  return best;
}

struct Replay {
  size_t moves = 0, accepted = 0, unreadable = 0;
  std::vector<int> refused_ms;
  std::map<int, aoc::worldd::MoveFault> faults;
  double fastest = 0;
  double fastest_dash = 0;
  size_t flight_moves = 0, mode_moves[8] = {0};
  size_t borderline = 0;
  double last_x = 0, last_y = 0, last_z = 0;
  float last_ts = -1;
  double off[3] = {0, 0, 0};
};

inline void record_accepted_move(Replay &out,
                                 const aoc::protocol::repl::NetworkMove &m,
                                 const aoc::worldd::MoveVerdict &v,
                                 double mult) {
  ++out.accepted;
  if (v.borderline) {
    ++out.borderline;
  }
  const aoc::protocol::repl::Vec3d now = m.location_cm();
  if (out.last_ts >= 0 && m.time_stamp - out.last_ts >= 0.03f &&
      m.time_stamp > out.last_ts) {
    const double dt = m.time_stamp - out.last_ts;
    const double dx = now.x - out.last_x;
    const double dy = now.y - out.last_y;
    const double dz = now.z - out.last_z;
    const double speed = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
    out.fastest = std::max(out.fastest, speed);
    if (mult > 1.0) {
      out.fastest_dash = std::max(out.fastest_dash, speed);
    }
  }
  out.last_x = now.x;
  out.last_y = now.y;
  out.last_z = now.z;
  out.last_ts = m.time_stamp;
}

inline void apply_move_verdict(Replay &out,
                               const aoc::protocol::repl::NetworkMove &m,
                               aoc::worldd::MoveChecker &checker, int ms,
                               double mult) {
  const aoc::worldd::MoveVerdict v = checker.check(m, at_ms(ms));
  ++out.moves;
  if (m.movement_mode < 8) {
    ++out.mode_moves[m.movement_mode];
  }
  if (v.accepted) {
    record_accepted_move(out, m, v, mult);
  } else if (v.fault == aoc::worldd::MoveFault::TimeStampNotRising) {
    ++out.moves;
  } else {
    out.refused_ms.push_back(ms);
    out.faults[ms] = v.fault;
    const aoc::worldd::GoodPosition &g = checker.good();
    const aoc::protocol::repl::Vec3d now = m.location_cm();
    out.off[0] += g.x - now.x;
    out.off[1] += g.y - now.y;
    out.off[2] += g.z - now.z;
  }
}

inline Replay replay(const capture_mount_fly::Case &c,
                     const aoc::worldd::Ride &ride, double extra_margin = 1.0) {
  (void)extra_margin;
  Replay out;
  aoc::worldd::MoveChecker checker(ride.limits(aoc::worldd::MoveLimits{}, 0),
                                   aoc::worldd::GoodPosition{});
  bool started = false;
  for (size_t i = 0; i < c.moves; ++i) {
    const capture_mount_fly::Move &mv =
        capture_mount_fly::kMoves[c.first_move + i];
    const capture_mount_fly::Payload &pl =
        capture_mount_fly::kPayloads[mv.payload];
    const auto data = bytes_of(pl.hex);
    const auto parsed = aoc::protocol::repl::read_move_packed(
        data.data(), data.size(), 0, pl.bits);
    if (!parsed) {
      ++out.unreadable;
      continue;
    }
    aoc::protocol::repl::NetworkMove m = parsed->new_move;
    const aoc::protocol::repl::Vec3d at = m.location_cm();
    m.location = aoc::protocol::repl::quantize_vector(
        100, at.x + out.off[0], at.y + out.off[1], at.z + out.off[2]);
    if (!started) {
      started = true;
      aoc::worldd::GoodPosition start{at.x, at.y, at.z};
      start.movement_mode = m.movement_mode;
      checker = aoc::worldd::MoveChecker(
          ride.limits(aoc::worldd::MoveLimits{}, 0), start);
    }
    const double mult = mult_at(c, mv.ms);
    checker.set_speed_mult(mult);
    checker.set_max_speed(ride.speed_limit(m.movement_mode, 0) * mult);
    apply_move_verdict(out, m, checker, mv.ms, mult);
  }
  return out;
}

inline const capture_mount_fly::Case &case_of(int capture) {
  for (const capture_mount_fly::Case &c : capture_mount_fly::kCases) {
    if (c.capture == capture) {
      return c;
    }
  }
  std::printf("  [FAIL] no capture %d in the header\n", capture);
  std::exit(2);
}

struct Flight {
  aoc::worldd::MoveChecker c;
  float ts = 10.0f;
  double x, y, z;
  Clock::time_point now = t0;
  size_t refused = 0, accepted = 0;
  aoc::worldd::MoveFault last_fault = aoc::worldd::MoveFault::None;
  aoc::worldd::MoveVerdict last_verdict;
  double x0, y0, z0, elapsed = 0, step_dt = 0.07;
  Flight(const Rig &rig, double ax, double ay, double az)
      : c(rig.checker(ax, ay, az)) {
    x = ax;
    y = ay;
    z = az;
    x0 = ax;
    y0 = ay;
    z0 = az;
  }
  double travelled() const {
    return std::sqrt((x - x0) * (x - x0) + (y - y0) * (y - y0) +
                     (z - z0) * (z - z0));
  }
  double average() const {
    return elapsed > 0 ? travelled() / elapsed : 0;
  }
  bool step(double dx, double dz, double dt, uint8_t mode, double pitch) {
    ts += static_cast<float>(dt);
    elapsed += dt;
    now += std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(dt));
    const aoc::protocol::repl::NetworkMove m =
        move_at(ts, {x + dx, y, z + dz}, mode, pitch);
    const aoc::worldd::MoveVerdict v = c.check(m, now);
    last_verdict = v;
    if (v.accepted) {
      x += dx;
      z += dz;
      ++accepted;
    } else {
      ++refused;
      last_fault = v.fault;
    }
    return v.accepted;
  }
  void run(double speed, double climb_degrees, double pitch, double seconds,
           uint8_t mode = aoc::protocol::repl::kMoveFlying) {
    const double a = climb_degrees * 3.14159265358979323846 / 180.0;
    for (double t = 0; t < seconds; t += step_dt) {
      step(speed * step_dt * std::cos(a), speed * step_dt * std::sin(a),
           step_dt, mode, pitch);
    }
  }
};

inline void test_goldens_dawn(const Mounts &m,
                              const aoc::worldd::content::MountRow &dawn) {
  Rig rig(m, dawn);
  CHECK(rig.ride.can_fly() &&
            std::fabs(rig.ride.speed_limit(5, 0) - 1290.0 * 1.5) < 1.0,
        "(the Dawnbreaker flies at 1,290)");
  const capture_mount_fly::Case &c = case_of(871);
  const Replay r = replay(c, rig.ride);
  std::printf("    871: %zu moves, %zu accepted, refused at ms:", r.moves,
              r.accepted);
  for (int ms : r.refused_ms) {
    std::printf(" %d", ms);
  }
  std::printf("; fastest %.0f cm/s, with the dash %.0f; modes 1/3/4/5: "
              "%zu/%zu/%zu/%zu, %zu beyond the plain law (logged only)\n",
              r.fastest, r.fastest_dash, r.mode_moves[1], r.mode_moves[3],
              r.mode_moves[4], r.mode_moves[5], r.borderline);
  CHECK(r.unreadable == 0 && r.moves >= c.moves,
        "all 4,771 moves of 871 read with the real parser");
  CHECK(r.mode_moves[1] == 5 && r.mode_moves[3] == 4 && r.mode_moves[5] == 4762,
        "the takeoff by ordinary moves: 5 walking, 4 falling (the jump), then "
        "4,762 in mode 5");
  const std::vector<int> glitches = {93994, 94103, 355893};
  CHECK(r.refused_ms == glitches,
        "every move passes except retail's own displaced samples (12,625 and "
        "6,506 cm/s at 88.8 s, the 5 km snap-back at 110.66 s)");
  CHECK(r.fastest > 3600.0 && r.fastest < 3800.0,
        "the dive to 3,671 cm/s is accepted (the fastest accepted step is the "
        "capture's)");
  CHECK(r.fastest_dash > 1700.0,
        "the dash in the air (1,290 x 1.35 = 1,741.5) is accepted");
}

inline void
test_goldens_drake_and_shoal(const Mounts &m,
                             const aoc::worldd::content::MountRow &drake,
                             const aoc::worldd::content::MountRow &shoal) {
  {
    Rig rig(m, drake);
    const Replay r = replay(case_of(883), rig.ride);
    std::printf("    883: %zu moves, %zu accepted, fastest %.0f cm/s, %zu "
                "beyond the plain law (logged only)\n",
                r.moves, r.accepted, r.fastest, r.borderline);
    CHECK(r.unreadable == 0 && r.moves == case_of(883).moves,
          "all 1,123 moves of 883 read");
    CHECK(r.refused_ms.empty(),
          "every move of the drake's flight passes, its steep dives included");
    CHECK(r.fastest > 5900.0,
          "the dive to 6,004 cm/s at 68 degrees down is accepted");
  }
  {
    Rig rig(m, shoal);
    const Replay r15 = replay(case_of(915), rig.ride);
    std::printf("    915: %zu moves, %zu accepted, modes 1/3/4: %zu/%zu/%zu, "
                "fastest %.0f cm/s%s",
                r15.moves, r15.accepted, r15.mode_moves[1], r15.mode_moves[3],
                r15.mode_moves[4], r15.fastest, "\n");
    CHECK(r15.unreadable == 0 && r15.refused_ms.empty() &&
              r15.mode_moves[1] == 44 && r15.mode_moves[4] == 189,
          "915: 600 cm/s on land (44 moves) and 920 in the water (189 moves in "
          "mode 4, from 7 s on) pass; entering the water dismounts nobody");
    const Replay r99 = replay(case_of(899), rig.ride);
    std::printf("    899: %zu moves, %zu accepted, modes 1/3/4: %zu/%zu/%zu, "
                "fastest %.0f cm/s%s",
                r99.moves, r99.accepted, r99.mode_moves[1], r99.mode_moves[3],
                r99.mode_moves[4], r99.fastest, "\n");
    CHECK(r99.unreadable == 0 && r99.refused_ms.empty() &&
              r99.mode_moves[4] == 418,
          "899: 418 moves in mode 4, 920 cm/s in 3D with the vertical share "
          "inside it, all pass");
    CHECK(r99.fastest > 915.0 && r99.fastest < 925.0,
          "(the fastest accepted step is the capture's 920)");
    const aoc::worldd::content::MountRow *bird =
        m.by_item("Mount_Grounded_TerrorBird_0");
    if (!bird) {
      bird = m.by_blueprint_prefix("Mount_Grounded_");
    }
    if (bird) {
      Rig g(m, *bird);
      const Replay bad = replay(case_of(899), g.ride);
      CHECK(bad.refused_ms.size() > 100,
            "the same swim on a ground mount (water speed 400, not 920) is "
            "refused again and again");
    }
  }
}

inline void test_goldens(const Mounts &m) {
  std::printf("the retail captures through the mount's move checker (871, 883, "
              "915, 899)\n");
  const aoc::worldd::content::MountRow *dawn =
      m.by_blueprint_prefix("Mount_Flying_Dawnbreaker");
  const aoc::worldd::content::MountRow *drake =
      m.by_blueprint_prefix("Mount_Flying_DrakeOfTheCanopy");
  const aoc::worldd::content::MountRow *shoal =
      m.by_blueprint_prefix("Mount_Aquatic_Shoalskipper");
  if (!dawn || !drake || !shoal) {
    CHECK(false,
          "(the Dawnbreaker, the drake and the Shoalskipper are in the book)");
    return;
  }
  test_goldens_dawn(m, *dawn);
  test_goldens_drake_and_shoal(m, *drake, *shoal);
}
