// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.

#include "capture_blocks.h"
#include "capture_bootstrap.h"
#include "capture_packets.h"
#include "protocol/wire/server_move_parser.h"
#include "test_world_session_common.h"
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

static svc::Character second_record() {
  svc::Character c = cyb_record();
  c.id = "11112222333344445555666677778888";
  c.name = "dee";
  return c;
}

Joined handshake(Rig &rig, const Address &addr, Clock::time_point now) {
  Joined j;
  j.addr = addr;
  rig.take(addr);
  const auto initial = capture::bytes(capture::kInitial);
  rig.ep.on_datagram(addr, initial.data(), initial.size(), now);
  auto out = rig.take(addr);
  if (out.size() != 1) {
    return j;
  }
  j.challenge = conn::parse_handshake(out[0].data(), out[0].size());
  conn::HandshakePacket resp = *j.challenge;
  resp.type = conn::HandshakeType::Response;
  resp.sent_count = 2;
  resp.padding.assign(11, 0x5A);
  const auto bytes = conn::build_handshake(resp);
  rig.ep.on_datagram(addr, bytes.data(), bytes.size(), now);
  out = rig.take(addr);
  if (out.size() != 1) {
    return j;
  }
  const auto seqs = conn::initial_sequences(j.challenge->cookie);
  j.client.emplace(conn::outer_header_of(*j.challenge),
                   j.challenge->ext->custom_field(), seqs.server, seqs.client);
  j.session =
      rig.logins.submitted.empty() ? 0 : rig.logins.submitted.back().first;
  return j;
}

std::vector<fake::Received> drain(Rig &rig, Joined &j, const Address &addr) {
  std::vector<fake::Received> got;
  for (const auto &p : rig.take(addr)) {
    got.push_back(j.client->receive(p));
    for (const auto &d : got.back().delivered) {
      j.got.push_back(d);
    }
  }
  return got;
}

std::vector<fake::Received> exchange(Rig &rig, Joined &j, const Address &addr,
                                     std::vector<fake::Bunch> bunches,
                                     Clock::time_point now) {
  const auto pkt = j.client->packet(std::move(bunches));
  rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
  rig.ep.tick(now);
  return drain(rig, j, addr);
}

const conn::ControlMessage *
find_message(const std::vector<fake::Received> &got, conn::Nmt type) {
  for (const auto &r : got) {
    for (const auto &m : r.messages) {
      if (m.type == type) {
        return &m;
      }
    }
  }
  return nullptr;
}

bool closed_by_server(const std::vector<fake::Received> &got) {
  for (const auto &r : got) {
    if (r.closed) {
      return true;
    }
  }
  return false;
}

LoginResult cyb_login() {
  LoginResult r;
  r.accepted = true;
  r.character = cyb_record();
  return r;
}

void log_in_as(Rig &rig, Joined &j, const svc::Character &rec,
               Clock::time_point now, const std::string &machine) {
  exchange(rig, j, j.addr, {j.client->hello()}, now);
  LoginResult r;
  r.accepted = true;
  r.character = rec;
  rig.ep.post_login_result(j.session, r);
  rig.ep.tick(now + milliseconds(1));
  drain(rig, j, j.addr);
  exchange(rig, j, j.addr, {j.client->login(machine, machine)},
           now + milliseconds(2));
}

void log_in(Rig &rig, Joined &j, const Address &addr, const svc::Character &rec,
            Clock::time_point now) {
  j.addr = addr;
  log_in_as(rig, j, rec, now, kMachine);
}

void join_now(Rig &rig, Joined &j, const Address &addr, Clock::time_point now) {
  const auto p = j.client->packet(
      {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)});
  rig.ep.on_datagram(addr, p.data(), p.size(), now);
  rig.ep.tick(now);
  drain(rig, j, addr);
}

