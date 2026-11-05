// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "nav_fixture.h"
#include "test_world_gm_travel_common.h"

#include "protocol/repl/block_reader.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/rep_block.h"
#include "town_platform_fixture.h"

namespace tpfix = aoc::worldd::tpfix;

static BunchWriter item_used(int storage, int slot, const std::string &guid_hex) {
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

static BunchWriter summon_trigger(Harness &h, int32_t client_instance, int slot, const svc::InventoryItem *item, uint8_t location = 3) {
  const schema::ClassSchema &cls = class_named("UAoCAbilityComponent");
  const schema::RpcSchema *rpc = cls.rpc("ServerTriggerAbility");
  repl::RpcParams p;
  if (!rpc)
    return BunchWriter();
  repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(108436548376185ll));
  if (client_instance != 0)
    repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(client_instance));
  repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
  if (slot >= 0) {
    const repl::ItemInstance inst = item ? items::to_instance(*item) : repl::ItemInstance{};
    repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(slot));
    repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(inst));
    repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(location));
  }
  BunchWriter out, field;
  bool failed = false;
  if (repl::write_rpc_field(cls, *rpc, p, field, {}, &failed) == 0 || failed)
    return out;
  repl::BlockHeader header;
  header.is_actor = false;
  header.has_rep_layout = false;
  header.object = h.s->bootstrap().setup().guids.ability;
  repl::write_content_block(out, header, field, /*server_form=*/false);
  return out;
}

static emit::FIntrepidNetworkGUID component_of(const world::WorldActor &m, uint64_t object_id) {
  emit::FIntrepidNetworkGUID g = m.actor.guid;
  g.ObjectId = object_id;
  return g;
}

static BunchWriter interact_with(const emit::FIntrepidNetworkGUID &component) {
  const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
  const schema::RpcSchema *rpc = pawn.rpc("ServerInteractWithInteractableComponent");
  repl::InteractRequest r;
  r.interactable = component;
  r.counter = 1;
  r.zone = 2;
  const auto params = repl::interact_params(*rpc, r);
  return params ? actor_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

static void summon_with(Harness &h, int slot, int32_t client_instance = 7) {
  const svc::InventoryItem *item = h.s->inventory().at(Bag, slot);
  const std::string guid = item ? item->instance_guid : std::string();
  h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid), true), h.client.actor(h.pawn_ch, summon_trigger(h, client_instance, slot, item), true)});
}

static void get_on(Harness &h) {
  const world::WorldActor *m = h.mount();
  if (!m)
    return;
  h.client_sends({h.client.actor(h.pawn_ch, interact_with(component_of(*m, m->npc.interact_component)), true)});
}

static void walk_up_to_mount(Harness &h) {
  const world::WorldActor *m = h.mount();
  if (!m)
    return;
  const GoodPosition g = h.s->movement().good();
  const double dx = m->x - g.x;
  const double dy = m->y - g.y;
  const double dist = std::hypot(dx, dy);
  if (dist > 250)
    walk_pawn_to(h, g.x + dx * (dist - 250) / dist, g.y + dy * (dist - 250) / dist);
}

static GmTeleportRequest to_point(double x, double y) {
  GmTeleportRequest rq;
  rq.character = "cyb";
  rq.x = x;
  rq.y = y;
  return rq;
}

static GmTeleportRequest to_place(const char *place) {
  GmTeleportRequest rq;
  rq.character = "cyb";
  rq.place = place;
  return rq;
}

static BunchWriter possession_ack(const emit::FIntrepidNetworkGUID &actor) {
  const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
  repl::RpcParams p;
  repl::set_rpc_param(*pc.rpc("ServerAcknowledgePossession"), p, "P", repl::Value(actor));
  return actor_rpc(pc, "ServerAcknowledgePossession", p);
}

using States = std::map<uint64_t, repl::ObjectState>;

static const schema::ClassSchema *block_schema_of(const repl::BlockView &b, const repl::Actor &actor, uint64_t &id) {
  if (b.header.is_actor) {
    id = 0;
    return actor.obj.schema;
  }
  const repl::Subobject *sub = actor.find(b.header.object);
  id = b.header.object.ObjectId;
  return sub ? sub->obj.schema : nullptr;
}

static States states_of(const fake::Delivered &d, const repl::Actor &actor) {
  States out;
  const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
  if (!blocks)
    return out;
  for (const repl::BlockView &b : *blocks) {
    uint64_t id = 0;
    const schema::ClassSchema *cls = block_schema_of(b, actor, id);
    if (!cls)
      continue;
    BunchWriter w;
    w.write_bit_range(d.content.data(), b.payload_pos, b.payload_bits);
    auto st = repl::load_object_state(*cls, w.data(), b.payload_bits, b.header.has_rep_layout);
    if (st)
      out.emplace(id, std::move(*st));
  }
  return out;
}

static std::optional<int64_t> int_of(const repl::ObjectState &o, const char *path) {
  const repl::Value *v = o.schema ? o.state.get(*o.schema, path) : nullptr;
  if (!v)
    return std::nullopt;
  if (const auto *i = v->get_if<int64_t>())
    return *i;
  if (const auto *u = v->get_if<uint64_t>())
    return static_cast<int64_t>(*u);
  if (const auto *b = v->get_if<bool>())
    return *b ? 1 : 0;
  return std::nullopt;
}

static std::optional<emit::FIntrepidNetworkGUID> object_of(const repl::ObjectState &o, const char *path) {
  const repl::Value *v = o.schema ? o.state.get(*o.schema, path) : nullptr;
  if (!v)
    return std::nullopt;
  if (const auto *g = v->get_if<emit::FIntrepidNetworkGUID>())
    return *g;
  return std::nullopt;
}

