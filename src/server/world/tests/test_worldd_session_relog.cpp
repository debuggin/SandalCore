// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.

#include "capture_blocks.h"
#include "capture_bootstrap.h"
#include "capture_packets.h"
#include "protocol/wire/server_move_parser.h"
#include "test_worldd_session_common.h"
#include "world_stats.h"

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace wire = aoc::protocol::wire;
namespace fs = std::filesystem;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
using std::chrono::milliseconds;
using std::chrono::seconds;

// ~ C-O-A & C.Y.B ~

static std::optional<repl::RepState>
read_single_rep_block(const uint8_t *data, size_t bits,
                      const schema::ClassSchema &cls) {
  const auto blocks = repl::read_content_blocks(data, bits, 0);
  if (!blocks || blocks->size() != 1) {
    return std::nullopt;
  }
  size_t p = (*blocks)[0].payload_pos;
  return repl::read_rep_block(
      cls, data, (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, p);
}

static void check_combat_echo(const fake::Delivered &d,
                              const schema::ClassSchema &pc_cls) {
  const auto echo =
      read_single_rep_block(d.content.data(), d.content.bit_pos(), pc_cls);
  const BunchWriter original = hex_bits(capture_bootstrap::kCombatEcho);
  const auto orig =
      read_single_rep_block(original.data(), original.bit_pos(), pc_cls);
  CHECK(echo && orig && echo->values().size() == 50 && d.reliable &&
            d.exports.empty(),
        "the echo is one reliable PlayerController block of the request's 50 values");
  if (!echo || !orig) {
    return;
  }
  constexpr std::array<std::string_view, 9> k_pkt620 = {
      "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MayHarmAngle",
      "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ProximityRadius",
      "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MaxDistance",
      "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ForwardOffset",
      "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.TabTargetMethod",
      "CharacterInGameSettings.CombatSettings.Targeting.bEnableHoverCasting",
      "CharacterInGameSettings.CombatSettings.Targeting.SoftLock.VisualsMask",
      "CharacterInGameSettings.CombatSettings.Input.BufferDuration",
      "CharacterInGameSettings.CombatSettings.Rendering.PartyVFX",
  };
  int equal = 0;
  int party_vfx_differs = 0;
  for (const std::string_view path : k_pkt620) {
    const repl::Value *mine = echo->get(pc_cls, path);
    const repl::Value *theirs = orig->get(pc_cls, path);
    if (mine && theirs && *mine == *theirs) {
      ++equal;
    } else if (path.find("PartyVFX") != std::string_view::npos && mine &&
               theirs) {
      ++party_vfx_differs;
    }
  }
  CHECK(equal == 8 && party_vfx_differs == 1,
        "eight of the nine values equal the original's (pkt#620); PartyVFX is "
        "the client's own (0 in this client's request, 1 in the original's "
        "reply)");
}

static void check_possession_ack_logs(Harness &h,
                                      const schema::ClassSchema &pc_cls,
                                      uint32_t pc_ch) {
  spdlog::default_logger()->flush();
  CHECK(g_log.str().find(
            "acknowledged possession of another object, not of its pawn") !=
            std::string::npos,
        "the captured acknowledgement names the original session's pawn, not "
        "this one's: logged as such");
  repl::RpcParams mine;
  repl::set_rpc_param(*pc_cls.rpc("ServerAcknowledgePossession"), mine, "P",
                      repl::Value(h.s->bootstrap().setup().guids.pawn));
  h.client_sends({h.client.actor(
      pc_ch, client_rpc(pc_cls, "ServerAcknowledgePossession", mine), true)});
  spdlog::default_logger()->flush();
  CHECK(g_log.str().find("the client acknowledged possession of its pawn") !=
            std::string::npos,
        "the acknowledgement of this session's pawn is logged");
}

static void test_possession_bunch(Harness &h) {
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
  const size_t before = h.got.size();
  const BunchWriter possession =
      hex_bits(capture_blocks::kClientBunchPossession);
  h.client_sends({h.client.actor(pc_ch, possession, true)});
  h.settle();
  const auto pc = h.on_channel(pc_ch, before);
  CHECK(pc.size() == 7,
        "the retail client's possession bunch (72 RPCs): seven chains back: "
        "the combat-settings echo and the load's six");
  if (pc.size() != 7) {
    return;
  }
  std::vector<std::string> names;
  for (const auto *d : pc) {
    const auto n = rpc_names(*d, pc_cls);
    names.push_back(n.empty() ? "(property block)" : n[0]);
  }
  CHECK(names[0] == "<not an rpc>" || names[0] == "(property block)" ||
            names[0] == "<unreadable>",
        "the first is a property block, not an RPC: the echo");
  const std::vector<std::string> rest = {
      "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
      "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
      "ClientUpdateGroup",           "ClientReceiveActionbarSlotPayload"};
  CHECK(std::vector<std::string>(names.begin() + 1, names.end()) == rest,
        "then the four storage payloads, the group and the action bar");
  check_combat_echo(*pc[0], pc_cls);
  const auto parsed = repl::read_client_bunch(
      pc_cls, nullptr, possession.data(), possession.bit_pos());
  std::map<std::string, int> seen;
  if (parsed) {
    for (const auto &r : parsed->rpcs) {
      ++seen[r.call.rpc->name];
    }
  }
  CHECK(parsed && parsed->rpcs.size() == 72 &&
            seen["ServerUpdateAccountCombatSettings"] == 1 &&
            seen["ServerAcknowledgePossession"] == 1 &&
            seen["ServerRequestClientLoad"] == 1 &&
            seen["NotifyAntiCheatState"] == 1 &&
            seen["ServerRequestNodeSummary"] == 34 &&
            seen["ServerRequestCurrentSiegeSummary"] == 34,
        "(the request has 72 RPCs, as the capture says; worldd answers two of "
        "them, the node and siege summaries and the client-state notice are "
        "ignored)");
  check_possession_ack_logs(h, pc_cls, pc_ch);
}

static void test_storage_requests(Harness &h) {
  const schema::ClassSchema &pawn_cls = class_named("PlayerPawn_C");
  const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
  const bs::SessionGuids &g = h.s->bootstrap().setup().guids;
  const size_t b2 = h.got.size();
  repl::RpcParams params;
  repl::set_rpc_param(*pawn_cls.rpc("ServerRequestStorageData"), params,
                      "StorageContainer", repl::Value(g.resource_storage[0]));
  h.client_sends({h.client.actor(
      pawn_ch, client_rpc(pawn_cls, "ServerRequestStorageData", params),
      true)});
  h.settle();
  auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0),
                               bs::GameStateMode::Tier2);
  const auto want =
      pb->storage_payload(bs::StorageKind::Resource0, bs::Role::Pawn);
  const auto reply = h.on_channel(pawn_ch, b2);
  CHECK(reply.size() == 1 && want &&
            same_bits(reply[0]->content, want->emission.payload) &&
            reply[0]->exports.empty(),
        "ServerRequestStorageData(resource storage): the pawn's empty resource "
        "payload (pkt#773), no export");
  repl::RpcParams other;
  repl::set_rpc_param(*pawn_cls.rpc("ServerRequestStorageData"), other,
                      "StorageContainer", repl::Value(guid(0x7777777)));
  const size_t b3 = h.got.size();
  h.client_sends({h.client.actor(
      pawn_ch, client_rpc(pawn_cls, "ServerRequestStorageData", other), true)});
  h.settle();
  CHECK(h.got.size() == b3, "a container worldd does not know gets no reply");
}

