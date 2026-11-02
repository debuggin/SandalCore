// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "nav_fixture.h"
#include "test_worldd_gm_travel_common.h"

#include "aoc/worldadmin/v1/worldadmin.grpc.pb.h"

namespace adminv1 = ::aoc::worldadmin::v1;


static void test_teleport_refusals() {
  std::printf("teleport: the dead are not moved; the player list\n");
  Harness h(cyb_on_the_ground(), on_the_ground);
  h.play();
  GmTravel travel = make_travel(h);
  std::vector<GmPlayerInfo> players = travel.list_players();
  CHECK(players.size() == 1 && players[0].name == "cyb" && players[0].character_id == kCyb && !players[0].mounted && !players[0].dead && players[0].move_speed_multiplier == 1.0, "the player list: name, guid, not mounted, speed 1");
  CHECK(near_to(players[0].position.x, 0, 1) && near_to(players[0].position.z, 1098, 1), "(and its position)");
  h.s->set_current_value(content::kGuidHealth, 0.0f);
  h.run(milliseconds(300));
  CHECK(h.s->dead(), "(the player is dead)");
  const GoodPosition before = h.s->movement().good();
  const GmTeleportResult r = travel.teleport(to_point_z(5000, 3000, 1100), "the test");
  CHECK(r.status == GmStatus::FailedPrecondition && near_to(h.s->movement().good().x, before.x), "a dead player is not moved");
  players = travel.list_players();
  CHECK(players.size() == 1 && players[0].dead, "(the list says it is dead)");
}

static void check_teleport_mounted_after(Harness &h, GmTravel &travel, const GoodPosition &on_mount) {
  const size_t from = h.got.size();
  GmTeleportResult t = travel.teleport(to_point_z(on_mount.x + 20000, on_mount.y, on_mount.z), "the test");
  CHECK(t.status == GmStatus::Ok && t.dismounted && !h.s->mounted() && h.s->dismounts() == 1, "the player is dismounted first (the ride is over)");
  CHECK(near_to(h.s->movement().good().x, on_mount.x + 20000) && near_to(h.s->movement().good().z, on_mount.z), "and put at the target");
  h.run(milliseconds(300));
  CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "the client is sent the position correction");
  CHECK(h.s->mounts()->phase() == MountController::Phase::Out && h.s->distance_desummons() == 0, "the mount stays where it was (200 m away is within its distance)");
  const size_t refused = h.s->moves_refused();
  pawn_move_to(h, on_mount.x + 20060, on_mount.y, on_mount.z);
  CHECK(h.s->moves_refused() == refused, "(the pawn's next move from there is not corrected)");
  t = travel.teleport(to_point_z(on_mount.x + 80000, on_mount.y, on_mount.z), "the test");
  h.run(milliseconds(300));
  CHECK(t.status == GmStatus::Ok && !t.dismounted && h.s->distance_desummons() == 1 && h.s->mounts()->phase() == MountController::Phase::Desummoning, "800 m away the mount is desummoned by distance");
}

static void test_teleport_mounted() {
  std::printf("teleport while mounted: the ride is over first, the mount desummons by distance\n");
  Harness h(cyb_record(), with_nodes);
  h.play();
  GmTravel travel = make_travel(h);
  CHECK(ride_on(h), "(the player rides)");
  if (!h.s->mounted())
    return;
  Rider r = start_riding(h);
  for (int i = 0; i < 5; ++i)
    ride_step(h, r, 60, 0);
  CHECK(travel.list_players().size() == 1 && travel.list_players()[0].mounted, "(the list says it is mounted)");
  const GoodPosition on_mount = h.s->movement().good();
  check_teleport_mounted_after(h, travel, on_mount);
}