static size_t mount_count(const Harness &h) {
  return h.fx.world->registry().count(world::ActorKind::Mount);
}

struct PcMountRpcs {
  bool restart_mount = false;
  bool camera = false;
  bool clear = false;
  bool slot = false;
  size_t order_restart = 99;
  size_t order_camera = 99;
  size_t order_clear = 99;
  size_t order_slot = 99;
  size_t n = 0;
};

static void scan_pc_mount_rpc(const repl::RpcCall &c, const emit::FIntrepidNetworkGUID &mount_guid, PcMountRpcs &r) {
  if (!c.rpc)
    return;
  const std::string name = c.rpc->name;
  if (name == "ClientRestart") {
    const auto it = c.params.values.find(1);
    const auto *o = it != c.params.values.end() ? it->second.get_if<emit::FIntrepidNetworkGUID>() : nullptr;
    r.restart_mount = o && *o == mount_guid;
    r.order_restart = r.n;
  } else if (name == "ClientSetCameraMode") {
    r.camera = true;
    r.order_camera = r.n;
  } else if (name == "ClientClearActionbarsForInstigators") {
    r.clear = true;
    r.order_clear = r.n;
  } else if (name == "ClientReceiveActionbarSlotPayload") {
    r.slot = true;
    r.order_slot = r.n;
  }
  ++r.n;
}

static void check_mount_pc_rpcs(const Harness &h, size_t from, const emit::FIntrepidNetworkGUID &mount_guid) {
  PcMountRpcs r;
  for (const fake::Delivered *d : chains_on(h, h.pc_ch, from)) {
    for (const repl::RpcCall &c : calls_of(*d, "AoCPlayerControllerBP_C")) {
      scan_pc_mount_rpc(c, mount_guid, r);
    }
  }
  CHECK(r.restart_mount && r.camera && r.clear && r.slot, "the PlayerController gets ClientRestart(the mount), the camera mode and the bank-18 bar");
  CHECK(r.order_restart < r.order_camera && r.order_camera < r.order_clear && r.order_clear < r.order_slot, "in retail's order (910 packet 306)");
}

struct MountStateFlags {
  bool mount_actor = false;
  bool stats_rider = false;
  bool known = false;
  bool rider_ptr = false;
  bool interaction = false;
  bool owner_arrays = false;
};

static bool has_mount_owner_stats(const repl::ObjectState &o) {
  const schema::NetField *nf = o.schema->field("StatRepInt32Owner");
  if (!nf) {
    return false;
  }
  const repl::FastArrayState *a = o.arrays.find(nf->index);
  return a && a->items().size() > 20;
}

static void scan_mount_subobject(const repl::ObjectState &o, const bs::SessionGuids &g, MountStateFlags &f) {
  if (!o.schema) {
    return;
  }
  const std::string &cls = o.schema->name;
  if (cls == "UAoCStatsComponent") {
    f.stats_rider = true;
    f.owner_arrays = f.owner_arrays || has_mount_owner_stats(o);
  } else if (cls == "UAoCAbilityComponent") {
    f.known = f.known || o.state.get(*o.schema, "KnownAbilities") != nullptr;
  } else if (cls == "UNPCSecondaryInfo") {
    f.rider_ptr = f.rider_ptr || object_of(o, "RiderPtr") == g.pawn;
  } else if (cls == "UInteractableComponent") {
    f.interaction = f.interaction || o.state.get(*o.schema, "CurrentInputStates") != nullptr;
  }
}

static void check_mount_actor_states(const Harness &h, uint32_t mch, size_t from, const world::WorldActor &m) {
  const bs::SessionGuids &g = h.s->bootstrap().setup().guids;
  MountStateFlags f;
  for (const fake::Delivered *d : chains_on(h, mch, from)) {
    for (const auto &[id, o] : states_of(*d, m.actor)) {
      if (id == 0) {
        f.mount_actor = int_of(o, "RemoteRole") == 2 && object_of(o, "Owner") == g.player_controller && object_of(o, "Controller") == g.player_controller && object_of(o, "PlayerState") == g.player_state;
      } else {
        scan_mount_subobject(o, g, f);
      }
    }
  }
  CHECK(f.mount_actor, "the mount's actor: RemoteRole 2, Owner and Controller the PlayerController, the PlayerState (910 packet 307)");
  CHECK(f.stats_rider && f.owner_arrays, "its StatsComponent: the rider's owner stat arrays");
  CHECK(f.known && f.rider_ptr && f.interaction, "its KnownAbilities, RiderPtr and interaction state");
}

static bool is_rider_pawn_attached_actor(const repl::ObjectState &o, const emit::FIntrepidNetworkGUID &mount_guid) {
  const emit::FIntrepidNetworkGUID empty{};
  return int_of(o, "RemoteRole") == 1 && object_of(o, "AttachmentReplication.AttachParent") == mount_guid && object_of(o, "PlayerState") == empty &&
         object_of(o, "Controller") == empty && int_of(o, "AttachmentState.bAttachParentValid") == 1 && int_of(o, "AttachmentState.InteractableZoneID") == 2;
}

static void scan_rider_pawn_object(uint64_t id, const repl::ObjectState &o, const emit::FIntrepidNetworkGUID &mount_guid, bool &pawn_attached, bool &mount_ptr) {
  if (id == 0) {
    pawn_attached = is_rider_pawn_attached_actor(o, mount_guid);
  } else if (o.schema && o.schema->command("MountPtr")) {
    mount_ptr = object_of(o, "MountPtr") == mount_guid;
  }
}

