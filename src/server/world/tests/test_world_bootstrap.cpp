// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "test_world_bootstrap_common.h"

#include "bootstrap/captured_gamestate.h"
#include "bootstrap/captured_level_list.h"
#include "bootstrap/captured_replies.h"
#include "capture_lingering.h"
#include "protocol/server_guid_allocator.h"
#include "world/world_clock.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace bs = aoc::worldd::bootstrap;
namespace wc = aoc::worldd::world;
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

static void check_client_set_time_rpc(const PlayerBootstrap &pb, const Send &second) {
    const schema::ClassSchema &cls = *pb.player_controller().obj.schema;
    const auto blocks = read_content_blocks(second.emission.payload.data(), 263, 0);
    bool ok = false;
    if (blocks && blocks->size() == 3) {
        size_t p = (*blocks)[2].payload_pos;
        const auto calls = read_rpc_fields(cls, second.emission.payload.data(), p + (*blocks)[2].payload_bits, p);
        const RpcCall *set_time = (calls && calls->size() == 1 && (*calls)[0].decoded) ? &(*calls)[0] : nullptr;
        const double *g = set_time ? set_time->params.values.at(1).get_if<double>() : nullptr;
        const double *y = set_time ? set_time->params.values.at(2).get_if<double>() : nullptr;
        ok = g && y && std::string(set_time->rpc->name) == "ClientSetTime" && std::fabs(*g - 174944068.06996086) < 0.5 && std::fabs(*y - 15061647.01444424) < 0.5;
    }
    CHECK(ok, "ClientSetTime from the world time: NewGameTime and NewSynchronizedTime within 0.5 s of pkt#23's (the formula's accuracy)");
}

static void check_pc_time_and_bandwidth(PlayerBootstrap &pb) {
    const auto second = pb.flush_bandwidth_and_time();
    const BunchWriter want959 = joined({ref(capture_blocks::kRpcClientFlushLevelStreaming), ref(capture_blocks::kRpcClientCapBandwidth), ref(capture_blocks::kRpcClientSetTime)});
    CHECK(second && second->emission.payload.bit_pos() == 263 && capture_blocks::same_bit_range(second->emission.payload.data(), want959.data(), 89, nullptr), "pkt#23 chSeq 959: 263 bits; ClientFlushLevelStreaming and ClientCapBandwidth(200000) equal the capture's 89 bits");
    if (second)
        check_client_set_time_rpc(pb, *second);
    const bs::ClientTimes t = bs::client_times(1593462.375);
    CHECK(std::fabs(t.synchronized_time - (1593462.375 + 13468184.6)) < 1e-6 && std::fabs(t.game_time - (9.0 * t.synchronized_time + 39389245.0)) < 1e-6, "client_times: synchronized = world + 13468184.6, game = 9 x synchronized + 39389245");
}

static void test_player_controller() {
    std::printf("PlayerController open and its RPCs (pkt#22, #23)\n");
    PlayerBootstrap pb = captured_bootstrap();
    const auto open = pb.player_controller_open();
    CHECK(open && open->role == Role::PlayerController && open->opens_channel && open->reliable(), "the open is the PlayerController's first chain, reliable");
    CHECK(emitted_as(open, {ref(capture_blocks::kPcOpenExports0)}, ref(capture_blocks::kPcOpenContent)), "pkt#22: the 3545-bit export bunch and the 1314-bit payload (7 properties, GlobalGMCommands) match bit for bit");
    CHECK(open && open->emission.exported.size() == 7 && open->emission.exported_by_bunch.size() == 1, "7 GUIDs exported with their path, in one bunch");

    const auto hud = pb.hud();
    CHECK(hud && hud->role == Role::PlayerController && !hud->opens_channel && emitted_as(hud, {ref(capture_blocks::kRpcClientSetHUDExports)}, ref(capture_blocks::kRpcClientSetHUD)), "pkt#22: ClientSetHUD, the 873-bit class export and the 173-bit block, bit for bit");

    const auto first = pb.voice_and_level_list();
    const BunchWriter want958 = joined({ref(capture_blocks::kRpcClientVoiceHandshakeComplete), ref(capture_blocks::kRpcClientEnableNetworkVoice), ref(capture_blocks::kRpcClientUpdateMultipleLevelsStreamingStatusCompressed)});
    CHECK(first && first->emission.exports.empty() && first->reliable() && first->emission.payload.bit_pos() == 3550 && capture_blocks::same_bit_range(first->emission.payload.data(), want958.data(), 3550, nullptr), "pkt#23 chSeq 958: voice handshake, voice off and the captured 429-byte level list, 3550 bits, bit for bit");
    check_pc_time_and_bandwidth(pb);
}

