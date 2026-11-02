// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_repl_actor_open_common.h"

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using capture_blocks::Bits;
using capture_blocks::Block;

static void test_pc_open() {
    std::printf("PlayerController open (ch 3 pkt#22 chSeq 954/955)\n");
    Actor pc = make_pc();
    CHECK(block_is(pc.obj, capture_blocks::kPcOpenProps), "the values set by path are the ones the captured block carries");
    ActorShadow shadow;
    ExportAcks acks;
    bool failed = true;
    const auto e = open_actor(pc, shadow, acks, &failed);
    CHECK(e && !failed, "open_actor");
    CHECK(emitted_as(e, {&capture_blocks::kPcOpenExports0}, capture_blocks::kPcOpenContent),
          "export bunch (archetype, level, GlobalGMCommands class: 3545 bits) and payload (1314 bits) match the original server bit for bit");
    CHECK(e && e->exported.size() == 7 && e->exported[0].ObjectId == 3503756484819958835ull &&
              e->exported[2].ObjectId == 16442478405498561049ull && e->exported[5].ObjectId == 485698175673737329ull,
          "the GUIDs exported, each written with its path: the archetype and its package, the level and its two outers, the class and its package");
    CHECK(shadow.actor.rep.sent.size() == 7 && shadow.subobjects.size() == 1, "the shadow has the actor's 7 values and the subobject");
    CHECK(!update_actor(pc, shadow, acks, &failed) && !failed, "an update right after the open sends nothing");

    const auto again = open_actor(pc, shadow, acks);
    CHECK(emitted_as(again, {&capture_blocks::kPcOpenExports0}, capture_blocks::kPcOpenContent), "opened again: the same bits");

    ExportAcks all;
    all.mark(e->exported);
    ActorShadow fresh;
    const auto quiet = open_actor(pc, fresh, all);
    CHECK(quiet && quiet->exports.empty() && same_as(quiet->payload, capture_blocks::kPcOpenContent),
          "with the exports acknowledged the open has no export bunch, the same payload");
}

static void test_player_state_open() {
    std::printf("PlayerState open (ch 193 pkt#434)\n");
    Actor ps;
    ps.guid = session_guid(10341528);
    ps.obj.schema = &schema_named("AoCPlayerStateBP_C");
    set(ps.obj, "AuthServerIDReplicated", Value::u64(0));
    set(ps.obj, "RemoteRole", Value::u64(1));
    set(ps.obj, "Owner", object(10341530));
    set(ps.obj, "Role", Value::u64(4));
    set(ps.obj, "PlayerId", Value::i64(1874));
    set(ps.obj, "StartTime", Value::i64(1593462));
    UniqueNetId id;
    id.flags = 8;
    id.id = "DESKTOP-EXAMPLE1-US-45739BC044607BD9A50196AF41F22492";
    set(ps.obj, "UniqueId", Value(id));
    set(ps.obj, "PlayerNamePrivate", Value::str("DESKTOP-EXAMPLE1-US-"));
    ps.open.archetype = {link(16037722909354717791ull, "Default__AoCPlayerStateBP_C", false, 0xf4f44182),
                         link(1938806816082851941ull, "/Game/ThirdPersonCPP/Blueprints/AoCPlayerStateBP", false, 0)};
    ps.open.level = level_chain();
    ps.open.spawn.location = session_location();
    CHECK(block_is(ps.obj, capture_blocks::kPlayerStateOpen434), "the values set by path are the ones the captured block carries");

    ExportAcks acks;
    acks.mark(guid(16442478405498561049ull));
    ActorShadow shadow;
    const auto e = open_actor(ps, shadow, acks);
    CHECK(emitted_as(e, {&capture_blocks::kPlayerStateOpenExports0}, capture_blocks::kPlayerStateOpenContent),
          "the archetype alone is exported (1177 bits); the payload (1453 bits) matches bit for bit");
    CHECK(e && e->exported.size() == 2, "one chain written: the archetype and its package");
}

