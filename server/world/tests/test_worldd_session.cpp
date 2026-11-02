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

static void test_identity_capture_and_control() {
  const auto bytes = capture::bytes(capture::kClientLogin);
  const auto p = conn::read_data_packet(bytes.data(), bytes.size(),
                                        wire::Direction::ClientToServer);
  std::optional<conn::ControlMessage> m;
  if (p) {
    const auto bunches = conn::packet_bunches(bytes.data(), bytes.size(), *p);
    if (bunches.size() == 1) {
      m = conn::parse_control_message(bytes.data(), bytes.size(),
                                      bunches[0].data_start_bit,
                                      bunches[0].bunch_data_bits);
    }
  }
  CHECK(m && m->type == conn::Nmt::Login && m->has_unique_id &&
            m->platform_name == "NULL",
        "the captured NMT_Login parses with its unique id (flags 8) and "
        "platform name 'NULL'");
  CHECK(m && m->request_url == "?Name=" + m->unique_id &&
            m->unique_id.rfind("DESKTOP-EXAMPL2-", 0) == 0 &&
            m->unique_id.size() == 48,
        "its unique id is the ?Name= value, the 48 characters the transcript "
        "names (section 2.3)");
  // (C.O.A | C | Y | B)

  ControlChannel c(WorldInfo{}, "X");
  conn::ControlMessage hello;
  hello.type = conn::Nmt::Hello;
  c.on_message(hello);
  CHECK(!c.login_message(), "no Login yet: nothing kept");
  if (m) {
    c.on_message(*m);
  }
  CHECK(c.login_message() && c.login_message()->has_unique_id &&
            c.login_message()->unique_id == m->unique_id &&
            c.login_message()->request_url == m->request_url,
        "the control channel keeps the Login's URL and unique id");
}

static void test_identity_url_and_fallbacks() {
  const bool url_ok = url_option("?Name=cyb", "Name") == "cyb" &&
                      url_option("?Name=cyb?Team=1", "name") == "cyb" &&
                      url_option("Map?Team=1?Name=a b", "Name") == "a b";
  const bool url_empty = url_option("?Team=1", "Name").empty() &&
                         url_option("", "Name").empty() &&
                         url_option("?Name", "Name").empty() &&
                         url_option("?Name=", "Name").empty();
  CHECK(url_ok && url_empty, "url_option: the value up to the next '?', the "
                             "key without case, nothing when absent");

  std::vector<std::string> notes;
  const svc::Character rec = cyb_record();
  auto id = make_login_identity(
      LoginMessage{"?Name=" + kMachine, true, kMachine}, rec, &notes);
  CHECK(id.name_option == kMachine && id.unique_id == kMachine && notes.empty(),
        "a full Login: the name option and the unique id as sent, no note");
  id = make_login_identity(LoginMessage{"?Name=" + kMachine, false, ""}, rec,
                           &notes);
  CHECK(id.name_option == kMachine && id.unique_id == kMachine &&
            notes.size() == 1,
        "no unique id: the name option stands in, with a note");
  notes.clear();
  id = make_login_identity(LoginMessage{"?Team=1", true, "ID"}, rec, &notes);
  CHECK(id.name_option == "cyb" && id.unique_id == "ID" && notes.size() == 1,
        "no ?Name=: the character's name stands in, with a note");
  notes.clear();
  id = make_login_identity(std::nullopt, rec, &notes);
  CHECK(id.name_option == "cyb" && id.unique_id == "cyb" && notes.size() == 2,
        "no Login at all: both fall back to the character's name");
}

static void test_identity() {
  std::printf("the login identity\n");
  test_identity_capture_and_control();
  test_identity_url_and_fallbacks();
}

static void test_content_missing() {
  std::printf("content.db missing\n");
  Fixture fx(bs::GameStateMode::Tier2, false);
  CHECK(!fx.world->ready() && !fx.world->content_problem().empty(),
        "the world is not ready and says why");
  EndpointConfig cfg;
  cfg.players = fx.world.get();
  Rig rig(cfg);
  g_log.str("");
  Joined j = handshake(rig, kA, t0);
  CHECK(j.client && rig.ep.session_count() == 1 && rig.logins.submitted.empty(),
        "the handshake completes but no login is submitted: the join ticket is "
        "not redeemed for a refusal");
  auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
  const conn::ControlMessage *failure = find_message(got, conn::Nmt::Failure);
  const bool closed = closed_by_server(got);
  CHECK(failure && failure->error == login_text::kContentMissing && closed,
        "Hello gets NMT_Failure with the content.db text, and the channel "
        "closes");
  CHECK(std::string(login_text::kContentMissing).find("content.db") !=
            std::string::npos,
        "the text names content.db for the operator");
  CHECK(!rig.ep.session_of(kCyb), "the character is not logged in");
  CHECK(g_log.str().find("worldd has no content.db (no such file)") !=
                std::string::npos &&
            g_log.str().find(kTicket) == std::string::npos,
        "the log says why, without the ticket");
}