static void check_game_state_blocks(const Send &open) {
    const auto ours = read_content_blocks(open.emission.payload.data(), open.emission.payload.bit_pos(), 388);
    const auto bytes = captured::bytes_of(captured::kGameStateOpenContent);
    const auto theirs = read_content_blocks(bytes.data(), captured::kGameStateOpenContent.bits, 388);
    CHECK(ours && theirs && ours->size() == 1 && theirs->size() == 1 && (*ours)[0].header.is_actor && (*theirs)[0].header.is_actor, "SerializeNewActor is 388 bits and one actor block follows, in both");
    CHECK(capture_blocks::same_bit_range(open.emission.payload.data(), bytes.data(), 388, nullptr), "SerializeNewActor (the GameState GUID, the archetype, the level, nothing else) equals the original's");
    CHECK(ours && (*ours)[0].payload_bits == captured::kGameStateRepLayoutBits, "Tier 2's block is the whole RepLayout part: 7324 bits");
    if (ours && theirs) {
        BunchWriter a, b;
        a.write_bit_range(open.emission.payload.data(), (*ours)[0].payload_pos, captured::kGameStateRepLayoutBits);
        b.write_bit_range(bytes.data(), (*theirs)[0].payload_pos, captured::kGameStateRepLayoutBits);
        CHECK(a.bit_pos() == b.bit_pos() && capture_blocks::same_bit_range(a.data(), b.data(), a.bit_pos(), nullptr), "the RepLayout block (33 handles: the 6 of Tier 1, GameTimeScalar, the 25 CombatSettings values, ServiceConnectionFlags) equals the original's, bit for bit");
        CHECK((*theirs)[0].payload_bits > (*ours)[0].payload_bits, "excluded: the original's actor block goes on with five fast arrays (123,427 bits in all)");
    }
}

static void test_game_state() {
    std::printf("GameState open (pkt#28)\n");
    bs::PlayerSetup setup = captured_setup();
    setup.world_time = std::bit_cast<double>(captured::kGameStateWorldTimeBits);
    PlayerBootstrap pb(setup, bs::GameStateMode::Tier2);
    CHECK(pb.ok(), "the Tier 2 GameState builds");
    const auto open = pb.game_state_open();
    CHECK(open && open->role == Role::GameState && open->opens_channel && open->reliable(), "the open is the GameState's first chain, reliable");
    CHECK(open && open->emission.exports.size() == 1 && same_as(open->emission.exports[0], ref(captured::kGameStateOpenExports)), "the export bunch (the CDO, the level, AoCGameModeBaseBP_C) equals the original's 3473 bits");
    CHECK(open && open->emission.exported.size() == 7, "7 GUIDs exported with their path");
    if (open)
        check_game_state_blocks(*open);
}