static void test_client_driven_log_lines() {
  std::printf("the log lines the client can cause\n");
  Harness h;
  h.join();
  h.settle();
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
  repl::RpcParams mine;
  repl::set_rpc_param(*pc_cls.rpc("ServerAcknowledgePossession"), mine, "P",
                      repl::Value(h.s->bootstrap().setup().guids.pawn));
  const BunchWriter ack =
      client_rpc(pc_cls, "ServerAcknowledgePossession", mine);
  g_log.str("");
  h.client_repeats(pc_ch, ack, 1000);
  spdlog::default_logger()->flush();
  const std::string log = g_log.str();
  CHECK(count_lines(log, "acknowledged possession") ==
            PlayerSession::kLoggedPerKind,
        "1,000 possession acknowledgements: 'acknowledged possession' is "
        "logged kLoggedPerKind (3) times, the rest at debug in other words");
  CHECK(count_lines(log, "possession ack #") ==
            1000 - PlayerSession::kLoggedPerKind,
        "(the debug lines count the rest)");
  const BunchWriter load = client_rpc(pc_cls, "ServerRequestClientLoad");
  g_log.str("");
  h.client_sends({h.client.actor(pc_ch, load, true)});
  h.settle();
  spdlog::default_logger()->flush();
  const std::string lines = g_log.str();
  CHECK(
      count_lines(lines, "[info] [session] cyb: sent ") ==
              PlayerSession::kLoggedPerKind &&
          count_lines(lines, "[debug] [session] cyb: sent ") == 3,
      "a load's six reply chains: three 'sent' lines at info, three at debug");
}

static void test_two_players() {
  std::printf("two players at once\n");
  Fixture fx;
  EndpointConfig cfg;
  cfg.players = fx.world.get();
  Rig rig(cfg);
  Joined a = handshake(rig, kA, t0);
  Joined b = handshake(rig, kB, t0 + milliseconds(1));
  log_in_as(rig, a, cyb_record(), t0, kMachine);
  log_in_as(rig, b, second_record(), t0 + milliseconds(3),
            "DESKTOP-OTHER-0000000000000000000000000000");
  CHECK(a.session != b.session, "two sessions");
  const auto pa = a.client->packet(
      {a.client->netspeed(200000), a.client->nmt(conn::Nmt::Join)});
  const auto pb = b.client->packet(
      {b.client->netspeed(200000), b.client->nmt(conn::Nmt::Join)});
  rig.ep.on_datagram(kA, pa.data(), pa.size(), t0 + milliseconds(40));
  rig.ep.on_datagram(kB, pb.data(), pb.size(), t0 + milliseconds(40));
  rig.ep.tick(t0 + milliseconds(40));
  drain(rig, a, kA);
  drain(rig, b, kB);
  CHECK(a.got.size() == 16 && b.got.size() == 16, "each client has 16 chains");
  if (a.got.size() < 15 || b.got.size() < 15) {
    return;
  }
  const NewActor gsa = read_new_actor(a.got[4].content, false);
  const NewActor gsb = read_new_actor(b.got[4].content, false);
  const NewActor pca = read_new_actor(a.got[0].content, true);
  const NewActor pcb = read_new_actor(b.got[0].content, true);
  const NewActor pwa = read_new_actor(a.got[6].content, false);
  const NewActor pwb = read_new_actor(b.got[6].content, false);
  const NewActor psa = read_new_actor(a.got[14].content, false);
  const NewActor psb = read_new_actor(b.got[14].content, false);
  CHECK(gsa.actor == gsb.actor,
        "the GameState GUID is the same for both players");
  CHECK(!(pca.actor == pcb.actor) && !(pwa.actor == pwb.actor) &&
            !(psa.actor == psb.actor),
        "PlayerController, pawn and PlayerState GUIDs differ");
  CHECK(!(psa.actor == pcb.actor) && !(pca.actor == pwb.actor),
        "no GUID of one is a GUID of the other");
  const schema::ClassSchema &ps_cls = class_named("AoCPlayerStateBP_C");
  const auto sa = block_of(a.got[14], psa.end, true, {}, ps_cls);
  const auto sb = block_of(b.got[14], psb.end, true, {}, ps_cls);
  const repl::Value *ida = sa ? sa->get(ps_cls, "PlayerId") : nullptr;
  const repl::Value *idb = sb ? sb->get(ps_cls, "PlayerId") : nullptr;
  CHECK(ida && idb && *ida == repl::Value::i64(1) &&
            *idb == repl::Value::i64(2),
        "PlayerId 1 and 2");
  CHECK(rig.ep.session_count() == 2, "two sessions live");
}