static void verify_refused_without_item_tables(PlayerWorld &world) {
  EndpointConfig cfg;
  cfg.players = &world;
  Rig rig(cfg);
  g_log.str("");
  Joined j = handshake(rig, kA, t0);
  CHECK(j.client && rig.ep.session_count() == 1 && rig.logins.submitted.empty(),
        "the handshake completes but no login is submitted: the join ticket is "
        "not redeemed");
  auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
  const conn::ControlMessage *failure = find_message(got, conn::Nmt::Failure);
  CHECK(failure && failure->error == login_text::kContentMissing &&
            closed_by_server(got),
        "Hello gets NMT_Failure with the content.db text, and the channel "
        "closes: every login is refused");
  CHECK(!rig.ep.session_of(kCyb), "the character is not logged in");
  Joined k = handshake(rig, kB, t0 + milliseconds(20));
  auto got_k = exchange(rig, k, kB, {k.client->hello()}, t0 + milliseconds(30));
  const conn::ControlMessage *failure_k =
      find_message(got_k, conn::Nmt::Failure);
  CHECK(failure_k && failure_k->error == login_text::kContentMissing &&
            rig.logins.submitted.empty(),
        "and so is the next client");
  CHECK(g_log.str().find("worldd has no content.db (") != std::string::npos &&
            g_log.str().find("rebuild it with this checkout's contentc") !=
                std::string::npos &&
            g_log.str().find(kTicket) == std::string::npos,
        "the log says why and how to rebuild the file, without the ticket");
}

static void test_content_built_before_b2() {
  std::printf("a content.db built before plan B2\n");
  const fs::path dir = content_fixture::make_temp_dir("worldd_session_old_");
  content_fixture::DbOptions old_schema;
  old_schema.item_tables = false;
  const fs::path path = dir / "content.db";
  if (!content_fixture::make_db(path, old_schema)) {
    std::exit(2);
  }
  std::string error;
  std::optional<content::ContentDb> content =
      content::ContentDb::open(path.string(), error);
  CHECK(!content &&
            error.find("does not have the tables this worldd reads") !=
                std::string::npos &&
            error.find("item") != std::string::npos &&
            error.find("rebuild it with this checkout's contentc") !=
                std::string::npos,
        "the file does not open: it lacks the item tables, and the error says "
        "to rebuild it with contentc");
  aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
  PlayerWorld world(content ? &*content : nullptr, error,
                    bs::GameStateMode::Tier2, kServerId, guids);
  CHECK(!world.ready() && world.content_problem() == error,
        "the world is not ready, and its reason is the one open() gave");
  verify_refused_without_item_tables(world);
  content.reset();
  std::error_code ec;
  fs::remove_all(dir, ec);
}
// ~ C-O-A :: C | Y | B ~

struct StatProgCase {
  const char *what;
  const std::vector<uint8_t> *bytes;
  const char *want;
  int64_t crc_xor = 0;
};