static void check_pawn_storage_payloads(PlayerBootstrap &pb) {
    auto payload = [&](StorageKind k, Role r) {
        return pb.storage_payload(k, r);
    };
    auto param_bits = [&](const std::optional<Send> &s) -> size_t {
        if (!s)
            return 0;
        const auto blocks = read_content_blocks(s->emission.payload.data(), s->emission.payload.bit_pos(), 0);
        if (!blocks || blocks->size() != 1)
            return 0;
        size_t pos = (*blocks)[0].payload_pos;
        const auto h = read_field_header(*pb.pawn().obj.schema, s->emission.payload.data(), pos + (*blocks)[0].payload_bits, pos);
        return h ? h->payload_bits : 0;
    };
    const auto backpack = payload(StorageKind::Backpack, Role::Pawn);
    CHECK(param_bits(backpack) == 692 + 70 * 178 + 145 && param_bits(payload(StorageKind::Equipment, Role::Pawn)) == 692 + 83 * 178 + 145, "the parameters of an empty storage are 692 + slots x 178 + 145 bits (13297 for the Backpack's 70 slots)");
    CHECK(emitted_as(backpack, {ref(capture_bootstrap::kPawnBackpackPayloadExports0)}, ref(capture_bootstrap::kPawnBackpackPayloadContent)), "pkt#82: the empty Backpack payload (692 + 70 x 178 + 145 bits) and the component's export equal the original's");
    const auto equipment = payload(StorageKind::Equipment, Role::Pawn);
    CHECK(equipment && equipment->emission.exports.size() == 1 && equipment->emission.payload.bit_pos() == 18 + 23 + 692 + 83 * 178 + 145, "pkt#89: the empty Equipment payload (83 slots) is 692 + 83 x 178 + 145 bits in an 18-bit block header and a 23-bit field header (the original's holds 13 items: excluded)");
    const auto quest = payload(StorageKind::QuestStorage, Role::Pawn);
    CHECK(emitted_as(quest, {ref(capture_bootstrap::kPawnQuestPayloadExports0)}, ref(capture_bootstrap::kPawnQuestPayloadContent)), "pkt#93: the empty QuestStorage payload (100 slots) equals the original's");
    const auto resource = payload(StorageKind::Resource0, Role::Pawn);
    CHECK(emitted_as(resource, {}, ref(capture_bootstrap::kPawnResourcePayloadContent)), "pkt#97: the empty resource storage payload (24 slots, Dimension 6 x 4, no export) equals the original's");

    const auto pc_backpack = payload(StorageKind::Backpack, Role::PlayerController);
    CHECK(pc_backpack && pc_backpack->role == Role::PlayerController && emitted_as(pc_backpack, {ref(capture_bootstrap::kPcBackpackPayloadExports0)}, ref(capture_bootstrap::kPcBackpackPayloadContent)), "pkt#597: the Backpack payload on the PlayerController equals the original's");
    const auto pc_resource = payload(StorageKind::Resource0, Role::PlayerController);
    CHECK(emitted_as(pc_resource, {}, ref(capture_bootstrap::kPcResourcePayloadContent)), "pkt#607: the resource payload on the PlayerController equals the original's");
    const auto reply = payload(StorageKind::Resource0, Role::Pawn);
    CHECK(emitted_as(reply, {}, ref(capture_bootstrap::kPawnResourceReplyContent)), "pkt#773: the reply to ServerRequestStorageData equals the original's");
    CHECK(!pb.storage_payload(StorageKind::Backpack, Role::PlayerState) && !pb.storage_payload(StorageKind::Backpack, Role::GameState), "a storage payload goes to the pawn or the PlayerController only");

    pb.mark_exported(backpack->emission.exported_by_bunch[0]);
    const auto quiet = payload(StorageKind::Backpack, Role::Pawn);
    CHECK(quiet && quiet->emission.exports.empty() && quiet->emission.payload.bit_pos() == capture_bootstrap::kPawnBackpackPayloadContent.bits, "with the export acknowledged the payload has no export bunch");
}

static void test_load_tracker_and_pawn_open() {
    std::printf("CharacterLoadTracker 1011...1 (pkt#72), the pawn's open (pkt#78) and its storage payloads\n");
    PlayerBootstrap pb = captured_bootstrap();
    pb.player_controller_open();
    const auto tracker = pb.load_tracker_before_possession();
    CHECK(tracker && tracker->role == Role::PlayerController && tracker->emission.exports.empty() && same_as(tracker->emission.payload, ref(capture_bootstrap::kPcTrackerBlock)), "pkt#72: the PlayerController's block with CharacterLoadTracker 1011...1 (841 bits) equals the original's; excluded: the quest and FTUE subobjects after it");

    const auto open = pb.pawn_open();
    CHECK(open && open->role == Role::Pawn && open->opens_channel, "the pawn's open is the pawn channel's first chain");
    CHECK(emitted_as(open, {ref(capture_blocks::kPawnOpenExports0)}, ref(capture_blocks::kPawnOpenContent)), "pkt#78: the 6321-bit export bunch (8 components) and the 29350-bit payload (the actor, 8 components, the 26,861-bit customization) match bit for bit");
    check_pawn_storage_payloads(pb);
}

