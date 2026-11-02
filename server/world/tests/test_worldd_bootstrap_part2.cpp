// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "test_worldd_bootstrap_common.h"

#include "bootstrap/captured_level_list.h"
#include "capture_lingering.h"
#include "protocol/server_guid_allocator.h"

#include <cmath>

namespace bs = aoc::worldd::bootstrap;
namespace captured = aoc::worldd::bootstrap::captured;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using aoc::protocol::ServerGuidAllocator;
using aoc::protocol::emit::FIntrepidNetworkGUID;
using namespace aoc::protocol::repl;
using bs::PlayerBootstrap;
using bs::Role;
using bs::Send;

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

static FIntrepidNetworkGUID guid_of(const capture_lingering::Guid &g) {
  return guid(g.object_id, g.server_id, g.randomizer);
}

static int64_t rep_i64(const RepState &st, uint16_t h) {
  const auto it = st.values().find(h);
  const int64_t *v = it == st.values().end() ? nullptr : it->second.get_if<int64_t>();
  return v ? *v : 0;
}

static FIntrepidNetworkGUID rep_object(const RepState &st, uint16_t h) {
  const auto it = st.values().find(h);
  const FIntrepidNetworkGUID *v = it == st.values().end() ? nullptr : it->second.get_if<FIntrepidNetworkGUID>();
  return v ? *v : FIntrepidNetworkGUID{};
}

static bool spec_from_capture(const capture_lingering::Open &open, bs::LingeringSpec &spec) {
  const auto bytes = capture_blocks::bytes_of(open.content_hex);
  const auto views = read_content_blocks(bytes.data(), open.content_bits, open.blocks_at);
  const schema::ClassSchema *cls = gen::find_class_schema("AAoCAbilityLingeringEffect");
  if (!views || views->size() != 4 || !cls || !(*views)[0].header.is_actor || !(*views)[0].header.has_rep_layout)
    return false;
  size_t pos = (*views)[0].payload_pos;
  const auto st = read_rep_block(*cls, bytes.data(), (*views)[0].payload_pos + (*views)[0].payload_bits, pos);
  if (!st || pos != (*views)[0].payload_pos + (*views)[0].payload_bits)
    return false;
  spec.actor = guid_of(open.actor);
  spec.movement = guid_of(open.subobjects[0]);
  spec.ability = guid_of(open.subobjects[1]);
  spec.stats = guid_of(open.subobjects[2]);
  spec.pawn_mesh = rep_object(*st, 14);
  spec.zone = rep_i64(*st, 24);
  spec.source = rep_i64(*st, 25);
  for (uint16_t i = 0; i < 4; ++i) {
    spec.effect_guid[i] = static_cast<uint32_t>(rep_i64(*st, static_cast<uint16_t>(19 + i)));
    spec.instance_guid[i] = static_cast<uint32_t>(rep_i64(*st, static_cast<uint16_t>(28 + i)));
  }
  spec.x = static_cast<double>(open.location.x) / 10.0;
  spec.y = static_cast<double>(open.location.y) / 10.0;
  spec.z = static_cast<double>(open.location.z) / 10.0;
  spec.yaw_degrees = open.rotation[1] * 360.0 / 65536.0;
  if (open.velocity.present)
    spec.velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, open.velocity.x, open.velocity.y, open.velocity.z};
  return true;
}

static void check_lingering_restore_and_fade(PlayerBootstrap &pb, const capture_lingering::Open &open, const std::optional<Send> &send, const Ref &content_ref) {
  const PlayerBootstrap::Checkpoint checkpoint = pb.checkpoint();
  const std::optional<Send> again = pb.lingering_open(open.actor.object_id);
  pb.restore(checkpoint);
  const std::optional<Send> third = pb.lingering_open(open.actor.object_id);
  CHECK(again && third && same_as(third->emission.payload, content_ref), "an open built again (a chain the connection refused) is the same");
  if (send)
    pb.mark_exported(send->emission.exported);
  if (open.packet == 302) {
    const std::optional<Send> fade = pb.lingering_fade(open.actor.object_id);
    CHECK(fade && !fade->opens_channel && fade->effect_actor == open.actor.object_id && fade->emission.exports.empty() && fade->emission.payload.bit_pos() == 28, "the fade-out is one 28-bit update on the actor's channel, as retail's (bFadingOut)");
    CHECK(!pb.lingering_fade(open.actor.object_id).has_value(), "(nothing changed, nothing to send twice)");
  }
}

static void check_lingering_open_entry(PlayerBootstrap &pb, const capture_lingering::Open &open) {
  bs::LingeringSpec spec;
  CHECK(spec_from_capture(open, spec), "the capture's actor block reads with the generated schema (a rep layout block and three subobject blocks)");
  CHECK(spec.zone != 0 && spec.source != 0 && spec.pawn_mesh.ObjectId != 0, "...and names the zone, the ability and the pawn's mesh");
  CHECK(pb.lingering_add(spec) && !pb.lingering_add(spec), "the actor is added once");
  const std::optional<Send> send = pb.lingering_open(open.actor.object_id);
  CHECK(send && send->opens_channel && send->reliable() && send->effect_actor == open.actor.object_id, "its open is a reliable chain that opens a channel of its own");
  Ref export_ref{"the export bunch", open.export_bits, open.export_hex};
  Ref content_ref{"the content", open.content_bits, open.content_hex};
  const bool exports_ok = send && send->emission.exports.size() == 1 && same_as(send->emission.exports[0], export_ref);
  CHECK(exports_ok, "the export bunch equals the original's (packet 302: the archetype, the pawn's CharacterMesh0 and the three components; packet 856: the components)");
  CHECK(send && same_as(send->emission.payload, content_ref), "SerializeNewActor (location, rotation, velocity) and the four content blocks equal the original's: 2,044 bits, bit for bit");
  check_lingering_restore_and_fade(pb, open, send, content_ref);
}

static void test_lingering_actor() {
  std::printf("a lingering zone's actor (the retail capture: the opens of the Shielding and Mesmerizing dances, bit for bit)\n");
  const capture_lingering::Open &first = capture_lingering::kOpens[0];
  bs::PlayerSetup setup = captured_setup();
  setup.guids.pawn = guid(701178, first.actor.server_id, first.actor.randomizer);
  PlayerBootstrap pb(std::move(setup), bs::GameStateMode::Tier2);
  CHECK(pb.ok() && pb.join_sequence().has_value(), "a bootstrap whose pawn is the capture's, joined");
  pb.export_acks().mark(guid(16442478405498561049ull));
  pb.export_acks().mark(guid(158953490572197689ull));
  CHECK(pb.lingering_count() == 0 && !pb.lingering_open(1).has_value(), "no actor, nothing to open");
  for (const capture_lingering::Open &open : capture_lingering::kOpens) {
    check_lingering_open_entry(pb, open);
  }
  CHECK(pb.lingering_count() == 2, "two actors held");
  pb.lingering_remove(capture_lingering::kOpens[0].actor.object_id);
  pb.lingering_remove(capture_lingering::kOpens[1].actor.object_id);
  CHECK(pb.lingering_count() == 0 && !pb.lingering_open(capture_lingering::kOpens[0].actor.object_id).has_value(), "a closed actor is forgotten");
}

void run_bootstrap_part2() {
  test_lingering_actor();
  test_sequence(bs::GameStateMode::Tier1, "Tier 1");
  test_sequence(bs::GameStateMode::Tier2, "Tier 2");
  test_sequence(bs::GameStateMode::Captured, "Captured");
  test_native_character();
  test_guids_and_parts();
}
