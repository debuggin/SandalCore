// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_gm_travel_common.h  (shared harness for GM travel tests)
// ============================================================================
#pragma once

#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/schema/generated/index.h"
#include "svc/world_admin.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bs = aoc::worldd::bootstrap;
namespace content = aoc::worldd::content;
namespace items = aoc::worldd::items;
namespace spawns = aoc::worldd::spawns;
namespace svc = aoc::worldd::svc;
namespace world = aoc::worldd::world;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace fs = std::filesystem;
namespace cf = content_fixture;
namespace fi = content_fixture::item;

using aoc::worldd::AbilityRuntime;
using aoc::worldd::Clock;
using aoc::worldd::Connection;
using aoc::worldd::ConnectionConfig;
using aoc::worldd::GmPlayerInfo;
using aoc::worldd::GmSpeedResult;
using aoc::worldd::GmStatus;
using aoc::worldd::GmTeleportRequest;
using aoc::worldd::GmTeleportResult;
using aoc::worldd::GmTravel;
using aoc::worldd::GoodPosition;
using aoc::worldd::InBunch;
using aoc::worldd::MountController;
using aoc::worldd::PlayerHandle;
using aoc::worldd::PlayerSession;
using aoc::worldd::PlayerWorld;
using aoc::worldd::PositionSink;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;
using svc::ItemContainer;

inline int g_pass = 0;
inline int g_fail = 0;
inline float g_pawn_ts = 0;

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

inline const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const Clock::time_point t0{seconds(5000)};
inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;
inline const ItemContainer Bag = ItemContainer::Backpack;
inline const ItemContainer Worn = ItemContainer::Equipment;

inline const schema::ClassSchema &class_named(const char *name) {
  const schema::ClassSchema *c = gen::find_class_schema(name);
  if (!c) {
    std::printf("  [FAIL] no schema %s\n", name);
    std::exit(2);
  }
  return *c;
}

inline std::string guid_n(uint32_t n) {
  char buf[33];
  std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0xA0000000u + n, 0x1000u + n, 0x2000u + n, n);
  return buf;
}

inline svc::InventoryItem make_item(ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1) {
  svc::InventoryItem it;
  it.container = c;
  it.slot = slot;
  it.item_record_id = record;
  it.instance_guid = guid_n(n);
  it.quantity = quantity;
  it.quality = 1;
  it.durability = 11;
  it.created_at_ms = 1'771'299'474'000ll;
  return it;
}

inline constexpr int kMountSlot = 5;
inline constexpr int kOtherMountSlot = 6;
inline constexpr int kLockedSlot = 7;
inline constexpr int kAquaticSlot = 8;
inline constexpr int kFlyingSlot = 9;
inline constexpr int kPotionSlot = 1;
inline constexpr int kSwordSlot = 0;
inline int64_t g_mount_record = 0;
inline int64_t g_other_mount_record = 0;
inline int64_t g_aquatic_record = 0;
inline int64_t g_flying_record = 0;

inline std::vector<svc::InventoryItem> kit() {
  const std::pair<int, int64_t> worn[] = {{1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook}, {23, fi::kBag}, {26, fi::kPickaxe}, {29, fi::kAxe}, {33, fi::kSickle}, {35, fi::kFishingRod}, {38, fi::kBow}};
  std::vector<svc::InventoryItem> out;
  for (const auto &[slot, record] : worn) {
    out.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
  }
  out.push_back(make_item(Bag, kSwordSlot, fi::kSword2H, 200));
  out.push_back(make_item(Bag, kPotionSlot, fi::kPotion, 201, 5));
  svc::InventoryItem a = make_item(Bag, kMountSlot, g_mount_record, 300);
  a.persisted_health = 3572.f;
  a.persisted_mana = 1.f;
  out.push_back(a);
  out.push_back(make_item(Bag, kOtherMountSlot, g_other_mount_record, 301));
  svc::InventoryItem locked = make_item(Bag, kLockedSlot, g_mount_record, 302);
  locked.lock_expiration = 4'000'000'000ll;
  out.push_back(locked);
  out.push_back(make_item(Bag, kAquaticSlot, g_aquatic_record, 303));
  out.push_back(make_item(Bag, kFlyingSlot, g_flying_record, 304));
  return out;
}

inline svc::Character cyb_record(std::vector<svc::InventoryItem> items = kit()) {
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
  c.items = std::move(items);
  return c;
}