static void scan_kicked_packets(fake::Client &client,
                                const std::vector<std::vector<uint8_t>> &pkts,
                                bool &failure, bool &closed) {
  for (const auto &p : pkts) {
    auto r = client.receive(p);
    for (const auto &m : r.messages) {
      if (m.type == conn::Nmt::Failure) {
        failure = true;
      }
    }
    if (r.closed) {
      closed = true;
    }
  }
}

static void test_kicked_session_answers_nothing() {
  std::printf("a duplicate login kicks a session whose join is still "
              "unacknowledged, and the kicked one answers nothing more\n");
  Fixture fx;
  EndpointConfig cfg;
  cfg.players = fx.world.get();
  Rig rig(cfg);
  Joined a = handshake(rig, kA, t0);
  log_in(rig, a, kA, cyb_record(), t0);
  const auto pa = a.client->packet(
      {a.client->netspeed(200000), a.client->nmt(conn::Nmt::Join)});
  rig.ep.on_datagram(kA, pa.data(), pa.size(), t0 + milliseconds(40));
  rig.ep.tick(t0 + milliseconds(40));
  auto held = rig.take(kA);
  Joined b = handshake(rig, kB, t0 + milliseconds(50));
  log_in(rig, b, kB, cyb_record(), t0 + milliseconds(60));
  CHECK(rig.ep.session_of(kCyb) && *rig.ep.session_of(kCyb) == kB,
        "the character is B's now");
  bool failure = false;
  bool closed = false;
  scan_kicked_packets(*a.client, held, failure, closed);
  scan_kicked_packets(*a.client, rig.take(kA), failure, closed);
  CHECK(failure && closed, "A got the Failure and the close after its join");
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  const auto pkt = a.client->packet({a.client->actor(
      3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
  rig.ep.on_datagram(kA, pkt.data(), pkt.size(), t0 + milliseconds(100));
  rig.ep.tick(t0 + milliseconds(100));
  size_t chains = 0;
  for (const auto &p : rig.take(kA)) {
    chains += a.client->receive(p).delivered.size();
  }
  CHECK(chains == 0, "a kicked (closing) session answers no further requests");
  join_now(rig, b, kB, t0 + milliseconds(110));
  CHECK(b.got.size() == 16, "B receives the full join");
  for (int i = 1; i <= 40; ++i) {
    rig.ep.tick(t0 + milliseconds(110 + i * 100));
  }
  CHECK(rig.ep.session_count() == 1, "A is dropped, B stays");
}
// [C | O | A - Cyb]

static std::string cyb_with_decals(size_t n) {
  std::string json = cyb::kAppearanceJsonb;
  const size_t close = json.rfind('}');
  std::string decals = ",\"decalData\":[";
  for (size_t i = 0; i < n; ++i) {
    if (i) {
      decals += ",";
    }
    decals +=
        "{\"guid\":\"" + std::to_string(1000 + i) +
        "\",\"strength\":1.0,\"scaleU\":1.0,\"scaleV\":1.0,\"slotIndex\":" +
        std::to_string(i % 200) +
        ",\"mergeSectionMaterialBase\":\"5\",\"offsetU\":0.5,\"offsetV\":0.5,"
        "\"angle\":0.25,\"color\":\"7\",\"discoloration\":0.1}";
  }
  decals += "]";
  json.insert(close, decals);
  return json;
}

static std::string morphs_only(size_t n) {
  std::string json = "{\"faceMorphWeightMaps\":{";
  for (size_t i = 0; i < n; ++i) {
    if (i) {
      json += ",";
    }
    json += "\"m" + std::to_string(i) + "\":0.5";
  }
  return json + "}}";
}

static void run_unsendable_join_case(EndpointConfig cfg, const char *what,
                                     const std::string &json) {
  Rig rig(cfg);
  g_log.str("");
  Joined a = handshake(rig, kA, t0);
  svc::Character rec = cyb_record();
  rec.appearance_json = json;
  log_in(rig, a, kA, rec, t0);
  auto got = exchange(
      rig, a, kA, {a.client->netspeed(200000), a.client->nmt(conn::Nmt::Join)},
      t0 + milliseconds(40));
  auto more = exchange(rig, a, kA, {}, t0 + milliseconds(100));
  const conn::ControlMessage *failure = find_message(got, conn::Nmt::Failure);
  if (!failure) {
    failure = find_message(more, conn::Nmt::Failure);
  }
  spdlog::default_logger()->flush();
  const std::string log = g_log.str();
  std::printf(
      "    %s: appearance JSON of %zu bytes, %zu chains arrived, Failure %d\n",
      what, rec.appearance_json.size(), a.got.size(), int(failure != nullptr));
  if (std::string(what) == "100 decals") {
    CHECK(!failure && a.got.size() == 16,
          "(control: an appearance with 100 decals, a pawn open of many parts, "
          "joins)");
  } else if (std::string(what) == "5000 morphs") {
    CHECK(rec.appearance_json.size() < bs::kMaxAppearanceBytes,
          "(5,000 morphs are under the appearance cap)");
    CHECK(failure && failure->error == login_text::kLoginFailed,
          "a join that cannot be sent (the pawn open's content is over what a "
          "chain may carry) ends with the Failure");
    CHECK(log.find("the join cannot be sent: pawn open") != std::string::npos,
          "and the log says which chain was refused");
    for (int i = 1; i <= 40; ++i) {
      rig.ep.tick(t0 + milliseconds(100 + i * 100));
    }
    CHECK(rig.ep.session_count() == 0,
          "the session is dropped after the linger");
  } else {
    CHECK(
        failure && failure->error == login_text::kLoginFailed &&
            a.got.empty() &&
            log.find("appearance: too large") != std::string::npos,
        "1,500 decals (271 KB of JSON) are refused at the setup, before "
        "anything is built: the Failure, nothing sent, 'too large' in the log");
  }
}

static void test_unsendable_join() {
  std::printf("a join that can never be sent\n");
  Fixture fx;
  EndpointConfig cfg;
  cfg.players = fx.world.get();
  run_unsendable_join_case(cfg, "100 decals", cyb_with_decals(100));
  run_unsendable_join_case(cfg, "5000 morphs", morphs_only(5000));
  run_unsendable_join_case(cfg, "1500 decals", cyb_with_decals(1500));
}

static void test_acks_before_requests() {
  std::printf("acknowledgements and a request in the same datagram\n");
  Fixture fx;
  EndpointConfig cfg;
  cfg.players = fx.world.get();
  Rig rig(cfg);
  Joined a = handshake(rig, kA, t0);
  log_in(rig, a, kA, cyb_record(), t0);
  join_now(rig, a, kA, t0 + milliseconds(40));
  CHECK(a.got.size() == 16, "(the join arrived)");
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  const size_t before = a.got.size();
  const auto req = a.client->packet({a.client->actor(
      3, client_rpc(pc_cls, "ServerRequestClientLoad"), true)});
  rig.ep.on_datagram(kA, req.data(), req.size(), t0 + milliseconds(100));
  rig.ep.tick(t0 + milliseconds(100));
  drain(rig, a, kA);
  size_t exports = 0;
  size_t chains = 0;
  for (size_t i = before; i < a.got.size(); ++i) {
    ++chains;
    exports += a.got[i].exports.size();
  }
  CHECK(chains == 6 && exports == 0,
        "the acks in a datagram are applied before its requests: the reply "
        "leaves out the exports the datagram acknowledged");
}

// built by CoA with C.Y.B

static void write_fstring_utf16(BunchWriter &w, const std::u16string &s) {
  w.write_int32(-static_cast<int32_t>(s.size() + 1));
  for (char16_t c : s) {
    w.write_bits(c, 16);
  }
  w.write_bits(0, 16);
}

static std::optional<conn::ControlMessage> parse_bits(const BunchWriter &w) {
  return conn::parse_control_message(w.data(), w.byte_size(), 0, w.bit_pos());
}

static BunchWriter login_base_bunch() {
  BunchWriter w;
  w.write_uint8(static_cast<uint8_t>(conn::Nmt::Login));
  w.write_fstring_ansi("0");
  w.write_fstring_ansi("?Name=cyb");
  return w;
}

static bool is_login_without_id(const std::optional<conn::ControlMessage> &m) {
  return m && m->type == conn::Nmt::Login && !m->has_unique_id &&
         m->request_url == "?Name=cyb";
}

static void test_login_tail_truncated_and_flags() {
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(8);
    w.write_fstring_ansi("cyb-id");
    w.write_fstring_ansi("NULL");
    const auto m = parse_bits(w);
    CHECK(m && m->has_unique_id && m->unique_id == "cyb-id" &&
              m->platform_name == "NULL" && m->request_url == "?Name=cyb",
          "the capture's form: flags 8, the id, the platform");
  }
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(8);
    w.write_int32(500);
    w.write_uint8('a');
    w.write_uint8('b');
    w.write_uint8('c');
    CHECK(is_login_without_id(parse_bits(w)),
          "a truncated id (count 500, 3 bytes): still a Login, without the "
          "unique id (the reader is restored)");
  }
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(8);
    w.write_fstring_ansi("cyb-id");
    CHECK(is_login_without_id(parse_bits(w)),
          "an id without a platform: a Login, no unique id");
  }
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(9);
    w.write_uint32(0xdeadbeef);
    w.write_uint32(0xdeadbeef);
    CHECK(is_login_without_id(parse_bits(w)),
          "flags 9 (encoded): a Login, no unique id");
  }
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(0x10);
    w.write_fstring_ansi("x");
    w.write_fstring_ansi("NULL");
    CHECK(is_login_without_id(parse_bits(w)),
          "flags 0x10 (another subsystem): a Login, no unique id (only the "
          "NULL form is read)");
  }
}