static void test_pawn_open() {
    std::printf("pawn open (ch 19 pkt#78)\n");
    Actor pawn = make_pawn();
    CHECK(block_is(pawn.obj, capture_blocks::kPawnOpenActor), "the actor's values are the captured block's");
    CHECK(block_is(pawn.subobjects[0].obj, capture_blocks::kPlayerInfoOpen), "BaseCharacterInfo's values are the captured block's");
    CHECK(block_is(pawn.subobjects[1].obj, capture_blocks::kPlayerCombatInfoOpen), "CombatInfo's values are the captured block's");

    ActorShadow shadow;
    ExportAcks acks;
    const auto whole = open_actor(pawn, shadow, acks);
    CHECK(whole && whole->exports.size() == 1 && same_as(whole->exports[0], capture_blocks::kPawnOpenExports0),
          "the export bunch: archetype, level and eight components, 6321 bits, bit for bit");
    CHECK(whole && whole->exported_by_bunch.size() == 1 && whole->exported_by_bunch[0] == whole->exported && whole->exported.size() == 13,
          "its GUIDs, per export bunch: the archetype and its package, the level and its outers, eight components");
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kPawnOpenContent.hex);
    const auto views = read_content_blocks(bytes.data(), capture_blocks::kPawnOpenContent.bits, 487);
    CHECK(views && views->size() == 9 && (*views)[8].header.object == session_guid(10341554) && (*views)[8].start == 2245 &&
              (*views)[8].payload_bits == 26950,
          "the captured chain: SerializeNewActor 487 bits, nine blocks, Character Appearance's 26950 bits last from bit 2245");
    CHECK(whole && same_prefix(whole->payload, capture_blocks::kPawnOpenContent, 2245),
          "the payload up to Character Appearance's block (SerializeNewActor, the actor, BaseCharacterInfo, CombatInfo and the five empty blocks) matches bit for bit");
    CHECK(emitted_as(whole, {&capture_blocks::kPawnOpenExports0}, capture_blocks::kPawnOpenContent),
          "the whole chain matches bit for bit: 6321 bits of exports, 29350 of content, Character Appearance's payload included");
    std::vector<ContentBlock> blocks;
    ActorShadow shadow2;
    CHECK(collect_open_blocks(pawn, shadow2, blocks) && blocks.size() == 9 && !blocks[0].sub && blocks[8].has_rep_layout &&
              blocks[8].payload.bit_pos() == 26950 && !blocks[3].has_rep_layout && blocks[3].payload.bit_pos() == 0,
          "collect_open_blocks: the actor's block and one per subobject; the four storages' are empty, Character Appearance's 26950 bits");
}

static Value attuned_blocks() {
    ArrayValue inner;
    ElementState idx;
    idx.values[1] = Value::u64(1);
    inner.elements.push_back(idx);
    ArrayValue outer;
    ElementState block;
    block.values[1] = Value(inner);
    outer.elements.push_back(block);
    return Value(outer);
}

