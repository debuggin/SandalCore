// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_world_node_layout_common.h"

#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

using namespace aoc::worldd;

namespace {

const conn::OuterHeader kOuter{0x500c7696u, 0, 5};
const conn::CustomField kCf{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d};
const Clock::time_point t0{seconds(1000)};

ConnectionConfig server_config() {
  ConnectionConfig c;
  c.outer = kOuter;
  c.custom_field = kCf;
  c.server_initial_seq = 14265;
  c.client_initial_seq = 15870;
  return c;
}

ConnectionConfig client_config() {
  ConnectionConfig c = server_config();
  std::swap(c.server_initial_seq, c.client_initial_seq);
  return c;
}

struct Seen {
  uint32_t channel;
  bool open, close, reliable;
  uint32_t reason;
  size_t bits;
};

struct Pair {
  Connection s{server_config(), t0};
  Connection cl{client_config(), t0};
  Clock::time_point t = t0;
  std::vector<Seen> seen;

  void step(ActorView *view, milliseconds dt = milliseconds(60)) {
    t += dt;
    for (const auto &p : s.flush(t)) {
      for (auto &b : cl.receive(p, t)) {
        if (b.channel != 0)
          seen.push_back(
              {b.channel, b.open, b.close, b.reliable, b.close_reason, b.bits});
      }
    }
    t += dt;
    for (const auto &p : cl.flush(t))
      s.receive(p, t);
    for (const AckEvent &e : s.take_ack_events()) {
      if (view)
        view->on_ack(e);
    }
  }
};

std::function<bool(uint32_t)> reserved_3_to_6() {
  return [](uint32_t i) { return i >= 3 && i <= 6; };
}

void run(ActorView &v, Pair &p, double x, double y, int steps = 1) {
  for (int i = 0; i < steps; ++i) {
    v.tick(p.s, x, y, p.t);
    p.step(&v, milliseconds(65));
  }
}

std::vector<Seen> take(Pair &p) {
  std::vector<Seen> out;
  out.swap(p.seen);
  return out;
}

size_t bits_on(const std::vector<Seen> &seen, uint32_t channel,
               bool *closed = nullptr, uint32_t *reason = nullptr) {
  size_t n = 0;
  for (const Seen &x : seen) {
    if (x.channel != channel)
      continue;
    n += x.bits;
    if (x.close) {
      if (closed)
        *closed = true;
      if (reason)
        *reason = x.reason;
    }
  }
  return n;
}

void check_view_walk_and_close(ActorView &v, Pair &p, uint64_t id, double jx,
                               double jy) {
  run(v, p, jx + 160000, jy, 4);
  CHECK(v.state_of(id) == world::ClientState::None && take(p).empty(),
        "at 1.6 km Joeva's replicator is not sent");
  run(v, p, jx + 140000, jy, 12);
  CHECK(
      v.state_of(id) == world::ClientState::Awake && v.channel_of(id) == 7,
      "at 1.4 km it is open, awake, on channel 7 (3-6 are the session's own)");
  auto seen = take(p);
  const size_t open_bits = bits_on(seen, 7);
  CHECKF(open_bits >= 58881 && open_bits < 58881 + 6000,
         "the open carries the whole payload (58,881 bits) and the actor's "
         "header and exports (%zu bits on the channel)",
         open_bits);
  CHECK(v.stats().opens == 1 && v.stats().snapshots == 0,
        "an awake open, never a snapshot (the replicator is never dormant)");
  run(v, p, jx + 140000, jy, 15);
  CHECK(take(p).empty(), "while the player stays nothing is sent");
  run(v, p, jx + 145500, jy, 3);
  CHECK(v.state_of(id) == world::ClientState::Awake,
        "10 m past the edge it stays open");
  run(v, p, jx + 147000, jy, 3);
  seen = take(p);
  bool closed = false;
  uint32_t reason = 0;
  bits_on(seen, 7, &closed, &reason);
  CHECK(closed && reason == kCloseRelevancy &&
            v.state_of(id) == world::ClientState::None,
        "beyond it is closed with a Relevancy close (the client destroys the "
        "actor)");
  run(v, p, jx + 120000, jy, 8);
  CHECK(v.state_of(id) == world::ClientState::Awake,
        "walking back opens it again");
  take(p);
}

void check_view_level_change_and_second(Scene &s, ActorView &v, Pair &p,
                                        uint64_t id) {
  const world::WorldActor *joeva = s.actor_of(kJoeva);
  const double jx = joeva->x, jy = joeva->y;
  const uint32_t channel = v.channel_of(id);
  const size_t updates = v.stats().updates;
  s.set_level(kJoeva, 3);
  s.scenery->tick();
  run(v, p, jx + 120000, jy, 8);
  auto seen = take(p);
  size_t update_bits = 0;
  bool reliable = true, any_open = false;
  for (const Seen &x : seen) {
    if (x.channel != channel)
      continue;
    update_bits += x.bits;
    reliable = reliable && x.reliable;
    any_open = any_open || x.open;
  }
  CHECK(v.stats().updates == updates + 1 && update_bits > 0 && !any_open &&
            reliable,
        "the level change is one reliable update on the replicator's channel "
        "(no new open)");
  const size_t want = default_items(kJoeva, 3).size();
  CHECKF(update_bits >= want * 800 && update_bits < want * 800 + 73 * 32 + 3000,
         "it carries the new list in full (%zu items) and deletes the old IDs "
         "(%zu bits)",
         want, update_bits);
  CHECK(v.channel_of(id) == channel &&
            v.state_of(id) == world::ClientState::Awake,
        "the replicator stays open");
  run(v, p, jx + 120000, jy, 6);
  CHECK(take(p).empty(), "and nothing more follows");
  Pair p2;
  ActorView v2(s.registry, ActorView::Config{}, reserved_3_to_6());
  run(v2, p2, jx - 100000, jy, 12);
  CHECK(v2.state_of(id) == world::ClientState::Awake,
        "a second player 1 km away has it open too");
  const size_t second_bits = bits_on(take(p2), v2.channel_of(id));
  CHECKF(second_bits >= 49281 && second_bits < 49281 + 6000,
         "with the Village payload (61 items, 49,281 bits): %zu bits",
         second_bits);
  s.set_level(kJoeva, 4);
  s.scenery->tick();
  run(v, p, jx + 120000, jy, 8);
  run(v2, p2, jx - 100000, jy, 8);
  CHECK(v.stats().updates == updates + 2 && v2.stats().updates == 1,
        "the next level change reaches both");
  take(p);
  take(p2);
}

void test_view() {
  std::printf("the view: a player walks up to Joeva\n");
  Scene s;
  s.scenery->start();
  const world::WorldActor *joeva = s.actor_of(kJoeva);
  if (!joeva) {
    CHECK(false, "Joeva has a replicator");
    return;
  }
  Pair p;
  ActorView v(s.registry, ActorView::Config{}, reserved_3_to_6());
  check_view_walk_and_close(v, p, joeva->id, joeva->x, joeva->y);
  check_view_level_change_and_second(s, v, p, joeva->id);
}

void test_reach() {
  std::printf("the view: a player far from the towns\n");
  Scene s;
  const size_t n = s.scenery->start();
  Pair p;
  ActorView v(s.registry, ActorView::Config{}, reserved_3_to_6());
  run(v, p, 0, 0, 40);
  size_t open = 0;
  s.registry.for_each([&](const world::WorldActor &a) {
    open += v.state_of(a.id) == world::ClientState::Awake ? 1 : 0;
  });
  CHECKF(open <= n,
         "a player at the world's origin has only the replicators within 1.45 "
         "km (%zu of %zu)",
         open, n);
  const world::WorldActor *joeva = s.actor_of(kJoeva);
  const world::WorldActor *winstead = s.actor_of(kWinstead);
  if (joeva && winstead) {
    const double mx = (joeva->x + winstead->x) / 2,
                 my = (joeva->y + winstead->y) / 2;
    const double d = std::hypot(joeva->x - winstead->x, joeva->y - winstead->y);
    Pair q;
    ActorView w(s.registry, ActorView::Config{}, reserved_3_to_6());
    run(w, q, mx, my, 60);
    const bool both = w.state_of(joeva->id) == world::ClientState::Awake &&
                      w.state_of(winstead->id) == world::ClientState::Awake;
    CHECKF(d / 2 > 145000.0 || both,
           "between Joeva and Winstead (%.0f m apart) the player has the "
           "replicators in range",
           d / 100.0);
  }
}

world::WorldActor stand_in(const world::WorldActor &from,
                           const emit::FIntrepidNetworkGUID &guid,
                           world::ActorKind kind, double x, double y) {
  world::WorldActor w = from;
  w.actor.guid = guid;
  w.id = guid.ObjectId;
  w.kind = kind;
  w.x = x;
  w.y = y;
  if (kind != world::ActorKind::NodeLayout) {
    w.awake_radius_cm = 0;
    w.never_dormant = false;
  }
  return w;
}

std::vector<uint64_t> scenery_in_reach(const world::ActorRegistry &registry,
                                       double x, double y) {
  std::vector<std::pair<double, uint64_t>> found;
  registry.for_each([&](const world::WorldActor &a) {
    const double d = world::horizontal_distance(a.x, a.y, x, y);
    if (a.kind == world::ActorKind::NodeLayout && d <= a.awake_radius_cm)
      found.push_back({d, a.id});
  });
  std::sort(found.begin(), found.end());
  std::vector<uint64_t> out;
  for (const auto &f : found)
    out.push_back(f.second);
  return out;
}

bool is_point_clear(const world::ActorRegistry &registry, double x, double y) {
  bool clear = true;
  registry.for_each([&](const world::WorldActor &a) {
    if (a.kind == world::ActorKind::NodeLayout &&
        world::horizontal_distance(a.x, a.y, x, y) <=
            a.awake_radius_cm + 10000.0)
      clear = false;
  });
  return clear;
}

bool point_far_from_towns(const world::ActorRegistry &registry, double cx,
                          double cy, double *out_x, double *out_y) {
  const double pi = 3.14159265358979323846;
  for (const double radius : {300000.0, 600000.0, 900000.0}) {
    for (int k = 0; k < 8; ++k) {
      const double x = cx + radius * std::cos(k * pi / 4),
                   y = cy + radius * std::sin(k * pi / 4);
      if (is_point_clear(registry, x, y)) {
        *out_x = x;
        *out_y = y;
        return true;
      }
    }
  }
  return false;
}

void check_scenery_first_cap_and_far(Scene &s, double jx, double jy, double px,
                                     double py) {
  size_t opened = 0;
  {
    Scene s2;
    s2.scenery->start();
    const world::WorldActor templ2 = *s2.actor_of(kJoeva);
    const emit::FIntrepidNetworkGUID second = s2.guids.next();
    CHECK(s2.registry.add(stand_in(templ2, second, world::ActorKind::NodeLayout,
                                   templ2.x + 5000, templ2.y)),
          "(a second replicator, 50 m east of Joeva's)");
    const size_t in_reach = scenery_in_reach(s2.registry, px, py).size();
    ActorView::Config capped;
    capped.relevancy.max_opens_per_tick = 1;
    Pair q;
    ActorView w(s2.registry, capped, reserved_3_to_6());
    const bool first_done = w.open_scenery(q.s, px, py, q.t, &opened);
    CHECK(!first_done && opened == 1 &&
              w.state_of(second.ObjectId) == world::ClientState::Awake &&
              w.state_of(templ2.id) == world::ClientState::None &&
              w.stats().capped == 1,
          "with a cap of one open a call, the first call opens the nearest "
          "replicator and says it is not done");
    bool last_done = false;
    size_t calls = 1, total = opened;
    while (!last_done && calls < 10) {
      last_done = w.open_scenery(q.s, px, py, q.t, &opened);
      total += opened;
      ++calls;
    }
    CHECKF(last_done && total == in_reach &&
               w.state_of(templ2.id) == world::ClientState::Awake,
           "...and the next calls open the rest (%zu replicators in %zu calls)",
           total, calls);
  }
  double fx = 0, fy = 0;
  CHECK(point_far_from_towns(s.registry, jx, jy, &fx, &fy),
        "(a point 1.5 km or more clear of every replicator)");
  Pair q;
  ActorView w(s.registry, ActorView::Config{}, reserved_3_to_6());
  const bool far_done = w.open_scenery(q.s, fx, fy, q.t, &opened);
  q.step(&w);
  CHECK(far_done && opened == 0 && w.tracked() == 0 && take(q).empty(),
        "far from every town: nothing within reach, nothing sent, nothing "
        "tracked");
}

void check_scenery_first_repeat_and_tick(Scene &s, ActorView &v, Pair &p,
                                         uint64_t id, uint64_t npc_id) {
  const world::WorldActor *joeva = s.actor_of(kJoeva);
  const double px = joeva->x + 100000, py = joeva->y;
  const size_t reach_size = scenery_in_reach(s.registry, px, py).size();
  CHECK(v.state_of(npc_id) == world::ClientState::None &&
            v.channel_of(npc_id) == 0 && v.tracked() == reach_size &&
            v.stats().opens == reach_size,
        "the NPC 20 m from the player is not touched: the call is for the "
        "scenery alone");
  size_t opened = 99;
  const bool done = v.open_scenery(p.s, px, py, p.t, &opened);
  p.step(&v);
  CHECK(done && opened == 0 && take(p).empty() && v.stats().opens == reach_size,
        "asked again it has nothing to do and sends nothing");
  run(v, p, px, py, 12);
  const auto seen = take(p);
  CHECK(v.state_of(npc_id) == world::ClientState::Awake &&
            v.channel_of(id) == 7 && v.stats().opens == reach_size + 1 &&
            v.stats().updates == 0 && bits_on(seen, 7) == 0,
        "the passes that follow open the NPC and leave the replicator as it "
        "is: the same channel, no second open, no update");
}

void test_scenery_first() {
  std::printf("the view: the towns' scenery is opened alone, before the rest "
              "of the world (the login fix)\n");
  Scene s;
  s.scenery->start();
  const world::WorldActor *joeva = s.actor_of(kJoeva);
  if (!joeva) {
    CHECK(false, "Joeva has a replicator");
    return;
  }
  const uint64_t id = joeva->id;
  const double jx = joeva->x, jy = joeva->y;
  const double px = jx + 100000, py = jy;
  const world::WorldActor templ = *joeva;
  const emit::FIntrepidNetworkGUID npc_guid = s.guids.next();
  const uint64_t npc_id = npc_guid.ObjectId;
  CHECK(s.registry.add(
            stand_in(templ, npc_guid, world::ActorKind::Npc, px + 2000, py)),
        "(a stand-in for an NPC, 20 m from the player)");
  const auto reach = scenery_in_reach(s.registry, px, py);
  CHECK(!reach.empty() && reach.front() == id,
        "(Joeva's replicator is the nearest one within reach)");
  Pair p;
  ActorView v(s.registry, ActorView::Config{}, reserved_3_to_6());
  size_t opened = 99;
  const bool done = v.open_scenery(p.s, px, py, p.t, &opened);
  p.step(&v);
  const auto seen = take(p);
  CHECKF(done && opened == reach.size() &&
             v.state_of(id) == world::ClientState::Awake &&
             v.channel_of(id) == 7,
         "open_scenery opens the %zu replicator(s) within reach, Joeva's awake "
         "on channel 7, and says nothing is left",
         reach.size());
  const size_t open_bits = bits_on(seen, 7);
  CHECKF(open_bits >= 58881 && open_bits < 58881 + 6000,
         "...with the whole payload (58,881 bits and the actor's header and "
         "exports: %zu bits)",
         open_bits);
  check_scenery_first_repeat_and_tick(s, v, p, id, npc_id);
  check_scenery_first_cap_and_far(s, jx, jy, px, py);
}

const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
const std::string kMachine =
    "DESKTOP-EXAMPLE1-US-45739BC044607BD9A50196AF41F22492";

const schema::ClassSchema &class_named(const char *name) {
  const schema::ClassSchema *c = gen::find_class_schema(name);
  if (!c) {
    std::printf("  [FAIL] no schema %s\n", name);
    std::exit(2);
  }
  return *c;
}

emit::BunchWriter actor_rpc(const schema::ClassSchema &cls, const char *name) {
  emit::BunchWriter out;
  const schema::RpcSchema *rpc = cls.rpc(name);
  if (!rpc)
    return out;
  emit::BunchWriter field;
  bool failed = false;
  repl::write_rpc_field(cls, *rpc, repl::RpcParams{}, field, {}, &failed);
  if (failed)
    return out;
  repl::BlockHeader header;
  repl::write_content_block(out, header, field, false);
  return out;
}

svc::Character cyb_at(double x, double y, double z) {
  svc::Character c;
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
  c.has_position = true;
  c.position.x = x;
  c.position.y = y;
  c.position.z = z;
  return c;
}

struct TownWorld {
  fs::path dir;
  std::optional<content::ContentDb> content;
  aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
  std::unique_ptr<world::CityNodes> nodes;
  std::unique_ptr<PlayerWorld> world;