static void check_rider_pawn_attached(const Harness &h, size_t from, const emit::FIntrepidNetworkGUID &mount_guid) {
  bool pawn_attached = false;
  bool mount_ptr = false;
  for (const fake::Delivered *d : chains_on(h, h.pawn_ch, from)) {
    for (const auto &[id, o] : states_of(*d, h.s->bootstrap().pawn())) {
      scan_rider_pawn_object(id, o, mount_guid, pawn_attached, mount_ptr);
    }
  }
  CHECK(pawn_attached, "the rider's pawn is attached to the mount (MountSocket), with no PlayerState or Controller (910 packet 310)");
  CHECK(mount_ptr, "its InteractInfo names the mount (MountPtr)");
}

static void check_pc_attached_and_ack(Harness &h, size_t from, const world::WorldActor &m) {
  const emit::FIntrepidNetworkGUID mount_guid = m.actor.guid;
  emit::FIntrepidNetworkGUID cylinder = mount_guid;
  cylinder.ObjectId = m.npc.collision_cylinder;
  bool pc_attached = false;
  for (const fake::Delivered *d : chains_on(h, h.pc_ch, from)) {
    const States st = states_of(*d, h.s->bootstrap().player_controller());
    const auto it = st.find(0);
    if (it != st.end() && object_of(it->second, "Pawn") == mount_guid && object_of(it->second, "AttachmentReplication.AttachParent") == mount_guid && object_of(it->second, "AttachmentReplication.AttachComponent") == cylinder) {
      pc_attached = true;
    }
  }
  CHECK(pc_attached, "the PlayerController is attached to the mount and its Pawn is the mount");
  h.client_sends({h.client.actor(h.pc_ch, possession_ack(mount_guid), true)});
  h.run(milliseconds(100));
  CHECK(!h.s->failed(), "the session takes the acknowledgement of the mount's possession");
  CHECK(h.s->ride() && h.s->ride()->speed_limit(1, 0) > 1400 && h.s->ride()->speed_limit(1, 0) < 1500, "the mount's move limit is its Blueprint's 990 cm/s times 1.5");
  CHECK(h.s->movement().good().x == m.x && h.s->movement().good().y == m.y, "the session's last good position is the mount's");
}

static void check_dismount_pawn_state(const repl::ObjectState &o, const bs::SessionGuids &g, const world::WorldActor &m, bool &detached, bool &detach_place) {
  const double yaw = m.yaw_degrees * 3.14159265358979 / 180.0;
  detached = int_of(o, "RemoteRole") == 2 && object_of(o, "AttachmentReplication.AttachParent") == emit::FIntrepidNetworkGUID{} && object_of(o, "PlayerState") == g.player_state && object_of(o, "Controller") == g.player_controller &&
             int_of(o, "AttachmentState.bAttachParentValid") == 0;
  const repl::Value *loc = o.state.get(*o.schema, "RepDetachmentLocation");
  const auto *v = loc ? loc->get_if<repl::Vec3d>() : nullptr;
  detach_place = v && std::fabs(v->x - (m.x + 54 * std::cos(yaw))) < 1.0 && std::fabs(v->y - (m.y + 54 * std::sin(yaw))) < 1.0 && std::fabs(v->z - (m.z + 197)) < 1.0;
}

static void scan_dismount_pc_rpc(const repl::RpcCall &c, const emit::FIntrepidNetworkGUID &pawn, bool &restart_pawn, size_t &restarts) {
  if (!c.rpc || std::string(c.rpc->name) != "ClientRestart") {
    return;
  }
  const auto it = c.params.values.find(1);
  const auto *o = it != c.params.values.end() ? it->second.get_if<emit::FIntrepidNetworkGUID>() : nullptr;
  restart_pawn = o && *o == pawn;
  ++restarts;
}

static void check_dismount_pc_restart(const Harness &h, size_t from2, const emit::FIntrepidNetworkGUID &pawn) {
  bool restart_pawn = false;
  size_t restarts = 0;
  for (const fake::Delivered *d : chains_on(h, h.pc_ch, from2)) {
    for (const repl::RpcCall &c : calls_of(*d, "AoCPlayerControllerBP_C")) {
      scan_dismount_pc_rpc(c, pawn, restart_pawn, restarts);
    }
  }
  CHECK(restart_pawn && restarts == 1, "the PlayerController is restarted on its own pawn, once (912 packet 139)");
}

struct DismountPawnFlags {
  bool detached = false;
  bool detach_place = false;
  bool unmounted_ptr = false;
};

static void scan_dismount_pawn_object(uint64_t id, const repl::ObjectState &o, const bs::SessionGuids &g, const world::WorldActor &m, DismountPawnFlags &f) {
  if (id == 0) {
    check_dismount_pawn_state(o, g, m, f.detached, f.detach_place);
  } else if (o.schema && o.schema->command("MountPtr")) {
    f.unmounted_ptr = object_of(o, "MountPtr") == emit::FIntrepidNetworkGUID{};
  }
}

static void check_dismount_pc_and_pawn(const Harness &h, size_t from2, const world::WorldActor &m) {
  const bs::SessionGuids &g = h.s->bootstrap().setup().guids;
  check_dismount_pc_restart(h, from2, g.pawn);
  DismountPawnFlags f;
  for (const fake::Delivered *d : chains_on(h, h.pawn_ch, from2)) {
    for (const auto &[id, o] : states_of(*d, h.s->bootstrap().pawn())) {
      scan_dismount_pawn_object(id, o, g, m, f);
    }
  }
  CHECK(f.detached && f.unmounted_ptr, "the pawn is detached: RemoteRole 2, its PlayerState and Controller back, MountPtr cleared (912 packet 139)");
  CHECK(f.detach_place, "RepDetachmentLocation is 54 cm ahead of the mount and 197 cm above its origin (912)");
}

