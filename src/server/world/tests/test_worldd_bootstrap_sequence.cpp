// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "test_worldd_bootstrap_common.h"

#include "bootstrap/captured_gamestate.h"
#include "bootstrap/captured_level_list.h"
#include "bootstrap/captured_replies.h"
#include "capture_lingering.h"
#include "protocol/server_guid_allocator.h"

#include <bit>
#include <cmath>

namespace bs = aoc::worldd::bootstrap;
namespace captured = aoc::worldd::bootstrap::captured;
namespace repl = aoc::protocol::repl;
namespace wire = aoc::protocol::wire;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using aoc::protocol::ServerGuidAllocator;
using aoc::protocol::emit::BunchWriter;
using aoc::protocol::emit::FIntrepidNetworkGUID;
using namespace aoc::protocol::repl;
using bs::PlayerBootstrap;
using bs::Role;
using bs::Send;
using bs::StorageKind;

static aoc::protocol::repl::CodecOptions stat_options() {
  aoc::protocol::repl::CodecOptions o;
  o.stat_int32_extended = [](int64_t record) {
    const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
    return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
  };
  return o;
}

struct Block {
  bool is_actor = true;
  bool has_rep = false;
  bool dynamic = false;
  aoc::protocol::emit::FIntrepidNetworkGUID object;
  std::string name;
  std::optional<aoc::protocol::repl::RepState> rep;
  std::vector<aoc::protocol::repl::FastArrayField> fast;
  std::vector<aoc::protocol::repl::RpcCall> rpcs;
  size_t payload_bits = 0;
  bool read_to_end = false;
  std::vector<std::pair<uint32_t, size_t>> raw_fields;
};

struct Decoded {
  bool ok = false;
  aoc::protocol::emit::FIntrepidNetworkGUID actor;
  std::vector<Block> blocks;
  size_t new_actor_bits = 0;
  std::optional<aoc::protocol::repl::QuantVec> location;
  std::optional<aoc::protocol::repl::Rot> rotation;
};

static const aoc::protocol::schema::ClassSchema *class_of_role(aoc::worldd::bootstrap::Role r) {
  namespace gen = aoc::protocol::schema::gen;
  using aoc::worldd::bootstrap::Role;
  switch (r) {
  case Role::PlayerController:
    return gen::find_class_schema("AoCPlayerControllerBP_C");
  case Role::GameState:
    return gen::find_class_schema("AoCGameStateBP_C");
  case Role::Pawn:
    return gen::find_class_schema("PlayerPawn_C");
  case Role::PlayerState:
    return gen::find_class_schema("AoCPlayerStateBP_C");
  }
  return nullptr;
}

static bool read_new_actor(const aoc::protocol::emit::BunchWriter &w, bool player_controller, Decoded &d) {
  namespace repl = aoc::protocol::repl;
  aoc::protocol::wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
  auto read_guid = [&] {
    aoc::protocol::emit::FIntrepidNetworkGUID g;
    const uint64_t lo = r.read_uint32(), hi = r.read_uint32();
    g.ObjectId = lo | (hi << 32);
    g.ServerId = r.read_uint32();
    g.Randomizer = r.read_uint32();
    return g;
  };
  d.actor = read_guid();
  read_guid();
  read_guid();
  auto vector = [&](std::optional<repl::QuantVec> *out) {
    if (!r.read_bit())
      return true;
    if (!r.read_bit())
      return false;
    const auto v = repl::read_quantized_vector(r);
    if (!v)
      return false;
    if (out)
      *out = *v;
    return true;
  };
  if (!vector(&d.location))
    return false;
  if (r.read_bit()) {
    const auto rot = repl::read_struct("FRotator", r);
    const repl::Rot *p = rot ? rot->get_if<repl::Rot>() : nullptr;
    if (!p)
      return false;
    d.rotation = *p;
  }
  if (!vector(nullptr) || !vector(nullptr))
    return false;
  if (player_controller) {
    r.read_uint8();
    r.read_uint32();
  }
  if (r.overflowed())
    return false;
  d.new_actor_bits = r.pos();
  return true;
}

static const aoc::protocol::schema::ClassSchema *subobject_class(const aoc::worldd::bootstrap::PlayerBootstrap &pb, const aoc::protocol::emit::FIntrepidNetworkGUID &g, std::string *name, bool *dynamic) {
  for (const aoc::protocol::repl::Actor *a : {&pb.pawn(), &pb.player_controller()}) {
    if (const aoc::protocol::repl::Subobject *s = a->find(g)) {
      *dynamic = s->dynamic_class.has_value();
      *name = s->dynamic_class ? s->dynamic_class->front().name : s->name;
      return s->obj.schema;
    }
  }
  return nullptr;
}