static void setup_possessed_pawn(Actor& pawn, ActorShadow& shadow) {
    set(pawn.obj, "RemoteRole", Value::u64(2));
    set(pawn.obj, "bHidden", Value::boolean(true));
    set(pawn.obj, "Instigator", object(10341538));
    set(pawn.obj, "PlayerState", object(10341528));
    set(pawn.obj, "Controller", object(10341530));
    shadow.actor.invalidate(1);
    shadow.subobjects[10341542].invalidate(5);

    Subobject alignment = component(10341560, "AlignmentComponent", 0x3fbd829c);
    alignment.obj.state = state_of(capture_blocks::kAlignmentPawn107);
    Subobject ability = component(10341564, "AbilityComponent", 0x9e63ae5c);
    ability.obj.state = state_of(capture_blocks::kAbilityPawn107);
    Subobject stats = component(10341566, "StatsComponent", 0xf04938d2);
    stats.obj.opts = stat_options();
    CHECK(load_fast_arrays(capture_blocks::kStatsPawn107, 0, stats.obj),
          "the StatsComponent's four fast arrays are loaded from the retail data (335 + 96 + 11 + 1 items)");
    Subobject resource;
    resource.guid = session_guid(10341556);
    resource.dynamic_class = gameplay_class(11144499416456600109ull, "ResourceStorageComponent", 0xf3927ccd);
    resource.obj.schema = &schema_named("UResourceStorageComponent");
    set(resource.obj, "ItemLocationType", Value::u64(11));
    pawn.subobjects.insert(pawn.subobjects.begin(), std::move(alignment));
    pawn.subobjects.insert(pawn.subobjects.begin() + 3, std::move(ability));
    pawn.subobjects.insert(pawn.subobjects.begin() + 4, std::move(stats));
    pawn.subobjects.push_back(std::move(resource));
    Subobject* base = pawn.find(session_guid(10341540));
    Subobject* owner = pawn.find(session_guid(10341544));
    Subobject* appearance = pawn.find(session_guid(10341554));
    set(base->obj, "ServerId", Value::i64(60));
    set(base->obj, "bIsPhasedOut", Value::boolean(true));
    set(base->obj, "bClientIsReadyToPhaseIn", Value::boolean(false));
    set(base->obj, "bHasBeenPossessed", Value::boolean(true));
    set(base->obj, "bShouldSendOrgsAtWarWithToast", Value::boolean(true));
    set(base->obj, "WorldAttunements.AttunedBlocks", attuned_blocks());
    CHECK(load_fast_arrays(capture_blocks::kPlayerInfoPossessed, capture_blocks::kPlayerInfoPossessed.rep_bits, base->obj),
          "HeldItems (23 items) is loaded from the capture");
    owner->obj.state = state_of(capture_blocks::kOwnerInfo107);
    const RepState ids = state_of(capture_blocks::kAppearanceIdsPawn107);
    bool ids_ok = !ids.empty();
    for (const auto& [handle, value] : ids.values()) {
        ids_ok = appearance->obj.state.set(*appearance->obj.schema, handle, value) && ids_ok;
    }
    CHECK(ids_ok, "AppearanceIDs is added to Character Appearance's state");
}

static void test_pawn_update() {
    std::printf("the pawn after possession, from the shadow (ch 19 pkt#107)\n");
    Actor pawn = make_pawn();
    ActorShadow shadow;
    ExportAcks acks;
    CHECK(open_actor(pawn, shadow, acks).has_value(), "the open");
    setup_possessed_pawn(pawn, shadow);

    bool failed = true;
    const auto e = update_actor(pawn, shadow, acks, &failed);
    CHECK(e && !failed, "update_actor");
    CHECK(emitted_as(e, {&capture_blocks::kPawnPossessedExports0}, capture_blocks::kPawnPossessedContent),
          "exports (8 entries, 4209 bits) and the 78235-bit payload match the original server bit for bit");
    CHECK(e && e->exported.size() == 9, "nine GUIDs exported: seven components, the class and its package");
    CHECK(update_actor(pawn, shadow, acks, &failed) == std::nullopt && !failed,
          "the shadow now has everything: the next update sends nothing");
}

