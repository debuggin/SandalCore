// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/schema/generated/index.h"
#include "session/position_saver.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

inline std::ostringstream g_log;
inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const aoc::worldd::Clock::time_point t0{std::chrono::seconds(5000)};
inline const aoc::worldd::Address kA{0x7F000001u, 50510};
inline const aoc::worldd::Address kB{0x7F000001u, 50511};
inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;

inline constexpr double kX = -777664.5, kY = 616578.6875, kZ = 15952.0498046875;
inline constexpr uint16_t kSpawnYaw = 16550;

inline const aoc::protocol::schema::ClassSchema& class_named(const char* name) {
    const aoc::protocol::schema::ClassSchema* c = aoc::protocol::schema::gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

struct Fixture {
    std::filesystem::path dir;
    std::optional<aoc::worldd::content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<aoc::worldd::PlayerWorld> world;

    Fixture() {
        dir = content_fixture::make_temp_dir("worldd_movement_");
        const std::filesystem::path db = dir / "content.db";
        if (!content_fixture::make_db(db)) std::exit(2);
        std::string error;
        content = aoc::worldd::content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        world = std::make_unique<aoc::worldd::PlayerWorld>(
            &*content, "", aoc::worldd::bootstrap::GameStateMode::Tier2, kServerId, guids);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

inline aoc::worldd::svc::Character cyb_record() {
    aoc::worldd::svc::Character c;
    c.id = kCyb;
    c.account_id = 3;
    c.name = "cyb";
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    return c;
}

inline aoc::protocol::repl::NetworkMove move_at(
    float ts, double x, double y, double z, uint16_t yaw = kSpawnYaw) {
    aoc::protocol::repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = aoc::protocol::repl::quantize_vector(10, 0, 0, 0);
    m.location = aoc::protocol::repl::quantize_vector(100, x, y, z);
    m.control_rotation.yaw = yaw;
    m.movement_mode = aoc::protocol::repl::kMoveWalking;
    return m;
}

inline aoc::protocol::emit::BunchWriter client_move(const aoc::protocol::repl::MovePacked& moves) {
    const aoc::protocol::schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const aoc::protocol::schema::NetField* f = cls.field("ServerMovePacked");
    aoc::protocol::emit::BunchWriter out;
    if (!f) return out;
    aoc::protocol::emit::BunchWriter params;
    if (!aoc::protocol::repl::write_move_packed(params, moves)) return out;
    aoc::protocol::emit::BunchWriter field;
    if (aoc::protocol::repl::write_field(cls, f->index, params, field) == 0) return out;
    aoc::protocol::repl::BlockHeader header;
    aoc::protocol::repl::write_content_block(out, header, field, false);
    return out;
}

inline aoc::protocol::emit::BunchWriter client_move(const aoc::protocol::repl::NetworkMove& m) {
    aoc::protocol::repl::MovePacked p;
    p.new_move = m;
    return client_move(p);
}

inline bool near(double a, double b) { return std::fabs(a - b) < 0.01; }

void run_movement_saver();