static void test_speed_on_foot_multiplier(Harness &h, GmTravel &travel, double base_limit) {
  size_t refused = h.s->moves_refused();
  pawn_move_to(h, h.s->movement().good().x + 600, 0, 1098);
  CHECK(h.s->moves_refused() == refused + 1, "(without the speed, 3,000 cm/s is refused)");
  const size_t from = h.got.size();
  GmSpeedResult s = travel.set_speed("cyb", 5, "the test");
  CHECK(s.status == GmStatus::Ok && s.character == "cyb" && s.previous == 1.0 && s.multiplier == 5.0 && h.s->gm_speed() == 5.0, "the multiplier is set: x5 (was x1)");
  const AbilityRuntime *ab = h.s->abilities();
  CHECK(ab && near_to(ab->stat(kMoveSpeedMult), 5.0, 1e-4), "the pawn's MoveSpeedMult stat is 5 (the stat the Sprint's effect raises)");
  h.run(milliseconds(100));
  size_t pawn_chains = 0;
  for (const fake::Delivered *d : chains_on(h, h.pawn_ch, from)) {
    pawn_chains += d ? 1 : 0;
  }
  CHECK(pawn_chains >= 1, "(the change went out on the pawn's channel)");
  CHECK(near_to(h.s->movement().limits().max_speed, base_limit * 5, 1e-6), "the move checker's horizontal limit is five times larger");
  CHECK(near_to(h.s->movement().limits().dash_speed, 3355.0 * 5, 1e-6), "(and the dash's)");
  refused = h.s->moves_refused();
  const size_t acked = h.s->moves_acknowledged();
  for (int i = 0; i < 4; ++i) {
    pawn_move_to(h, h.s->movement().good().x + 600, 0, 1098);
  }
  CHECK(h.s->moves_refused() == refused && h.s->moves_acknowledged() == acked + 4, "the checker accepts moves at the multiplied speed (3,000 cm/s)");
  pawn_move_to(h, h.s->movement().good().x + 3000, 0, 1098);
  CHECK(h.s->moves_refused() == refused + 1, "a move beyond the multiplied limit (15,000 cm/s) is still refused");
}

static void test_speed_on_foot_sprint_and_bounds(Harness &h, GmTravel &travel, double base_limit) {
  const AbilityRuntime *ab = h.s->abilities();
  const bs::SessionGuids &g = h.s->bootstrap().setup().guids;
  h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, content::kGuidSprint, 31), true)});
  h.run(milliseconds(100));
  CHECK(ab && ab->sprinting() && ab->stat(kMoveSpeedMult) > 5.1 && ab->stat(kMoveSpeedMult) < 5.5, "a Sprint adds its own 0.2 on top of the GM's base");
  GmSpeedResult s = travel.set_speed("cyb", 1, "the test");
  CHECK(s.status == GmStatus::Ok && s.previous == 5.0 && s.multiplier == 1.0 && h.s->gm_speed() == 1.0, "x1 resets");
  CHECK(ab && ab->sprinting() && ab->stat(kMoveSpeedMult) > 1.1 && ab->stat(kMoveSpeedMult) < 1.5, "(the Sprint's own raise stays: 1.2)");
  CHECK(near_to(h.s->movement().limits().max_speed, base_limit * 5, 1e-6), "(a second's grace: the limit is still the old one)");
  h.run(milliseconds(1500));
  pawn_move_to(h, h.s->movement().good().x + 60, 0, 1098);
  CHECK(near_to(h.s->movement().limits().max_speed, base_limit, 1e-6), "after the grace the limits are the default again");
  CHECK(travel.set_speed("cyb", 0.4, "the test").status == GmStatus::InvalidArgument, "0.4 is refused");
  CHECK(travel.set_speed("cyb", 10.5, "the test").status == GmStatus::InvalidArgument, "10.5 is refused");
  CHECK(travel.set_speed("cyb", std::numeric_limits<double>::quiet_NaN(), "the test").status == GmStatus::InvalidArgument, "NaN is refused");
  CHECK(travel.set_speed("nobody", 2, "the test").status == GmStatus::NotFound, "an unknown character is NotFound");
  CHECK(travel.set_speed("cyb", 0.5, "the test").status == GmStatus::Ok && travel.set_speed("cyb", 10, "the test").status == GmStatus::Ok, "0.5 and 10 are the bounds");
  travel.set_speed("cyb", 0.5, "the test");
  h.run(milliseconds(1500));
  pawn_move_to(h, h.s->movement().good().x + 60, 0, 1098);
  CHECK(near_to(h.s->movement().limits().max_speed, base_limit, 1e-6) && near_to(h.s->movement().limits().dash_speed, 3355.0, 1e-6), "x0.5 never lowers the checker's limits");
  CHECK(ab && near_to(ab->stat(kMoveSpeedMult), 0.5 + 0.0, 0.31), "(the pawn's stat is 0.5 plus the Sprint's)");
}