  TownWorld() {
    dir = cf::make_temp_dir("world_node_layout_join_");
    const fs::path db = dir / "content.db";
    if (!cf::make_db(db))
      std::exit(2);
    std::string error;
    content = content::ContentDb::open(db.string(), error);
    if (!content) {
      std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
      std::exit(2);
    }
    nodes = world::CityNodes::load(db_path(), error);
    if (!nodes || !nodes->apply_seed_file(NODES_SEED_PATH, error)) {
      std::printf("  [FAIL] the nodes do not load: %s\n", error.c_str());
      std::exit(2);
    }
    world = std::make_unique<PlayerWorld>(
        &*content, "", bootstrap::GameStateMode::Tier3, kServerId, guids);
    world->set_city_nodes(nodes.get());
    world->set_node_wire(g_wire.get());
    world->set_node_layout_data(g_data.get());
    if (world->start_scenery(world::NodeScenery::Config{}) < 2) {
      std::printf("  [FAIL] the scenery does not start\n");
      std::exit(2);
    }
  }
  ~TownWorld() {
    world.reset();
    content.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  TownWorld(const TownWorld &) = delete;
  TownWorld &operator=(const TownWorld &) = delete;

  uint64_t add_npc(double x, double y) {
    const world::WorldActor *joeva = nullptr;
    world->registry().for_each([&](const world::WorldActor &a) {
      if (a.kind == world::ActorKind::NodeLayout && a.scenery.node == kJoeva)
        joeva = &a;
    });
    if (!joeva)
      std::exit(2);
    const emit::FIntrepidNetworkGUID g = world->mint_guid();
    if (!world->registry().add(
            stand_in(*joeva, g, world::ActorKind::Npc, x, y)))
      std::exit(2);
    return g.ObjectId;
  }
};

struct Joined {
  Connection conn{server_config(), t0};
  fake::Client client{kOuter, kCf, 14265, 15870};
  std::unique_ptr<PlayerSession> s;
  Clock::time_point t = t0;
  std::vector<fake::Delivered> got;
  std::vector<Clock::time_point> got_at;
  uint32_t pawn_ch = 0;
  std::string error;