static std::vector<ExportChain> build_npc_export_chains(const FIntrepidNetworkGUID& npc) {
    std::vector<ExportChain> chains;
    chains.push_back({link(15775025627955649865ull, "Default__CSQ0006_EmissaryVaeluneEmpire_C", false, 0x2cdc5b6),
                      link(17259758083969458627ull,
                           "/Game/DesignAssets/Quests/StoryArcAssets/Side_Quests_Alpha01/CSQ_FTU/CSQ0006_EmissaryVaeluneEmpire", false, 0)});
    chains.push_back(level_chain());
    chains.push_back(component_export(npc, session_guid(2932), "AlignmentComponent", 0x3fbd829c));
    chains.push_back(component_export(npc, session_guid(2934), "InteractInfo", 0xe2e9ad91));
    chains.push_back(component_export(npc, session_guid(2936), "BaseCharacterInfo", 0xcc5f18a9));
    chains.push_back(component_export(npc, session_guid(2938), "CombatInfo", 0x38e78c41));
    chains.push_back(component_export(npc, session_guid(2940), "AbilityComponent", 0x9e63ae5c));
    chains.push_back(component_export(npc, session_guid(2942), "StatsComponent", 0xf04938d2));
    chains.push_back(gameplay_class(8542084888825683641ull, "InteractableComponent", 0x8163c481));
    chains.push_back(gameplay_class(4937921067877173103ull, "NPCAppearanceComponent", 0x8523f385));
    return chains;
}

static void test_exports() {
    std::printf("exports: the NPC open of ch 16 pkt#66, packing and repeated GUIDs\n");
    const FIntrepidNetworkGUID npc = session_guid(2926);
    const std::vector<ExportChain> chains = build_npc_export_chains(npc);

    ExportAcks none;
    const PackedExports p = pack_exports(chains, none);
    CHECK(p.ok && p.bunches.size() == 1 && same_as(p.bunches[0], capture_blocks::kNpcOpenExports16),
          "ten chains pack into the captured 7289-bit bunch bit for bit: the actor's outer entries are flags 6, "
          "the second /Script/GameSystemsPlugin is its GUID and flags 4");
    CHECK(p.exported.size() == 14, "fourteen GUIDs written with a path: the package of the second class is not");
    CHECK(p.bunches.size() == 1 && export_count(p.bunches[0]) == 10, "the count in the header is 10");

    const PackedExports split = pack_exports(chains, none, 7288);
    CHECK(split.ok && split.bunches.size() == 2 && export_count(split.bunches[0]) == 9 && export_count(split.bunches[1]) == 1,
          "with room for 7288 bits the last chain starts a second bunch");
    CHECK(split.bunches.size() == 2 && split.bunches[0].bit_pos() <= 7288 && split.bunches[1].bit_pos() > 33 + 128 + 8 + 32,
          "each bunch within the limit; the second one carries the package in full again");
    {
        const PackedExports big = pack_exports(chains, none, 100);
        CHECK(!big.ok, "a chain that does not fit an empty bunch: refused");
    }
    ExportAcks acks;
    acks.mark(guid(16442478405498561049ull));
    acks.mark(session_guid(2936));
    const PackedExports skipped = pack_exports(chains, acks);
    CHECK(skipped.ok && skipped.bunches.size() == 1 && export_count(skipped.bunches[0]) == 8 && skipped.exported.size() == 10,
          "with the level and one component acknowledged, eight chains are written");
    {
        const ExportChain cls = gameplay_class(4937921067877173103ull, "NPCAppearanceComponent", 0x8523f385);
        ExportAcks plugin;
        plugin.mark(guid(158953490572197689ull));
        const PackedExports with_ack = pack_exports(std::span<const ExportChain>(&cls, 1), plugin);
        const PackedExports without = pack_exports(std::span<const ExportChain>(&cls, 1), none);
        CHECK(with_ack.ok && without.ok && with_ack.bunches.size() == 1 && without.bunches.size() == 1 &&
                  with_ack.bunches[0].bit_pos() + 128 + 32 + 8 * 26 + 32 == without.bunches[0].bit_pos() && with_ack.exported.size() == 1,
              "an acknowledged package is not written again with its path: 128 + 32 + 8 * 26 + 32 bits less");
    }
    CHECK(pack_exports(chains, [&] { ExportAcks a; for (const auto& c : chains) a.mark(c.front().guid); return a; }()).bunches.empty(),
          "everything acknowledged: no bunch");
    CHECK(component_export(npc, session_guid(1), "X", 5).size() == 2 && component_export(npc, session_guid(1), "X", 5)[1].name.empty(),
          "a component's chain is the component and the actor without a name");
}