static void decode_raw_rep_fields(const aoc::protocol::schema::ClassSchema &cls, const aoc::protocol::emit::BunchWriter &w, size_t end, size_t &p, Block &b) {
  namespace repl = aoc::protocol::repl;
  size_t r = p;
  while (r < end) {
    const auto h = repl::read_field_header(cls, w.data(), end, r);
    if (!h)
      break;
    b.raw_fields.emplace_back(h->field, h->payload_bits);
    r += h->payload_bits;
  }
  p = r;
}

static void decode_rep_block(const aoc::protocol::schema::ClassSchema &cls, const aoc::protocol::emit::BunchWriter &w, size_t end, size_t &p, Block &b) {
  namespace repl = aoc::protocol::repl;
  const repl::CodecOptions opts = stat_options();
  b.rep = repl::read_rep_block(cls, w.data(), end, p, opts);
  if (!b.rep || p >= end) {
    b.read_to_end = b.rep.has_value() && p == end;
    return;
  }
  size_t q = p;
  if (const auto f = repl::read_fast_array_fields(cls, w.data(), end, q, opts)) {
    b.fast = *f;
    p = q;
  } else {
    decode_raw_rep_fields(cls, w, end, p, b);
  }
  b.read_to_end = p == end;
}

static void decode_non_rep_block(const aoc::protocol::schema::ClassSchema &cls, const aoc::protocol::emit::BunchWriter &w, size_t end, size_t &p, Block &b) {
  namespace repl = aoc::protocol::repl;
  const repl::CodecOptions opts = stat_options();
  size_t q = p;
  if (const auto f = repl::read_fast_array_fields(cls, w.data(), end, q, opts)) {
    b.fast = *f;
    p = q;
  } else {
    q = p;
    if (const auto r = repl::read_rpc_fields(cls, w.data(), end, q, opts)) {
      b.rpcs = *r;
      p = q;
    }
  }
  b.read_to_end = p == end;
}

static Block decode_single_block(const aoc::worldd::bootstrap::PlayerBootstrap &pb, const aoc::worldd::bootstrap::Send &s, const aoc::protocol::repl::BlockView &v) {
  const aoc::protocol::emit::BunchWriter &w = s.emission.payload;
  Block b;
  b.is_actor = v.header.is_actor;
  b.has_rep = v.header.has_rep_layout;
  b.object = v.header.object;
  b.payload_bits = v.payload_bits;
  const aoc::protocol::schema::ClassSchema *cls = v.header.is_actor ? class_of_role(s.role) : subobject_class(pb, v.header.object, &b.name, &b.dynamic);
  size_t p = v.payload_pos;
  const size_t end = v.payload_pos + v.payload_bits;
  if (!cls || v.payload_bits == 0) {
    b.read_to_end = v.payload_bits == 0;
  } else if (v.header.has_rep_layout) {
    decode_rep_block(*cls, w, end, p, b);
  } else {
    decode_non_rep_block(*cls, w, end, p, b);
  }
  if (!v.header.is_actor && b.name.empty())
    b.name = "?";
  return b;
}

static Decoded decode(const aoc::worldd::bootstrap::PlayerBootstrap &pb, const aoc::worldd::bootstrap::Send &s) {
  namespace repl = aoc::protocol::repl;
  Decoded d;
  const aoc::protocol::emit::BunchWriter &w = s.emission.payload;
  size_t pos = 0;
  if (s.opens_channel) {
    if (!read_new_actor(w, s.role == aoc::worldd::bootstrap::Role::PlayerController, d))
      return d;
    pos = d.new_actor_bits;
  }
  const auto views = repl::read_content_blocks(w.data(), w.bit_pos(), pos);
  if (!views)
    return d;
  d.ok = true;
  for (const repl::BlockView &v : *views)
    d.blocks.push_back(decode_single_block(pb, s, v));
  return d;
}

static std::string block_summary(const Block &b) {
  std::string out = b.is_actor ? "actor" : "sub:" + b.name;
  if (!b.rpcs.empty()) {
    for (const aoc::protocol::repl::RpcCall &c : b.rpcs)
      out += std::string(".") + c.rpc->name;
    return out;
  }
  if (b.payload_bits == 0)
    return out + "(empty)";
  if (!b.has_rep)
    return out + "(fast)";
  return out + ((b.fast.empty() && b.raw_fields.empty()) ? "(rep)" : "(rep+fast)");
}

static std::string summary(const Decoded &d) {
  std::string out;
  for (const Block &b : d.blocks) {
    if (!out.empty())
      out += ' ';
    out += block_summary(b);
  }
  return out;
}

static bool all_read(const Decoded &d) {
  if (!d.ok)
    return false;
  for (const Block &b : d.blocks) {
    if (!b.read_to_end)
      return false;
  }
  return !d.blocks.empty();
}

static const aoc::protocol::repl::Value *value_of(const Block &b, const aoc::protocol::schema::ClassSchema &cls, std::string_view path) {
  return b.rep ? b.rep->get(cls, path) : nullptr;
}

static size_t parts_of(const aoc::worldd::bootstrap::Send &s) {
  const size_t bits = s.emission.payload.bit_pos();
  const size_t content = bits <= aoc::worldd::Connection::kMaxSingleBunchBits ? 1 : (bits + 7615) / 7616;
  return s.emission.exports.size() + content;
}