inline bs::LoginIdentity identity() {
  return {kMachine, kMachine};
}
inline double world_time_at(Clock::time_point t) {
  return std::chrono::duration<double>(t - t0).count();
}

struct RecordingSink : PositionSink {
  std::vector<std::vector<svc::InventoryItem>> inventories;
  std::vector<svc::Position> positions;
  void save(const std::string &, const svc::Position &p) override {
    positions.push_back(p);
  }
  void save_inventory(const std::string &, std::vector<svc::InventoryItem> items) override {
    inventories.push_back(std::move(items));
  }
  void save_progress(const std::string &, svc::Progress) override {
  }
};

inline std::string real_db_path() {
  const char *env = std::getenv("AOC_CONTENT_DB");
  return env && *env ? env : MOUNT_CONTENT_DB_PATH;
}

struct Fixture {
  fs::path dir;
  std::optional<content::ContentDb> content;
  std::optional<content::ContentDb> real;
  aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
  std::unique_ptr<PlayerWorld> world;

  Fixture() {
    dir = cf::make_temp_dir("worldd_mounts_ride_");
    const fs::path db = dir / "content.db";
    if (!cf::make_db(db))
      std::exit(2);
    std::string error;
    content = content::ContentDb::open(db.string(), error);
    if (!content)
      std::exit(2);
    real = content::ContentDb::open(real_db_path(), error);
    if (!real)
      std::exit(2);
    world = std::make_unique<PlayerWorld>(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
    auto book = spawns::MountBook::load(*real, error);
    if (!book || !book->load_speeds(MOUNT_SPEEDS_PATH, error))
      std::exit(2);
    world->set_mounts(std::move(*book));
  }
  Fixture(const Fixture &) = delete;
  Fixture &operator=(const Fixture &) = delete;
  ~Fixture() {
    world.reset();
    content.reset();
    real.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

struct Harness {
  Fixture fx;
  RecordingSink sink;
  Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
  fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
  std::unique_ptr<PlayerSession> s;
  Clock::time_point t = t0;
  std::vector<fake::Delivered> got;
  std::vector<Clock::time_point> got_at;
  std::string error;
  uint32_t pawn_ch = 0;
  uint32_t pc_ch = 0;
  svc::Character record;

  explicit Harness(svc::Character r, const std::function<void(PlayerWorld &)> &setup = nullptr)
      : record(std::move(r)) {
    fx.world->set_position_sink(&sink);
    if (setup)
      setup(*fx.world);
    s = PlayerSession::join(conn, *fx.world, record, identity(), world_time_at(t), error);
    if (!s)
      std::exit(2);
    pawn_ch = s->channel_of(bs::Role::Pawn);
    pc_ch = s->channel_of(bs::Role::PlayerController);
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
    fx.world->tick_world(world_time_at(t));
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
      quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
    }
  }
  void play();
  const world::WorldActor *mount() const {
    return s->mounts() ? s->mounts()->mount() : nullptr;
  }
  uint32_t mount_ch() const {
    const world::WorldActor *m = mount();
    return m && s->actor_view() ? s->actor_view()->channel_of(m->id) : 0;
  }
};

inline BunchWriter actor_rpc(const schema::ClassSchema &cls, const char *name, const repl::RpcParams &params) {
  BunchWriter out;
  const schema::RpcSchema *rpc = cls.rpc(name);
  if (!rpc)
    return out;
  BunchWriter field;
  bool failed = false;
  if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 && !params.values.empty()) {
    return out;
  }
  if (failed)
    return out;
  repl::BlockHeader header;
  repl::write_content_block(out, header, field, /*server_form=*/false);
  return out;
}

inline void Harness::play() {
  g_pawn_ts = 0;
  settle();
  client_sends({client.actor(pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
  settle();
  run(milliseconds(400));
}

inline BunchWriter item_used(int storage, int slot, const std::string &guid_hex) {
  const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
  const schema::RpcSchema *rpc = pawn.rpc("ServerItemUsed");
  repl::RpcParams p;
  const auto g = items::guid_of(guid_hex);
  const repl::GuidValue gv = g ? *g : repl::GuidValue{};
  repl::set_rpc_param(*rpc, p, "Storage", repl::Value::u64(static_cast<uint64_t>(storage)));
  repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
  repl::set_rpc_param(*rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
  repl::set_rpc_param(*rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
  repl::set_rpc_param(*rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
  repl::set_rpc_param(*rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
  repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
  return actor_rpc(pawn, "ServerItemUsed", p);
}

struct TriggerItemSpec {
  int slot = -1;
  const svc::InventoryItem *item = nullptr;
  uint8_t location = 3;
  int32_t animation_index = 0;
};

inline bool fill_trigger_params(const schema::RpcSchema &rpc, repl::RpcParams &p, int64_t ability, int32_t client_instance, TriggerItemSpec spec) {
  bool ok = repl::set_rpc_param(rpc, p, "AbilityGuid", repl::Value::i64(ability));
  if (ok && client_instance != 0) {
    ok = repl::set_rpc_param(rpc, p, "ClientInstanceId", repl::Value::i64(client_instance));
  }
  if (ok && spec.animation_index != 0) {
    ok = repl::set_rpc_param(rpc, p, "AnimationIndex", repl::Value::i64(spec.animation_index));
  }
  ok = ok && repl::set_rpc_param(rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
  if (ok && spec.slot >= 0) {
    const repl::ItemInstance inst = spec.item ? items::to_instance(*spec.item) : repl::ItemInstance{};
    ok = repl::set_rpc_param(rpc, p, "ItemData.Slot", repl::Value::i64(spec.slot)) && repl::set_rpc_param(rpc, p, "ItemData.ItemInstance", repl::Value(inst)) &&
         repl::set_rpc_param(rpc, p, "ItemData.Location", repl::Value::u64(spec.location));
  }
  return ok;
}

inline BunchWriter trigger_on(Harness &h, const emit::FIntrepidNetworkGUID &component, int64_t ability, int32_t client_instance, TriggerItemSpec spec = {}) {
  const schema::ClassSchema &cls = class_named("UAoCAbilityComponent");
  const schema::RpcSchema *rpc = cls.rpc("ServerTriggerAbility");
  repl::RpcParams p;
  if (!rpc || !fill_trigger_params(*rpc, p, ability, client_instance, spec)) {
    return BunchWriter();
  }
  BunchWriter out;
  BunchWriter field;
  bool failed = false;
  if (repl::write_rpc_field(cls, *rpc, p, field, {}, &failed) == 0 || failed) {
    return out;
  }
  repl::BlockHeader header;
  header.is_actor = false;
  header.has_rep_layout = false;
  header.object = component;
  repl::write_content_block(out, header, field, /*server_form=*/false);
  (void)h;
  return out;
}

inline BunchWriter summon_trigger(Harness &h, int32_t client_instance, int slot, const svc::InventoryItem *item, uint8_t location = 3) {
  return trigger_on(h, h.s->bootstrap().setup().guids.ability, 108436548376185ll, client_instance, TriggerItemSpec{slot, item, location, 0});
}

inline BunchWriter dismount_rpc() {
  return actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerDismountInteraction", {});
}

inline emit::FIntrepidNetworkGUID component_of(const world::WorldActor &m, uint64_t object_id) {
  emit::FIntrepidNetworkGUID g = m.actor.guid;
  g.ObjectId = object_id;
  return g;
}

inline BunchWriter interact_with(const emit::FIntrepidNetworkGUID &component) {
  const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
  const schema::RpcSchema *rpc = pawn.rpc("ServerInteractWithInteractableComponent");
  repl::InteractRequest r;
  r.interactable = component;
  r.counter = 1;
  r.zone = 2;
  const auto params = repl::interact_params(*rpc, r);
  return params ? actor_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

inline repl::NetworkMove move_at(float ts, double x, double y, double z, uint16_t yaw = 0) {
  repl::NetworkMove m;
  m.time_stamp = ts;
  m.control_rotation.pitch = 0;
  m.acceleration = repl::quantize_vector(10, 0, 0, 0);
  m.location = repl::quantize_vector(100, x, y, z);
  m.control_rotation.yaw = yaw;
  m.movement_mode = repl::kMoveWalking;
  return m;
}

inline BunchWriter client_move(const char *cls_name, const repl::NetworkMove &m) {
  const schema::ClassSchema &cls = class_named(cls_name);
  const schema::NetField *f = cls.field("ServerMovePacked");
  BunchWriter out;
  if (!f)
    return out;
  repl::MovePacked p;
  p.new_move = m;
  BunchWriter params;
  if (!repl::write_move_packed(params, p))
    return out;
  BunchWriter field;
  if (repl::write_field(cls, f->index, params, field) == 0)
    return out;
  repl::BlockHeader header;
  repl::write_content_block(out, header, field, /*server_form=*/false);
  return out;
}

inline void move_pawn(Harness &h, const repl::NetworkMove &m) {
  h.client_sends({h.client.actor(h.pawn_ch, client_move("PlayerPawn_C", m), false)});
}
inline void move_mount(Harness &h, const repl::NetworkMove &m) {
  const uint32_t ch = h.s->mount_channel();
  h.client_sends({h.client.actor(ch ? ch : 999, client_move("ANPCCharacter", m), false)});
}

inline void summon_with(Harness &h, int slot, int32_t client_instance = 7) {
  const svc::InventoryItem *item = h.s->inventory().at(Bag, slot);
  const std::string guid = item ? item->instance_guid : std::string();
  h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid), true), h.client.actor(h.pawn_ch, summon_trigger(h, client_instance, slot, item), true)});
}

inline void get_on(Harness &h) {
  const world::WorldActor *m = h.mount();
  if (!m)
    return;
  h.client_sends({h.client.actor(h.pawn_ch, interact_with(component_of(*m, m->npc.interact_component)), true)});
}

inline void get_off(Harness &h) {
  h.client_sends({h.client.actor(h.pc_ch, dismount_rpc(), true)});
}

inline uint16_t yaw16_of(double degrees) {
  return static_cast<uint16_t>(static_cast<int>(std::lround(degrees * 65536.0 / 360.0)) & 0xFFFF);
}

inline void walk_pawn_to(Harness &h, double x, double y, double speed = 600.0) {
  const GoodPosition g = h.s->movement().good();
  const double dx = x - g.x;
  const double dy = y - g.y;
  const double dist = std::hypot(dx, dy);
  const int steps = std::max(1, static_cast<int>(std::ceil(dist / (speed * 0.2))));
  for (int i = 1; i <= steps; ++i) {
    g_pawn_ts += 0.2f;
    move_pawn(h, move_at(g_pawn_ts, g.x + dx * i / steps, g.y + dy * i / steps, g.z, yaw16_of(g.yaw_degrees)));
    h.step(milliseconds(200));
  }
}

inline void walk_up_to_mount(Harness &h) {
  const world::WorldActor *m = h.mount();
  if (!m)
    return;
  const GoodPosition g = h.s->movement().good();
  const double dx = m->x - g.x;
  const double dy = m->y - g.y;
  const double dist = std::hypot(dx, dy);
  if (dist <= 250)
    return;
  walk_pawn_to(h, g.x + dx * (dist - 250) / dist, g.y + dy * (dist - 250) / dist);
}

inline bool ride_on(Harness &h, int slot = kMountSlot) {
  summon_with(h, slot);
  h.run(milliseconds(1900));
  if (!h.mount() || h.s->mounts()->phase() != MountController::Phase::Out) {
    return false;
  }
  walk_up_to_mount(h);
  get_on(h);
  h.run(milliseconds(200));
  return h.s->mounted();
}

inline std::vector<repl::RpcCall> calls_of(const fake::Delivered &d, const char *cls) {
  std::vector<repl::RpcCall> out;
  const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
  if (!blocks)
    return out;
  for (const auto &b : *blocks) {
    if (b.header.has_rep_layout)
      continue;
    size_t p = b.payload_pos;
    auto calls = repl::read_rpc_fields(class_named(cls), d.content.data(), b.payload_pos + b.payload_bits, p);
    if (!calls)
      continue;
    for (auto &c : *calls)
      out.push_back(std::move(c));
  }
  return out;
}

inline std::vector<const fake::Delivered *> chains_on(const Harness &h, uint32_t channel, size_t from) {
  std::vector<const fake::Delivered *> out;
  for (size_t i = from; i < h.got.size(); ++i) {
    if (h.got[i].channel == channel && !h.got[i].open && !h.got[i].close) {
      out.push_back(&h.got[i]);
    }
  }
  return out;
}

inline size_t corrections_on(const Harness &h, uint32_t channel, size_t from, const char *cls = "ANPCCharacter") {
  size_t n = 0;
  for (const fake::Delivered *d : chains_on(h, channel, from)) {
    for (const repl::RpcCall &c : calls_of(*d, cls)) {
      n += c.rpc && std::string(c.rpc->name) == "ClientAdjustPosition";
    }
  }
  return n;
}

struct Rider {
  double x = 0;
  double y = 0;
  double z = 0;
  float ts = 0;
  uint16_t yaw = 0;
};

inline Rider start_riding(Harness &h) {
  Rider r;
  const GoodPosition g = h.s->movement().good();
  r.x = g.x;
  r.y = g.y;
  r.z = g.z;
  r.yaw = yaw16_of(g.yaw_degrees);
  return r;
}

inline void ride_step(Harness &h, Rider &r, double dx, double dy, double dz = 0) {
  const double dt = 0.1;
  r.ts += static_cast<float>(dt);
  r.x += dx;
  r.y += dy;
  r.z += dz;
  move_mount(h, move_at(r.ts, r.x, r.y, r.z, r.yaw));
  h.step(milliseconds(static_cast<int>(std::lround(dt * 1000))));
}

inline std::optional<double> mult_of(const world::WorldActor &m) {
  return spawns::stat_value(m, spawns::kStatMoveSpeedMult);
}

inline svc::Character cyb_on_the_ground() {
  svc::Character c = cyb_record();
  c.has_position = true;
  c.position = {0.0, 0.0, 1098.0, 0.0f};
  return c;
}

inline constexpr int64_t kMoveSpeedMult = 109228544644729ll;
inline std::unique_ptr<world::CityNodes> g_nodes;

inline bool near_to(double got, double want, double tol = 0.5) {
  return std::fabs(got - want) <= tol;
}

inline GmTravel make_travel(Harness &h) {
  return GmTravel(*h.fx.world, [&h] { return std::vector<PlayerHandle>{PlayerHandle{h.s.get(), &h.conn}}; }, [&h] { return h.t; });
}

inline void adopt_the_ground(PlayerWorld &w) {
  std::string err;
  auto nav = aoc::worldd::nav::AreaNav::from_file(aoc::worldd::navfix::build_file(), &err);
  if (!nav || !w.adopt_ground(std::move(nav), err))
    std::exit(2);
}

inline void with_nodes(PlayerWorld &w) {
  w.set_city_nodes(g_nodes.get());
}

inline void on_the_ground(PlayerWorld &w) {
  adopt_the_ground(w);
  with_nodes(w);
}

inline void pawn_move_to(Harness &h, double x, double y, double z, double dt = 0.2) {
  g_pawn_ts += static_cast<float>(dt);
  move_pawn(h, move_at(g_pawn_ts, x, y, z, yaw16_of(h.s->movement().good().yaw_degrees)));
  h.step(milliseconds(static_cast<int>(std::lround(dt * 1000))));
}

inline GmTeleportRequest to_point(double x, double y) {
  GmTeleportRequest rq;
  rq.character = "cyb";
  rq.x = x;
  rq.y = y;
  return rq;
}

inline GmTeleportRequest to_point_z(double x, double y, double z) {
  GmTeleportRequest rq = to_point(x, y);
  rq.z = z;
  rq.has_z = true;
  return rq;
}

inline GmTeleportRequest to_place(const char *place) {
  GmTeleportRequest rq;
  rq.character = "cyb";
  rq.place = place;
  return rq;
}

inline void assign_mount_row(const content::MountRow &r) {
  if (r.item_name == "Mount_Grounded_TerrorBird_0")
    g_mount_record = r.item;
  if (r.item_name == "Mount_Grounded_Horse_0")
    g_other_mount_record = r.item;
  if (r.kind == "aquatic" && !g_aquatic_record)
    g_aquatic_record = r.item;
  if (r.kind == "flying" && !g_flying_record)
    g_flying_record = r.item;
}

inline bool init_gm_travel_globals() {
  spdlog::set_level(std::getenv("AOC_TEST_LOG") ? spdlog::level::debug : spdlog::level::off);
  std::string error;
  auto real = content::ContentDb::open(real_db_path(), error);
  const auto data = real ? real->mount_data(error) : std::nullopt;
  if (!data)
    return false;
  for (const content::MountRow &r : data->mounts)
    assign_mount_row(r);
  g_nodes = world::CityNodes::load(real_db_path(), error);
  return g_nodes != nullptr;
}