static void test_speed_on_foot() {
  std::printf("run speed on foot: the pawn's MoveSpeedMult, the checker's limits, the reset\n");
  Harness h(cyb_on_the_ground(), on_the_ground);
  h.play();
  GmTravel travel = make_travel(h);
  walk_pawn_to(h, 300, 0);
  const double base_limit = h.s->movement().limits().max_speed;
  test_speed_on_foot_multiplier(h, travel, base_limit);
  test_speed_on_foot_sprint_and_bounds(h, travel, base_limit);
}

static void resync_rider(const Harness &h, Rider &r) {
  const GoodPosition g = h.s->movement().good();
  r.x = g.x;
  r.y = g.y;
  r.z = g.z;
}

static void test_speed_mounted_ride(Harness &h, GmTravel &travel, Rider &r, double limit_before) {
  size_t refused = h.s->moves_refused();
  ride_step(h, r, 250, 0);
  CHECK(h.s->moves_refused() == refused + 1, "(without the speed, 2,500 cm/s on the mount is refused)");
  resync_rider(h, r);
  const GmSpeedResult s = travel.set_speed("cyb", 3, "the test");
  CHECK(s.status == GmStatus::Ok, "(the speed is set while riding)");
  CHECK(h.mount() && near_to(mult_of(*h.mount()).value_or(0), 3.0, 1e-6) && h.s->ride()->speed_multiplier() == 3.0, "the mount's MoveSpeedMult is 3 (the stat the road's and the dash's effects raise)");
  CHECK(near_to(h.s->ride()->speed_limit(1, 1.0e6), limit_before * 3, 1e-6), "the mount's move limit is three times larger");
  CHECK(h.s->abilities() && near_to(h.s->abilities()->stat(kMoveSpeedMult), 3.0, 1e-4), "(the rider's own stat is 3 too: it is the same when it gets off)");
  refused = h.s->moves_refused();
  const size_t acked = h.s->mount_moves_acknowledged();
  for (int i = 0; i < 4; ++i)
    ride_step(h, r, 250, 0);
  CHECK(h.s->moves_refused() == refused && h.s->mount_moves_acknowledged() == acked + 4, "the checker accepts the mount at the multiplied speed (2,500 cm/s)");
  ride_step(h, r, 700, 0);
  CHECK(h.s->moves_refused() == refused + 1, "a move beyond the multiplied limit is still refused");
  resync_rider(h, r);
}

static void test_speed_mounted_reset_and_dismount(Harness &h, GmTravel &travel, Rider &r) {
  travel.set_speed("cyb", 1, "the test");
  CHECK(near_to(mult_of(*h.mount()).value_or(0), 1.0, 1e-6), "x1: the mount's MoveSpeedMult is 1 again");
  resync_rider(h, r);
  const size_t refused = h.s->moves_refused();
  ride_step(h, r, 250, 0);
  CHECK(h.s->moves_refused() == refused, "(within the grace second the old limit holds)");
  for (int i = 0; i < 15; ++i)
    ride_step(h, r, 0, 0);
  ride_step(h, r, 250, 0);
  CHECK(h.s->moves_refused() == refused + 1, "after it 2,500 cm/s is refused again");
  resync_rider(h, r);
  travel.set_speed("cyb", 3, "the test");
  get_off(h);
  h.run(milliseconds(300));
  CHECK(!h.s->mounted() && h.s->abilities() && near_to(h.s->abilities()->stat(kMoveSpeedMult), 3.0, 1e-4), "dismounted, the pawn's MoveSpeedMult is still 3");
  pawn_move_to(h, h.s->movement().good().x + 10, h.s->movement().good().y, h.s->movement().good().z);
  CHECK(h.s->movement().limits().max_speed > 2135.0 * 2.9, "(and the pawn's limits are scaled)");
}