static void run_stat_program_case(const fs::path &dir, int idx,
                                  const StatProgCase &c) {
  content_fixture::DbOptions o;
  o.stat_program_bytes = c.bytes;
  o.stat_program_crc_xor = c.crc_xor;
  const fs::path path = dir / (std::string("c") + std::to_string(idx) + ".db");
  if (!content_fixture::make_db(path, o)) {
    std::exit(2);
  }
  std::string error;
  std::optional<content::ContentDb> content =
      content::ContentDb::open(path.string(), error);
  CHECK(content.has_value(),
        (std::string(c.what) + ": the file opens (its tables are there)")
            .c_str());
  if (!content) {
    return;
  }
  aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
  PlayerWorld world(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
  CHECK(
      count_lines(world.content_problem(), "go run ./cmd/contentc") == 1,
      (std::string(c.what) + ": the way out is said once, not twice").c_str());
  CHECK(!world.ready() &&
            world.content_problem().find(c.want) != std::string::npos &&
            world.content_problem().find("go run ./cmd/contentc") !=
                std::string::npos &&
            world.stat_bundle() == nullptr && world.content() == nullptr,
        (std::string(c.what) + ": the world is not ready, and says '" + c.want +
         "' and how to rebuild")
            .c_str());
  if (idx == 0) {
    EndpointConfig cfg;
    cfg.players = &world;
    Rig rig(cfg);
    Joined j = handshake(rig, kA, t0);
    auto got = exchange(rig, j, kA, {j.client->hello()}, t0 + milliseconds(10));
    const conn::ControlMessage *failure = find_message(got, conn::Nmt::Failure);
    CHECK(failure && failure->error == login_text::kContentMissing &&
              rig.logins.submitted.empty(),
          "a login is refused with the content.db text, before the ticket is "
          "redeemed");
  }
}

static void test_stat_program_problems() {
  std::printf("a content.db with no usable stat program (plan B3)\n");
  const fs::path dir =
      content_fixture::make_temp_dir("worldd_session_statprog_");
  std::vector<uint8_t> damaged = stats_fixture::main_bundle();
  damaged[damaged.size() / 2] ^= 0x01;
  const std::vector<uint8_t> none;
  const std::vector<uint8_t> &main_bytes = stats_fixture::main_bundle();
  const std::vector<uint8_t> &tiny = stats_fixture::corpus().bundles.at("tiny");
  const StatProgCase cases[] = {
      {"a damaged bundle (one flipped byte)", &damaged, "does not verify"},
      {"no row in stat_program", &none, "no stat program"},
      {"a valid bundle of another graph (the tiny one of the tests)", &tiny,
       "not one this worldd can run"},
      {"a row whose crc32 is not the bundle header's", &main_bytes,
       "bundle's header says", 0x5a5a},
  };
  for (int i = 0; i < 4; ++i) {
    run_stat_program_case(dir, i, cases[i]);
  }
  content_fixture::DbOptions ok;
  const fs::path path = dir / "ok.db";
  if (!content_fixture::make_db(path, ok)) {
    std::exit(2);
  }
  std::string error;
  std::optional<content::ContentDb> content =
      content::ContentDb::open(path.string(), error);
  aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
  PlayerWorld world(content ? &*content : nullptr, "", bs::GameStateMode::Tier2,
                    kServerId, guids);
  const bs::StatContext ctx = world.stat_context();
  CHECK(world.ready() && world.stat_bundle() &&
            ctx.bundle == world.stat_bundle() && ctx.gear &&
            ctx.rounding == stats::Rounding::HalfUp &&
            world.stat_bundle()->classes.size() == 10,
        "(control) the fixture's real stat program makes a ready world with a "
        "bundle of ten classes (the eight player classes and the two NPC "
        "profiles) and the item data");
  content.reset();
  std::error_code ec;
  fs::remove_all(dir, ec);
}

static void verify_join_actor_opens(const NewActor &pc, const NewActor &gs,
                                    const NewActor &pawn, const NewActor &ps) {
  CHECK(pc.ok && gs.ok && pawn.ok && ps.ok,
        "the four opens' SerializeNewActor read");
  CHECK(gs.actor == guid(0x1000000) && ps.actor == guid(0x1000002) &&
            pc.actor == guid(0x1000004) && pawn.actor == guid(0x100000a),
        "GUIDs are minted under worldd's ServerId: the GameState's first, then "
        "the PlayerState, PlayerController and pawn (even ObjectIds; "
        "GlobalGMCommands and the tutorial component come between the "
        "controller and the pawn)");
  CHECK(pc.archetype.ObjectId ==
                bs::pc_archetype_export().front().guid.ObjectId &&
            gs.archetype.ObjectId ==
                bs::game_state_archetype_export().front().guid.ObjectId &&
            pawn.archetype.ObjectId ==
                bs::pawn_archetype_export().front().guid.ObjectId &&
            ps.archetype.ObjectId ==
                bs::player_state_archetype_export().front().guid.ObjectId,
        "each open names its class: AoCPlayerControllerBP_C, AoCGameStateBP_C, "
        "PlayerPawn_C, AoCPlayerStateBP_C");
}

static bool prop_eq(const std::optional<repl::RepState> &blk,
                    const schema::ClassSchema &cls, const char *name,
                    const repl::Value &expected) {
  const repl::Value *v = blk ? blk->get(cls, name) : nullptr;
  return v && *v == expected;
}

static void verify_join_opens_and_states(const Joined &j,
                                         const bs::PlayerSetup &setup) {
  if (j.got.size() < 15) {
    return;
  }
  const NewActor pc = read_new_actor(j.got[0].content, true);
  const NewActor gs = read_new_actor(j.got[4].content, false);
  const NewActor pawn = read_new_actor(j.got[6].content, false);
  const NewActor ps = read_new_actor(j.got[14].content, false);
  verify_join_actor_opens(pc, gs, pawn, ps);
  const schema::ClassSchema &base_cls =
      *gen::find_component_schema("BaseCharacterInfo");
  const auto base = block_of(j.got[6], pawn.end, false,
                             setup.guids.base_character_info, base_cls);
  CHECK(prop_eq(base, base_cls, "CharacterName", repl::Value::str("cyb")) &&
            prop_eq(base, base_cls, "CharacterGuid.A",
                    repl::Value::i64(static_cast<int32_t>(0xbbec5c86u))) &&
            prop_eq(base, base_cls, "CharacterGuid.D",
                    repl::Value::i64(static_cast<int32_t>(0xfa22e7d0u))),
        "the pawn's BaseCharacterInfo: CharacterName 'cyb' and the record's "
        "Guid bbec5c86 ... fa22e7d0");
  const schema::ClassSchema &ps_cls = class_named("AoCPlayerStateBP_C");
  const auto ps_state = block_of(j.got[14], ps.end, true, {}, ps_cls);
  const repl::Value *uid =
      ps_state ? ps_state->get(ps_cls, "UniqueId") : nullptr;
  const auto *u = uid ? uid->get_if<repl::UniqueNetId>() : nullptr;
  CHECK(u && u->flags == 8 && u->id == kMachine &&
            prop_eq(ps_state, ps_cls, "PlayerNamePrivate",
                    repl::Value::str(kMachine.substr(0, 20))),
        "the PlayerState: the login's unique id (flags 8) and the first 20 "
        "characters of its ?Name=, not the character's name");
  CHECK(prop_eq(ps_state, ps_cls, "PlayerId", repl::Value::i64(1)),
        "PlayerId 1, the first player");
}

static void verify_join_acks_and_logs(Rig &rig, Joined &j,
                                      Clock::time_point t_join) {
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  const size_t before = j.got.size();
  exchange(rig, j, kA, {}, t_join + milliseconds(100));
  exchange(
      rig, j, kA,
      {j.client->actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)},
      t_join + milliseconds(200));
  CHECK(j.got.size() == before + 6,
        "ServerRequestClientLoad is answered with six chains on the "
        "PlayerController's channel");
  if (j.got.size() == before + 6) {
    const auto names = rpc_names(j.got[before], pc_cls);
    CHECK(j.got[before].channel == 3 && names.size() == 1 &&
              names[0] == "ClientReceiveStoragePayload" &&
              j.got[before].exports.empty(),
          "the Backpack payload: the component's export was acknowledged with "
          "the join, so it goes without its path");
  }
  spdlog::default_logger()->flush();
  const std::string log = g_log.str();
  CHECK(log.find("[session] cyb: joining as ServerId 60, PlayerId 1") !=
                std::string::npos &&
            log.find("sent PlayerController open on channel 3 (2 parts)") !=
                std::string::npos &&
            log.find("sent GameState open (tier 2) on channel 4") !=
                std::string::npos &&
            log.find("sent PlayerState open on channel 6") != std::string::npos,
        "the log has the join and each step sent: name, channel, parts");
  CHECK(log.find("default spawn") != std::string::npos,
        "and the setup's notes (the default spawn)");
  CHECK(log.find(kTicket) == std::string::npos, "the ticket is not in the log");
}