static void test_malformed_requests(Harness &h) {
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
  const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
  const size_t b4 = h.got.size();
  h.client_sends({h.client.actor(
      pawn_ch, hex_bits(capture_blocks::kClientSetActiveTarget), true)});
  h.client_sends({h.client.actor(
      40, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
  BunchWriter garbage;
  for (int i = 0; i < 12; ++i) {
    garbage.write_bits(0xFFFFFFFFu, 32);
  }
  h.client_sends({h.client.actor(pc_ch, garbage, true)});
  const BunchWriter possession =
      hex_bits(capture_blocks::kClientBunchPossession);
  BunchWriter truncated;
  truncated.write_bit_range(possession.data(), 0, 300);
  h.client_sends({h.client.actor(pc_ch, truncated, true)});
  h.settle();
  CHECK(h.got.size() == b4 && !h.s->failed(),
        "ServerSetActiveTarget of an object that is no actor, a bunch on a "
        "channel that is not the player's, garbage and a cut bunch: nothing "
        "sent");
  h.client_sends({h.client.actor(
      pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
  h.settle();
  CHECK(h.got.size() == b4 + 6, "and the session still answers afterwards");
  spdlog::default_logger()->flush();
  CHECK(g_log.str().find("does not read: ignored") != std::string::npos,
        "the malformed bunches are logged");
}

static void test_possession_bunch_and_requests() {
  std::printf("the possession bunch, storage requests, the unknown and the "
              "malformed\n");
  Harness h;
  h.join();
  h.settle();
  test_possession_bunch(h);
  test_storage_requests(h);
  test_malformed_requests(h);
}
// [C_Y_B ~ CoA]

static void clog_channel(Harness &h, uint32_t channel, size_t bunches) {
  conn::BunchHeader header;
  header.channel = channel;
  header.reliable = true;
  header.name_index = Connection::kActorChannelName;
  BunchWriter filler;
  filler.write_bits(0xA5, 8);
  for (size_t i = 0; i < bunches; ++i) {
    h.conn.send(header, filler);
  }
}

static void test_join_waits_for_the_window() {
  std::printf("a join that does not fit the window\n");
  Harness h;
  h.client_receives = false;
  clog_channel(h, 3, 248);
  CHECK(h.conn.unacked_reliable(3) == 248,
        "(248 reliable bunches in flight on channel 3)");
  CHECK(h.join() && !h.s->failed(), "the session joins");
  const size_t pending = h.s->join_pending();
  CHECK(pending > 0 && pending < 15,
        "part of the join is on the wire and the rest waits for the window: a "
        "chain stays at the head of the queue");
  std::printf("    %zu of the 16 chains were waiting\n", pending);
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  h.client_sends(
      {h.client.actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
  CHECK(h.s->replies_pending() == 6 && h.s->join_pending() == pending,
        "the request's six reply steps wait behind the join");
  h.s->tick(h.conn);
  CHECK(h.s->join_pending() == pending,
        "a tick with the window still full changes nothing");
  h.flush();

  h.client_receives = true;
  h.give_held();
  h.settle();
  CHECK(
      h.s->join_pending() == 0 && h.s->replies_pending() == 0 && !h.s->failed(),
      "once the client acknowledges, the join completes and then the replies");
  std::vector<fake::Delivered> mine;
  for (const auto &d : h.got) {
    if (d.content.bit_pos() != 8) {
      mine.push_back(d);
    }
  }
  auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0),
                               bs::GameStateMode::Tier2);
  auto seq = pb->join_sequence();
  CHECK(seq && mine.size() == 16 + 6 &&
            compare_sends(mine, 0, *seq, kChannels).empty(),
        "the client has the join's 16 chains in the bootstrap's order, equal "
        "to it, and then the six replies");
  bool replies_last = mine.size() == 22;
  for (size_t i = 16; replies_last && i < 22; ++i) {
    replies_last = mine[i].channel == 3 && !rpc_names(mine[i], pc_cls).empty();
  }
  CHECK(replies_last,
        "the six replies are the last six, on the PlayerController's channel");
  spdlog::default_logger()->flush();
  CHECK(g_log.str().find("the window of channel 3 is full") !=
            std::string::npos,
        "the wait is logged (once)");
}

static void verify_window_full_drain(Harness &h, uint32_t pc_ch,
                                     const schema::ClassSchema &pc_cls) {
  h.client_receives = true;
  h.give_held();
  h.settle();
  CHECK(h.s->replies_pending() == 0 && h.s->join_pending() == 0,
        "once it acknowledges, every waiting step goes out");
  const auto pc = h.on_channel(pc_ch);
  CHECK(pc.size() >= 8 + 6 * 10 && (pc.size() - 8) % 6 == 0,
        "the PlayerController's chains are the join's eight and whole loads of "
        "six after them");
  if (pc.size() < 8) {
    return;
  }
  const std::vector<std::string> cycle = {
      "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
      "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
      "ClientUpdateGroup",           "ClientReceiveActionbarSlotPayload"};
  bool in_order = true;
  for (size_t i = 8; i < pc.size(); ++i) {
    const auto n = rpc_names(*pc[i], pc_cls);
    in_order = in_order && n.size() == 1 && n[0] == cycle[(i - 8) % 6];
  }
  CHECK(in_order, "every load arrived whole and in order: storage x4, group, "
                  "action bar, again and again");
  std::printf("    %zu loads delivered of 40 asked\n", (pc.size() - 8) / 6);
  auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0),
                               bs::GameStateMode::Tier2);
  auto seq = pb->join_sequence();
  std::vector<fake::Delivered> first_16(
      h.got.begin(), h.got.begin() + std::min<size_t>(16, h.got.size()));
  CHECK(seq && compare_sends(first_16, 0, *seq, kChannels).empty(),
        "the join itself was delivered in order, first, on its four channels");
}

static void test_window_full() {
  std::printf("a client that stops acknowledging\n");
  Harness h;
  h.client_receives = false;
  CHECK(h.join(), "join");
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
  size_t most_unacked = h.conn.unacked_reliable(pc_ch);
  for (int i = 0; i < 40; ++i) {
    h.t += PlayerSession::kLoadInterval;
    h.client_sends({h.client.actor(
        pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
    h.s->tick(h.conn);
    h.flush();
    most_unacked = std::max(most_unacked, h.conn.unacked_reliable(pc_ch));
  }
  CHECK(
      most_unacked <= Connection::kMaxUnackedReliable,
      "the channel never holds more than 256 unacknowledged reliable bunches");
  CHECK(h.s->replies_pending() > 0 && !h.s->failed(),
        "replies wait for the window (they are not dropped, and the session "
        "does not fail)");
  CHECK(h.s->replies_pending() <= PlayerSession::kMaxPendingReplies,
        "and at most kMaxPendingReplies steps wait: the rest of the requests "
        "are ignored");
  const size_t waiting = h.s->replies_pending();
  std::printf(
      "    %zu reply steps were waiting, %zu bunches unacknowledged at most\n",
      waiting, most_unacked);
  CHECK(h.s->requests_dropped() == 0,
        "no request was over its budget: the waiting is the window's doing");
  verify_window_full_drain(h, pc_ch, pc_cls);
}

static void test_phase_in_waits() {
  std::printf("the phase-in waits for the pawn channel's window\n");
  Harness h;
  h.join();
  h.settle();
  const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
  h.client_receives = false;
  const size_t room =
      Connection::kMaxUnackedReliable - h.conn.unacked_reliable(pawn_ch);
  clog_channel(h, pawn_ch, room);
  CHECK(h.conn.unacked_reliable(pawn_ch) == Connection::kMaxUnackedReliable,
        "(the pawn channel's window is full)");
  const size_t before = h.got.size();
  h.client_sends({h.client.actor(
      pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
  h.s->tick(h.conn);
  h.flush();
  CHECK(h.s->replies_pending() == 1, "the phase-in waits");
  h.client_receives = true;
  h.give_held();
  h.settle();
  size_t real = 0;
  for (const auto *d : h.on_channel(pawn_ch, before)) {
    if (d->content.bit_pos() != 8) {
      ++real;
    }
  }
  CHECK(real == 1,
        "after the acknowledgement the phase-in update arrives, exactly once "
        "(the checkpoint was restored while it waited)");
  CHECK(h.s->replies_pending() == 0, "nothing pending");
}
// [CoA x C-Y-B]

static void test_echo_waits() {
  std::printf("the combat-settings echo waits for the PlayerController "
              "channel's window\n");
  Harness h;
  h.join();
  h.settle();
  const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
  h.client_receives = false;
  const size_t room =
      Connection::kMaxUnackedReliable - h.conn.unacked_reliable(pc_ch);
  clog_channel(h, pc_ch, room);
  const size_t before = h.got.size();
  h.client_sends({h.client.actor(
      pc_ch, hex_bits(capture_blocks::kClientBunchPossession), true)});
  h.s->tick(h.conn);
  h.flush();
  CHECK(h.s->replies_pending() == 7, "the echo and the load's six wait");
  h.client_receives = true;
  h.give_held();
  h.settle();
  size_t real = 0;
  for (const auto *d : h.on_channel(pc_ch, before)) {
    if (d->content.bit_pos() != 8) {
      ++real;
    }
  }
  CHECK(real == 7, "after the acknowledgement: the echo and the six replies, "
                   "each once (the PlayerController shadow was restored too)");
}

static bool has_pawn_exports(const std::vector<uint8_t> &pkt,
                             uint32_t pawn_ch) {
  const auto p = conn::read_data_packet(pkt.data(), pkt.size(),
                                        wire::Direction::ServerToClient);
  if (!p) {
    return false;
  }
  for (const auto &b : conn::packet_bunches(pkt.data(), pkt.size(), *p)) {
    if (b.channel == pawn_ch && b.has_package_map_exports) {
      return true;
    }
  }
  return false;
}

static size_t join_dropping_pawn_exports(Harness &h) {
  h.client_receives = false;
  h.join();
  h.step();
  const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
  std::vector<std::vector<uint8_t>> keep;
  size_t dropped = 0;
  for (const auto &pkt : h.held) {
    if (has_pawn_exports(pkt, pawn_ch)) {
      ++dropped;
    } else {
      keep.push_back(pkt);
    }
  }
  h.held.clear();
  for (const auto &pkt : keep) {
    h.take(h.client.receive(pkt));
  }
  return dropped;
}

static size_t big_payloads(const std::vector<fake::Delivered> &chains,
                           const schema::ClassSchema &pc_cls,
                           size_t *with_export) {
  size_t big = 0;
  *with_export = 0;
  for (const auto &d : chains) {
    const auto n = rpc_names(d, pc_cls);
    if (n.empty() || n[0] != "ClientReceiveStoragePayload" ||
        d.content.bit_pos() <= 10000) {
      continue;
    }
    ++big;
    if (!d.exports.empty()) {
      ++*with_export;
    }
  }
  return big;
}

static void test_lost_export() {
  std::printf("a lost packet's exports are not marked exported\n");
  Harness h;
  const size_t dropped = join_dropping_pawn_exports(h);
  CHECK(dropped > 0,
        "(the packets with the pawn's export bunches were dropped)");
  const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  h.t += milliseconds(20);
  h.client_sends({});
  h.t += milliseconds(20);
  h.client_sends({h.client.actor(
      pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
  h.s->tick(h.conn);
  std::vector<fake::Delivered> pc_chains;
  for (const auto &pkt : h.conn.flush(h.t)) {
    auto r = h.client.receive(pkt);
    for (const auto &d : r.delivered) {
      if (d.channel == pc_ch) {
        pc_chains.push_back(d);
      }
    }
  }
  size_t with_export = 0;
  const size_t big = big_payloads(pc_chains, pc_cls, &with_export);
  CHECK(big == 3 && with_export == 3,
        "the Backpack, Equipment and QuestStorage payloads still carry their "
        "exports: the pawn's export packet was lost");
}
// CoA : C.Y.B

static void test_lost_then_resent() {
  std::printf("a lost export packet is resent and delivered: later replies "
              "leave its exports out\n");
  Harness h;
  join_dropping_pawn_exports(h);
  const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  h.client_receives = true;
  h.settle();
  CHECK(h.s->exports_tracked() == 0,
        "once the resend is acknowledged nothing is tracked");
  const size_t before = h.got.size();
  h.client_sends({h.client.actor(
      pc_ch, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
  h.settle();
  std::vector<fake::Delivered> chains;
  for (const auto *d : h.on_channel(pc_ch, before)) {
    chains.push_back(*d);
  }
  size_t with_export = 0;
  const size_t big = big_payloads(chains, pc_cls, &with_export);
  CHECK(big == 3 && with_export == 0,
        "once the resent packet is acknowledged its exports are marked: the "
        "replies carry none");
}

static void test_budget_unit() {
  RequestBudget b(4, seconds(2));
  int taken = 0;
  for (int i = 0; i < 10; ++i) {
    taken += b.take(t0) ? 1 : 0;
  }
  CHECK(taken == 4, "a fresh budget of 4 gives 4 at once and no more");
  CHECK(!b.take(t0 + seconds(1)) && b.take(t0 + seconds(2)) &&
            !b.take(t0 + seconds(2)),
        "one more after the interval, not before");
  CHECK(!b.take(t0 + seconds(3) + milliseconds(999)) && b.take(t0 + seconds(4)),
        "the remainder of an interval carries over");
  taken = 0;
  for (int i = 0; i < 10; ++i) {
    taken += b.take(t0 + seconds(100)) ? 1 : 0;
  }
  CHECK(taken == 4, "a long quiet period refills to the burst, not beyond");
}

static void test_load_budget_flood(Harness &h) {
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
  const BunchWriter load = client_rpc(pc_cls, "ServerRequestClientLoad");
  const size_t bytes_before = h.bytes;
  const size_t chains_before = h.got.size();
  g_log.str("");
  const size_t packets = h.client_repeats(pc_ch, load, 1000);
  h.settle();
  const size_t sent = h.bytes - bytes_before;
  const size_t loads = h.on_channel(pc_ch, chains_before).size() / 6;
  std::printf("    1,000 loads in %zu packets within a second: %zu answered, "
              "%zu bytes sent, %zu requests dropped\n",
              packets, loads, sent, h.s->requests_dropped());
  CHECK(!h.s->failed() && !h.conn.failed(),
        "(the session and the connection took the flood)");
  CHECK(loads == PlayerSession::kLoadBurst &&
            h.s->requests_dropped() == 1000 - PlayerSession::kLoadBurst,
        "the burst's worth are answered, the rest dropped");
  CHECK(sent < 300 * 1024,
        "under a few hundred KB went out for the 1,000 requests");
  spdlog::default_logger()->flush();
  CHECK(count_lines(g_log.str(), "faster than its budget") == 1,
        "the first drop is logged, once");
  h.t += seconds(5);
  const size_t at = h.got.size();
  h.client_sends({h.client.actor(pc_ch, load, true)});
  h.settle();
  CHECK(h.on_channel(pc_ch, at).size() == 6,
        "a load after a quiet period is answered");
}

static void test_storage_budget_flood(Harness &h) {
  const schema::ClassSchema &pawn_cls = class_named("PlayerPawn_C");
  const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
  repl::RpcParams params;
  repl::set_rpc_param(
      *pawn_cls.rpc("ServerRequestStorageData"), params, "StorageContainer",
      repl::Value(h.s->bootstrap().setup().guids.resource_storage[0]));
  const BunchWriter storage =
      client_rpc(pawn_cls, "ServerRequestStorageData", params);
  size_t at = h.got.size();
  const size_t dropped_before = h.s->requests_dropped();
  h.client_repeats(pawn_ch, storage, 40);
  h.settle();
  CHECK(h.on_channel(pawn_ch, at).size() == PlayerSession::kStorageBurst &&
            h.s->requests_dropped() ==
                dropped_before + 40 - PlayerSession::kStorageBurst,
        "40 storage requests within a second: the storage burst's worth are "
        "answered");
  h.t += PlayerSession::kStorageInterval * 3;
  at = h.got.size();
  h.client_sends({h.client.actor(pawn_ch, storage, true)});
  h.client_sends({h.client.actor(pawn_ch, storage, true)});
  h.client_sends({h.client.actor(pawn_ch, storage, true)});
  h.client_sends({h.client.actor(pawn_ch, storage, true)});
  h.settle();
  CHECK(h.on_channel(pawn_ch, at).size() == 3,
        "three intervals later: three more, not four");
}

static void test_request_budget() {
  std::printf("the request budgets\n");
  test_budget_unit();
  Harness h;
  h.join();
  h.settle();
  test_load_budget_flood(h);
  test_storage_budget_flood(h);
}
// [C O A x C-Y-B]

void run_session_relog() {
  test_possession_bunch_and_requests();
  test_join_waits_for_the_window();
  test_window_full();
  test_phase_in_waits();
  test_echo_waits();
  test_lost_export();
  test_lost_then_resent();
  test_request_budget();
}