static void test_speed_mounted() {
  std::printf("run speed on a mount: the mount's MoveSpeedMult, the mount's limits, survives the dismount\n");
  Harness h(cyb_record());
  h.play();
  GmTravel travel = make_travel(h);
  CHECK(ride_on(h), "(the player rides)");
  if (!h.s->mounted())
    return;
  Rider r = start_riding(h);
  for (int i = 0; i < 6; ++i)
    ride_step(h, r, 60, 0);
  const double limit_before = h.s->ride()->speed_limit(1, 1.0e6);
  test_speed_mounted_ride(h, travel, r, limit_before);
  test_speed_mounted_reset_and_dismount(h, travel, r);
}

static void test_speed_before_mounting() {
  std::printf("run speed set on foot: the mount starts at it; a relog starts at 1\n");
  {
    Harness h(cyb_record());
    h.play();
    GmTravel travel = make_travel(h);
    travel.set_speed("cyb", 2, "the test");
    CHECK(ride_on(h), "(the player rides)");
    CHECK(h.mount() && near_to(mult_of(*h.mount()).value_or(0), 2.0, 1e-6), "a mount ridden after the speed was set starts at MoveSpeedMult 2");
  }
  {
    Harness h(cyb_record());
    h.play();
    CHECK(h.s->gm_speed() == 1.0 && (!h.s->abilities() || near_to(h.s->abilities()->stat(kMoveSpeedMult), 1.0, 1e-6)), "a new session (a relog) starts at x1");
  }
}

static BunchWriter gm_rpc(const char *name, const std::function<void(const schema::RpcSchema &, repl::RpcParams &)> &fill) {
  const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
  const schema::RpcSchema *rpc = pc.rpc(name);
  if (!rpc)
    return BunchWriter();
  repl::RpcParams p;
  fill(*rpc, p);
  return actor_rpc(pc, name, p);
}

static void send_xyz(Harness &h, int x, int y, int z) {
  h.client_sends({h.client.actor(h.pc_ch,
                                 gm_rpc("ServerTeleportXYZ",
                                        [&](const schema::RpcSchema &rpc, repl::RpcParams &p) {
                                          repl::set_rpc_param(rpc, p, "X", repl::Value::i64(x));
                                          repl::set_rpc_param(rpc, p, "Y", repl::Value::i64(y));
                                          repl::set_rpc_param(rpc, p, "Z", repl::Value::i64(z));
                                          repl::set_rpc_param(rpc, p, "OptionalRadius", repl::Value::f32(0));
                                        }),
                                 true)});
  h.run(milliseconds(300));
}

static void send_key(Harness &h, const char *key) {
  h.client_sends({h.client.actor(h.pc_ch,
                                 gm_rpc("ServerTeleport",
                                        [&](const schema::RpcSchema &rpc, repl::RpcParams &p) {
                                          repl::set_rpc_param(rpc, p, "DestinationLookupKey", repl::Value::str(key));
                                          repl::set_rpc_param(rpc, p, "OptionalRadius", repl::Value::f32(0));
                                        }),
                                 true)});
  h.run(milliseconds(300));
}

static void send_player(Harness &h, const char *names, double x, double y, double z) {
  h.client_sends({h.client.actor(h.pc_ch,
                                 gm_rpc("ServerTeleportPlayer",
                                        [&](const schema::RpcSchema &rpc, repl::RpcParams &p) {
                                          repl::set_rpc_param(rpc, p, "CharacterNames", repl::Value::str(names));
                                          repl::set_rpc_param(rpc, p, "X", repl::Value::f64(x));
                                          repl::set_rpc_param(rpc, p, "Y", repl::Value::f64(y));
                                          repl::set_rpc_param(rpc, p, "Z", repl::Value::f64(z));
                                        }),
                                 true)});
  h.run(milliseconds(300));
}