static std::optional<std::vector<aoc::worldd::bootstrap::Send>> sequence_of(aoc::worldd::bootstrap::PlayerBootstrap &pb) {
  auto seq = pb.join_sequence();
  if (!seq)
    std::printf("    join_sequence failed (%s)\n", pb.error().c_str());
  return seq;
}

static void log_sequence_bits(const std::vector<aoc::worldd::bootstrap::Send> &seq) {
  using aoc::worldd::bootstrap::Send;
  static const char *const kRole[] = {"PC", "GameState", "pawn", "PlayerState"};
  size_t total_bits = 0;
  for (const Send &s : seq) {
    size_t export_bits = 0;
    for (const aoc::protocol::emit::BunchWriter &e : s.emission.exports)
      export_bits += e.bit_pos();
    total_bits += export_bits + s.emission.payload.bit_pos();
    std::printf("    %-11s %-40s exports %5zu bits, payload %6zu bits, %2zu parts\n", kRole[static_cast<size_t>(s.role)], s.what, export_bits, s.emission.payload.bit_pos(), parts_of(s));
  }
  std::printf("    in all %zu bits (%.1f KB)\n", total_bits, total_bits / 8192.0);
}

static bool exports_within_limits(const aoc::worldd::bootstrap::Send &s) {
  for (const aoc::protocol::emit::BunchWriter &e : s.emission.exports) {
    if (e.bit_pos() > aoc::worldd::Connection::kMaxSingleBunchBits || e.bit_pos() == 0)
      return false;
  }
  return true;
}

static bool send_shape_within_limits(const aoc::worldd::bootstrap::Send &s) {
  if (s.emission.exported_by_bunch.size() != s.emission.exports.size())
    return false;
  return s.emission.payload.bit_pos() > 0 && s.emission.payload.bit_pos() <= aoc::worldd::Connection::kMaxPartialBits;
}

static size_t max_channel_parts(const std::map<aoc::worldd::bootstrap::Role, size_t> &per_channel) {
  size_t worst = 0;
  for (const auto &[role, parts] : per_channel)
    worst = std::max(worst, parts);
  return worst;
}

static void check_limits(const std::vector<aoc::worldd::bootstrap::Send> &seq, const char *what) {
  using aoc::worldd::bootstrap::Role;
  using aoc::worldd::bootstrap::Send;
  bool parts_ok = true, export_ok = true, shape_ok = true;
  std::map<Role, size_t> per_channel;
  log_sequence_bits(seq);
  for (const Send &s : seq) {
    const size_t parts = parts_of(s);
    parts_ok = parts_ok && parts <= aoc::worldd::Connection::kMaxUnackedReliable;
    export_ok = export_ok && exports_within_limits(s);
    shape_ok = shape_ok && send_shape_within_limits(s);
    if (s.reliable())
      per_channel[s.role] += parts;
  }
  const size_t worst = max_channel_parts(per_channel);
  const bool window_ok = worst <= aoc::worldd::Connection::kMaxUnackedReliable;
  char msg[200];
  std::snprintf(msg, sizeof msg, "%s: every chain has at most 256 parts and every export bunch at most 7621 bits", what);
  CHECK(parts_ok && export_ok && shape_ok, msg);
  std::snprintf(msg, sizeof msg, "%s: no channel has more than 256 reliable parts in flight if nothing is acknowledged (the most: %zu)", what, worst);
  CHECK(window_ok, msg);
}

static void check_sequence_blocks(const std::vector<Send> &s, const std::vector<Decoded> &d) {
  CHECK(summary(d[0]) == "actor(rep) sub:GlobalGMCommands(empty)", "PlayerController open: the actor's properties and the dynamic GlobalGMCommands");
  CHECK(summary(d[1]) == "actor.ClientSetHUD", "ClientSetHUD");
  CHECK(summary(d[2]) == "actor.ClientVoiceHandshakeComplete actor.ClientEnableNetworkVoice actor.ClientUpdateMultipleLevelsStreamingStatusCompressed", "voice handshake, voice, the level list");
  CHECK(summary(d[3]) == "actor.ClientFlushLevelStreaming actor.ClientCapBandwidth actor.ClientSetTime", "flush, bandwidth cap, clock");
  CHECK(summary(d[4]) == "actor(rep)" || summary(d[4]) == "actor(rep+fast)", "GameState: one actor block");
  CHECK(summary(d[5]) == "actor(rep)", "the load tracker: the PlayerController's block");
  CHECK(summary(d[6]) == "actor(rep) sub:BaseCharacterInfo(rep) sub:CombatInfo(rep) sub:OwnerInfo(empty) sub:BackpackComponent(empty) "
                         "sub:EquipmentComponent(empty) sub:QuestStorageComponent(empty) sub:RewardStorageComponent(empty) sub:Character Appearance(rep)",
        "pawn open: the actor and its eight components in the original's order");
  for (size_t i = 7; i <= 10; ++i)
    CHECK(summary(d[i]) == "actor.ClientReceiveStoragePayload", "a ClientReceiveStoragePayload on the pawn");
  CHECK(summary(d[11]) == "actor.ClientRestart actor.ClientSetCameraMode actor.ClientSetRotation", "ClientRestart, camera mode, rotation");
  CHECK(summary(d[12]) == "actor(rep)", "the PlayerController after possession");
  CHECK(summary(d[13]) == "actor(rep) sub:AlignmentComponent(rep) sub:BaseCharacterInfo(rep+fast) sub:CombatInfo(rep) sub:AbilityComponent(rep) "
                          "sub:StatsComponent(fast) sub:OwnerInfo(rep) sub:Character Appearance(rep) sub:ResourceStorageComponent(rep)",
        "pawn second state: the actor, Alignment, BaseCharacterInfo with HeldItems, CombatInfo, Ability, Stats, OwnerInfo, appearance ids, the resource storage");
  CHECK(summary(d[14]) == "actor(rep)", "PlayerState open: the actor");
  CHECK(summary(d[15]).find("sub:XPTracker") != std::string::npos && !s[15].opens_channel, "the XPTracker: the PlayerController's last chain, a component block on the open channel");
}