static void test_possession() {
    std::printf("possession: ClientRestart (pkt#97), the PlayerController (pkt#103), the pawn's second state (pkt#107)\n");
    PlayerBootstrap pb = captured_bootstrap();
    pb.player_controller_open();
    pb.load_tracker_before_possession();
    pb.pawn_open();
    const auto restart = pb.restart_camera_rotation();
    CHECK(restart && restart->emission.exports.empty() && restart->emission.payload.bit_pos() == 268 && same_as(restart->emission.payload, ref(capture_bootstrap::kPcRestartContent)), "pkt#97: ClientRestart(pawn), ClientSetCameraMode(EName 204) and ClientSetRotation((0, 16550, 0), true): 268 bits, bit for bit");
    CHECK(restart && same_prefix(restart->emission.payload, ref(capture_blocks::kRpcClientRestart), 173), "ClientRestart's block is the captured 173 bits");

    const auto update = pb.possession_update();
    CHECK(update && update->role == Role::PlayerController && update->emission.exports.size() == 1 && update->emission.exported.size() == 1 && update->emission.exported[0].ObjectId == 10341558, "pkt#103: the update exports CollisionCylinder (the component the PlayerController attaches to) and nothing else");
    CHECK(update && same_as(update->emission.payload, ref(capture_bootstrap::kPcPossessionBlock)), "pkt#103: the actor block (Pawn, ControllersOriginalPawn, Name, AttachmentReplication, CharacterLoadTracker 0111...1; 1552 bits) equals the original's; excluded: XPTracker and the quest cache");
    CHECK(!pb.possession_update(), "a second update has nothing to send");

    const auto second = pb.pawn_second_state();
    CHECK(emitted_as(second, {ref(capture_blocks::kPawnPossessedExports0)}, ref(capture_blocks::kPawnPossessedContent)), "pkt#107: the 4209-bit export bunch (8 entries) and the 78235-bit payload (RemoteRole 2, Instigator, PlayerState, Controller, bHidden 1, Alignment, "
                                                                                                                         "BaseCharacterInfo, Ability, Stats with 442 stats, OwnerInfo, appearance ids, the dynamic resource storage) match bit for bit");
    CHECK(second && second->emission.exported.size() == 9, "nine GUIDs exported with their path: seven components, the resource storage class and its package");
    CHECK(!pb.pawn_second_state(), "the second state is sent once");
}

static void test_player_state() {
    std::printf("PlayerState open (pkt#434)\n");
    PlayerBootstrap pb = captured_bootstrap();
    pb.export_acks().mark(guid(16442478405498561049ull));
    const auto open = pb.player_state_open();
    CHECK(open && open->role == Role::PlayerState && open->opens_channel && emitted_as(open, {ref(capture_blocks::kPlayerStateOpenExports0)}, ref(capture_blocks::kPlayerStateOpenContent)), "pkt#434: the archetype's export (1177 bits) and the 1453-bit payload (PlayerId 1874, the login's name and unique id) match bit for bit");

    bs::PlayerSetup s = captured_setup();
    s.login.name_option = "SHORT";
    s.login.unique_id = "SHORT-ID";
    PlayerBootstrap other(s, bs::GameStateMode::Tier1);
    const Value *name = other.player_state().obj.state.get(*other.player_state().obj.schema, "PlayerNamePrivate");
    CHECK(name && *name == Value::str("SHORT"), "a name shorter than 20 characters is whole");
    s.login.name_option = std::string(19, 'a') + "\xC3\xA9" + "bcd";
    PlayerBootstrap utf(s, bs::GameStateMode::Tier1);
    const Value *cut = utf.player_state().obj.state.get(*utf.player_state().obj.schema, "PlayerNamePrivate");
    CHECK(cut && *cut == Value::str(std::string(19, 'a') + "\xC3\xA9"), "the name is cut at 20 characters, never inside a UTF-8 sequence");
}

