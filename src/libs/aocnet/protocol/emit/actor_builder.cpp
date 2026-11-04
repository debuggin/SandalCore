// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "protocol/emit/actor_builder.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/emit/package_map_exporter.h"
#include <cstdio>
#include <cstring>
#include <spdlog/spdlog.h>

namespace aoc { namespace protocol { namespace emit {

namespace {

void write_bunch_header(BunchWriter& out, const EmitContext& ctx,
                        const schema::ActorSchema& schema,
                        uint32_t bunch_data_bits) {
    out.write_bit(ctx.is_control ? 1 : 0);
    if (ctx.is_control) {
        out.write_bit(ctx.b_open  ? 1 : 0);
        out.write_bit(ctx.b_close ? 1 : 0);
        if (ctx.b_close) {
            out.write_serialize_int(0u, 7);
        }
    }

    out.write_bit(0);
    out.write_bit(ctx.is_reliable ? 1 : 0);
    out.write_sip(ctx.channel);

    const bool has_pme = ctx.explicit_has_pme
                             ? ctx.has_pme_value
                             : !ctx.package_map_exports.empty();
    out.write_bit(has_pme ? 1 : 0);
    out.write_bit(ctx.has_mbg_value ? 1 : 0);
    out.write_bit(ctx.is_partial ? 1 : 0);

    // Client reads ChSeq as 10 bits via SerializeInt(MAX=1024); 12 bits drifts the bit cursor.
    if (ctx.is_reliable) {
        out.write_serialize_int(ctx.ch_sequence, 1024);
    }

    if (ctx.is_partial) {
        out.write_bit(ctx.partial_initial              ? 1 : 0);
        out.write_bit(ctx.partial_custom_exports_final ? 1 : 0);
        out.write_bit(ctx.partial_final                ? 1 : 0);
    }

    const bool chname_present =
        ctx.is_reliable && (!ctx.is_partial || ctx.partial_initial);
    if (chname_present) {
        out.write_bit(ctx.ch_name_is_hardcoded ? 1 : 0);
        if (ctx.ch_name_is_hardcoded) {
            out.write_sip(ctx.ch_name_ename_idx);
        } else {
            out.write_fstring_ansi(ctx.ch_name_string);
            out.write_int32(ctx.ch_name_number);
        }
        (void)schema;
    }

    out.write_serialize_int(bunch_data_bits, 1024 * 8);
}

void write_v3_subobject_creation_dynamic(BunchWriter& out,
                                         uint64_t sub_guid,
                                         uint64_t class_obj_id,
                                         const uint8_t* payload_bits,
                                         uint32_t num_payload_bits) {
    out.write_bit(0);
    out.write_bit(0);
    out.write_sip(sub_guid);
    out.write_bit(0);
    out.write_bit(0);
    out.write_sip(class_obj_id);
    out.write_bit(1);
    out.write_sip(num_payload_bits);
    if (num_payload_bits > 0 && payload_bits != nullptr) {
        out.write_bit_range(payload_bits, 0, num_payload_bits);
    }
}

/// First bit is bHasRepLayout and must be 1 for the explicit-size form where NumPayloadBits SIP follows.
void write_v3_subobject_stably_named(BunchWriter& out,
                                     uint64_t sub_guid,
                                     const uint8_t* payload_bits,
                                     uint32_t num_payload_bits) {
    out.write_bit(1);
    out.write_bit(0);
    out.write_sip(sub_guid);
    out.write_bit(1);
    out.write_sip(num_payload_bits);
    if (num_payload_bits > 0 && payload_bits != nullptr) {
        out.write_bit_range(payload_bits, 0, num_payload_bits);
    }
}

/// Function J rollback semantic: drop both cmd_index and data if emit_property wrote zero bits.
void write_content_block(BunchWriter& out, bool is_actor,
                         uint64_t subobject_netguid,
                         const std::vector<schema::PropertySchema>& props,
                         const std::unordered_map<uint32_t, SchemaValue>& values) {
    (void)is_actor;
    (void)subobject_netguid;

    for (const auto& prop : props) {
        auto it = values.find(prop.handle);
        if (it == values.end()) continue;

        BunchWriter prop_data(64);
        ActorBuilder::emit_property(prop, it->second, prop_data);
        if (prop_data.bit_pos() == 0) continue;

        out.write_uint32(prop.handle);
        out.write_bit_range(prop_data.data(), 0, prop_data.bit_pos());
    }
}

void collect_component_values(size_t ci,
                              const schema::ComponentSchema& comp,
                              const ActorRuntime& runtime,
                              std::unordered_map<uint32_t, SchemaValue>& comp_vals) {
    for (const auto& prop : comp.properties) {
        uint64_t key = (static_cast<uint64_t>(ci) << 32) | prop.handle;
        auto it = runtime.component_values.find(key);
        if (it != runtime.component_values.end()) {
            comp_vals[prop.handle] = it->second;
        }
    }
}

void emit_component_blocks(BunchWriter& payload,
                           const schema::ActorSchema& schema,
                           const ActorRuntime& runtime,
                           const EmitContext& ctx) {
    int v3_mode = 0;
    if (std::FILE* fp = std::fopen("probe_v3_subobjects.txt", "r")) {
        (void)std::fscanf(fp, "%d", &v3_mode);
        std::fclose(fp);
    }

    for (size_t ci = 0; ci < schema.components.size(); ++ci) {
        const auto& comp = schema.components[ci];
        std::unordered_map<uint32_t, SchemaValue> comp_vals;
        collect_component_values(ci, comp, runtime, comp_vals);
        uint64_t subobj_obj_id = runtime.actor_netguid + ci + 1;

        BunchWriter prop_payload(128);
        write_content_block(prop_payload, false, subobj_obj_id, comp.properties, comp_vals);
        uint32_t payload_bits = static_cast<uint32_t>(prop_payload.bit_pos());

        if (ci == ctx.appearance_subobject_index &&
            ctx.appearance_payload_bit_count > 0 &&
            ctx.appearance_payload_bits != nullptr) {
            prop_payload.reset();
            prop_payload.write_bit_range(ctx.appearance_payload_bits, 0, ctx.appearance_payload_bit_count);
            payload_bits = ctx.appearance_payload_bit_count;
            spdlog::warn("[ActorBuilder] PD2.1 — CharacterAppearance subobject (ci={}) using supplied payload: {} bits",
                         ci, payload_bits);
        }

        const bool is_appearance_with_payload =
            (ci == ctx.appearance_subobject_index &&
             ((ctx.appearance_payload_bit_count > 0 && ctx.appearance_payload_bits != nullptr) ||
              ctx.appearance_force_v3_wrap));

        if (v3_mode == 3 || is_appearance_with_payload) {
            write_v3_subobject_stably_named(payload, subobj_obj_id, prop_payload.data(), payload_bits);
        } else if (v3_mode == 1 && comp.class_netguid != 0) {
            write_v3_subobject_creation_dynamic(payload, subobj_obj_id, comp.class_netguid,
                                                prop_payload.data(), payload_bits);
        } else if (payload_bits > 0) {
            payload.write_bit_range(prop_payload.data(), 0, payload_bits);
        }
    }

    // Only emit trailing actor-root terminator when V3 subobject wrapping is active; unconditional trailing blocks reset default properties.
    const bool need_trailing =
        (v3_mode == 3) ||
        ((ctx.appearance_payload_bit_count > 0 && ctx.appearance_payload_bits != nullptr &&
          ctx.appearance_subobject_index < schema.components.size()) ||
         (ctx.appearance_force_v3_wrap && ctx.appearance_subobject_index < schema.components.size()));

    if (need_trailing && !schema.components.empty()) {
        payload.write_bit(0);
        payload.write_bit(1);
        payload.write_sip(0);
    }
}

} // namespace

size_t ActorBuilder::build_spawn(const schema::ActorSchema& schema,
                                 const ActorRuntime& runtime,
                                 const EmitContext& ctx,
                                 BunchWriter& out) {
    size_t start = out.bit_pos();
    BunchWriter payload(2048);

    if (ctx.splice_full_payload &&
        ctx.spliced_tail_bits != nullptr &&
        ctx.spliced_tail_bit_count > 0) {
        payload.write_bit_range(ctx.spliced_tail_bits, ctx.spliced_tail_start_bit,
                                ctx.spliced_tail_bit_count);
        spdlog::debug("[ActorBuilder] Using FULL-payload splice ({} bits) — header emitted from parsed fields, rest spliced",
                      ctx.spliced_tail_bit_count);
        const uint32_t bunch_data_bits = static_cast<uint32_t>(payload.bit_pos());
        write_bunch_header(out, ctx, schema, bunch_data_bits);
        out.write_bit_range(payload.data(), 0, bunch_data_bits);
        return out.bit_pos() - start;
    }

    if (!ctx.package_map_exports.empty()) {
        PackageMapExporter::write_export_section(payload, ctx.package_map_exports);
    }

    write_serialize_new_actor(payload, runtime, schema);
    if (runtime.type == schema::ActorType::PlayerController) {
        write_player_controller_open_fields(payload, 0, 0);
    }

    if (ctx.spliced_tail_bits != nullptr && ctx.spliced_tail_bit_count > 0) {
        payload.write_bit_range(ctx.spliced_tail_bits, ctx.spliced_tail_start_bit,
                                ctx.spliced_tail_bit_count);
        spdlog::debug("[ActorBuilder] Using spliced RepLayout tail ({} bits) instead of content-block emission",
                      ctx.spliced_tail_bit_count);
    } else {
        write_content_block(payload, true, 0, schema.root_properties, runtime.root_values);
        emit_component_blocks(payload, schema, runtime, ctx);
    }

    uint32_t bunch_data_bits = static_cast<uint32_t>(payload.bit_pos());
    write_bunch_header(out, ctx, schema, bunch_data_bits);
    out.write_bit_range(payload.data(), 0, bunch_data_bits);

    size_t total = out.bit_pos() - start;
    spdlog::debug("[ActorBuilder] build_spawn: class={} channel={} total={} bits (header={}, payload={})",
                  schema.class_name, ctx.channel, total, total - bunch_data_bits, bunch_data_bits);
    return total;
}

size_t ActorBuilder::build_delta(const schema::ActorSchema& schema,
                                 const ActorRuntime& runtime,
                                 const std::vector<uint32_t>& changed_root_handles,
                                 const EmitContext& ctx,
                                 BunchWriter& out) {
    (void)schema; (void)runtime; (void)changed_root_handles; (void)ctx; (void)out;
    spdlog::warn("[ActorBuilder] build_delta not yet implemented");
    return 0;
}

size_t ActorBuilder::build_destroy(const EmitContext& ctx, BunchWriter& out) {
    size_t start = out.bit_pos();
    out.write_bit(1);
    out.write_bit(0);
    out.write_bit(1);
    out.write_serialize_int(0, 15);
    out.write_bit(0);
    out.write_bit(1);
    out.write_sip(ctx.channel);
    out.write_bit(0);
    out.write_bit(0);
    out.write_bit(0);
    out.write_serialize_int(ctx.ch_sequence, 1024);
    out.write_serialize_int(0, 1024 * 8);
    return out.bit_pos() - start;
}

}}} // namespace aoc::protocol::emit
