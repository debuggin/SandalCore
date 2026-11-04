// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "../world/test_harness.h"

#include "protocol/repl/codecs.h"
#include "protocol/repl/codecs_internal.h"
#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace {

using aoc::protocol::emit::BunchWriter;
using aoc::protocol::repl::ByteBlob;
using aoc::protocol::repl::is_known_struct;
using aoc::protocol::repl::is_unset_optional_command;
using aoc::protocol::repl::kMaxDomainBlobBytes;
using aoc::protocol::repl::read_rep_block;
using aoc::protocol::repl::read_struct;
using aoc::protocol::repl::read_value;
using aoc::protocol::repl::RepShadow;
using aoc::protocol::repl::RepState;
using aoc::protocol::repl::struct_accepts;
using aoc::protocol::repl::Value;
using aoc::protocol::repl::Vec3d;
using aoc::protocol::repl::write_rep_block;
using aoc::protocol::repl::write_struct;
using aoc::protocol::repl::write_value;
using aoc::protocol::repl::zero_struct;
using aoc::protocol::repl::zero_value;
using aoc::protocol::schema::ClassSchema;
using aoc::protocol::schema::PropType;
using aoc::protocol::schema::RepCommand;
using aoc::protocol::schema::RpcSchema;
using aoc::protocol::schema::gen::all_class_schemas;
using aoc::protocol::schema::gen::find_class_schema;
using aoc::protocol::wire::PacketReader;

int assert_command_span_structs_known(std::span<const RepCommand> cmds, size_t& checked, bool check_unknown) {
    for (const RepCommand& cmd : cmds) {
        if (cmd.type == PropType::NetSerializeStruct) {
            SANDAL_ASSERT(cmd.struct_name != nullptr);
            SANDAL_ASSERT(!std::string_view(cmd.struct_name).empty());
            SANDAL_ASSERT(is_known_struct(cmd.struct_name));
            ++checked;
        } else if (check_unknown && cmd.type == PropType::Unknown) {
            SANDAL_ASSERT(is_unset_optional_command(cmd));
        }
    }
    return 0;
}

int assert_rpc_span_structs_known(std::span<const RpcSchema> rpcs, size_t& checked) {
    for (const RpcSchema& rpc : rpcs) {
        SANDAL_ASSERT(assert_command_span_structs_known(rpc.commands, checked, false) == 0);
        SANDAL_ASSERT(assert_command_span_structs_known(rpc.element_commands, checked, false) == 0);
    }
    return 0;
}

int test_all_generated_schemas_zero_unhandled_net_serialize() {
    const auto schemas = all_class_schemas();
    SANDAL_ASSERT(schemas.size() == 63);
    size_t checked_structs = 0;
    for (const ClassSchema* cls : schemas) {
        SANDAL_ASSERT(cls != nullptr);
        SANDAL_ASSERT(assert_command_span_structs_known(cls->commands, checked_structs, true) == 0);
        SANDAL_ASSERT(assert_command_span_structs_known(cls->element_commands, checked_structs, true) == 0);
        SANDAL_ASSERT(assert_rpc_span_structs_known(cls->rpcs, checked_structs) == 0);
    }
    SANDAL_ASSERT(checked_structs >= 150);
    return 0;
}

int assert_struct_roundtrip(std::string_view name, const Value& val) {
    BunchWriter out;
    SANDAL_ASSERT(write_struct(name, val, out));
    PacketReader r(out.data(), out.byte_size(), out.bit_pos());
    auto decoded = read_struct(name, r);
    SANDAL_ASSERT(decoded.has_value());
    SANDAL_ASSERT(!r.overflowed());
    SANDAL_ASSERT(r.remaining_bits() == 0);
    SANDAL_ASSERT(*decoded == val);
    return 0;
}

