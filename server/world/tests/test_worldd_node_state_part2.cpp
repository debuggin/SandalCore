// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_node_state_common.h"

#include "aoc/citynode/v1/citynode.grpc.pb.h"
#include "svc/city_node_control.h"
#include "svc/service_token.h"

#include <grpcpp/grpcpp.h>

#include <mutex>

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace nodev1 = aoc::citynode::v1;
using std::chrono::milliseconds;

static void check_later_join_after_level_change(Harness &h) {
  Connection conn2{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
  std::string join_error;
  auto later = PlayerSession::join(conn2, *h.fx.world, h.record, identity(), world_time_at(h.t), join_error);
  CHECK(later && later->bootstrap().setup().nodes == g_wire->maps(*h.fx.nodes) && later->node_maps_version() == h.fx.nodes->version(), "a session that joins later has the registry's levels as of now in its open");
  const repl::NodeStateMaps now = g_wire->maps(*h.fx.nodes);
  size_t joeva_level = 9, halcyon_level = 9;
  for (const repl::NodeDataEntry &n : now.nodes) {
    if (n.guid == kJoeva)
      joeva_level = n.level;
    if (n.guid == kHalcyon)
      halcyon_level = n.level;
  }
  CHECK(joeva_level == 3 && halcyon_level == 0 && now.percent.size() == 1 && now.citizens.size() == 1, "(the maps: Joeva a Village, Halcyon Wilderness, one node in each small map)");
}

static void test_level_change() {
  std::printf("a level change reaches every client as a delta of the maps; a client that joins later has the new levels\n");
  Harness h(cyb_record());
  h.play();
  const schema::ClassSchema &gs_schema = class_named("AoCGameStateBP_C");
  const uint64_t before_version = h.s->node_maps_version();
  const size_t from = h.got.size();
  world::LiveNode joeva;
  joeva.guid = kJoeva;
  joeva.level = 3;
  CHECK(h.fx.nodes->apply_change(joeva), "(the registry takes the change)");
  h.run(milliseconds(300));
  CHECK(h.s->node_maps_version() != before_version && h.s->node_map_updates() == 1, "the session saw the registry change and sent one update");
  std::vector<repl::NodeStateMaps> deltas;
  for (const fake::Delivered *d : chains_on(h, h.gs_ch, from, false)) {
    const States st = states_of(*d, h.s->bootstrap().game_state());
    const auto it = st.find(0);
    if (it != st.end())
      deltas.push_back(repl::read_node_maps(gs_schema, it->second.arrays));
  }
  const bool delta_ok = deltas.size() == 1 && deltas[0].nodes.size() == 1 && deltas[0].nodes[0].guid == kJoeva && deltas[0].nodes[0].level == 3 && deltas[0].estates.empty();
  CHECK(delta_ok, "the client gets Joeva's node data at level 3 and nothing else of the 244 estates and 46 nodes");
  world::LiveNode halcyon;
  halcyon.guid = kHalcyon;
  halcyon.level = 0;
  h.fx.nodes->apply_change(halcyon);
  const size_t from2 = h.got.size();
  h.run(milliseconds(300));
  CHECK(h.s->node_map_updates() == 2 && !chains_on(h, h.gs_ch, from2, false).empty(), "a second change is a second update");
  check_later_join_after_level_change(h);
}

static void test_restore() {
  std::printf("a node update the connection does not take is built again, identically\n");
  Harness h(cyb_record());
  h.settle();
  bs::PlayerBootstrap pb(h.s->bootstrap().setup(), bs::GameStateMode::Tier3);
  const auto join = pb.join_sequence();
  CHECK(pb.ok() && join && !join->empty(), "(an offline bootstrap of the same setup)");
  if (!join)
    return;
  repl::NodeStateMaps changed = pb.game_state().obj.schema ? repl::read_node_maps(*pb.game_state().obj.schema, pb.game_state().obj.arrays) : repl::NodeStateMaps();
  CHECK(changed.nodes.size() == 46, "(its GameState holds the 46 nodes)");
  changed.nodes[3].level = 2;
  const auto checkpoint = pb.checkpoint();
  const auto first = pb.node_maps_update(changed);
  CHECK(first.has_value(), "the first update builds");
  pb.restore(checkpoint);
  const auto again = pb.node_maps_update(changed);
  CHECK(first && again && first->emission.payload.bit_pos() == again->emission.payload.bit_pos() && first->emission.payload.bytes() == again->emission.payload.bytes(), "after a restore the same update builds to the same bits (the arrays and the shadow went back)");
  const auto nothing = pb.node_maps_update(changed);
  CHECK(!nothing.has_value(), "and once built and kept, the same maps again send nothing");
  changed.nodes[4].level = 3;
  const auto sent = pb.node_maps_update(changed);
  CHECK(sent.has_value(), "(a second update builds and is kept)");
  const auto other_step = pb.checkpoint();
  pb.restore(other_step);
  CHECK(!pb.node_maps_update(changed).has_value(), "the restore of a later checkpoint leaves the sent node maps as they are");
  bs::PlayerBootstrap tier2(h.s->bootstrap().setup(), bs::GameStateMode::Tier2);
  CHECK(!tier2.node_maps_update(changed).has_value(), "a tier-2 bootstrap has no node maps to update");
}

static void check_console_without_service() {
  const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
  Harness bare(cyb_record(), /*seeded=*/true, /*with_control=*/false);
  bare.play();
  bare.fx.world->set_gm_commands(true);
  repl::RpcParams p;
  repl::set_rpc_param(pc_rpc("ServerSetNodeLevel"), p, "InNode", repl::Value::str("Joeva"));
  repl::set_rpc_param(pc_rpc("ServerSetNodeLevel"), p, "InLevel", repl::Value::i64(2));
  bare.client_sends({bare.client.actor(bare.pc_ch, actor_rpc(pc, "ServerSetNodeLevel", p), true)});
  bare.run(milliseconds(200));
  CHECK(bare.s->node_gm_rpcs() == 1 && bare.s->gm_rpcs_answered() == 0, "without a node service the RPC is received and refused");
}

static void test_console() {
  std::printf("the client's node GM console\n");
  Harness h(cyb_record());
  h.play();
  const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
  const auto send = [&](const char *name, const std::function<void(const schema::RpcSchema &, repl::RpcParams &)> &fill) {
    repl::RpcParams p;
    fill(pc_rpc(name), p);
    h.client_sends({h.client.actor(h.pc_ch, actor_rpc(pc, name, p), true)});
    h.run(milliseconds(300));
  };
  const auto set_level = [&](const char *node, int level) {
    send("ServerSetNodeLevel", [&](const schema::RpcSchema &rpc, repl::RpcParams &p) {
      repl::set_rpc_param(rpc, p, "InNode", repl::Value::str(node));
      repl::set_rpc_param(rpc, p, "InLevel", repl::Value::i64(level));
      repl::set_rpc_param(rpc, p, "bDisableVassalshipSystem", repl::Value::boolean(true));
    });
  };
  CHECK(!h.fx.world->gm_commands(), "(the GM switch is off by default)");
  set_level("Verra_RVR_Joeva", 3);
  send("ServerAddNodeExperience", [&](const schema::RpcSchema &rpc, repl::RpcParams &p) {
    repl::set_rpc_param(rpc, p, "InNode", repl::Value::str("Halcyon"));
    repl::set_rpc_param(rpc, p, "Experience", repl::Value::i64(5000));
  });
  CHECK(h.s->node_gm_rpcs() == 2 && h.fx.control.calls.empty(), "with the switch off the two RPCs are received (and logged) and reach no one");
  h.fx.world->set_gm_commands(true);
  set_level("Verra_RVR_Joeva", 3);
  CHECK(h.fx.control.calls.size() == 1 && h.fx.control.calls[0].rfind("set_level Verra_RVR_Joeva 3 dv", 0) == 0, "ServerSetNodeLevel with the switch on reaches the node service with its node, level and flag");
  CHECK(h.fx.control.calls[0].find("by cyb") != std::string::npos, "... saying who asked");
  send("ServerAddNodeExperience", [&](const schema::RpcSchema &rpc, repl::RpcParams &p) {
    repl::set_rpc_param(rpc, p, "InNode", repl::Value::str("Halcyon"));
    repl::set_rpc_param(rpc, p, "Experience", repl::Value::i64(-250));
  });
  CHECK(h.fx.control.calls.size() == 2 && h.fx.control.calls[1].rfind("add_xp Halcyon -250", 0) == 0, "ServerAddNodeExperience reaches it, a negative amount negative");
  send("ServerGMGetNodeData", [&](const schema::RpcSchema &rpc, repl::RpcParams &p) {
    repl::set_rpc_param(rpc, p, "NodeName", repl::Value::str("Miraleth"));
  });
  CHECK(h.fx.control.calls.size() == 3 && h.fx.control.calls[2].rfind("node_data Miraleth", 0) == 0, "ServerGMGetNodeData reaches it");
  send("ServerGMWhatTimeIsIt", [&](const schema::RpcSchema &, repl::RpcParams &) {
  });
  CHECK(h.fx.control.calls.size() == 4 && h.fx.control.calls[3].rfind("clock", 0) == 0, "ServerGMWhatTimeIsIt asks for the node clock");
  send("ServerGMFastForwardRealTime", [&](const schema::RpcSchema &rpc, repl::RpcParams &p) {
    repl::Ticks ticks{3600ll * 10000000ll};
    repl::set_rpc_param(rpc, p, "FastForwardDuration", repl::Value(ticks));
  });
  CHECK(h.fx.control.calls.size() == 5 && h.fx.control.calls[4].rfind("advance 3600", 0) == 0, "ServerGMFastForwardRealTime skips the node clock by its seconds");
  send("ServerGMSetMayor", [&](const schema::RpcSchema &rpc, repl::RpcParams &p) {
    repl::set_rpc_param(rpc, p, "NodeName", repl::Value::str("Joeva"));
    repl::set_rpc_param(rpc, p, "CharacterName", repl::Value::str("cyb"));
  });
  CHECK(h.fx.control.calls.size() == 5 && h.s->node_gm_rpcs() == 8, "any other ServerGM* call is received and logged, and goes nowhere (its milestone is later)");
  check_console_without_service();
}

struct FakeService final : nodev1::CityNodeService::Service {
  std::mutex mu;
  std::string token_seen;
  std::vector<std::string> calls;

  static nodev1::Node node(const std::string &name, uint32_t level, int64_t xp) {
    nodev1::Node n;
    n.set_guid(kJoeva);
    n.set_name("Verra_RVR_" + name);
    n.set_display_name(name);
    n.set_level(level);
    n.set_level_name(level == 4 ? "Town" : "Village");
    n.set_experience(xp);
    n.set_percent(level == 4 ? 0 : 12);
    return n;
  }
  void note(grpc::ServerContext *ctx, const std::string &what) {
    std::lock_guard<std::mutex> lock(mu);
    const auto it = ctx->client_metadata().find(svc::kAuthorizationMetadata);
    token_seen = it == ctx->client_metadata().end() ? "" : std::string(it->second.data(), it->second.size());
    calls.push_back(what);
  }
  grpc::Status SetNodeLevel(grpc::ServerContext *ctx, const nodev1::SetNodeLevelRequest *req, nodev1::SetNodeLevelResponse *resp) override {
    note(ctx, "SetNodeLevel " + req->node().name() + " " + std::to_string(req->level()) + (req->disable_vassalship() ? " dv" : ""));
    if (req->node().name() == "Atlantis")
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "no node named Atlantis");
    if (req->level() > 4)
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "level 9 is above the highest, 4");
    *resp->mutable_node() = node("Joeva", static_cast<uint32_t>(req->level()), 0);
    resp->set_changed(true);
    return grpc::Status::OK;
  }
  grpc::Status AddNodeExperience(grpc::ServerContext *ctx, const nodev1::AddNodeExperienceRequest *req, nodev1::AddNodeExperienceResponse *resp) override {
    note(ctx, "AddNodeExperience " + req->node().name() + " " + std::to_string(req->amount()));
    *resp->mutable_node() = node("Joeva", 3, 1200);
    return grpc::Status::OK;
  }
  grpc::Status GetNode(grpc::ServerContext *ctx, const nodev1::GetNodeRequest *req, nodev1::GetNodeResponse *resp) override {
    note(ctx, "GetNode " + req->node().name());
    *resp->mutable_node() = node("Joeva", 4, 0);
    return grpc::Status::OK;
  }
  grpc::Status GetClock(grpc::ServerContext *ctx, const nodev1::GetClockRequest *, nodev1::GetClockResponse *resp) override {
    note(ctx, "GetClock");
    resp->mutable_clock()->set_speed(2.0);
    resp->mutable_clock()->set_node_unix_ms(1771000000000ll);
    return grpc::Status::OK;
  }
  grpc::Status AdvanceClock(grpc::ServerContext *ctx, const nodev1::AdvanceClockRequest *req, nodev1::AdvanceClockResponse *) override {
    note(ctx, "AdvanceClock " + std::to_string(req->seconds()));
    return grpc::Status::OK;
  }
};