static void check_dismount_mount_released(Harness &h, uint32_t mch, const world::WorldActor &m) {
  const size_t from2 = h.got.size();
  get_off(h);
  h.run(milliseconds(200));
  CHECK(!h.s->mounted() && h.s->dismounts() == 1, "ServerDismountInteraction puts the rider off the mount");
  check_dismount_pc_and_pawn(h, from2, m);
  bool mount_released = false;
  for (const fake::Delivered *d : chains_on(h, mch, from2)) {
    const States st = states_of(*d, h.mount()->actor);
    const auto it = st.find(0);
    if (it != st.end()) {
      mount_released = int_of(it->second, "RemoteRole") == 1 && object_of(it->second, "Owner") == emit::FIntrepidNetworkGUID{} && object_of(it->second, "Controller") == emit::FIntrepidNetworkGUID{};
    }
  }
  CHECK(mount_released, "the mount's Owner, Controller and PlayerState are released, RemoteRole 1 (912 packet 139)");
  CHECK(h.s->mounts()->phase() == MountController::Phase::Out && mount_count(h) == 1, "the mount stays");
  CHECK(h.s->movement().good().z > m.z + 150, "the pawn's checker starts where it lands");
}

static void test_get_on_and_off() {
  std::printf("getting on and off a mount\n");
  Harness h(cyb_record());
  h.play();
  summon_with(h, kMountSlot);
  h.run(milliseconds(300));
  CHECK(h.mount() != nullptr && h.s->mounts()->phase() == MountController::Phase::Summoning, "(the mount is being summoned)");
  get_on(h);
  h.run(milliseconds(100));
  CHECK(!h.s->mounted() && h.s->mount_interactions_refused() == 1, "an interaction while the mount is being summoned is refused");
  h.run(milliseconds(1500));
  CHECK(h.s->mounts()->phase() == MountController::Phase::Out, "(the mount is out)");
  const world::WorldActor *m = h.mount();
  const uint32_t mch = h.mount_ch();
  CHECK(m && mch != 0 && spawns::sub_of(*m, "UInteractableComponent") != nullptr, "(the mount is open on a channel)");
  if (!m || mch == 0)
    return;
  get_on(h);
  h.run(milliseconds(100));
  CHECK(!h.s->mounted() && h.s->mount_interactions_refused() == 2, "an interaction 575 cm from the mount is refused (too far)");
  walk_up_to_mount(h);
  const size_t from = h.got.size();
  get_on(h);
  h.run(milliseconds(200));
  CHECK(h.s->mounted() && h.s->mount_ups() == 1, "the player is on the mount");
  m = h.mount();
  check_mount_pc_rpcs(h, from, m->actor.guid);
  check_mount_actor_states(h, mch, from, *m);
  check_rider_pawn_attached(h, from, m->actor.guid);
  check_pc_attached_and_ack(h, from, *m);
  check_dismount_mount_released(h, mch, *m);
}

static void test_teleport_to_a_point_ground(Harness &h, GmTravel &travel) {
  walk_pawn_to(h, 600, 0);
  CHECK(h.s->moves_refused() == 0 && h.s->moves_acknowledged() > 0, "(the player walks)");
  const size_t from = h.got.size();
  const GmTeleportResult r = travel.teleport(to_point(5000, 3000), "the test");
  CHECK(r.status == GmStatus::Ok && r.character == "cyb" && r.ground_height && !r.dismounted, "a point with no z, over ground the server has: done, the height is the ground's");
  CHECK(near_to(r.to.x, 5000) && near_to(r.to.y, 3000) && near_to(r.to.z, 1098), "the player stands on the ground (terrain 1,000 + the capsule's 98 cm)");
  CHECK(near_to(r.from.x, 600, 5) && near_to(r.from.y, 0, 5), "it says where the player was");
  const GoodPosition g = h.s->movement().good();
  CHECK(near_to(g.x, 5000) && near_to(g.y, 3000) && near_to(g.z, 1098), "the move checker's last good position is the new place");
  h.run(milliseconds(100));
  CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 1, "the client is sent one ClientAdjustPosition");
  CHECK(h.s->gm_teleports() == 1, "(the teleport is counted)");
  const size_t acked = h.s->moves_acknowledged();
  const size_t refused = h.s->moves_refused();
  pawn_move_to(h, 5060, 3000, 1098);
  CHECK(h.s->moves_acknowledged() == acked + 1 && h.s->moves_refused() == refused, "the next move, from the new place, is accepted and not corrected");
  pawn_move_to(h, 660, 0, 1098);
  CHECK(h.s->moves_refused() == refused + 1, "a move from the old place is refused (it is a teleport back)");
  CHECK(!h.sink.positions.empty() && near_to(h.sink.positions.back().x, 5000) && near_to(h.sink.positions.back().y, 3000), "the new position is handed to the saver at once");
}