int assert_struct_rejects_malformed_wire(std::string_view name) {
    const uint8_t dummy[4] = {0, 0, 0, 0};
    PacketReader r_empty(dummy, 0, 0);
    SANDAL_ASSERT(!read_struct(name, r_empty).has_value());

    PacketReader r_short_hdr(dummy, 2, 16);
    SANDAL_ASSERT(!read_struct(name, r_short_hdr).has_value());

    BunchWriter w_trunc;
    w_trunc.write_int32(4);
    w_trunc.write_uint8(0xAA);
    w_trunc.write_uint8(0xBB);
    PacketReader r_trunc(w_trunc.data(), w_trunc.byte_size(), w_trunc.bit_pos());
    SANDAL_ASSERT(!read_struct(name, r_trunc).has_value());

    BunchWriter w_neg;
    w_neg.write_int32(-1);
    PacketReader r_neg(w_neg.data(), w_neg.byte_size(), w_neg.bit_pos());
    SANDAL_ASSERT(!read_struct(name, r_neg).has_value());

    BunchWriter w_huge;
    w_huge.write_int32(static_cast<int32_t>(kMaxDomainBlobBytes + 1));
    PacketReader r_huge(w_huge.data(), w_huge.byte_size(), w_huge.bit_pos());
    SANDAL_ASSERT(!read_struct(name, r_huge).has_value());
    return 0;
}

int assert_single_blob_struct_codec(std::string_view name) {
    SANDAL_ASSERT(is_known_struct(name));
    SANDAL_ASSERT(!zero_struct(name).has_value());

    const Value empty_blob{ByteBlob{}};
    const Value sample_blob{ByteBlob("\x00\x01\x7f\x80\xff\x42", 6)};
    SANDAL_ASSERT(struct_accepts(name, empty_blob));
    SANDAL_ASSERT(struct_accepts(name, sample_blob));

    SANDAL_ASSERT(!struct_accepts(name, Value(Vec3d{})));
    SANDAL_ASSERT(!struct_accepts(name, Value::i64(42)));
    SANDAL_ASSERT(!struct_accepts(name, Value::boolean(true)));
    SANDAL_ASSERT(!struct_accepts(name, Value(ByteBlob(kMaxDomainBlobBytes + 1, 'x'))));

    BunchWriter rejected_out;
    SANDAL_ASSERT(!write_struct(name, Value(Vec3d{}), rejected_out));
    SANDAL_ASSERT(rejected_out.bit_pos() == 0);

    SANDAL_ASSERT(assert_struct_roundtrip(name, empty_blob) == 0);
    SANDAL_ASSERT(assert_struct_roundtrip(name, sample_blob) == 0);
    SANDAL_ASSERT(assert_struct_rejects_malformed_wire(name) == 0);
    return 0;
}

int assert_pawn_root_motion_compatibility() {
    const ClassSchema* pawn = find_class_schema("PlayerPawn_C");
    SANDAL_ASSERT(pawn != nullptr);

    RepState st;
    SANDAL_ASSERT(!st.set(*pawn, "RepRootMotion.AuthoritativeRootMotion", Value(Vec3d{})));
    const Value payload{ByteBlob("\x10\x20\x30\x40", 4)};
    SANDAL_ASSERT(st.set(*pawn, "RepRootMotion.AuthoritativeRootMotion", payload));

    RepShadow shadow;
    BunchWriter out;
    bool failed = false;
    SANDAL_ASSERT(write_rep_block(*pawn, st, shadow, out, {}, &failed) > 0);
    SANDAL_ASSERT(!failed);
    size_t pos = 0;
    auto decoded = read_rep_block(*pawn, out.data(), out.bit_pos(), pos);
    SANDAL_ASSERT(decoded.has_value());
    SANDAL_ASSERT(pos == out.bit_pos());
    SANDAL_ASSERT(*decoded == st);
    return 0;
}