static void test_login_tail_encodings_and_bounds() {
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(8);
    write_fstring_utf16(w, u"Müller-PC");
    w.write_fstring_ansi("NULL");
    const auto m = parse_bits(w);
    CHECK(
        m && m->has_unique_id && m->unique_id.size() == 9,
        "a UTF-16 id parses (its non-ASCII character as read_fstring maps it)");
  }
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(8);
    w.write_int32(1025);
    for (int i = 0; i < 1025; ++i) {
      w.write_uint8('a');
    }
    w.write_fstring_ansi("NULL");
    CHECK(is_login_without_id(parse_bits(w)),
          "an id over 1024 characters: a Login, no unique id");
  }
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(8);
    w.write_int32(std::numeric_limits<int32_t>::min());
    w.write_fstring_ansi("NULL");
    CHECK(is_login_without_id(parse_bits(w)),
          "an INT32_MIN length: a Login, no unique id");
  }
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(8);
    w.write_int32(0);
    w.write_int32(0);
    const auto m = parse_bits(w);
    CHECK(m && m->has_unique_id && m->unique_id.empty(),
          "an empty id and platform: has_unique_id with an empty id");
    std::vector<std::string> notes;
    const auto id = make_login_identity(LoginMessage{m ? m->request_url : "",
                                                     m && m->has_unique_id,
                                                     m ? m->unique_id : ""},
                                        cyb_record(), &notes);
    CHECK(id.unique_id == "cyb",
          "...and the identity falls back to the name option");
  }
}