static RpcParams read_captured_rpc_params(const schema::ClassSchema& s, const Bits& b) {
    const auto bytes = capture_blocks::bytes_of(b.hex);
    const auto blocks = read_content_blocks(bytes.data(), b.bits, 0);
    size_t pos = blocks ? (*blocks)[0].payload_pos : 0;
    const auto calls = blocks ? read_rpc_fields(s, bytes.data(), pos + (*blocks)[0].payload_bits, pos) : std::nullopt;
    return calls && calls->size() == 1 && (*calls)[0].decoded ? (*calls)[0].params : RpcParams{};
}

static BunchWriter join_bits(std::initializer_list<const Bits*> parts) {
    BunchWriter w;
    for (const Bits* b : parts) {
        const auto bytes = capture_blocks::bytes_of(b->hex);
        w.write_bit_range(bytes.data(), 0, b->bits);
    }
    return w;
}

static void test_rpcs_batched(const Actor& pc, ExportAcks& acks) {
    auto equal = [](const BunchWriter& a, const BunchWriter& b) {
        return a.bit_pos() == b.bit_pos() && capture_blocks::same_bit_range(a.data(), b.data(), a.bit_pos(), nullptr);
    };
    std::vector<RpcRequest> second(3);
    second[0].rpc = "ClientFlushLevelStreaming";
    second[1].rpc = "ClientCapBandwidth";
    second[1].params = read_captured_rpc_params(*pc.obj.schema, capture_blocks::kRpcClientCapBandwidth);
    second[2].rpc = "ClientSetTime";
    second[2].params = read_captured_rpc_params(*pc.obj.schema, capture_blocks::kRpcClientSetTime);
    const auto b959 = call_rpcs(pc, second, acks);
    CHECK(b959 && b959->exports.empty() &&
              equal(b959->payload, join_bits({&capture_blocks::kRpcClientFlushLevelStreaming, &capture_blocks::kRpcClientCapBandwidth,
                                              &capture_blocks::kRpcClientSetTime})) &&
              b959->payload.bit_pos() == 263,
          "three RPCs in one bunch: pkt#23 chSeq 959's 263 bits (ClientFlushLevelStreaming, ClientCapBandwidth, ClientSetTime)");
    std::vector<RpcRequest> first(3);
    first[0].rpc = "ClientVoiceHandshakeComplete";
    first[1].rpc = "ClientEnableNetworkVoice";
    first[1].params = read_captured_rpc_params(*pc.obj.schema, capture_blocks::kRpcClientEnableNetworkVoice);
    first[2].rpc = "ClientUpdateMultipleLevelsStreamingStatusCompressed";
    first[2].params = read_captured_rpc_params(*pc.obj.schema, capture_blocks::kRpcClientUpdateMultipleLevelsStreamingStatusCompressed);
    const auto b958 = call_rpcs(pc, first, acks);
    CHECK(b958 && b958->exports.empty() &&
              equal(b958->payload, join_bits({&capture_blocks::kRpcClientVoiceHandshakeComplete, &capture_blocks::kRpcClientEnableNetworkVoice,
                                              &capture_blocks::kRpcClientUpdateMultipleLevelsStreamingStatusCompressed})) &&
              b958->payload.bit_pos() == 3550,
          "and chSeq 958's 3550 bits (ClientVoiceHandshakeComplete, ClientEnableNetworkVoice, the 429-byte level statuses)");
    CHECK(!call_rpcs(pc, {}, acks), "no calls: nothing");
}