static void check_control_lines(const std::vector<std::string> &lines) {
  CHECK(lines.size() == 7, "each call ended in one line");
  if (lines.size() != 7)
    return;
  CHECK(lines[0].find("Joeva") != std::string::npos && lines[0].find("level 3") != std::string::npos && lines[0].find("(changed)") != std::string::npos, "the level change says the node and its new level");
  CHECK(lines[1].find("1,200 XP added") != std::string::npos, "the experience says the amount with its thousands");
  CHECK(lines[2].find("level 4 (Town)") != std::string::npos, "the node data says the level and its name");
  CHECK(lines[3].find("node time 1771000000") != std::string::npos && lines[3].find("speed 2") != std::string::npos, "the clock says the node time and speed");
  CHECK(lines[5].find("refused") != std::string::npos && lines[5].find("no node named Atlantis") != std::string::npos, "an unknown node is refused with the service's reason");
  CHECK(lines[6].find("refused") != std::string::npos && lines[6].find("above the highest") != std::string::npos, "a level the service refuses is refused with its reason");
}

static void check_unreachable_control(const svc::GrpcNodeControl::Options &o) {
  svc::GrpcNodeControl::Options dead = o;
  dead.address = "127.0.0.1:1";
  dead.deadline_ms = 400;
  std::vector<std::string> dead_lines;
  std::mutex dead_mu;
  {
    svc::GrpcNodeControl control(dead, [&](const std::string &l) {
      std::lock_guard<std::mutex> lock(dead_mu);
      dead_lines.push_back(l);
    });
    control.set_level("Joeva", 3, false, "ServerSetNodeLevel by cyb");
    control.drain();
  }
  std::lock_guard<std::mutex> lock(dead_mu);
  CHECK(dead_lines.size() == 1 && dead_lines[0].find("the node service is unavailable") != std::string::npos, "with the service unreachable the console says \"the node service is unavailable\"");
}