static void test_login_tail_garbage_and_stray() {
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(8);
    w.write_fstring_ansi("id");
    w.write_fstring_ansi("NULL");
    for (int i = 0; i < 40; ++i) {
      w.write_uint8(0xAB);
    }
    const auto m = parse_bits(w);
    CHECK(m && m->has_unique_id && m->unique_id == "id",
          "trailing garbage after the platform is ignored");
  }
  {
    BunchWriter w = login_base_bunch();
    w.write_uint8(8);
    w.write_int32(10);
    const uint8_t raw[10] = {'a', 'b', 0, 'z', 'z', 'z', 'z', 'z', 'z', 'z'};
    for (uint8_t c : raw) {
      w.write_uint8(c);
    }
    w.write_fstring_ansi("NULL");
    const auto m = parse_bits(w);
    CHECK(m && m->type == conn::Nmt::Login,
          "an id with an embedded NUL still parses as a Login");
  }
  {
    BunchWriter w = login_base_bunch();
    w.write_bits(1, 7);
    CHECK(is_login_without_id(parse_bits(w)),
          "7 stray bits after the URL: a Login, no unique id");
  }
  {
    BunchWriter w;
    w.write_uint8(static_cast<uint8_t>(conn::Nmt::Login));
    w.write_fstring_ansi("0");
    w.write_int32(300);
    CHECK(!parse_bits(w), "a truncated URL: unreadable");
  }
}