static void check_group_bar_and_move_acks(PlayerBootstrap &pb) {
    const auto group = pb.empty_group();
    const auto bar = pb.default_action_bar();
    auto rpc_name = [&](const std::optional<Send> &s) -> std::string {
        if (!s)
            return "";
        const auto blocks = read_content_blocks(s->emission.payload.data(), s->emission.payload.bit_pos(), 0);
        if (!blocks || blocks->size() != 1)
            return "";
        size_t pos = (*blocks)[0].payload_pos;
        const auto calls = read_rpc_fields(*pb.player_controller().obj.schema, s->emission.payload.data(), pos + (*blocks)[0].payload_bits, pos);
        return calls && calls->size() == 1 ? (*calls)[0].rpc->name : "";
    };
    CHECK(group && group->emission.payload.bit_pos() == 527 && rpc_name(group) == "ClientUpdateGroup", "ClientUpdateGroup: the empty group, 527 bits (pkt#606)");
    CHECK(bar && bar->emission.payload.bit_pos() == 2537 && rpc_name(bar) == "ClientReceiveActionbarSlotPayload", "ClientReceiveActionbarSlotPayload: the default action bar, 2537 bits (pkt#77)");

    for (const Ref &r : {ref(capture_bootstrap::kMoveResponse1), ref(capture_bootstrap::kMoveResponse2), ref(capture_bootstrap::kMoveResponse3)}) {
        const auto bytes = capture_blocks::bytes_of(r.hex);
        wire::PacketReader rd(bytes.data(), bytes.size(), r.bits);
        rd.read_bits(36);
        const uint32_t bits = static_cast<uint32_t>(rd.read_bits(32));
        const auto ack = pb.good_move_ack(std::bit_cast<float>(bits));
        CHECK(ack && ack->role == Role::Pawn && !ack->reliable() && ack->emission.exports.empty() && same_as(ack->emission.payload, r), (std::string("the acknowledgement of a good move equals the original's 68 bits (") + r.name + ")").c_str());
    }
}