static void test_control() {
  std::printf("the GM control over gRPC against a fake of the node service\n");
  const std::string token(64, 'c');
  FakeService service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server || port == 0) {
    std::printf("  [FAIL] the fake service does not start\n");
    ++g_fail;
    return;
  }
  std::mutex lines_mu;
  std::vector<std::string> lines;
  svc::GrpcNodeControl::Options o;
  o.address = "127.0.0.1:" + std::to_string(port);
  o.token = token;
  o.deadline_ms = 2000;
  {
    svc::GrpcNodeControl control(o, [&](const std::string &l) {
      std::lock_guard<std::mutex> lock(lines_mu);
      lines.push_back(l);
    });
    control.set_level("Verra_RVR_Joeva", 3, true, "ServerSetNodeLevel by cyb");
    control.add_experience("Halcyon", 1200, "ServerAddNodeExperience by cyb");
    control.node_data("Joeva", "ServerGMGetNodeData by cyb");
    control.clock("ServerGMWhatTimeIsIt by cyb");
    control.advance_clock(3600, "ServerGMFastForwardRealTime by cyb");
    control.set_level("Atlantis", 2, false, "ServerSetNodeLevel by cyb");
    control.set_level("Joeva", 9, false, "ServerSetNodeLevel by cyb");
    control.drain();
    CHECK(control.calls_made() == 7, "the seven calls were made, one after the other, on the worker thread");
    {
      std::lock_guard<std::mutex> lock(service.mu);
      const bool calls_ok = service.calls.size() == 7 && service.calls[0] == "SetNodeLevel Verra_RVR_Joeva 3 dv" && service.calls[1] == "AddNodeExperience Halcyon 1200" && service.calls[2] == "GetNode Joeva" && service.calls[3] == "GetClock" && service.calls[4] == "AdvanceClock 3600";
      CHECK(calls_ok, "the service got them in order with the node names, the level, the flag and the amounts");
      CHECK(service.token_seen == "Bearer " + token, "every call carries the service token");
    }
    std::lock_guard<std::mutex> lock(lines_mu);
    check_control_lines(lines);
  }
  server->Shutdown();
  check_unreachable_control(o);
}

void run_node_state_part2() {
  test_level_change();
  test_restore();
  test_console();
  test_control();
}