static void test_join_through_endpoint() {
  std::printf("a join through the endpoint\n");
  Fixture fx;
  EndpointConfig cfg;
  cfg.players = fx.world.get();
  Rig rig(cfg);
  g_log.str("");
  Joined j = handshake(rig, kA, t0);
  log_in(rig, j, kA, cyb_record(), t0);
  const Clock::time_point t_join = t0 + milliseconds(40);
  auto got = exchange(
      rig, j, kA, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)},
      t_join);
  CHECK(!find_message(got, conn::Nmt::Failure) && rig.ep.session_count() == 1,
        "Netspeed and Join are accepted");

  auto want_pb =
      expected_bootstrap(*fx.content, cyb_record(), world_time_at(t_join),
                         bs::GameStateMode::Tier2);
  const bs::PlayerSetup setup = want_pb->setup();
  auto seq = want_pb->join_sequence();
  CHECK(seq && seq->size() == 16,
        "the bootstrap's join sequence has 16 chains");
  if (!seq) {
    return;
  }
  CHECK(j.got.size() == 16, "the client receives 16 chains after Join");
  const std::string diff = compare_sends(j.got, 0, *seq, kChannels);
  CHECK(diff.empty(), "they are the bootstrap's sequence, bit for bit and in "
                      "order, on channels 3, 4, 5, 6");
  if (!diff.empty()) {
    std::printf("    %s\n", diff.c_str());
  }
  const std::array<uint32_t, 15> order = {3, 3, 3, 3, 4, 3, 5, 5,
                                          5, 5, 5, 3, 3, 5, 6};
  bool channels_ok = j.got.size() == 16;
  for (size_t i = 0; channels_ok && i < order.size(); ++i) {
    channels_ok = j.got[i].channel == order[i];
  }
  CHECK(channels_ok,
        "the order across channels: PlayerController x4, GameState, "
        "PlayerController, pawn x5, PlayerController x2, pawn, PlayerState");
  const bool opens_ok = j.got.size() == 16 && j.got[0].open && j.got[4].open &&
                        j.got[6].open && j.got[14].open;
  const bool non_opens_ok =
      j.got.size() == 16 && !j.got[1].open && !j.got[5].open && !j.got[13].open;
  CHECK(opens_ok && non_opens_ok,
        "the four actors' first chains open their channels and no other does");
  verify_join_opens_and_states(j, setup);
  verify_join_acks_and_logs(rig, j, t_join);
}