static void check_sequence_pc_open(const Decoded &d0) {
  const schema::ClassSchema &pc_cls = *class_of_role(Role::PlayerController);
  const Block &a = d0.blocks[0];
  const Value *spawn = value_of(a, pc_cls, "SpawnLocation");
  const Vec3d *v = spawn ? spawn->get_if<Vec3d>() : nullptr;
  const Value *ps = value_of(a, pc_cls, "PlayerState");
  const auto *psg = ps ? ps->get_if<FIntrepidNetworkGUID>() : nullptr;
  CHECK(d0.actor == guid(10341530, 60, 1860730596u) && v && v->x == -777664.5 && v->y == 616578.6875 && psg && psg->ObjectId == 10341528, "PlayerController open: the actor GUID, SpawnLocation (-777664.5, 616578.6875, 15952.05) and the PlayerState's GUID");
  CHECK(d0.location && d0.location->x == -7776645 && d0.location->y == 6165787 && d0.location->z == 159520, "SerializeNewActor carries the spawn point (x10 quantized)");
}

static void check_sequence_pawn_open_combat_and_appearance(const Decoded &d6) {
  const schema::ClassSchema &combat_cls = *gen::find_component_schema("CombatInfo");
  const Value *cls = value_of(d6.blocks[2], combat_cls, "PrimaryClassId.Guid");
  CHECK(cls && *cls == Value::i64(108640158570105ll), "pawn open: CombatInfo.PrimaryClassId is the Bard's record Guid");
  const schema::ClassSchema &app_cls = *gen::find_component_schema("Character Appearance");
  const Value *custom = value_of(d6.blocks[8], app_cls, "CharacterCustomization");
  const auto *cc = custom ? custom->get_if<CharacterCustomization>() : nullptr;
  CHECK(cc && cc->random_seed && *cc->random_seed == 1454 && cc->sections_values && cc->sections_values->size() == 30 && cc->face_morph_weights && cc->face_morph_weights->size() == 83, "pawn open: the customization (RandomSeed 1454, 30 SectionsValues, 83 FaceMorphWeightMaps)");
}

static void check_sequence_pawn_open_subobjects(const Decoded &d6) {
  const Block &info = d6.blocks[1];
  const schema::ClassSchema &info_cls = *gen::find_component_schema("BaseCharacterInfo");
  const Value *name = value_of(info, info_cls, "CharacterName");
  const Value *ga = value_of(info, info_cls, "CharacterGuid.A");
  const Value *d4 = value_of(info, info_cls, "CharacterGuid.D");
  CHECK(name && *name == Value::str("RandomChar") && ga && *ga == Value::i64(static_cast<int32_t>(3635609113u)) && d4 && *d4 == Value::i64(static_cast<int32_t>(1814768596u)), "pawn open: CharacterName and the four CharacterGuid words");
  CHECK(!value_of(info, info_cls, "Race") && !value_of(info, info_cls, "Gender") && !value_of(info, info_cls, "Level"), "a level-1 Kaelar male sends no Race, Gender or Level (the class defaults)");
  check_sequence_pawn_open_combat_and_appearance(d6);
}