static void test_teleport_to_a_point_bounds(Harness &h, GmTravel &travel) {
  const GmTeleportResult with_z = travel.teleport(to_point_z(-4000, 1200, 1500), "the test");
  CHECK(with_z.status == GmStatus::Ok && !with_z.ground_height && near_to(with_z.to.z, 1500), "a point with z: z as given");
  const GmTeleportResult none = travel.teleport(to_point(100000, 100000), "the test");
  CHECK(none.status == GmStatus::InvalidArgument && none.message.find("give z") != std::string::npos, "a point with no ground under it and no z is refused, asking for z");
  CHECK(near_to(h.s->movement().good().x, -4000), "(and nothing moved)");
  const GmTeleportResult far_with_z = travel.teleport(to_point_z(100000, 100000, 12000), "the test");
  CHECK(far_with_z.status == GmStatus::Ok && near_to(far_with_z.to.z, 12000), "the same point with z is done");
  CHECK(travel.teleport(to_point_z(5.0e6, 0, 1000), "the test").status == GmStatus::InvalidArgument, "a point outside the world is refused");
  CHECK(travel.teleport(to_point_z(std::numeric_limits<double>::quiet_NaN(), 0, 1000), "the test").status == GmStatus::InvalidArgument, "a NaN is refused");
  CHECK(travel.teleport(to_point_z(0, std::numeric_limits<double>::infinity(), 1000), "the test").status == GmStatus::InvalidArgument, "an infinity is refused");
  GmTeleportRequest nobody = to_point_z(0, 0, 1000);
  nobody.character = "nobody";
  CHECK(travel.teleport(nobody, "the test").status == GmStatus::NotFound, "an unknown character is NotFound");
  nobody.character = "";
  CHECK(travel.teleport(nobody, "the test").status == GmStatus::InvalidArgument, "no character is InvalidArgument");
  GmTeleportRequest upper = to_point_z(0, 0, 1100);
  upper.character = "CyB";
  CHECK(travel.teleport(upper, "the test").status == GmStatus::Ok, "the character's name is matched without case");
}

static void test_teleport_to_a_point() {
  std::printf("teleport on foot: a point, the ground's height, the checker rebased\n");
  Harness h(cyb_on_the_ground(), on_the_ground);
  h.play();
  GmTravel travel = make_travel(h);
  test_teleport_to_a_point_ground(h, travel);
  test_teleport_to_a_point_bounds(h, travel);
}

static void test_teleport_to_a_place_nodes(Harness &h, GmTravel &travel) {
  const world::CityNodeInfo *joeva = g_nodes->by_name("Joeva");
  const world::CityNodeInfo *aela = g_nodes->by_name("New Aela");
  CHECK(joeva && aela, "(the registry has Joeva and New Aela)");
  if (!joeva || !aela)
    return;
  GmTeleportResult r = travel.teleport(to_place("joeva"), "the test");
  CHECK(r.status == GmStatus::Ok && r.place == "Verra_RVR_Joeva", "'joeva' is Verra_RVR_Joeva");
  CHECK(near_to(r.to.x, joeva->x, 0.01) && near_to(r.to.y, joeva->y, 0.01) && near_to(r.to.z, joeva->z + PlayerWorld::kPlaceLiftCm, 0.01),
        "the player is put at the node's recorded place, 150 cm above its z where the server has no ground");
  CHECK(near_to(h.s->movement().good().x, joeva->x, 0.01), "(the checker is there)");
  for (const char *spelling : {"New Aela", "Verra_RVR_NewAela", "RVR_NewAela", "newaela", "NEW_AELA"}) {
    r = travel.teleport(to_place(spelling), "the test");
    CHECK(r.status == GmStatus::Ok && r.place == "Verra_RVR_NewAela" && near_to(r.to.x, aela->x, 0.01), (std::string("'") + spelling + "' is New Aela").c_str());
  }
  r = travel.teleport(to_place("login"), "the test");
  const world::Places places = h.fx.world->places();
  const world::Place *login = places.find("login");
  CHECK(r.status == GmStatus::Ok && login && near_to(r.to.x, login->x, 0.01) && near_to(r.to.y, login->y, 0.01), "'login' is the default spawn");
  CHECK(travel.teleport(to_place("Login Spot"), "the test").status == GmStatus::Ok, "'Login Spot' is the same place");
  for (const char *bad : {"Narnia", "Bucktown", "Mt. Dun", "Open Ocean", "RVR"}) {
    const GmTeleportResult nope = travel.teleport(to_place(bad), "the test");
    CHECK(nope.status == GmStatus::NotFound, (std::string("'") + bad + "' is no place (test maps, nodes with no location, unknown names)").c_str());
  }
}

static bool has_place_named(const std::vector<world::Place> &list, const char *name) {
  for (const world::Place &p : list) {
    if (p.name == name) {
      return true;
    }
  }
  return false;
}

static bool has_origin_node_place(const std::vector<world::Place> &list) {
  for (const world::Place &p : list) {
    if (p.kind == "node" && p.x == 0 && p.y == 0) {
      return true;
    }
  }
  return false;
}

static void check_added_garden_place() {
  world::Places with_garden = world::Places::build(g_nodes.get(), 0, 0, 1098);
  CHECK(with_garden.find("garden") == nullptr, "no garden place before one is added");
  world::Place garden;
  garden.name = garden.display_name = garden.kind = "garden";
  garden.x = 1234;
  garden.y = 5678;
  garden.z = 90;
  with_garden.add(garden);
  const world::Place *found_garden = with_garden.find("Garden");
  CHECK(found_garden && near_to(found_garden->x, 1234, 0.01) && near_to(found_garden->y, 5678, 0.01) && found_garden->kind == "garden", "'garden' is the added place");
}