static void test_setup_refused_at_join() {
  std::printf("a character the bootstrap refuses\n");
  Fixture fx;
  EndpointConfig cfg;
  cfg.players = fx.world.get();
  Rig rig(cfg);
  g_log.str("");
  Joined j = handshake(rig, kA, t0);
  svc::Character rec = cyb_record();
  rec.race_id = 8;
  log_in(rig, j, kA, rec, t0);
  auto got = exchange(rig, j, kA, {j.client->nmt(conn::Nmt::Join)},
                      t0 + milliseconds(40));
  const conn::ControlMessage *failure = find_message(got, conn::Nmt::Failure);
  CHECK(failure && failure->error == login_text::kRaceUnsupported &&
            closed_by_server(got),
        "Join of a Tulnar (a race that is never created): NMT_Failure with the "
        "race's own text and the channel closes");
  CHECK(j.got.empty(), "nothing was spawned");
  spdlog::default_logger()->flush();
  CHECK(g_log.str().find("cannot join the world") != std::string::npos &&
            g_log.str().find("race") != std::string::npos,
        "the log has the reason: the race id");

  Rig other(cfg);
  Joined k = handshake(other, kB, t0);
  svc::Character broken = cyb_record();
  broken.appearance_json = "[";
  log_in(other, k, kB, broken, t0);
  got = exchange(other, k, kB, {k.client->nmt(conn::Nmt::Join)},
                 t0 + milliseconds(40));
  failure = find_message(got, conn::Nmt::Failure);
  CHECK(failure && failure->error == login_text::kLoginFailed &&
            closed_by_server(got),
        "(control: broken appearance JSON is refused with the generic 'Login "
        "failed')");
}