static void check_sequence_pawn_open_actor(const Decoded &d6) {
  const schema::ClassSchema &pawn_cls = *class_of_role(Role::Pawn);
  const Block &actor = d6.blocks[0];
  const Value *owner = value_of(actor, pawn_cls, "Owner");
  const Value *remote = value_of(actor, pawn_cls, "RemoteRole");
  const Value *move = value_of(actor, pawn_cls, "ReplicatedMovement");
  const auto *o = owner ? owner->get_if<FIntrepidNetworkGUID>() : nullptr;
  const auto *m = move ? move->get_if<RepMovement>() : nullptr;
  CHECK(o && o->ObjectId == 10341530 && remote && *remote == Value::u64(1) && m && m->location.x == -77766450 && m->rotation.yaw == 65, "pawn open: Owner is the PlayerController, RemoteRole 1 (simulated), ReplicatedMovement at the spawn point, yaw byte 65");
  CHECK(d6.rotation && d6.rotation->yaw == 16550, "SerializeNewActor's rotation is the yaw as a compressed short, 16550");
}

static void check_sequence_pc_and_pawn_open(const std::vector<Decoded> &d) {
  check_sequence_pc_open(d[0]);
  check_sequence_pawn_open_subobjects(d[6]);
  check_sequence_pawn_open_actor(d[6]);
}

static void check_sequence_second_state_components(const Decoded &d13) {
  const Block &stats = d13.blocks[5];
  size_t items = 0, fields = 0;
  std::map<uint32_t, size_t> by_field;
  for (const FastArrayField &f : stats.fast) {
    items += f.delta.changed.size();
    by_field[f.field] = f.delta.changed.size();
    ++fields;
  }
  CHECK(fields == 4 && items == 443 && by_field[3] == 335 && by_field[5] == 96 && by_field[6] == 11 && by_field[10] == 1, "pawn second state: StatsComponent has 335 Owner, 96 OwnerProxy and 11 EveryoneProxy stats and the float array's item (442 stats)");
  const schema::ClassSchema &ab_cls = *gen::find_component_schema("AbilityComponent");
  const Value *known = value_of(d13.blocks[4], ab_cls, "KnownAbilities");
  const Value *records = value_of(d13.blocks[4], ab_cls, "ReplicatedRecordsToLoad");
  const auto *ka = known ? known->get_if<ArrayValue>() : nullptr;
  const auto *ra = records ? records->get_if<ArrayValue>() : nullptr;
  CHECK(ka && ra && ka->elements.size() == 57 && ra->elements.size() == 58, "pawn second state: 57 KnownAbilities and 58 ReplicatedRecordsToLoad");
}

static void check_sequence_second_state_actor_and_info(const Decoded &d13) {
  const schema::ClassSchema &pawn_cls = *class_of_role(Role::Pawn);
  const Value *hidden = value_of(d13.blocks[0], pawn_cls, "bHidden");
  const Value *remote = value_of(d13.blocks[0], pawn_cls, "RemoteRole");
  CHECK(hidden && *hidden == Value::boolean(true) && remote && *remote == Value::u64(2), "pawn second state: bHidden 1, RemoteRole 2 (autonomous)");
  const schema::ClassSchema &info_cls = *gen::find_component_schema("BaseCharacterInfo");
  const Value *server = value_of(d13.blocks[2], info_cls, "ServerId");
  const Value *phased = value_of(d13.blocks[2], info_cls, "bIsPhasedOut");
  const Value *possessed = value_of(d13.blocks[2], info_cls, "bHasBeenPossessed");
  CHECK(server && *server == Value::i64(60) && phased && *phased == Value::boolean(true) && possessed && *possessed == Value::boolean(true), "pawn second state: ServerId 60, bIsPhasedOut 1, bHasBeenPossessed 1");
}

static void check_sequence_tracker(const std::vector<Decoded> &d) {
  const schema::ClassSchema &pc_cls = *class_of_role(Role::PlayerController);
  const Value *name = value_of(d[12].blocks[0], pc_cls, "Name");
  const Value *tracker = value_of(d[12].blocks[0], pc_cls, "CharacterLoadTracker");
  const auto *t = tracker ? tracker->get_if<LoadTracker>() : nullptr;
  CHECK(name && *name == Value::str("RandomChar") && t && t->flags.size() == 25 && !t->flags[0] && t->flags[1] && t->flags[24], "after possession: Name and CharacterLoadTracker 0111...1");
  const Value *tr0 = value_of(d[5].blocks[0], pc_cls, "CharacterLoadTracker");
  const auto *t0 = tr0 ? tr0->get_if<LoadTracker>() : nullptr;
  CHECK(t0 && t0->flags.size() == 25 && t0->flags[0] && !t0->flags[1] && t0->flags[2], "before possession: CharacterLoadTracker 1011...1");
}

static void check_sequence_second_state_and_tracker(const std::vector<Decoded> &d) {
  check_sequence_second_state_components(d[13]);
  check_sequence_second_state_actor_and_info(d[13]);
  check_sequence_tracker(d);
  const schema::ClassSchema &ps_cls = *class_of_role(Role::PlayerState);
  const Value *pid = value_of(d[14].blocks[0], ps_cls, "PlayerId");
  const Value *uid = value_of(d[14].blocks[0], ps_cls, "UniqueId");
  const auto *u = uid ? uid->get_if<UniqueNetId>() : nullptr;
  CHECK(pid && *pid == Value::i64(1874) && u && u->flags == 8 && u->id == "DESKTOP-EXAMPLE1-US-45739BC044607BD9A50196AF41F22492", "PlayerState: PlayerId and the login's unique id");
}