int test_p0_12_and_all_30_struct_roundtrips() {
    constexpr std::array<std::string_view, 12> kP0Structs = {
        "FEssentialCitizenshipWindowData",
        "FNodeInventoryData",
        "FClientBuyOrderData",
        "FNodeLayoutAssetSetGuids",
        "FCommissionBoardList",
        "FUpdateLevelVisibilityLevelInfo",
        "FRootMotionSourceGroup",
        "FSharedRepMaster",
        "FClientSpawnDeployableData",
        "FMapMarkerOverrideWrapper",
        "FPlayerCommodityCounts",
        "FCharacterMoveResponsePackedBits",
    };
    constexpr std::array<std::string_view, 19> kAdditionalStructs = {
        "FPredictionKey",
        "FActorLocation",
        "FCharacterChatInfo",
        "FCharacterServerMovePackedBits",
        "FClientChatChannelData",
        "FConsumerQuestInstance",
        "FCurrencyContainer",
        "FCurrencyContainerCheatHelper",
        "FGatherableInfoData",
        "FGlobalQuestUpdate",
        "FHitInst",
        "FHitResult",
        "FItemCheatHelper",
        "FMailHeader",
        "FNarrativeNPCKillData",
        "FNetLevelVisibilityTransactionId",
        "FNodeStabledVehicleStorageCache",
        "FPlayerBuyOrderContributions",
        "FPlayerCommodityCostsPerNode",
    };

    for (std::string_view name : kP0Structs) {
        SANDAL_ASSERT(assert_single_blob_struct_codec(name) == 0);
    }
    for (std::string_view name : kAdditionalStructs) {
        SANDAL_ASSERT(assert_single_blob_struct_codec(name) == 0);
    }
    SANDAL_ASSERT(assert_pawn_root_motion_compatibility() == 0);
    return 0;
}

const RepCommand* find_command_by_path(const ClassSchema& cls, std::string_view path) {
    for (const RepCommand& c : cls.commands) {
        if (c.path == path) return &c;
    }
    for (const RepCommand& c : cls.element_commands) {
        if (c.path == path) return &c;
    }
    for (const RpcSchema& rpc : cls.rpcs) {
        for (const RepCommand& c : rpc.commands) {
            if (c.path == path) return &c;
        }
        for (const RepCommand& c : rpc.element_commands) {
            if (c.path == path) return &c;
        }
    }
    return nullptr;
}

int assert_unknown_optional_command(std::string_view class_name, std::string_view prop_path) {
    const ClassSchema* cls = find_class_schema(class_name);
    SANDAL_ASSERT(cls != nullptr);
    const RepCommand* cmd = find_command_by_path(*cls, prop_path);
    SANDAL_ASSERT(cmd != nullptr);
    SANDAL_ASSERT(cmd->type == PropType::Unknown);
    SANDAL_ASSERT(is_unset_optional_command(*cmd));

    auto z = zero_value(*cmd);
    SANDAL_ASSERT(z.has_value());
    SANDAL_ASSERT(*z == Value::boolean(false));

    BunchWriter w_ok;
    SANDAL_ASSERT(write_value(*cmd, cls->element_commands, Value::boolean(false), w_ok));
    SANDAL_ASSERT(w_ok.bit_pos() == 1);
    PacketReader r_ok(w_ok.data(), w_ok.byte_size(), w_ok.bit_pos());
    auto v_ok = read_value(*cmd, cls->element_commands, r_ok);
    SANDAL_ASSERT(v_ok.has_value());
    SANDAL_ASSERT(*v_ok == Value::boolean(false));
    SANDAL_ASSERT(r_ok.remaining_bits() == 0);

    BunchWriter w_bad;
    SANDAL_ASSERT(!write_value(*cmd, cls->element_commands, Value::boolean(true), w_bad));
    SANDAL_ASSERT(!write_value(*cmd, cls->element_commands, Value::i64(0), w_bad));

    BunchWriter w_set_bit;
    w_set_bit.write_bit(1);
    PacketReader r_set(w_set_bit.data(), w_set_bit.byte_size(), w_set_bit.bit_pos());
    SANDAL_ASSERT(!read_value(*cmd, cls->element_commands, r_set).has_value());
    return 0;
}