static void test_replies() {
    std::printf("the replies: phase-in, combat settings, group and action bar, move acknowledgements\n");
    PlayerBootstrap pb = captured_bootstrap();
    CHECK(!pb.phase_in(), "no phase-in before the pawn's second state");
    pb.player_controller_open();
    pb.pawn_open();
    pb.pawn_second_state();
    pb.mark_exported(std::vector<FIntrepidNetworkGUID>{guid(10341540, 60, 1860730596u), guid(10341538, 60, 1860730596u)});
    const auto phase = pb.phase_in();
    CHECK(phase && phase->role == Role::Pawn && !phase->opens_channel && phase->emission.exports.empty() && same_as(phase->emission.payload, ref(capture_bootstrap::kPhaseIn)), "pkt#2523: bHidden 0 and BaseCharacterInfo bIsPhasedOut 0 / bClientIsReadyToPhaseIn 1: the original's 194 bits");
    CHECK(phase && phase->reliable(), "sent reliable (the original's was unreliable): a lost update could not be recovered from the shadow");
    CHECK(!pb.phase_in(), "nothing changed: no second phase-in");

    std::vector<bs::PropertyValue> values = {
        {"CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MayHarmAngle", Value::f32(140)},
        {"CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ProximityRadius", Value::f32(400)},
        {"CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MaxDistance", Value::f32(3500)},
        {"CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ForwardOffset", Value::f32(0)},
        {"CharacterInGameSettings.CombatSettings.Targeting.TabTarget.TabTargetMethod", Value::u64(3)},
        {"CharacterInGameSettings.CombatSettings.Targeting.bEnableHoverCasting", Value::boolean(false)},
        {"CharacterInGameSettings.CombatSettings.Targeting.SoftLock.VisualsMask", Value::u64(3)},
        {"CharacterInGameSettings.CombatSettings.Input.BufferDuration", Value::f32(0.5f)},
        {"CharacterInGameSettings.CombatSettings.Rendering.PartyVFX", Value::u64(1)},
    };
    const auto echo = pb.combat_settings_echo(values);
    CHECK(echo && echo->role == Role::PlayerController && echo->emission.exports.empty() && same_as(echo->emission.payload, ref(capture_bootstrap::kCombatEcho)), "pkt#620: the nine CombatSettings values: the original's 281 bits");
    CHECK(!pb.combat_settings_echo(values), "the same values again: nothing to send (the original resent them at pkt#774; its shadow is the replicator's)");
    std::vector<bs::PropertyValue> bad = {{"NoSuchProperty", Value::f32(1)}};
    CHECK(!pb.combat_settings_echo(bad), "a path the PlayerController does not have is refused");

    check_group_bar_and_move_acks(pb);
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

static double stepped_advance(double second_of_day, double real) {
    double s = second_of_day, advanced = 0, left = real;
    while (left > 1e-9) {
        const bool day = s >= 21600.0 && s < 64800.0;
        const double rate = day ? 6.0 : 9.0;
        const double edge = day ? 64800.0 : (s >= 64800.0 ? 86400.0 + 21600.0 : 21600.0);
        const double need = (edge - s) / rate;
        if (need > left) {
            advanced += left * rate;
            break;
        }
        advanced += edge - s;
        left -= need;
        s = edge >= 86400.0 ? edge - 86400.0 : edge;
    }
    return advanced;
}

struct MeasuredPush {
    const char* what;
    double      first, second, real;
};
static const MeasuredPush kMeasuredPushes[] = {
    {"day, from hour 8.2", 166867867.858669, 166868588.000207, 120.0212},
    {"day, from hour 10.9", 165840770.583005, 165841490.920560, 120.0527},
    {"day, from hour 16.1", 165686900.832577, 165687621.146091, 120.0494},
    {"night, from hour 0.1", 166838870.210261, 166839950.419907, 120.0212},
    {"night, from hour 4.7", 169188177.418339, 169189096.803119, 102.1474},
    {"night, from hour 18.7", 169929739.111834, 169930819.030645, 119.9982},
    {"across hour 18, from hour 17.9", 169321931.930148, 169322763.819905, 118.4409},
};

static void test_world_clock_measured() {
    std::printf("the world clock: pushes measured in the original game, 6 game seconds per real second by day and 9 by night\n");
    for (const MeasuredPush& m : kMeasuredPushes) {
        const wc::WorldClock clock = wc::WorldClock::reading(m.first, 0.0);
        const double got = clock.game_time(m.real);
        char msg[240];
        std::snprintf(msg, sizeof(msg), "%s: %.2f game s in %.2f s (rate %.3f); the clock gives %.2f game s, %+.3f off", m.what, m.second - m.first, m.real, (m.second - m.first) / m.real,
                      got - m.first, got - m.second);
        CHECK(std::fabs(got - m.second) < 0.1, msg);
    }
}

static void test_world_clock_cycle() {
    std::printf("the world clock: one game day is 12,000 real seconds, from whatever hour the clock starts at\n");
    double worst = 0, worst_hour = 0;
    for (int i = 0; i < 48; ++i) {
        const wc::WorldClock c = wc::WorldClock::starting_at(i * 0.5);
        const double off = std::fabs(c.hour(0.0) - i * 0.5);
        worst_hour = std::max(worst_hour, std::min(off, 24.0 - off));
        for (const double t : {0.0, 1.0, 600.5, 3599.75, 7199.9, 7200.0, 9000.0, 11999.5, 31337.25})
            worst = std::max(worst, std::fabs(c.game_time(t + 12000.0) - c.game_time(t) - 86400.0));
    }
    char msg[200];
    std::snprintf(msg, sizeof(msg), "game(t + 12,000) - game(t) = 86,400 for 48 start hours (every half hour) at 9 times each (worst error %.2g s)", worst);
    CHECK(worst < 1e-6, msg);
    CHECK(worst_hour < 1e-9, "starting_at(h) reads h at world time 0, for the same 48 hours");
    const wc::WorldClock six = wc::WorldClock::starting_at(6.0);
    CHECK(std::fabs(six.game_time(7200.0) - six.game_time(0.0) - 43200.0) < 1e-6 && std::fabs(six.game_time(12000.0) - six.game_time(7200.0) - 43200.0) < 1e-6,
          "from hour 6: 7,200 s of day carry the clock 12 game hours on, then 4,800 s of night carry it 12 more");
    const wc::WorldClock dusk = wc::WorldClock::starting_at(17.5);
    CHECK(std::fabs(dusk.hour(300.0) - 18.0) < 1e-9 && std::fabs(dusk.game_time(300.0) - dusk.game_time(299.0) - 6.0) < 1e-6 && std::fabs(dusk.game_time(301.0) - dusk.game_time(300.0) - 9.0) < 1e-6,
          "the clock reaches hour 18 after 300 s: 6 game s per real second before it, 9 from it");
    const wc::WorldClock dawn = wc::WorldClock::starting_at(5.5);
    CHECK(std::fabs(dawn.hour(200.0) - 6.0) < 1e-9 && std::fabs(dawn.game_time(200.0) - dawn.game_time(199.0) - 9.0) < 1e-6 && std::fabs(dawn.game_time(201.0) - dawn.game_time(200.0) - 6.0) < 1e-6,
          "the clock reaches hour 6 after 200 s: 9 game s per real second before it, 6 from it");
    CHECK(!wc::WorldClock::is_night(6.0) && !wc::WorldClock::is_night(17.99) && wc::WorldClock::is_night(18.0) && wc::WorldClock::is_night(23.99) && wc::WorldClock::is_night(0.0) &&
              wc::WorldClock::is_night(5.99),
          "night is hour 18 up to, not including, hour 6");
}

static void test_world_clock_start() {
    std::printf("the world clock: every start begins at the default hour\n");
    CHECK(wc::kDefaultStartHour == 7.0, "the default start is hour 7, in the morning (one constant)");
    const wc::WorldClock first = wc::default_world_clock(), second = wc::default_world_clock();
    CHECK(std::fabs(first.hour(0.0) - 7.0) < 1e-9 && std::fabs(second.hour(0.0) - 7.0) < 1e-9 && first.game_time(0.0) == second.game_time(0.0),
          "a start reads hour 7 at world time 0 on the same date, whenever the process started");
    double worst = 0;
    for (const double wall : {3700.0, 3701.0, 4300.0, 7000.0, 11000.4, 20000.25, 86400.0}) {
        const double a = wall, b = wall - 3700.0;
        worst = std::max(worst, std::fabs(first.game_time(a) - first.game_time(0.0) - stepped_advance(25200.0, a)));
        worst = std::max(worst, std::fabs(second.game_time(b) - second.game_time(0.0) - stepped_advance(25200.0, b)));
    }
    char msg[200];
    std::snprintf(msg, sizeof(msg), "each process agrees with the rule stepped second by second from hour 7, 7 wall instants (worst %.2g game s)", worst);
    CHECK(worst < 1e-6, msg);
    CHECK(std::fabs(first.hour(3700.0) - (7.0 + 3700.0 * 6.0 / 3600.0)) < 1e-9 && std::fabs(second.hour(0.0) - 7.0) < 1e-9,
          "when the second process starts the first has run 3,700 s and reads hour 13.17: the restart starts the day over at hour 7");
    const auto shown = [](int hour, int minute, const char* half) {
        char text[32];
        std::snprintf(text, sizeof(text), "%d:%02d %s", hour, minute, half);
        return std::string(text);
    };
    CHECK(wc::hour_text(7.0) == shown(7, 0, "AM") && wc::hour_text(12.0) == shown(12, 0, "PM") && wc::hour_text(0.25) == shown(12, 15, "AM") && wc::hour_text(19.5833) == shown(7, 34, "PM") &&
              wc::hour_text(23.999) == shown(11, 59, "PM"),
          "the hour as the map window shows it");
}

static void test_world_clock_first_login() {
    std::printf("the world clock: the first login of the original game and the pushes that followed it\n");
    const double world = 1593462.375, game = 174944068.06996086;
    const wc::WorldClock clock = wc::WorldClock::reading(game, world);
    CHECK(std::fabs(clock.game_time(world) - game) < 1e-6, "the clock reads the login's time at its world time");
    double worst = 0;
    for (int k = 1; k <= 7; ++k) worst = std::max(worst, std::fabs(clock.game_time(world + 120.0 * k) - (game + 1080.0 * k)));
    char msg[200];
    std::snprintf(msg, sizeof(msg), "the next 7 pushes, 120 s apart, are 1,080 game s on each, within 0.5 s (worst %.2g s)", worst);
    CHECK(worst < 0.5, msg);
    CHECK(wc::WorldClock::is_night(clock.hour(world)) && wc::WorldClock::is_night(clock.hour(world + 840.0)), "all of it is night (hour 19.6 up to hour 21.7)");
}

static void test_world_clock_samples() {
    std::printf("the world clock: every measured pair of pushes\n");
    const char* path = std::getenv("AOC_CLOCK_SAMPLES");
    if (!path || !*path) {
        std::printf("  [skip] SKIP: AOC_CLOCK_SAMPLES names no file of measured pairs of pushes\n");
        return;
    }
    std::ifstream in(path);
    CHECK(static_cast<bool>(in), "AOC_CLOCK_SAMPLES names a file that opens");
    if (!in) return;
    size_t pairs = 0, day = 0, night = 0;
    double worst = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        double first = 0, second = 0, real = 0;
        if (std::sscanf(line.c_str(), "%lf %lf %lf", &first, &second, &real) != 3) continue;
        const wc::WorldClock clock = wc::WorldClock::reading(first, 0.0);
        worst = std::max(worst, std::fabs(clock.game_time(real) - second));
        ++pairs;
        (wc::WorldClock::is_night(std::fmod(first, 86400.0) / 3600.0) ? night : day) += 1;
    }
    char msg[240];
    std::snprintf(msg, sizeof(msg), "the file holds the measured pairs (%zu read: %zu starting by day, %zu by night)", pairs, day, night);
    CHECK(pairs >= 50 && day > 0 && night > 0, msg);
    std::snprintf(msg, sizeof(msg), "the rule gives the second push of every pair within 0.5 s (worst %.3f s)", worst);
    CHECK(worst < 0.5, msg);
}