static void test_login_tail() {
  std::printf("NMT_Login's unique id in hostile and odd forms\n");
  test_login_tail_truncated_and_flags();
  test_login_tail_encodings_and_bounds();
  test_login_tail_garbage_and_stray();
}
// C-O-A.C.Y.B

static void test_real_content_db() {
  std::printf("against the real content.db (AOC_CONTENT_DB)\n");
  const char *env = std::getenv("AOC_CONTENT_DB");
  if (!env || !*env) {
    std::printf("  [skip] AOC_CONTENT_DB is not set (cd services && go run "
                "./cmd/contentc; AOC_CONTENT_DB=dist\\content\\content.db)\n");
    return;
  }
  for (const auto mode : {bs::GameStateMode::Tier1, bs::GameStateMode::Tier2,
                          bs::GameStateMode::Captured}) {
    Harness h(mode, env);
    const bool joined = h.join();
    h.settle();
    auto pb = expected_bootstrap(*h.fx.content, cyb_record(), world_time_at(t0),
                                 mode);
    auto seq = pb->join_sequence();
    const std::string diff =
        joined && seq ? compare_sends(h.got, 0, *seq, kChannels) : "no join";
    const char *name =
        mode == bs::GameStateMode::Tier1
            ? "tier1"
            : (mode == bs::GameStateMode::Tier2 ? "tier2" : "captured");
    std::string msg =
        std::string("the join against the real content.db, GameState ") + name +
        ": 16 chains equal to the bootstrap's, in order";
    CHECK(joined && !h.s->failed() && h.got.size() == 16 && diff.empty(),
          msg.c_str());
    if (!diff.empty()) {
      std::printf("    %s\n", diff.c_str());
    }
  }
}

static void test_combat_settings_echo_pkt620() {
  std::printf("the combat-settings echo carries all 50 values of the request (pkt#620)\n");
  Harness h;
  h.join();
  h.settle();
  const schema::ClassSchema &pc_cls = class_named("AoCPlayerControllerBP_C");
  const uint32_t pc_ch = h.s->channel_of(bs::Role::PlayerController);
  const size_t before = h.got.size();
  const BunchWriter possession = hex_bits(capture_blocks::kClientBunchPossession);
  h.client_sends({h.client.actor(pc_ch, possession, true)});
  h.settle();
  const auto pc = h.on_channel(pc_ch, before);
  if (pc.empty()) {
    CHECK(false, "the possession bunch produces the combat-settings echo");
    return;
  }
  const auto blocks = repl::read_content_blocks(pc[0]->content.data(), pc[0]->content.bit_pos(), 0);
  std::optional<repl::RepState> echo;
  if (blocks && blocks->size() == 1) {
    size_t p = (*blocks)[0].payload_pos;
    echo = repl::read_rep_block(pc_cls, pc[0]->content.data(), (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, p);
  }
  const BunchWriter original = hex_bits(capture_bootstrap::kCombatEcho);
  const auto ob = repl::read_content_blocks(original.data(), original.bit_pos(), 0);
  std::optional<repl::RepState> orig;
  if (ob && ob->size() == 1) {
    size_t p = (*ob)[0].payload_pos;
    orig = repl::read_rep_block(pc_cls, original.data(), (*ob)[0].payload_pos + (*ob)[0].payload_bits, p);
  }
  CHECK(echo && orig && echo->values().size() == 50 && pc[0]->reliable && pc[0]->exports.empty(),
        "the echo is one reliable PlayerController block of the request's 50 values");
  if (!echo || !orig) return;
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
  int equal = 0, party_vfx_differs = 0;
  for (const std::string_view path : k_pkt620) {
    const repl::Value *mine = echo->get(pc_cls, path);
    const repl::Value *theirs = orig->get(pc_cls, path);
    if (mine && theirs && *mine == *theirs) ++equal;
    else if (path.find("PartyVFX") != std::string_view::npos && mine && theirs) ++party_vfx_differs;
  }
  CHECK(equal == 8 && party_vfx_differs == 1,
        "eight of the nine values equal the original's (pkt#620); PartyVFX is the client's own");
}

void run_session_lifecycle() {
  test_client_driven_log_lines();
  test_combat_settings_echo_pkt620();
  test_two_players();
  test_kicked_session_answers_nothing();
  test_unsendable_join();
  test_acks_before_requests();
  test_login_tail();
  test_real_content_db();
}