static void test_rpcs_subobject() {
    Actor pawn = make_pawn();
    pawn.subobjects.push_back(component(10341564, "AbilityComponent", 0x9e63ae5c));
    const Subobject* ability = pawn.find(session_guid(10341564));
    const schema::RpcSchema* end = ability->obj.schema->rpc("ClientEndAbility");
    RpcParams ep;
    set_rpc_param(*end, ep, end->params[0].name, Value::i64(77));
    const auto s = call_rpc(pawn, ability, "ClientEndAbility", ep, {}, ExportAcks{});
    CHECK(s && s->exports.size() == 1 && export_count(s->exports[0]) == 1,
          "on the AbilityComponent: its export bunch (name, checksum, the pawn as outer) first");
    if (!s) return;
    const auto blocks = read_content_blocks(s->payload.data(), s->payload.bit_pos(), 0);
    CHECK(blocks && blocks->size() == 1 && !(*blocks)[0].header.is_actor && !(*blocks)[0].header.has_rep_layout &&
              (*blocks)[0].header.stably_named && (*blocks)[0].header.object == session_guid(10341564),
          "the block is the server's subobject form: [0][0][GUID][stably named 1]");
    size_t pos = (*blocks)[0].payload_pos;
    const auto calls = read_rpc_fields(*ability->obj.schema, s->payload.data(), pos + (*blocks)[0].payload_bits, pos);
    CHECK(calls && calls->size() == 1 && (*calls)[0].decoded && std::string((*calls)[0].rpc->name) == "ClientEndAbility" &&
              (*calls)[0].params == ep,
          "and its RPC field reads back to the values");
}

static void test_rpcs() {
    std::printf("RPCs through the replicator\n");
    Actor pc = make_pc();
    const schema::RpcSchema& hud = *pc.obj.schema->rpc("ClientSetHUD");
    RpcParams params;
    set_rpc_param(hud, params, "NewHUDClass", Value(guid(8463636446363390223ull)));
    const ExportChain hud_class = {link(8463636446363390223ull, "BP_AOCHUD_C", false, 0xac6f1251),
                                   link(7805116325763694055ull, "/Game/UI/Widgets/BP_AOCHUD", false, 0)};
    ExportAcks acks;
    const auto e = call_rpc(pc, nullptr, "ClientSetHUD", params, std::span<const ExportChain>(&hud_class, 1), acks);
    CHECK(emitted_as(e, {&capture_blocks::kRpcClientSetHUDExports}, capture_blocks::kRpcClientSetHUD),
          "ClientSetHUD: the 873-bit export of the HUD class and the 173-bit block, as the original server sent them (pkt#22 chSeq 956/957)");
    acks.mark(guid(8463636446363390223ull));
    const auto quiet = call_rpc(pc, nullptr, "ClientSetHUD", params, std::span<const ExportChain>(&hud_class, 1), acks);
    CHECK(quiet && quiet->exports.empty() && same_as(quiet->payload, capture_blocks::kRpcClientSetHUD), "once the export is acknowledged only the block goes");
    CHECK(!call_rpc(pc, nullptr, "NoSuchRpc", params, {}, acks), "an RPC the class does not have: refused");
    RpcParams bad;
    bad.values[9] = Value::i64(1);
    CHECK(!call_rpc(pc, nullptr, "ClientSetHUD", bad, {}, acks), "parameters that do not match: refused");

    test_rpcs_batched(pc, acks);
    test_rpcs_subobject();
}

int main() {
    ObjectShadow invalidation;
    invalidation.rep.sent[1] = Value::boolean(true);
    invalidation.invalidate(65537);
    CHECK(invalidation.rep.sent.count(1) == 1, "out-of-range invalidation cannot alias a valid shadow handle");
    invalidation.invalidate(1);
    CHECK(invalidation.rep.sent.empty(), "valid invalidation still clears the shadow value");
    test_pc_open();
    test_player_state_open();
    test_pawn_open();
    test_pawn_update();
    test_exports();
    test_rpcs();
    run_repl_actor_open_part2();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