static void check_sequence_gamestate_captured(const Decoded &d4, const Block &g) {
  CHECK(d4.actor == guid(4608, 60, 1860730596u) && g.rep && g.raw_fields.size() == 5 && g.read_to_end, "Captured: the original's actor block (GUID 4608), its RepLayout part and the five fast-array fields up to the last bit");
  CHECK(g.raw_fields.size() == 5 && g.raw_fields[0].first == 25 && g.raw_fields[1].first == 26 && g.raw_fields[2].first == 28 && g.raw_fields[3].first == 29 && g.raw_fields[4].first == 32, "Captured: EstateIDToOwnerGuid, NodeIDToClientNodeData, the two node maps and ReplicatedGlobalQuestCache");
}

static void check_sequence_gamestate_tier_handles(bs::GameStateMode mode, const Block &g, const schema::ClassSchema &gs_cls, size_t handles) {
  if (mode == bs::GameStateMode::Tier1) {
    CHECK(handles == 6 && !value_of(g, gs_cls, "GameTimeScalar") && !value_of(g, gs_cls, "ServiceConnectionFlags") && g.fast.empty(), "Tier 1: six handles (AuthServerIDReplicated, RemoteRole, Role, GameModeClass, bReplicatedHasBegunPlay, the world time), no combat settings, no world arrays");
  } else {
    const Value *scalar = value_of(g, gs_cls, "GameTimeScalar");
    const Value *flags = value_of(g, gs_cls, "ServiceConnectionFlags");
    const Value *combat = value_of(g, gs_cls, "CombatSettings.General.InCombatTimer");
    CHECK(handles == 33 && scalar && *scalar == Value::f32(6) && flags && *flags == Value::u64(18357) && combat && *combat == Value::f32(6) && g.fast.empty(), "Tier 2: 33 handles: GameTimeScalar 6, ServiceConnectionFlags 18357 and the 25 combat settings, no world arrays");
  }
}

static void check_sequence_gamestate_mode(bs::GameStateMode mode, const std::vector<Decoded> &d) {
  const schema::ClassSchema &gs_cls = *class_of_role(Role::GameState);
  const Block &g = d[4].blocks[0];
  if (mode == bs::GameStateMode::Captured) {
    check_sequence_gamestate_captured(d[4], g);
    return;
  }
  const Value *has_begun = value_of(g, gs_cls, "bReplicatedHasBegunPlay");
  const Value *mode_class = value_of(g, gs_cls, "GameModeClass");
  const auto *mc = mode_class ? mode_class->get_if<FIntrepidNetworkGUID>() : nullptr;
  const Value *time = value_of(g, gs_cls, "ReplicatedWorldTimeSecondsDouble");
  const size_t handles = g.rep ? g.rep->values().size() : 0;
  CHECK(d[4].actor == guid(4608, 60, 1860730596u) && has_begun && *has_begun == Value::boolean(true) && mc && mc->ObjectId == 17588721769397045729ull && time && *time == Value::f64(1593462.375), "GameState: bReplicatedHasBegunPlay 1, GameModeClass AoCGameModeBaseBP_C, the world time");
  check_sequence_gamestate_tier_handles(mode, g, gs_cls, handles);
}

static std::vector<Decoded> decode_and_check_sequence(const PlayerBootstrap &pb, const std::vector<Send> &s) {
  std::vector<Decoded> d;
  for (const Send &x : s)
    d.push_back(decode(pb, x));
  bool all = true;
  for (size_t i = 0; i < d.size(); ++i) {
    if (!all_read(d[i])) {
      std::printf("    chain %zu (%s) does not read to its last bit\n", i, s[i].what);
      all = false;
    }
  }
  CHECK(all, "every block of every chain reads to its last bit with read_content_blocks / read_rep_block / read_fast_array_fields / read_rpc_fields");
  return d;
}

static void test_sequence(bs::GameStateMode mode, const char *name) {
  std::printf("the join sequence, GameState %s\n", name);
  PlayerBootstrap pb = captured_bootstrap(mode);
  const auto seq = sequence_of(pb);
  CHECK(seq.has_value() && seq->size() == 16, "join_sequence: sixteen chains (plan D1 added the XPTracker)");
  if (!seq || seq->size() != 16)
    return;
  const std::vector<Send> &s = *seq;

  const Role roles[] = {Role::PlayerController, Role::PlayerController, Role::PlayerController, Role::PlayerController, Role::GameState, Role::PlayerController, Role::Pawn, Role::Pawn, Role::Pawn, Role::Pawn, Role::Pawn, Role::PlayerController, Role::PlayerController, Role::Pawn, Role::PlayerState, Role::PlayerController};
  bool order = true;
  for (size_t i = 0; i < s.size(); ++i)
    order = order && s[i].role == roles[i];
  CHECK(order, "order: PlayerController open, HUD, two RPC bunches, GameState, tracker, pawn open, four storage payloads, restart, possession, pawn second state, PlayerState, the XPTracker");
  CHECK(s[0].opens_channel && s[4].opens_channel && s[6].opens_channel && s[14].opens_channel, "the four actors' first chains open their channels");
  int opens = 0;
  for (const Send &x : s)
    opens += x.opens_channel ? 1 : 0;
  CHECK(opens == 4, "and no other chain does");
  bool reliable = true;
  for (const Send &x : s)
    reliable = reliable && x.reliable();
  CHECK(reliable, "every chain of the join is reliable");

  const std::vector<Decoded> d = decode_and_check_sequence(pb, s);
  check_sequence_blocks(s, d);
  check_sequence_pc_and_pawn_open(d);
  check_sequence_second_state_and_tracker(d);
  check_sequence_gamestate_mode(mode, d);
  check_limits(s, name);
}