static void test_client_rpcs_switch_on(Harness &h) {
  h.fx.world->set_gm_commands(true);
  send_xyz(h, 5000, 3000, 1500);
  CHECK(h.s->gm_rpcs_answered() == 1 && near_to(h.s->movement().good().x, 5000) && near_to(h.s->movement().good().y, 3000) && near_to(h.s->movement().good().z, 1500),
        "ServerTeleportXYZ with the switch on: the player is at (5000, 3000, 1500)");
  const world::CityNodeInfo *joeva = g_nodes->by_name("Joeva");
  send_key(h, "Verra_RVR_Joeva");
  CHECK(joeva && h.s->gm_rpcs_answered() == 2 && near_to(h.s->movement().good().x, joeva->x, 0.01) && near_to(h.s->movement().good().y, joeva->y, 0.01), "ServerTeleport 'Verra_RVR_Joeva': the player is at the node");
  send_key(h, "Atlantis");
  CHECK(h.s->gm_rpcs_answered() == 2 && near_to(h.s->movement().good().x, joeva ? joeva->x : 0, 0.01), "ServerTeleport with a key worldd does not know moves no one");
  send_player(h, "nobody", 100, 100, 1500);
  CHECK(h.s->gm_rpcs_answered() == 2, "ServerTeleportPlayer naming no one in the world moves no one");
  send_player(h, "nobody, cyb", 7000, 2000, 1500);
  CHECK(h.s->gm_rpcs_answered() == 3 && near_to(h.s->movement().good().x, 7000) && near_to(h.s->movement().good().y, 2000), "ServerTeleportPlayer 'nobody, cyb' moves cyb");
  CHECK(h.s->gm_rpcs_received() == 8, "(every RPC was received and counted)");
  h.fx.world->set_gm_commands(false);
  send_xyz(h, 100, 100, 1500);
  CHECK(near_to(h.s->movement().good().x, 7000), "switched off again, the next one is refused");
}

static void test_client_rpcs() {
  std::printf("the client's own GM RPCs: ServerTeleportXYZ, ServerTeleport, ServerTeleportPlayer, behind the switch\n");
  Harness h(cyb_on_the_ground(), on_the_ground);
  h.play();
  GmTravel travel = make_travel(h);
  h.fx.world->set_gm_port(&travel);
  const GoodPosition start = h.s->movement().good();
  CHECK(!h.fx.world->gm_commands(), "(the GM switch is off by default)");
  send_xyz(h, 5000, 3000, 1500);
  send_key(h, "Verra_RVR_Joeva");
  send_player(h, "cyb", 7000, 2000, 1500);
  CHECK(h.s->gm_rpcs_received() == 3, "with the switch off the three RPCs are received (and logged) ...");
  CHECK(h.s->gm_rpcs_answered() == 0 && h.s->gm_teleports() == 0 && near_to(h.s->movement().good().x, start.x) && near_to(h.s->movement().good().y, start.y), "... and none moves the player");
  test_client_rpcs_switch_on(h);
}

static size_t lines_that_begin(const std::string &log_str, const std::string &begins) {
  std::istringstream in(log_str);
  std::string line;
  size_t n = 0;
  while (std::getline(in, line)) {
    n += line.rfind(begins, 0) == 0 ? 1 : 0;
  }
  return n;
}

static bool has_line_with_both(const std::string &log_str, const std::string &a, const std::string &b) {
  std::istringstream in(log_str);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) {
      return true;
    }
  }
  return false;
}