static std::optional<std::pair<double, double>> decoded_times(PlayerBootstrap& pb, const std::optional<Send>& s) {
    if (!s) return std::nullopt;
    const auto blocks = read_content_blocks(s->emission.payload.data(), s->emission.payload.bit_pos(), 0);
    if (!blocks || blocks->size() != 1) return std::nullopt;
    size_t pos = (*blocks)[0].payload_pos;
    const auto calls = read_rpc_fields(*pb.player_controller().obj.schema, s->emission.payload.data(), pos + (*blocks)[0].payload_bits, pos);
    if (!calls || calls->size() != 1 || !(*calls)[0].decoded || std::string((*calls)[0].rpc->name) != "ClientSetTime") return std::nullopt;
    const double* g = (*calls)[0].params.values.at(1).get_if<double>();
    const double* y = (*calls)[0].params.values.at(2).get_if<double>();
    if (!g || !y) return std::nullopt;
    return std::make_pair(*g, *y);
}

static void test_world_clock_pushes() {
    std::printf("the world clock: the game time of the pushes of ClientSetTime\n");
    const wc::WorldClock clock = wc::WorldClock::starting_at(17.5);
    bs::PlayerSetup setup = captured_setup();
    setup.world_time = 100.0;
    PlayerBootstrap pb(setup, bs::GameStateMode::Tier2);
    CHECK(pb.ok() && pb.player_controller_open().has_value(), "the bootstrap of a player on a clock that starts at hour 17.5 builds");
    bs::set_global_calendar_date(1, 1);
    double last = 0, worst = 0, worst_sync = 0;
    bool rising = true, all = true;
    for (const double t : {100.0, 145.5, 149.5, 269.5, 389.5, 509.5}) {
        bs::set_global_time_of_day_hours(clock.hour(t));
        const auto got = decoded_times(pb, pb.set_time(t));
        all = all && got.has_value();
        if (!got) continue;
        worst = std::max(worst, std::fabs(got->first - clock.game_time(t)));
        worst_sync = std::max(worst_sync, std::fabs(got->second - (t + 13468184.6)));
        rising = rising && got->first > last;
        last = got->first;
    }
    char msg[200];
    std::snprintf(msg, sizeof(msg), "6 pushes decode, each carries the clock's game time (worst %.2g s) and its synchronized time as before (worst %.2g s)", worst, worst_sync);
    CHECK(all && worst < 1e-3 && worst_sync < 1e-6, msg);
    CHECK(rising, "no push is lower than the one before it, across hour 18");
    bs::set_global_time_of_day_hours(wc::default_world_clock().hour(100.0));
    const auto first = decoded_times(pb, pb.set_time(100.0));
    CHECK(first && std::fabs(first->first - wc::default_world_clock().game_time(100.0)) < 1e-3, "a push reads the clock's current hour");
}

int main() {
    test_player_controller();
    test_game_state();
    test_load_tracker_and_pawn_open();
    test_possession();
    test_player_state();
    test_replies();
    test_lingering_actor();
    run_bootstrap_sequence_tests();
    test_world_clock_measured();
    test_world_clock_cycle();
    test_world_clock_start();
    test_world_clock_first_login();
    test_world_clock_samples();
    test_world_clock_pushes();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