static void test_teleport_to_a_place_list(GmTravel &travel) {
  const std::vector<world::Place> list = travel.list_places();
  size_t expected = 1;
  for (const world::CityNodeInfo &n : g_nodes->nodes()) {
    expected += world::has_travel_place(n) ? 1 : 0;
  }
  CHECK(list.size() == expected && list.size() > 30 && list.size() < 81, "the places are the login spot and the nodes that have a place on the Verra map");
  const bool has_joeva = has_place_named(list, "Verra_RVR_Joeva");
  const bool has_lionhold = has_place_named(list, "Verra_RVR_Lionhold");
  const bool has_test = has_place_named(list, "Node_Playground_Bucktown");
  const bool has_origin = has_origin_node_place(list);
  CHECK(has_joeva && has_lionhold && !has_test && !has_origin, "Joeva and Lionhold are there; the test maps' nodes and the nodes at the origin are not");
  check_added_garden_place();
}

static void test_teleport_to_a_place() {
  std::printf("teleport to a place: the city nodes by name, and login\n");
  Harness h(cyb_on_the_ground(), on_the_ground);
  h.play();
  GmTravel travel = make_travel(h);
  test_teleport_to_a_place_nodes(h, travel);
  test_teleport_to_a_place_list(travel);
}

static void a_bowl_at(PlayerWorld &w, double cx, double cy) {
  on_the_ground(w);
  std::vector<world::TownFloor::Person> people;
  for (int i = -3; i <= 3; ++i) {
    for (int j = -3; j <= 3; ++j) {
      people.push_back({cx + i * 2000.0, cy + j * 2000.0, 1000.0, 6060.0});
    }
  }
  w.town_floor().set(4242, people);
}
static void a_bowl_at_5000_3000(PlayerWorld &w) {
  a_bowl_at(w, 5000, 3000);
}
static void a_bowl_at_the_origin(PlayerWorld &w) {
  a_bowl_at(w, 0, 0);
}

static constexpr double kTownFloor = 1000.0 + 5060.0;

static void test_teleport_into_a_bowl() {
  std::printf("teleport into a town's bowl: the town's floor, not the baked ground 50 m under it\n");
  Harness h(cyb_on_the_ground(), a_bowl_at_5000_3000);
  h.play();
  GmTravel travel = make_travel(h);
  GmTeleportResult r = travel.teleport(to_point(5000, 3000), "the test");
  CHECK(r.status == GmStatus::Ok && r.ground_height && near_to(r.to.z, kTownFloor + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm),
        "a point in the town with no z: the player is put on the town's floor (50.6 m over the baked plain) plus the slack");
  CHECK(near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm), "(the checker is there)");
  r = travel.teleport(to_point_z(5000, 3000, 1500), "the test");
  CHECK(r.status == GmStatus::Ok && !r.ground_height && near_to(r.to.z, 1500), "a GM who names z is believed (in a bowl too)");
  r = travel.teleport(to_point(5000 + 20000, 3000), "the test");
  CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1098), "200 m from the town: the baked ground + the capsule, as before");
  CHECK(h.fx.world->ground_at(5000, 3000) && near_to(*h.fx.world->ground_at(5000, 3000), kTownFloor + PlayerWorld::kBowlSlackCm), "(the world's ground_at says so for every caller)");
}

static void test_login_under_a_town() {
  std::printf("login: a character saved on the baked ground under a town is put on its floor\n");
  std::ostringstream log;
  auto previous = spdlog::default_logger();
  auto logger = std::make_shared<spdlog::logger>("bowl_login", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
  logger->set_level(spdlog::level::info);
  spdlog::set_default_logger(logger);
  {
    Harness h(cyb_on_the_ground(), a_bowl_at_the_origin);
    const double want = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm;
    CHECK(near_to(h.s->movement().good().z, want) && near_to(h.s->movement().good().x, 0, 0.01), "a character saved at the plain's height under the town joins on the town's floor (+ slack)");
    std::istringstream lines(log.str());
    std::string line;
    std::string said;
    size_t said_count = 0;
    while (std::getline(lines, line)) {
      if (line.find("under the floor of a town in a bowl") != std::string::npos) {
        said = line;
        ++said_count;
      }
    }
    CHECK(said_count == 1 && said.find("[session] cyb") != std::string::npos, "...and exactly one line says so");
    std::printf("    %s\n", said.c_str());
  }
  {
    svc::Character c = cyb_on_the_ground();
    c.position.z = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm;
    log.str("");
    Harness h(c, a_bowl_at_the_origin);
    CHECK(near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kCapsuleHalfHeightCm) && log.str().find("under the floor of a town in a bowl") == std::string::npos,
          "a character saved on the town's floor joins where it was, with no line");
  }
  {
    svc::Character c = cyb_on_the_ground();
    c.position.x = 30000;
    Harness h(c, a_bowl_at_the_origin);
    CHECK(near_to(h.s->movement().good().z, 1098.0), "a character saved on the open ground 300 m from the town joins where it was");
  }
  {
    Harness h(cyb_on_the_ground(), on_the_ground);
    CHECK(near_to(h.s->movement().good().z, 1098.0), "a world with no bowl: the character joins where it was");
  }
  spdlog::set_default_logger(previous);
}

static void a_platform_bowl(PlayerWorld &w, double cx, double cy, double floor, double slope = 0.0) {
  on_the_ground(w);
  w.set_town_platforms(tpfix::platforms_at(4242, cx, cy, floor, 0.0, tpfix::grid(30, 0.0, slope, 2700.0)));
}
static void a_platform_bowl_at_5000_3000(PlayerWorld &w) {
  a_platform_bowl(w, 5000, 3000, kTownFloor, 0.05);
}
static void a_platform_bowl_at_the_origin(PlayerWorld &w) {
  a_platform_bowl(w, 0, 0, kTownFloor);
}