  bool join(PlayerWorld &world, const svc::Character &record) {
    s = PlayerSession::join(
        conn, world, record, bootstrap::LoginIdentity{kMachine, kMachine},
        std::chrono::duration<double>(t - t0).count(), error);
    if (!s) {
      std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
      return false;
    }
    pawn_ch = s->channel_of(bootstrap::Role::Pawn);
    return true;
  }
  void flush() {
    for (auto &pkt : conn.flush(t)) {
      const auto r = client.receive(pkt);
      for (const auto &d : r.delivered) {
        got.push_back(d);
        got_at.push_back(t);
      }
    }
  }
  void client_sends(std::vector<fake::Bunch> bunches) {
    const auto pkt = client.packet(std::move(bunches));
    const std::vector<InBunch> in = conn.receive(pkt, t);
    s->take_acks(conn);
    for (const InBunch &b : in)
      s->on_bunch(conn, b, t);
    s->end_of_datagram(conn);
  }
  void step(milliseconds d = milliseconds(20)) {
    t += d;
    s->tick(conn, t);
    flush();
    s->take_acks(conn);
    client_sends({});
  }
  void run(milliseconds total) {
    for (milliseconds done{0}; done < total; done += milliseconds(20))
      step();
  }
  void settle() {
    for (int quiet = 0, i = 0; quiet < 5 && i < 600; ++i) {
      const size_t before = got.size();
      step();
      quiet = (got.size() == before && s->join_pending() == 0 &&
               s->replies_pending() == 0)
                  ? quiet + 1
                  : 0;
    }
  }
  void request_phase_in() {
    client_sends({client.actor(
        pawn_ch,
        actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn"),
        true)});
    settle();
  }
  bool player_channel(uint32_t c) const {
    return c == s->channel_of(bootstrap::Role::PlayerController) ||
           c == pawn_ch || c == s->channel_of(bootstrap::Role::GameState) ||
           c == s->channel_of(bootstrap::Role::PlayerState);
  }
  std::vector<size_t> world_opens() const {
    std::vector<size_t> out;
    for (size_t i = 0; i < got.size(); ++i) {
      if (got[i].open && !player_channel(got[i].channel))
        out.push_back(i);
    }
    return out;
  }
  size_t pawn_open() const {
    for (size_t i = 0; i < got.size(); ++i) {
      if (got[i].open && got[i].channel == pawn_ch)
        return i;
    }
    return static_cast<size_t>(-1);
  }
};

void check_join_after_phase_in(Joined &j, const std::vector<uint64_t> &reach,
                               uint64_t npc, size_t chains_before,
                               std::ostringstream &log_text) {
  const ActorView &v = *j.s->actor_view();
  j.request_phase_in();
  j.run(milliseconds(1500));
  CHECK(v.state_of(npc) == world::ClientState::Awake,
        "after the phase-in the NPC is opened, as before");
  size_t reopened = 0;
  for (const size_t i : j.world_opens()) {
    for (const uint64_t id : reach) {
      if (i >= chains_before && v.channel_of(id) == j.got[i].channel)
        ++reopened;
    }
  }
  CHECK(reopened == 0 && v.stats().opens == reach.size() + 1,
        "...and the replicators are not opened a second time");
  const std::string text = log_text.str();
  const size_t at = text.find("[join] cyb: phase-in after ");
  double seconds_logged = -1.0;
  if (at != std::string::npos)
    seconds_logged = std::atof(text.c_str() + at +
                               std::strlen("[join] cyb: phase-in after "));
  CHECKF(at != std::string::npos && seconds_logged > 4.8 &&
             seconds_logged < 5.4,
         "the log says it: \"[join] cyb: phase-in after %.1f s\" (the client "
         "asked at about 5 s)",
         seconds_logged);
  size_t join_lines = 0;
  for (size_t pos = text.find("[join] cyb:"); pos != std::string::npos;
       pos = text.find("[join] cyb:", pos + 1))
    ++join_lines;
  const std::string join_line = at == std::string::npos
                                    ? std::string()
                                    : text.substr(at, text.find('\n', at) - at);
  std::printf("    the join's log line: %s\n", join_line.c_str());
  std::printf("    %zu replicator(s) within reach of the square; sent at %.2f "
              "s of the session; the client's ready call at %.1f s\n",
              reach.size(), j.s->scenery_opened_at(), seconds_logged);
  CHECK(join_lines == 1 &&
            join_line.find("town replicator") != std::string::npos &&
            join_line.find("no town replicator") == std::string::npos,
        "...one \"[join]\" line for the join, and it says the town's "
        "replicator(s) were sent before the phase-in");
  j.s->finish("test");
}

void check_join_far_from_towns(TownWorld &w, double mx, double my,
                               std::ostringstream &log_text) {
  double fx = 0, fy = 0;
  CHECK(point_far_from_towns(w.world->registry(), mx, my, &fx, &fy),
        "(a point 1.5 km or more clear of every town)");
  const uint64_t far_npc = w.add_npc(fx + 2000, fy);
  Joined k;
  if (k.join(*w.world, cyb_at(fx, fy, 12000.0))) {
    k.run(milliseconds(5000));
    CHECK(k.world_opens().empty() && k.s->scenery_opened() == 0 &&
              k.s->actor_view()->tracked() == 0,
          "joined far from every town: nothing of the world is sent before the "
          "phase-in");
    k.request_phase_in();
    k.run(milliseconds(1500));
    CHECK(k.s->actor_view()->state_of(far_npc) == world::ClientState::Awake &&
              k.world_opens().size() == 1,
          "...the NPC comes with the phase-in, alone");
    const std::string all = log_text.str();
    CHECK(all.find("no town replicator was sent before it") !=
              std::string::npos,
          "...and its log line says no town replicator was sent before the "
          "phase-in");
    k.s->finish("test");
  }
}

void test_join() {
  std::printf("the session: joined in Joeva's bowl, the town's scenery comes "
              "before the phase-in\n");
  TownWorld w;
  const double mx = -1087417.6, my = 503591.5, mz = 12626.0;
  const uint64_t npc = w.add_npc(mx + 2000, my);
  const auto reach = scenery_in_reach(w.world->registry(), mx, my);
  CHECK(!reach.empty(), "(Joeva's replicator is within reach of the square)");
  std::ostringstream log_text;
  const auto previous = spdlog::default_logger();
  const auto previous_level = spdlog::get_level();
  spdlog::set_default_logger(std::make_shared<spdlog::logger>(
      "join_test", std::make_shared<spdlog::sinks::ostream_sink_mt>(log_text)));
  spdlog::set_level(spdlog::level::info);
  Joined j;
  const bool joined = j.join(*w.world, cyb_at(mx, my, mz));
  CHECK(joined, "cyb joins at Joeva's market square");
  if (!joined) {
    spdlog::set_default_logger(previous);
    spdlog::set_level(previous_level);
    return;
  }
  j.run(milliseconds(5000));
  const ActorView &v = *j.s->actor_view();
  const std::vector<size_t> before = j.world_opens();
  CHECKF(before.size() == reach.size() && j.s->scenery_opened() == reach.size(),
         "before the phase-in the client has been sent the %zu replicator(s) "
         "within reach (the session says %zu)",
         reach.size(), j.s->scenery_opened());
  bool all_open = true;
  for (const uint64_t id : reach)
    all_open = all_open && v.state_of(id) == world::ClientState::Awake;
  CHECK(all_open && v.state_of(npc) == world::ClientState::None,
        "...and nothing else of the world: the NPC 20 m from the square waits "
        "for the phase-in");
  const size_t pawn = j.pawn_open();
  CHECK(!before.empty() && pawn != static_cast<size_t>(-1) &&
            pawn < before.front(),
        "the pawn opens first and the replicator after it");
  CHECK(!before.empty() && j.got_at[before.front()] - t0 < seconds(1) &&
            j.s->scenery_opened_at() >= 0.0 && j.s->scenery_opened_at() < 1.0,
        "...within a second of the join");
  check_join_after_phase_in(j, reach, npc, j.got.size(), log_text);
  check_join_far_from_towns(w, mx, my, log_text);
  spdlog::set_default_logger(previous);
  spdlog::set_level(previous_level);
}

void test_world() {
  std::printf("the world: start_scenery and tick_world\n");
  const fs::path dir = cf::make_temp_dir("world_node_layout_");
  const fs::path db = dir / "content.db";
  if (!cf::make_db(db))
    std::exit(2);
  std::string error;
  auto content = content::ContentDb::open(db.string(), error);
  if (!content) {
    std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
    std::exit(2);
  }
  {
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    PlayerWorld world(&*content, "", bootstrap::GameStateMode::Tier3, kServerId,
                      guids);
    auto nodes = world::CityNodes::load(db_path(), error);
    nodes->apply_seed_file(NODES_SEED_PATH, error);
    world::NodeScenery::Config cfg;
    CHECK(world.start_scenery(cfg) == 0 && world.scenery() == nullptr,
          "without nodes, a node wire and the layout data there is no scenery");
    world.set_city_nodes(nodes.get());
    world.set_node_wire(g_wire.get());
    CHECK(world.start_scenery(cfg) == 0,
          "...still none without the layout data");
    world.set_node_layout_data(g_data.get());
    world::NodeScenery::Config off;
    off.enabled = false;
    CHECK(world.start_scenery(off) == 0 && world.scenery() == nullptr,
          "disabled: none");
    const size_t n = world.start_scenery(cfg);
    CHECK(n >= 2 && world.scenery() &&
              world.registry().count(world::ActorKind::NodeLayout) == n,
          "start_scenery makes the replicators in the world's registry");
    CHECK(world.start_scenery(cfg) == 0, "once");
    world::LiveNode ln;
    ln.guid = kJoeva;
    ln.level = 2;
    nodes->apply_change(ln);
    CHECK(world.scenery()->rebuilds() == 0,
          "a level change is not followed until the world ticks");
    world.tick_world(10.0);
    const auto *e = world.scenery()->entry_of(kJoeva);
    CHECK(world.scenery()->rebuilds() == 1 && e && e->level == 2 &&
              e->plot_version == 2,
          "tick_world follows the node: Joeva rebuilt at level 2, "
          "PlotChangeVersion 2");
    world.tick_world(10.1);
    CHECK(world.scenery()->rebuilds() == 1, "and only once");
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

} // namespace

int main() {
  if (const int rc = init_node_layout_globals(); rc != 0)
    return rc;
  test_view();
  test_reach();
  test_scenery_first();
  test_join();
  test_world();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