static void test_channels() {
  std::printf("actor channels\n");
  Harness h;
  CHECK(h.join(), "a session joins a fresh connection");
  CHECK(h.s->channel_of(bs::Role::PlayerController) == 3 &&
            h.s->channel_of(bs::Role::GameState) == 4 &&
            h.s->channel_of(bs::Role::Pawn) == 5 &&
            h.s->channel_of(bs::Role::PlayerState) == 6,
        "it takes the lowest free indexes from 3: 3, 4, 5, 6");

  Harness busy;
  Chain open;
  open.open = true;
  open.payload.write_bits(1, 8);
  for (uint32_t ch : {3u, 4u, 5u}) {
    open.channel = ch;
    busy.conn.send_chain(open);
  }
  busy.conn.close_channel(4);
  CHECK(busy.conn.channel_open(3) && busy.conn.channel_closing(4) &&
            busy.conn.channel_open(5),
        "(channels 3 and 5 are open, 4 is closing)");
  CHECK(busy.join(), "a session joins a connection that has actor channels");
  CHECK(busy.s->channel_of(bs::Role::PlayerController) == 6 &&
            busy.s->channel_of(bs::Role::GameState) == 7 &&
            busy.s->channel_of(bs::Role::Pawn) == 8 &&
            busy.s->channel_of(bs::Role::PlayerState) == 9,
        "it skips the open and the closing ones: 6, 7, 8, 9");
  busy.settle();
  CHECK(busy.on_channel(6).size() == 8 && busy.on_channel(7).size() == 1 &&
            busy.on_channel(8).size() == 6 && busy.on_channel(9).size() == 1,
        "and sends on them: eight chains on the PlayerController's (the "
        "XPTracker's last), one on the GameState's, six on the pawn's, one on "
        "the PlayerState's");
  bool filler = false;
  for (const auto &d : busy.got) {
    if (d.channel < 6) {
      filler = true;
    }
  }
  CHECK(filler,
        "(the filler's own chains on 3, 4 and 5 reached the client as well)");
}

static void mark_all_join_exports(bs::PlayerBootstrap &pb) {
  auto seq = pb.join_sequence();
  if (!seq) {
    return;
  }
  for (const bs::Send &snd : *seq) {
    for (const auto &bunch : snd.emission.exported_by_bunch) {
      pb.mark_exported(bunch);
    }
  }
}

