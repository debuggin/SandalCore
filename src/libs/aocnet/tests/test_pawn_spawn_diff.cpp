// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Baseline bit-diff calibration harness comparing synthesized PlayerPawn_C spawn bunches against captured packet fixtures.
#include "protocol/emit/actor_builder.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/emit/package_map_exporter.h"
#include "protocol/emit/schema_value.h"
#include "protocol/schema/schema_registry.h"
#include "recordings.h"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace aoc::protocol;

static std::vector<uint8_t> read_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    size_t n = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(n);
    f.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

static emit::ExportEntry build_two_level(uint64_t leaf_obj,
                                          const std::string& leaf_path,
                                          uint32_t leaf_checksum,
                                          uint64_t outer_obj,
                                          const std::string& outer_path,
                                          uint32_t outer_checksum,
                                          bool no_load) {
    emit::ExportEntry leaf = emit::ExportEntry::asset(
        leaf_obj, leaf_path, leaf_checksum, no_load);
    emit::ExportEntry outer = emit::ExportEntry::asset(
        outer_obj, outer_path, outer_checksum, no_load);
    leaf.outer = std::make_unique<emit::ExportEntry>(std::move(outer));
    return leaf;
}

int main() {
    std::printf("=== test_pawn_spawn_diff -- baseline calibration run ===\n\n");

    const std::string fixture_path = recordings::find(recordings::kPacketB);
    if (fixture_path.empty()) return recordings::skip_test(recordings::kPacketB);
    auto captured = read_binary(fixture_path);
    if (captured.empty()) {
        std::fprintf(stderr, "[FAIL] Could not read the recording: %s\n",
                     fixture_path.c_str());
        return 1;
    }
    std::printf("  captured packet : %zu bytes (%zu bits)\n",
                 captured.size(), captured.size() * 8);

    schema::SchemaRegistry::instance().load_all();
    const auto* pawn_schema = schema::SchemaRegistry::instance()
        .get_schema(schema::ActorType::Pawn);
    if (!pawn_schema) {
        std::fprintf(stderr, "[FAIL] Pawn schema not loaded\n");
        return 1;
    }
    std::printf("  pawn schema     : %s\n", pawn_schema->class_name.c_str());
    std::printf("  root properties : %zu\n", pawn_schema->root_properties.size());
    std::printf("  subobject count : %zu\n", pawn_schema->components.size());

    // Synthetic export chain mirrors the two-level PlayerPawn_C archetype hierarchy in captured packets.
    std::vector<emit::ExportEntry> exports;
    {
        auto e = build_two_level(
            /*leaf_obj=*/       0x1111111111111111ULL,
            /*leaf_path=*/      "Default__PlayerPawn_C",
            /*leaf_checksum=*/  0x00000000,
            /*outer_obj=*/      0x2222222222222222ULL,
            /*outer_path=*/     "/Game/ThirdPersonCPP/Blueprints/PlayerPawn",
            /*outer_checksum=*/ 0x00000000,
            /*no_load=*/        false);
        exports.push_back(std::move(e));
    }
    exports[0].has_checksum = true;
    exports[0].outer->has_checksum = true;

    // Synthetic runtime GUIDs and quantized origin transform isolate schema handle layout from dynamic spawn variance.
    emit::ActorRuntime rt;
    rt.type = schema::ActorType::Pawn;

    rt.actor_netguid       = 0xDEAD0000ULL;
    rt.actor_server_id     = 60;
    rt.actor_randomizer    = 0x12345678U;

    rt.archetype_netguid    = 0x1111111111111111ULL;
    rt.archetype_server_id  = 0;
    rt.archetype_randomizer = 0;

    rt.level_netguid        = 16442478405498561049ULL;
    rt.level_server_id      = 0;
    rt.level_randomizer     = 0;

    rt.serialize_location     = true;
    rt.quantize_location      = true;
    rt.location_scaled_x      = 0;
    rt.location_scaled_y      = 0;
    rt.location_scaled_z      = 0;
    rt.location_max_bits      = 24;
    rt.serialize_rotation     = false;
    rt.serialize_scale        = false;
    rt.serialize_velocity     = false;

    emit::BunchWriter bw;
    emit::EmitContext ctx;
    ctx.channel              = 7;
    ctx.ch_sequence          = 1;
    ctx.is_reliable          = true;
    ctx.is_partial           = false;
    ctx.partial_initial      = true;
    ctx.partial_final        = true;
    ctx.is_control           = true;
    ctx.b_open               = true;
    ctx.package_map_exports  = std::move(exports);
    ctx.ch_name_is_hardcoded = true;
    ctx.ch_name_ename_idx    = 102;

    emit::ActorBuilder builder;
    size_t built_bits = builder.build_spawn(*pawn_schema, rt, ctx, bw);
    std::printf("\n  our builder output : %zu bits (%zu bytes)\n",
                 built_bits, bw.byte_size());

    if (built_bits == 0) {
        std::fprintf(stderr, "[FAIL] build_spawn returned 0 bits\n");
        return 1;
    }

    // Scan packet header offsets in [120, 200] bits because captured fixtures prepend outer UDP/packet headers before the bunch payload.
    std::printf("\n  Scanning for best alignment with captured...\n");
    size_t best_offset = 0;
    size_t best_diffs = SIZE_MAX;
    size_t best_cmp = 0;
    for (size_t hdr = 120; hdr <= 200; ++hdr) {
        if (captured.size() * 8 <= hdr) break;
        size_t cap_payload = captured.size() * 8 - hdr;
        size_t cmp = std::min(built_bits, cap_payload);
        size_t diffs = 0;
        for (size_t i = 0; i < cmp; ++i) {
            int cap_b = (captured[(hdr + i) >> 3] >> ((hdr + i) & 7)) & 1;
            int our_b = (bw.data()[i >> 3] >> (i & 7)) & 1;
            if (cap_b != our_b) ++diffs;
        }
        if (diffs < best_diffs) {
            best_diffs = diffs;
            best_offset = hdr;
            best_cmp = cmp;
        }
    }
    std::printf("  best alignment offset : %zu bits\n", best_offset);
    std::printf("  compared              : %zu bits\n", best_cmp);
    std::printf("  differing bits        : %zu (%.1f%%)\n",
                 best_diffs, 100.0 * best_diffs / (best_cmp ? best_cmp : 1));
    std::printf("  matching bits         : %zu (%.1f%%)\n",
                 best_cmp - best_diffs,
                 100.0 * (best_cmp - best_diffs) / (best_cmp ? best_cmp : 1));

    std::printf("\n=== CALIBRATION STATE ===\n");
    if (best_diffs < built_bits / 10) {
        std::printf("  Status: CLOSE -- < 10%% diff.  Fine-tune specific props.\n");
    } else if (best_diffs < built_bits / 2) {
        std::printf("  Status: PARTIAL -- structure roughly right; major props need values.\n");
    } else {
        std::printf("  Status: BASELINE -- this is the starting point.  Next:\n");
        std::printf("    1. Decode pkt#78 bunch header (channel, BDB, ChSeq).\n");
        std::printf("    2. Decode SerializeNewActor (actor/archetype/level GUIDs).\n");
        std::printf("    3. Decode transform + property stream.\n");
        std::printf("    4. Update placeholder rt.* values in this file.\n");
        std::printf("    5. Iterate on pawn_schema.cpp handles/types.\n");
    }
    return 0;
}