static void test_teleport_into_a_platform_bowl() {
  std::printf("teleport into a bowl whose floor is the platform: its height at the point, not the baked pit, not the model\n");
  Harness h(cyb_on_the_ground(), a_platform_bowl_at_5000_3000);
  h.play();
  GmTravel travel = make_travel(h);
  GmTeleportResult r = travel.teleport(to_point(5000, 3000), "the test");
  CHECK(r.status == GmStatus::Ok && r.ground_height && near_to(r.to.z, kTownFloor + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm), "the platform's centre: the floor + the slack + the capsule (a pit of 1,000 under it)");
  r = travel.teleport(to_point(5000 + 2000, 3000 - 500), "the test");
  CHECK(r.status == GmStatus::Ok && near_to(r.to.z, kTownFloor + 100.0 + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm),
        "20 m along the slope (5 cm per metre): the floor there is 100 cm higher, and the player is put on that");
  CHECK(h.fx.world->ground_at(5000, 3000) && near_to(*h.fx.world->ground_at(5000, 3000), kTownFloor + PlayerWorld::kBowlSlackCm), "(the world's ground_at says so for every caller)");
  r = travel.teleport(to_point(5000 + 20000, 3000), "the test");
  CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1098), "200 m from the town: the baked ground + the capsule, as before");
  r = travel.teleport(to_point_z(5000, 3000, 9000), "the test");
  CHECK(r.status == GmStatus::Ok && !r.ground_height && near_to(r.to.z, 9000), "a z that is given is believed (a tower)");
  h.run(milliseconds(4000));
  CHECK(h.s->floor_net_rescues() == 0 && near_to(h.s->movement().good().z, 9000), "...and the net leaves a player who is above the floor alone");
}

static void test_a_hill_through_a_platform_town() {
  std::printf("a hill of the landscape that pokes through a platform town is the ground where it is higher\n");
  Harness h(cyb_on_the_ground(), [](PlayerWorld &w) { a_platform_bowl(w, 6000, 0, 1200.0); });
  h.play();
  GmTravel travel = make_travel(h);
  GmTeleportResult r = travel.teleport(to_point(6000, 0), "the test");
  CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1400.0 + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm), "on the hill top (the landscape 1,400, the platform 1,200) the player is put on the hill");
  r = travel.teleport(to_point(6000 + 2500, 0), "the test");
  CHECK(r.status == GmStatus::Ok && near_to(r.to.z, 1200.0 + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm), "past the hill's foot (the landscape at 1,067) on the platform");
  h.run(milliseconds(2500));
  CHECK(h.s->floor_net_rescues() == 0, "(the net does not touch either)");
}

static void test_login_under_a_platform_town() {
  std::printf("login: a character saved on the baked pit under a platform town joins on the platform's floor\n");
  std::ostringstream log;
  auto previous = spdlog::default_logger();
  auto logger = std::make_shared<spdlog::logger>("platform_login", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
  logger->set_level(spdlog::level::info);
  spdlog::set_default_logger(logger);
  {
    Harness h(cyb_on_the_ground(), a_platform_bowl_at_the_origin);
    const double want = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm;
    CHECK(near_to(h.s->movement().good().z, want) && near_to(h.s->movement().good().x, 0, 0.01), "saved at the pit's height (1,098) under the town: joins at the floor + the capsule + the slack");
    CHECK(log.str().find("under the floor of a town in a bowl") != std::string::npos, "...and the log says so");
  }
  {
    svc::Character c = cyb_on_the_ground();
    c.position.z = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + 300.0;
    Harness h(c, a_platform_bowl_at_the_origin);
    CHECK(near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + 300.0), "saved above the floor: joins where it was");
  }
  {
    svc::Character c = cyb_on_the_ground();
    c.position.z = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm - 2000.0;
    Harness h(c, a_platform_bowl_at_the_origin);
    CHECK(near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kCapsuleHalfHeightCm - 2000.0), "saved 20 m under the floor: left alone (only more than 25 m under it is the pit under the town)");
  }
  {
    svc::Character c = cyb_on_the_ground();
    c.position.z = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm - 3000.0;
    Harness h(c, a_platform_bowl_at_the_origin);
    CHECK(near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm), "saved 30 m under the floor: put on it");
  }
  spdlog::set_default_logger(previous);
}

static void test_floor_net_first_rescue(Harness &h, GmTravel &travel, std::ostringstream &log) {
  const double on_floor = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm;
  const size_t from = h.got.size();
  GmTeleportResult r = travel.teleport(to_point_z(5000, 3000, -50000), "the test");
  CHECK(r.status == GmStatus::Ok && near_to(r.to.z, -50000), "(a GM puts the player 500 m under the town: a z that is given is believed)");
  h.run(milliseconds(800));
  CHECK(h.s->floor_net_rescues() == 0 && near_to(h.s->movement().good().z, -50000), "0.8 s later it is still there (the net waits a second: one bad position is not a fall)");
  h.run(milliseconds(500));
  CHECK(h.s->floor_net_rescues() == 1 && near_to(h.s->movement().good().z, on_floor) && near_to(h.s->movement().good().x, 5000, 0.01) && near_to(h.s->movement().good().y, 3000, 0.01),
        "a second later it is back on the floor (floor + 98 + 200), at the same x and y");
  CHECK(corrections_on(h, h.pawn_ch, from, "PlayerPawn_C") == 2, "the client is sent two position corrections: the GM's and the net's");
  CHECK(!h.sink.positions.empty() && near_to(h.sink.positions.back().z, on_floor), "the saved position is the floor's (a crash must not take the player back under)");
  CHECK(log.str().find("[floor-net] cyb:") != std::string::npos && log.str().find("under the floor of") != std::string::npos, "the log says who, where and whose floor");
  std::istringstream lines(log.str());
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find("[floor-net] cyb: its last") != std::string::npos) {
      std::printf("    %s\n", line.c_str());
    }
  }
  r = travel.teleport(to_point_z(5000, 3000, -50000), "the test");
  h.run(milliseconds(1500));
  CHECK(h.s->floor_net_rescues() == 1, "a player that is under again 0.1 s after a rescue is not rescued again within 3 s of it");
  h.run(milliseconds(2000));
  CHECK(h.s->floor_net_rescues() == 2 && near_to(h.s->movement().good().z, on_floor), "...but is, once the 3 s have passed");
}