static void verify_acked_client_load(Harness &h) {
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  const size_t before = h.got.size();
  h.client_sends(
      {h.client.actor(3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
  h.settle();
  const auto pc = h.on_channel(3, before);
  CHECK(pc.size() == 6, "ServerRequestClientLoad: six chains");
  std::vector<std::string> names;
  for (const auto *d : pc) {
    names.push_back(rpc_names(*d, pc_cls).empty() ? ""
                                                  : rpc_names(*d, pc_cls)[0]);
  }
  const std::vector<std::string> expect = {
      "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
      "ClientReceiveStoragePayload", "ClientReceiveStoragePayload",
      "ClientUpdateGroup",           "ClientReceiveActionbarSlotPayload"};
  CHECK(names == expect,
        "four storage payloads (Backpack, Equipment, QuestStorage, the "
        "resource storage), the group and the action bar, in that order");
  bool no_exports = true;
  for (const auto *d : pc) {
    no_exports = no_exports && d->exports.empty();
  }
  CHECK(no_exports, "all acknowledged: no export bunch in any of them (the "
                    "join's acks reached mark_exported)");
  CHECK(h.s->exports_tracked() == 0, "(and the replies, which carried no "
                                     "export, left nothing tracked either)");
  const auto *backpack = pc.size() > 0 ? pc[0] : nullptr;
  repl::ActionbarPayload bar;
  auto pb_acked =
      expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0),
                         bs::GameStateMode::Tier2, &bar);
  mark_all_join_exports(*pb_acked);
  const auto want_backpack = pb_acked->storage_payload(
      bs::StorageKind::Backpack, bs::Role::PlayerController);
  CHECK(backpack && want_backpack &&
            same_bits(backpack->content, want_backpack->emission.payload) &&
            want_backpack->emission.exports.empty(),
        "the Backpack payload equals the bootstrap's after every export of the "
        "join is acknowledged");
  const auto want_bar = pb_acked->action_bar(bar);
  CHECK(pc.size() == 6 &&
            pc[4]->content.bit_pos() == bs::captured::kEmptyGroupBlock.bits,
        "the group is the original's 527 bits (pkt#606)");
  CHECK(pc.size() == 6 && want_bar &&
            same_bits(pc[5]->content, want_bar->emission.payload),
        "the action bar is the setup's own bar: the weapon bank and the "
        "class's skills");
}

static void verify_unacked_client_load() {
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  Harness fresh;
  fresh.client_receives = false;
  fresh.join();
  fresh.step();
  fresh.client_sends({fresh.client.actor(
      3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
  fresh.give_held();
  fresh.client_receives = true;
  fresh.step();
  repl::ActionbarPayload bar;
  auto pb =
      expected_bootstrap(*fresh.fx.content, cyb_record(), world_time_at(t0),
                         bs::GameStateMode::Tier2, &bar);
  const auto want_bar = pb->action_bar(bar);
  const auto fpc = fresh.on_channel(3);
  CHECK(!fpc.empty() && want_bar &&
            same_bits(fpc.back()->content, want_bar->emission.payload) &&
            fpc.size() >= 13,
        "(control: the same request from a client that acknowledged nothing "
        "yet)");
  size_t with_export = 0;
  for (size_t i = fpc.size() >= 6 ? fpc.size() - 6 : 0; i < fpc.size(); ++i) {
    with_export += fpc[i]->exports.empty() ? 0 : 1;
  }
  CHECK(with_export == 3,
        "its Backpack, Equipment and QuestStorage payloads carry the "
        "component's export (the resource storage has none)");
}

static void test_join_and_acks() {
  std::printf("the join and its acknowledgements\n");
  Harness h;
  CHECK(h.join() && !h.s->failed(), "join");
  CHECK(h.s->join_pending() == 0,
        "the whole join went to the connection at once (the windows had room)");
  h.step();
  const size_t join_packets = h.packets;
  const size_t join_bytes = h.bytes;
  h.settle();
  auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0),
                               bs::GameStateMode::Tier2);
  auto seq = pb->join_sequence();
  CHECK(seq && h.got.size() == 16 &&
            compare_sends(h.got, 0, *seq, kChannels).empty(),
        "16 chains, the bootstrap's sequence in order");
  CHECK(!h.s->failed() && h.s->replies_pending() == 0,
        "nothing pending, nothing failed");
  CHECK(h.s->exports_tracked() == 0,
        "every export part of the join had its final ack event: no chain is "
        "tracked any more");
  std::printf("    the join of cyb (Tier 2) is %zu packets, %zu bytes (the "
              "original's: 64 packets, 51 KB in its first 174 ms)\n",
              join_packets, join_bytes);
  // C.O.A + C.Y.B
  verify_acked_client_load(h);
  verify_unacked_client_load();
}

static void verify_phase_in_blocks(Harness &h, const fake::Delivered &d,
                                   const schema::ClassSchema &pawn_cls) {
  const auto blocks =
      repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
  CHECK(blocks && blocks->size() == 2, "the actor's block and one subobject's");
  if (!blocks || blocks->size() != 2) {
    return;
  }
  const repl::BlockView &actor = (*blocks)[0];
  const repl::BlockView &sub = (*blocks)[1];
  size_t p = actor.payload_pos;
  const auto a = repl::read_rep_block(
      pawn_cls, d.content.data(), actor.payload_pos + actor.payload_bits, p);
  const schema::ClassSchema &info =
      *gen::find_component_schema("BaseCharacterInfo");
  size_t q = sub.payload_pos;
  const auto b = repl::read_rep_block(info, d.content.data(),
                                      sub.payload_pos + sub.payload_bits, q);
  const bool headers_ok =
      a && b && actor.header.is_actor &&
      sub.header.object == h.s->bootstrap().setup().guids.base_character_info;
  const bool vals_ok =
      prop_eq(a, pawn_cls, "bHidden", repl::Value::boolean(false)) &&
      prop_eq(b, info, "bIsPhasedOut", repl::Value::boolean(false)) &&
      prop_eq(b, info, "bClientIsReadyToPhaseIn", repl::Value::boolean(true));
  CHECK(headers_ok && vals_ok,
        "bHidden 0, BaseCharacterInfo.bIsPhasedOut 0 and "
        "bClientIsReadyToPhaseIn 1 (the original's pkt#2523)");
}

static void test_phase_in_once(Harness &h, uint32_t pawn_ch) {
  const schema::ClassSchema &pawn_cls = class_named("PlayerPawn_C");
  const size_t before = h.got.size();
  h.client_sends({h.client.actor(
      pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
  h.settle();
  const auto phase = h.on_channel(pawn_ch, before);
  CHECK(phase.size() == 1 && phase[0]->reliable && phase[0]->exports.empty(),
        "ServerClientIsReadyToPhaseIn: one reliable update on the pawn's "
        "channel, no exports");
  if (phase.size() == 1) {
    verify_phase_in_blocks(h, *phase[0], pawn_cls);
  }
  h.client_sends({h.client.actor(
      pawn_ch, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)});
  h.settle();
  CHECK(h.on_channel(pawn_ch, before).size() == 1,
        "a second request is not answered again: once");
}

static void verify_moves_log_and_mangled(Harness &h, uint32_t pawn_ch) {
  spdlog::default_logger()->flush();
  const std::string log = g_log.str();
  CHECK(log.find("the client is ready to phase in") != std::string::npos &&
            log.find("sent phase-in on channel 5") != std::string::npos &&
            log.find("the first move is acknowledged (TimeStamp 0.81252") !=
                std::string::npos,
        "the log has the phase-in and the first move acknowledged (once)");
  CHECK(count_lines(log, "the first move is acknowledged") == 1,
        "the move acknowledgement is logged once, not per move");
  BunchWriter damaged = hex_bits(capture_bootstrap::kClientMovePacked1);
  BunchWriter cut;
  const auto bytes =
      capture_blocks::bytes_of(capture_bootstrap::kClientMovePacked1.hex);
  std::vector<uint8_t> mangled(bytes.begin(), bytes.end());
  mangled[6] ^= 0x04;
  cut.write_bit_range(mangled.data(), 0, damaged.bit_pos());
  const size_t acked = h.s->moves_acknowledged();
  h.client_sends({h.client.actor(pawn_ch, cut, false)});
  h.step();
  CHECK(h.s->moves_acknowledged() == acked,
        "a ServerMovePacked whose parameter does not read as the capture's is "
        "not acknowledged");
}

static void test_moves_and_mangled(Harness &h, uint32_t pawn_ch) {
  const std::array<const capture_bootstrap::Bits *, 4> moves = {
      &capture_bootstrap::kClientMovePacked1,
      &capture_bootstrap::kClientMovePacked2,
      &capture_bootstrap::kClientMovePacked3,
      &capture_bootstrap::kClientMovePacked4};
  auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0),
                               bs::GameStateMode::Tier2);
  const size_t moves_from = h.got.size();
  std::vector<float> stamps;
  bool all_legacy = true;
  for (const auto *m : moves) {
    const BunchWriter bits = hex_bits(*m);
    const auto legacy = legacy_time_stamp(bits);
    all_legacy &= legacy.has_value();
    stamps.push_back(legacy.value_or(-1.0f));
    h.client_sends({h.client.actor(pawn_ch, bits, false)});
  }
  h.step();
  CHECK(all_legacy && stamps[0] > 0.8f && stamps[0] < 0.82f &&
            stamps[0] < stamps[1] && stamps[1] < stamps[2],
        "(the captured moves' TimeStamps read by the legacy parser: 0.8125 s "
        "and rising)");
  const auto acks = h.on_channel(pawn_ch, moves_from);
  CHECK(acks.size() == 4, "each ServerMovePacked is answered");
  bool unreliable = true;
  bool stamps_ok = true;
  bool bits_ok = true;
  for (size_t i = 0; i < std::min(acks.size(), stamps.size()); ++i) {
    unreliable &= (!acks[i]->reliable && acks[i]->exports.empty() &&
                   acks[i]->content.bit_pos() == 68);
    stamps_ok &= (response_time_stamp(*acks[i]) == stamps[i]);
    const auto want = pb->good_move_ack(stamps[i]);
    bits_ok &= (want && same_bits(acks[i]->content, want->emission.payload));
  }
  CHECK(unreliable, "unreliable, 68 bits, on the pawn's channel");
  CHECK(stamps_ok,
        "ClientMoveResponsePacked carries the move's own TimeStamp (the legacy "
        "server's parse of the same bunch), also the 533-bit bunch's");
  CHECK(bits_ok, "equal to the bootstrap's good_move_ack, which equals the "
                 "original's 68 bits (test_worldd_bootstrap)");
  CHECK(h.s->moves_acknowledged() == 4, "four moves acknowledged");
  verify_moves_log_and_mangled(h, pawn_ch);
}

static void test_phase_in_and_moves() {
  std::printf("phase-in and moves\n");
  Harness h;
  h.join();
  h.settle();
  const uint32_t pawn_ch = h.s->channel_of(bs::Role::Pawn);
  test_phase_in_once(h, pawn_ch);
  test_moves_and_mangled(h, pawn_ch);
}

int main() {
  auto logger = std::make_shared<spdlog::logger>(
      "test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
  logger->set_level(spdlog::level::debug);
  spdlog::set_default_logger(logger);

  test_identity();
  test_content_missing();
  test_content_built_before_b2();
  test_stat_program_problems();
  test_join_through_endpoint();
  test_setup_refused_at_join();
  test_channels();
  test_join_and_acks();
  test_phase_in_and_moves();

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