int assert_synthetic_unknown_optional_rep_block() {
    static constexpr RepCommand kCmds[] = {
        {1, 0, "Locations.MapMarkerOverride", "UTest", PropType::Unknown, 0, 0, nullptr, nullptr, 0, 0},
        {2, 1, "Quests.GeneralQuestMarker", "UTest", PropType::Unknown, 0, 0, nullptr, nullptr, 0, 0},
        {3, 2, "SpawnQueryResults.ChildSocketGuids", "UTest", PropType::Unknown, 0, 0, nullptr, nullptr, 0, 0},
        {4, 3, "SpawnQueryResults.UsedChildSocketGuids", "UTest", PropType::Unknown, 0, 0, nullptr, nullptr, 0, 0},
    };
    static constexpr uint16_t kByPath[] = {1, 2, 3, 4};
    const ClassSchema synth{"UTest", "UObject", false, 1, 0, kCmds, {}, {}, {}, kByPath, {}};

    RepState st;
    for (const RepCommand& c : kCmds) {
        SANDAL_ASSERT(st.set(synth, c.path, Value::boolean(false)));
        SANDAL_ASSERT(!st.set(synth, c.path, Value::boolean(true)));
        SANDAL_ASSERT(!st.set(synth, c.path, Value::i64(1)));
    }
    RepShadow shadow;
    BunchWriter w_blk;
    bool failed = false;
    SANDAL_ASSERT(write_rep_block(synth, st, shadow, w_blk, {}, &failed) > 0);
    SANDAL_ASSERT(!failed);
    size_t pos = 0;
    auto decoded = read_rep_block(synth, w_blk.data(), w_blk.bit_pos(), pos);
    SANDAL_ASSERT(decoded.has_value());
    SANDAL_ASSERT(pos == w_blk.bit_pos());
    SANDAL_ASSERT(*decoded == st);
    return 0;
}

int test_unknown_optional_commands_rep_block() {
    SANDAL_ASSERT(assert_unknown_optional_command("AAoCGameStateBase", "Quests.GeneralQuestMarker") == 0);
    SANDAL_ASSERT(assert_unknown_optional_command("UQuestConsumerComponent", "Quests.QuestObjectiveMarker.MapMarkerOverride") == 0);
    SANDAL_ASSERT(assert_unknown_optional_command("UQuestConsumerComponent", "Locations.MapMarkerOverride") == 0);
    SANDAL_ASSERT(assert_unknown_optional_command("UQuestConsumerComponent", "SpawnQueryResults.ChildSocketGuids") == 0);
    SANDAL_ASSERT(assert_unknown_optional_command("UQuestConsumerComponent", "SpawnQueryResults.UsedChildSocketGuids") == 0);
    SANDAL_ASSERT(assert_unknown_optional_command("AAoCPlayerController", "QuestInstance.QuestObjectiveMarker.MapMarkerOverride") == 0);
    SANDAL_ASSERT(assert_synthetic_unknown_optional_rep_block() == 0);

    const RepCommand non_optional{1, 0, "SomeOtherUnknownField", "AActor", PropType::Unknown, 0, 0, nullptr, nullptr, 0, 0};
    SANDAL_ASSERT(!is_unset_optional_command(non_optional));
    SANDAL_ASSERT(!zero_value(non_optional).has_value());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    if (sandal::test::should_run_case(cfg, "all_generated_schemas_zero_unhandled_net_serialize")) {
        SANDAL_ASSERT(test_all_generated_schemas_zero_unhandled_net_serialize() == 0);
    }
    if (sandal::test::should_run_case(cfg, "p0_12_and_all_30_struct_roundtrips")) {
        SANDAL_ASSERT(test_p0_12_and_all_30_struct_roundtrips() == 0);
    }
    if (sandal::test::should_run_case(cfg, "unknown_optional_commands_rep_block")) {
        SANDAL_ASSERT(test_unknown_optional_commands_rep_block() == 0);
    }
    return 0;
}