static void test_client_text_in_the_log() {
  std::printf("the client's text is cleaned before it reaches the log, with the GM switch off and on\n");
  std::ostringstream log;
  auto previous = spdlog::default_logger();
  auto logger = std::make_shared<spdlog::logger>("gm_text", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
  logger->set_level(spdlog::level::info);
  spdlog::set_default_logger(logger);
  {
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    GmTravel travel = make_travel(h);
    h.fx.world->set_gm_port(&travel);
    const char *forged = "Atlantis\n[2026-10-10 20:00:00.000] [gm_text] [info] [gm] nobody: a line the player made";
    log.str("");
    send_key(h, forged);
    send_player(h, forged, 0, 0, 0);
    CHECK(lines_that_begin(log.str(), "[2026-10-10 20:00:00.000]") == 0, "switch off: no line of the player's making reaches the log");
    CHECK(has_line_with_both(log.str(), "the client sent ServerTeleport (", "a line the player made") && has_line_with_both(log.str(), "the client sent ServerTeleportPlayer (", "a line the player made"),
          "... the text is there, on the one line of the RPC");
    h.fx.world->set_gm_commands(true);
    log.str("");
    send_key(h, forged);
    send_player(h, forged, 0, 0, 0);
    CHECK(lines_that_begin(log.str(), "[2026-10-10 20:00:00.000]") == 0, "switch on: the warnings that name the key and the names are clean too");
    CHECK(has_line_with_both(log.str(), "worldd knows no such place", "a line the player made") && has_line_with_both(log.str(), "moved no one", "a line the player made"), "... and still name them");
  }
  spdlog::set_default_logger(previous);
}

static const std::string kToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct Admin {
    std::unique_ptr<svc::AdminQueue> queue = std::make_unique<svc::AdminQueue>();
    std::unique_ptr<GmTravel> travel;
    std::unique_ptr<svc::WorldAdminServer> server;
    std::unique_ptr<adminv1::WorldAdminService::Stub> stub;
    std::thread::id net_thread = std::this_thread::get_id();
    std::atomic<bool> directory_off_thread{false};

    explicit Admin(Harness &h, std::chrono::milliseconds timeout = std::chrono::milliseconds(4000)) {
        travel = std::make_unique<GmTravel>(
            *h.fx.world,
            [this, &h] {
                if (std::this_thread::get_id() != net_thread) {
                    directory_off_thread = true;
                }
                return std::vector<PlayerHandle>{PlayerHandle{h.s.get(), &h.conn}};
            },
            [&h] { return h.t; });
        svc::WorldAdminServer::Options o;
        o.address = "127.0.0.1:0";
        o.token = kToken;
        o.call_timeout = timeout;
        server = std::make_unique<svc::WorldAdminServer>(*travel, *queue, o);
        std::string error;
        if (!server->start(&error)) {
            std::printf("  [FAIL] the admin server does not start: %s\n", error.c_str());
            std::exit(2);
        }
        stub = adminv1::WorldAdminService::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(server->port()), grpc::InsecureChannelCredentials()));
    }

    template <class F> grpc::Status via_net_thread(F &&call) {
        std::atomic<bool> done{false};
        grpc::Status status;
        std::thread t([&] {
            status = call();
            done = true;
        });
        while (!done) {
            queue->drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        t.join();
        return status;
    }

    static void authorize(grpc::ClientContext &ctx, const std::string &token) { ctx.AddMetadata("authorization", "Bearer " + token); }
};

static void test_admin_api_auth_refusals(Harness &h, Admin &a, const GoodPosition &start) {
    grpc::ClientContext ctx;
    adminv1::TeleportRequest rq;
    rq.set_character("cyb");
    rq.mutable_point()->set_x(5000);
    rq.mutable_point()->set_y(3000);
    rq.mutable_point()->set_z(1500);
    rq.mutable_point()->set_has_z(true);
    adminv1::TeleportResponse rs;
    const grpc::Status s = a.via_net_thread([&] { return a.stub->Teleport(&ctx, rq, &rs); });
    CHECK(s.error_code() == grpc::StatusCode::UNAUTHENTICATED, "a call with no token is refused (Unauthenticated)");
    grpc::ClientContext wrong;
    Admin::authorize(wrong, std::string(64, 'f'));
    const grpc::Status s2 = a.via_net_thread([&] { return a.stub->Teleport(&wrong, rq, &rs); });
    CHECK(s2.error_code() == grpc::StatusCode::UNAUTHENTICATED, "a call with a wrong token is refused");
    grpc::ClientContext bare;
    bare.AddMetadata("authorization", "Bearer ");
    const grpc::Status s3 = a.via_net_thread([&] { return a.stub->Teleport(&bare, rq, &rs); });
    CHECK(s3.error_code() == grpc::StatusCode::UNAUTHENTICATED, "a bare 'Bearer ' is refused");
    grpc::ClientContext raw;
    raw.AddMetadata("authorization", kToken);
    const grpc::Status s4 = a.via_net_thread([&] { return a.stub->Teleport(&raw, rq, &rs); });
    CHECK(s4.error_code() == grpc::StatusCode::UNAUTHENTICATED, "the token without 'Bearer ' is refused");
    CHECK(a.server->refused() == 4 && near_to(h.s->movement().good().x, start.x) && h.s->gm_teleports() == 0, "four refusals counted, the player did not move");
    CHECK(s.error_message().find(kToken) == std::string::npos && s2.error_message().find(kToken) == std::string::npos, "(the token is not in the answer)");
}