static void check_native_refusals_and_restore() {
  bs::PlayerSetup bad = captured_setup();
  bad.name = std::string("a\0b", 3);
  PlayerBootstrap broken(bad, bs::GameStateMode::Tier1);
  CHECK(!broken.ok() && broken.error().find("CharacterName") != std::string::npos && !broken.join_sequence(), "a character name with a NUL is refused at construction, naming the property");
  {
    bs::PlayerSetup nan = captured_setup();
    nan.x = std::nan("");
    bs::PlayerSetup late = captured_setup();
    late.world_time = 3e9;
    bs::PlayerSetup race = captured_setup();
    race.race = 9;
    bs::PlayerSetup gender = captured_setup();
    gender.gender = 2;
    CHECK(!PlayerBootstrap(nan, bs::GameStateMode::Tier1).ok() && !PlayerBootstrap(late, bs::GameStateMode::Tier1).ok() && !PlayerBootstrap(race, bs::GameStateMode::Tier1).ok() && !PlayerBootstrap(gender, bs::GameStateMode::Tier1).ok(), "a spawn point that is not finite, a world time past an int32 and a race or gender the client does not have are refused");
  }

  PlayerBootstrap again(captured_setup(), bs::GameStateMode::Tier2);
  again.pawn_open();
  const auto cp = again.checkpoint();
  const auto first = again.pawn_second_state();
  again.restore(cp);
  const auto second = again.pawn_second_state();
  CHECK(first && second && first->emission.payload.bit_pos() == second->emission.payload.bit_pos() && capture_blocks::same_bit_range(first->emission.payload.data(), second->emission.payload.data(), first->emission.payload.bit_pos(), nullptr), "restore(checkpoint) puts the shadows back: the refused update is built again, the same bits");
}

static void check_native_character_info(const std::vector<Decoded> &d) {
  const schema::ClassSchema &info_cls = *gen::find_component_schema("BaseCharacterInfo");
  const Block &info = d[6].blocks[1];
  const Value *race = value_of(info, info_cls, "Race");
  const Value *gender = value_of(info, info_cls, "Gender");
  const Value *level = value_of(info, info_cls, "Level");
  const Value *b = value_of(info, info_cls, "CharacterGuid.B");
  CHECK(race && *race == Value::u64(6) && gender && *gender == Value::u64(1) && level && *level == Value::i64(7) && b && *b == Value::i64(static_cast<int32_t>(0x80000000u)), "BaseCharacterInfo: Race (Dunir 6), Gender 1, Level 7 and the Guid words as int32");
  CHECK(summary(d[13]) == "actor(rep) sub:AlignmentComponent(rep) sub:BaseCharacterInfo(rep) sub:CombatInfo(rep) sub:AbilityComponent(rep) "
                          "sub:StatsComponent(fast) sub:OwnerInfo(rep) sub:ResourceStorageComponent(rep)",
        "unequipped: no HeldItems and no AppearanceIDs in the second state");
  CHECK(d[0].location && d[0].location->x == 10003 && d[0].location->y == -20005 && d[0].location->z == 3000, "the spawn point is quantized x10, halves rounded away from zero: 1000.25 -> 10003, -2000.5 -> -20005");
}

static void check_native_character_pawn_and_names(const std::vector<Decoded> &d) {
  const schema::ClassSchema &pawn_cls = *class_of_role(Role::Pawn);
  const Value *move = value_of(d[6].blocks[0], pawn_cls, "ReplicatedMovement");
  const auto *m = move ? move->get_if<RepMovement>() : nullptr;
  CHECK(m && m->rotation.yaw == 192 && d[6].rotation && d[6].rotation->yaw == 49152, "yaw -90 degrees: the byte 192 and the compressed short 49152");
  const Value *ps_name = value_of(d[14].blocks[0], *class_of_role(Role::PlayerState), "PlayerNamePrivate");
  CHECK(ps_name && *ps_name == Value::str("dev-machine"), "the PlayerState carries the login name, not the character's");
  const Value *name = value_of(d[12].blocks[0], *class_of_role(Role::PlayerController), "Name");
  CHECK(name && *name == Value::str("cyb"), "the PlayerController's Name is the character's");
}

