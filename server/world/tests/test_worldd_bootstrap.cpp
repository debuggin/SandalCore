// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "test_worldd_bootstrap_common.h"

#include "bootstrap/captured_gamestate.h"
#include "bootstrap/captured_level_list.h"
#include "bootstrap/captured_replies.h"

#include <bit>
#include <cmath>

namespace bs = aoc::worldd::bootstrap;
namespace captured = aoc::worldd::bootstrap::captured;
namespace wire = aoc::protocol::wire;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
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

int main() {
    test_player_controller();
    test_game_state();
    test_load_tracker_and_pawn_open();
    test_possession();
    test_player_state();
    test_replies();
    run_bootstrap_part2();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