static void test_floor_net_pit_and_plain(Harness &h, GmTravel &travel, std::ostringstream &log) {
  const double on_floor = kTownFloor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm;
  travel.teleport(to_point_z(5000, 3000, kTownFloor - 9000.0), "the test");
  h.run(milliseconds(4500));
  CHECK(h.s->floor_net_rescues() == 2 && near_to(h.s->movement().good().z, kTownFloor - 9000.0), "90 m under the floor: left alone");
  travel.teleport(to_point_z(5000, 3000, kTownFloor - 10100.0), "the test");
  h.run(milliseconds(1500));
  CHECK(h.s->floor_net_rescues() == 3 && near_to(h.s->movement().good().z, on_floor), "101 m under the floor: put back");
  travel.teleport(to_point_z(5000, 3000, 1000.0 + PlayerWorld::kCapsuleHalfHeightCm + 1000.0), "the test");
  h.run(milliseconds(4500));
  CHECK(h.s->floor_net_rescues() == 3 && near_to(h.s->movement().good().z, 1000.0 + PlayerWorld::kCapsuleHalfHeightCm + 1000.0), "10 m over the pit, in the air: left alone (a fall through that height is not standing on it)");
  travel.teleport(to_point_z(5000, 3000, 1000.0 + PlayerWorld::kCapsuleHalfHeightCm), "the test");
  h.run(milliseconds(1500));
  CHECK(h.s->floor_net_rescues() == 4 && near_to(h.s->movement().good().z, on_floor), "standing on the baked pit 50 m under the town for a second: put back on the floor");
  CHECK(log.str().find("stands on the pit") != std::string::npos, "...and the log says it was the pit");
  travel.teleport(to_point_z(5000 + 20000, 3000, -9000.0), "the test");
  h.run(milliseconds(4500));
  CHECK(h.s->floor_net_rescues() == 4 && near_to(h.s->movement().good().z, -9000.0), "over the open plain, above the kill plane: left alone");
  travel.teleport(to_point_z(5000 + 20000, 3000, -20000.0), "the test");
  h.run(milliseconds(1500));
  CHECK(h.s->floor_net_rescues() == 5 && near_to(h.s->movement().good().z, 1000.0 + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm) && near_to(h.s->movement().good().x, 25000, 0.01),
        "over the open plain under the kill plane: put back on the baked ground (+ 98 + 200)");
  travel.teleport(to_point_z(5000, 3000, -50000), "the test");
  h.s->set_current_value(content::kGuidHealth, 0.0f);
  h.run(milliseconds(1800));
  CHECK(h.s->dead() && h.s->floor_net_rescues() == 5 && near_to(h.s->movement().good().z, -50000), "a dead player under the town is not touched by the net (it comes back at a shrine)");
}

static void test_the_floor_net() {
  std::printf("the floor net: a player far under a town's floor, or under the kill plane, is put back on it\n");
  std::ostringstream log;
  auto previous = spdlog::default_logger();
  auto logger = std::make_shared<spdlog::logger>("floor_net", std::make_shared<spdlog::sinks::ostream_sink_mt>(log));
  logger->set_level(spdlog::level::info);
  spdlog::set_default_logger(logger);
  {
    Harness h(cyb_on_the_ground(), a_platform_bowl_at_5000_3000);
    h.play();
    GmTravel travel = make_travel(h);
    test_floor_net_first_rescue(h, travel, log);
    test_floor_net_pit_and_plain(h, travel, log);
  }
  {
    Harness h(cyb_on_the_ground(), a_bowl_at_5000_3000);
    h.play();
    GmTravel travel = make_travel(h);
    travel.teleport(to_point_z(5000, 3000, -50000), "the test");
    h.run(milliseconds(1500));
    CHECK(h.s->floor_net_rescues() == 1 && near_to(h.s->movement().good().z, kTownFloor + PlayerWorld::kBowlSlackCm + PlayerWorld::kCapsuleHalfHeightCm), "without a platform the kill plane puts the player on the interim model's floor");
  }
  {
    Harness h(cyb_on_the_ground(), a_platform_bowl_at_5000_3000);
    h.run(milliseconds(1500));
    CHECK(h.s->floor_net_rescues() == 0, "(no rescue before the client's pawn has phased in)");
  }
  spdlog::set_default_logger(previous);
}

int main() {
  if (!init_gm_travel_globals()) {
    return 2;
  }
  test_get_on_and_off();
  test_teleport_to_a_point();
  test_teleport_to_a_place();
  test_teleport_into_a_bowl();
  test_login_under_a_town();
  test_teleport_into_a_platform_bowl();
  test_a_hill_through_a_platform_town();
  test_login_under_a_platform_town();
  test_the_floor_net();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