static void test_native_character() {
  std::printf("a character that is not the capture's\n");
  bs::PlayerSetup s = captured_setup();
  s.name = "cyb";
  s.race = static_cast<uint8_t>(bs::ClientRace::Dunir);
  s.gender = static_cast<uint8_t>(bs::ClientGender::Female);
  s.level = 7;
  s.x = 1000.25;
  s.y = -2000.5;
  s.z = 300;
  s.yaw_degrees = -90;
  s.held_items = {};
  s.appearance_ids.clear();
  s.weapon_skill_line = {};
  s.character_guid = {1, 0x80000000u, 3, 0xFFFFFFFFu};
  s.server_id = 7;
  s.world_time = 1000.5;
  s.player_id = 3;
  s.login.name_option = "dev-machine";
  s.login.unique_id = "dev-machine";
  PlayerBootstrap pb(s, bs::GameStateMode::Tier2);
  CHECK(pb.ok(), "a native setup builds");
  const auto seq = sequence_of(pb);
  CHECK(seq.has_value() && seq->size() == 16, "the whole sequence builds");
  if (!seq)
    return;
  std::vector<Decoded> d;
  for (const Send &x : *seq)
    d.push_back(decode(pb, x));
  bool all = true;
  for (const Decoded &x : d)
    all = all && all_read(x);
  CHECK(all, "every block reads to its last bit");
  check_native_character_info(d);
  check_native_character_pawn_and_names(d);
  check_native_refusals_and_restore();
}

static void check_captured_bard_parts() {
  const bs::StatsSetup stats = bs::captured_bard_level1_stats();
  CHECK(stats.owner.items.size() == 335 && stats.owner_proxy.items.size() == 96 && stats.everyone_proxy.items.size() == 11 && stats.float_everyone_proxy.size() == 1 && stats.owner.array_key == 417, "the captured Bard's stats: 335 + 96 + 11 and one float item, Owner key 417");
  size_t extended = 0;
  for (const auto &it : stats.owner.items)
    extended += it.extended ? 1 : 0;
  CHECK(extended == 4, "four Owner stats carry Base and EquipmentValue");
  CHECK(bs::captured_bard_known_abilities().size() == 57 && bs::captured_bard_records_to_load().size() == 58, "the captured Bard's abilities: 57 known, 58 to load");
  CHECK(std::size(captured::kLevelStatuses) == 429 && captured::kLevelStatuses[8] == 0x8c && captured::kLevelStatuses[9] == 0x0a, "the level list: 429 bytes, the 8-byte prefix then 8C 0A");
  CHECK(bs::load_tracker(bs::kLoadTrackerSecond).flags.size() == 25 && bs::load_tracker(bs::kLoadTrackerSecond).flags[2] && !bs::load_tracker(bs::kLoadTrackerSecond).flags[9], "the four load tracker values are 25 flags each");
  CHECK(bs::game_state_tier2_values() && bs::game_state_tier2_values()->values().size() == 27, "the Tier 2 values read: 27 handles");
}

static void test_guids_and_parts() {
  std::printf("minted GUIDs and the pieces\n");
  ServerGuidAllocator alloc(60, 1860730596u);
  const auto g = bs::mint_session_guids(alloc, guid(4608, 60, 1860730596u));
  CHECK(g.player_state.ObjectId == ServerGuidAllocator::kFirstObjectId && g.player_controller.ObjectId == g.player_state.ObjectId + 2 && g.stats.ObjectId == g.player_state.ObjectId + 2 * 17 && g.ftue.ObjectId == g.global_gm_commands.ObjectId + 2 && g.pawn.ObjectId == g.ftue.ObjectId + 2 && g.game_state.ObjectId == 4608 && g.pawn.ServerId == 60 && g.pawn.Randomizer == 1860730596u,
        "mint_session_guids: consecutive even ids under the ServerId, the GameState's passed through");
  bool even = true;
  for (const auto *x : {&g.player_state, &g.player_controller, &g.global_gm_commands, &g.ftue, &g.pawn, &g.base_character_info, &g.combat_info, &g.owner_info, &g.backpack, &g.equipment, &g.quest_storage, &g.reward_storage, &g.appearance, &g.resource_storage[0], &g.resource_storage[1], &g.resource_storage[2], &g.collision_cylinder, &g.alignment, &g.ability, &g.stats}) {
    even = even && (x->ObjectId % 2) == 0;
  }
  CHECK(even, "every minted ObjectId is even");
  check_captured_bard_parts();
}

void run_bootstrap_sequence_tests() {
    test_sequence(bs::GameStateMode::Tier1, "Tier 1");
    test_sequence(bs::GameStateMode::Tier2, "Tier 2");
    test_sequence(bs::GameStateMode::Captured, "Captured");
    test_native_character();
    test_guids_and_parts();
}