static void test_admin_api_list_and_teleport(Harness &h, Admin &a) {
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::ListPlayersResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->ListPlayers(&ctx, adminv1::ListPlayersRequest(), &rs); });
        CHECK(s.ok() && rs.players_size() == 1 && rs.players(0).name() == "cyb" && rs.players(0).character_id() == kCyb && !rs.players(0).mounted(), "ListPlayers: name, guid, position, mounted");
        CHECK(!a.directory_off_thread, "the session list was read on the network thread, not the gRPC thread");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::ListPlacesResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->ListPlaces(&ctx, adminv1::ListPlacesRequest(), &rs); });
        CHECK(s.ok() && static_cast<size_t>(rs.places_size()) == a.travel->list_places().size() && rs.places(0).name() == "login", "ListPlaces: the login spot and the nodes");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::TeleportRequest rq;
        rq.set_character("cyb");
        rq.set_place("joeva");
        adminv1::TeleportResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->Teleport(&ctx, rq, &rs); });
        const world::CityNodeInfo *joeva = g_nodes->by_name("Joeva");
        CHECK(s.ok() && rs.place() == "Verra_RVR_Joeva" && joeva && near_to(rs.to().x(), joeva->x, 0.01) && near_to(h.s->movement().good().x, joeva->x, 0.01), "Teleport to a place: applied");
        CHECK(!a.directory_off_thread, "(and applied on the network thread)");
    }
}

static void test_admin_api_errors_and_speed(Harness &h, Admin &a) {
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::TeleportRequest rq;
        rq.set_character("cyb");
        rq.mutable_point()->set_x(100000);
        rq.mutable_point()->set_y(100000);
        adminv1::TeleportResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->Teleport(&ctx, rq, &rs); });
        CHECK(s.error_code() == grpc::StatusCode::INVALID_ARGUMENT && s.error_message().find("give z") != std::string::npos, "a point with no ground and no z: InvalidArgument, asking for z");
        grpc::ClientContext ctx2;
        Admin::authorize(ctx2, kToken);
        adminv1::TeleportRequest none;
        none.set_character("cyb");
        const grpc::Status s2 = a.via_net_thread([&] { return a.stub->Teleport(&ctx2, none, &rs); });
        CHECK(s2.error_code() == grpc::StatusCode::INVALID_ARGUMENT, "no target: InvalidArgument");
        grpc::ClientContext ctx3;
        Admin::authorize(ctx3, kToken);
        none.set_character("nobody");
        none.set_place("joeva");
        const grpc::Status s3 = a.via_net_thread([&] { return a.stub->Teleport(&ctx3, none, &rs); });
        CHECK(s3.error_code() == grpc::StatusCode::NOT_FOUND, "an unknown character: NotFound");
        grpc::ClientContext ctx4;
        Admin::authorize(ctx4, kToken);
        none.set_character("cyb");
        none.set_place("Narnia");
        const grpc::Status s4 = a.via_net_thread([&] { return a.stub->Teleport(&ctx4, none, &rs); });
        CHECK(s4.error_code() == grpc::StatusCode::NOT_FOUND, "an unknown place: NotFound");
    }
    {
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::SetMoveSpeedRequest rq;
        rq.set_character("cyb");
        rq.set_multiplier(4);
        adminv1::SetMoveSpeedResponse rs;
        const grpc::Status s = a.via_net_thread([&] { return a.stub->SetMoveSpeed(&ctx, rq, &rs); });
        CHECK(s.ok() && rs.multiplier() == 4 && rs.previous() == 1 && h.s->gm_speed() == 4.0, "SetMoveSpeed: applied");
        grpc::ClientContext ctx2;
        Admin::authorize(ctx2, kToken);
        rq.set_multiplier(11);
        const grpc::Status s2 = a.via_net_thread([&] { return a.stub->SetMoveSpeed(&ctx2, rq, &rs); });
        CHECK(s2.error_code() == grpc::StatusCode::INVALID_ARGUMENT && h.s->gm_speed() == 4.0, "a multiplier of 11: InvalidArgument, nothing changed");
    }
    CHECK(!a.directory_off_thread, "(no call touched the sessions off the network thread)");
}

static void test_admin_api() {
    std::printf("the admin API: the service token, the queue, the network thread\n");
    Harness h(cyb_on_the_ground(), on_the_ground);
    h.play();
    Admin a(h);
    const GoodPosition start = h.s->movement().good();
    test_admin_api_auth_refusals(h, a, start);
    test_admin_api_list_and_teleport(h, a);
    test_admin_api_errors_and_speed(h, a);
}

static void test_admin_queue() {
    std::printf("the admin queue: a call the network thread does not take is not applied\n");
    {
        Harness h(cyb_on_the_ground(), on_the_ground);
        h.play();
        Admin a(h, std::chrono::milliseconds(200));
        const GoodPosition start = h.s->movement().good();
        grpc::ClientContext ctx;
        Admin::authorize(ctx, kToken);
        adminv1::TeleportRequest rq;
        rq.set_character("cyb");
        rq.mutable_point()->set_x(5000);
        rq.mutable_point()->set_y(3000);
        rq.mutable_point()->set_z(1500);
        rq.mutable_point()->set_has_z(true);
        adminv1::TeleportResponse rs;
        const grpc::Status s = a.stub->Teleport(&ctx, rq, &rs);
        CHECK(s.error_code() == grpc::StatusCode::UNAVAILABLE, "a call the network thread does not take in time ends with Unavailable");
        CHECK(a.queue->abandoned() == 1, "(it is counted as given up)");
        CHECK(a.queue->drain() == 0 && near_to(h.s->movement().good().x, start.x) && h.s->gm_teleports() == 0, "and it is never applied, even when the network thread comes round later");
        a.server->stop();
        CHECK(!a.queue->run([] {}, std::chrono::milliseconds(50)), "(a stopped server's queue takes nothing)");
    }
    {
        svc::AdminQueue q;
        int ran = 0;
        std::atomic<bool> in_task{false};
        std::thread t([&] {
            q.run(
                [&] {
                    in_task = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    ++ran;
                },
                std::chrono::milliseconds(100));
        });
        while (!in_task) {
            q.drain();
        }
        t.join();
        CHECK(ran == 1, "a task the network thread has started is waited for, whatever the timeout");
        q.run([] {}, std::chrono::milliseconds(1));
        std::thread t2([&] { q.run([&] { throw std::runtime_error("boom"); }, std::chrono::milliseconds(2000)); });
        size_t drained = 0;
        for (int i = 0; i < 2000 && drained == 0; ++i) {
            drained = q.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        t2.join();
        CHECK(drained == 1, "a task that throws is answered, not left to hang its caller");
    }
}

int main() {
  if (!init_gm_travel_globals()) {
    return 2;
  }
  test_teleport_refusals();
  test_teleport_mounted();
  test_speed_on_foot();
  test_speed_mounted();
  test_speed_before_mounting();
  test_client_rpcs();
  test_client_text_in_the_log();
  test_admin_api();
  test_admin_queue();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
