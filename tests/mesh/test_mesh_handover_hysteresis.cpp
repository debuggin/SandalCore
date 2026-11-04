// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "server/tether/services/tether/ics_token_file.h"
#include "server/tether/services/tether/tether_wire.h"
#include "spawns/area.h"
#include "world/mesh_border.h"
#include "world/relevancy.h"
#include "world/zoi_surface.h"

#include <cstdio>
#include <cstring>

namespace {

using namespace aoc::worldd;

bool test_custom_zoi_boundary_index() {
    const spawns::region::Rect footprint[] = {{-200000, -100000, 200000, 100000}};
    const spawns::region::Rect zoi_rects[] = {
        {-200000, -100000, 0, 100000},
        {0, -100000, 200000, 100000}};
    const spawns::region::ZoiRows zois[] = {
        {1001ULL, "Verra_Zoi_West", 0, 1, 100},
        {1002ULL, "Verra_Zoi_East", 1, 1, 100}};

    const spawns::region::Index custom_idx(footprint, 1, zois, 2, zoi_rects, 2);
    if (!custom_idx.contains(-5000.0, 0.0) || !custom_idx.contains(5000.0, 0.0)) return false;
    if (custom_idx.contains(250000.0, 0.0)) return false;
    if (custom_idx.zoi_guid(-15000.0, 0.0) != 1001ULL) return false;
    if (custom_idx.zoi_guid(15000.0, 0.0) != 1002ULL) return false;
    if (custom_idx.zoi_guid(250000.0, 0.0) != 0ULL) return false;
    return std::strcmp(custom_idx.zoi_name(-15000.0, 0.0), "Verra_Zoi_West") == 0 &&
           std::strcmp(custom_idx.zoi_name(15000.0, 0.0), "Verra_Zoi_East") == 0;
}

bool test_relevancy_hysteresis_band_200m() {
    world::RelevancyConfig cfg{};
    cfg.awake_cm = 15000.0;
    cfg.snapshot_cm = 35000.0;
    cfg.hysteresis_cm = 20000.0;

    if (std::fabs(world::query_radius(cfg) - 55000.0) > 1e-6) return false;
    if (std::fabs(world::horizontal_distance(0.0, 0.0, 300.0, 400.0) - 500.0) > 1e-6) return false;

    if (world::classify(cfg, 16000.0, 0.0, false, world::ClientState::None) !=
        world::Ring::Snapshot) {
        return false;
    }
    if (world::classify(cfg, 34000.0, 0.0, false, world::ClientState::Awake) !=
        world::Ring::Awake) {
        return false;
    }
    if (world::classify(cfg, 35500.0, 0.0, false, world::ClientState::Awake) !=
        world::Ring::Snapshot) {
        return false;
    }
    if (world::classify(cfg, 36000.0, 0.0, true, world::ClientState::Awake) !=
        world::Ring::Out) {
        return false;
    }
    return world::classify(cfg, 54000.0, 0.0, false, world::ClientState::Dormant) ==
               world::Ring::Snapshot &&
           world::classify(cfg, 56000.0, 0.0, false, world::ClientState::Dormant) ==
               world::Ring::Out;
}

bool test_border_hysteresis_and_area_handover() {
    if (std::fabs(world::kBorderHysteresisBandCm - 2000.0f) > 1e-5f) return false;

    using world::AuthorityState;
    if (world::evaluate_border_handover(-2500.0f, AuthorityState::HandoverPending) != AuthorityState::Local) return false;
    if (world::evaluate_border_handover(-2500.0f, AuthorityState::RemoteGhost) != AuthorityState::Local) return false;

    AuthorityState s_local = AuthorityState::Local;
    AuthorityState s_pending = AuthorityState::HandoverPending;
    for (float d : {-2000.0f, -500.0f, 0.0f, 500.0f, 2000.0f, -1500.0f, 1500.0f}) {
        s_local = world::evaluate_border_handover(d, s_local);
        s_pending = world::evaluate_border_handover(d, s_pending);
        if (s_local != AuthorityState::Local || s_pending != AuthorityState::HandoverPending) return false;
    }

    if (world::evaluate_border_handover(2500.0f, AuthorityState::Local) != AuthorityState::HandoverPending) return false;
    if (world::evaluate_border_handover(2500.0f, AuthorityState::HandoverPending) != AuthorityState::RemoteGhost) return false;
    if (world::evaluate_border_handover(2500.0f, AuthorityState::RemoteGhost) != AuthorityState::RemoteGhost) return false;

    spawns::AreaConfig area{};
    area.center_x = 0.0;
    area.center_y = 0.0;
    area.radius_cm = 100000.0;
    if (std::fabs(area.signed_border_distance_cm(97500.0, 0.0) - (-2500.0f)) > 1e-3f) return false;
    if (area.evaluate_handover(97500.0, 0.0, AuthorityState::HandoverPending) != AuthorityState::Local) return false;
    if (area.evaluate_handover(101000.0, 0.0, AuthorityState::Local) != AuthorityState::Local) return false;
    return area.evaluate_handover(102500.0, 0.0, AuthorityState::Local) == AuthorityState::HandoverPending;
}

int run_hysteresis_band_200m_authority() {
    if (!test_custom_zoi_boundary_index()) return 1;
    if (!test_relevancy_hysteresis_band_200m()) return 1;
    if (!test_border_hysteresis_and_area_handover()) return 1;

    const auto& riverlands = spawns::region::Index::get();
    if (!riverlands.contains(-1000000.0, 500000.0)) return 1;
    if (riverlands.zoi_guid(-1000000.0, 500000.0) == 0) return 1;
    if (riverlands.contains(5000000.0, 5000000.0)) return 1;

    const auto surf_info = world::zoi_surface_info();
    if (surf_info.chunks <= 0 || surf_info.uniform < 0) return 1;
    const auto palette = world::zoi_surface_palette();
    if (palette.empty()) return 1;
    const uint64_t surface_guid = world::zoi_at(-1000000.0, 500000.0);
    if (surface_guid == 0 || surface_guid != riverlands.zoi_guid(-1000000.0, 500000.0)) {
        return 1;
    }
    return 0;
}

bool test_tether_wire_header_and_big_endian() {
    uint8_t scratch[12]{};
    tether::wire::w32(scratch + 0, 17U);
    tether::wire::w32(scratch + 4, 1042U);
    tether::wire::w16(scratch + 8, 512U);
    tether::wire::w16(scratch + 10, 5U);
    if (tether::wire::r32(scratch + 0) != 17U || tether::wire::r32(scratch + 4) != 1042U) {
        return false;
    }
    if (tether::wire::r16(scratch + 8) != 512U || tether::wire::r16(scratch + 10) != 5U) {
        return false;
    }

    tether::wire::Header hdr{};
    hdr.magic_number = (tether::wire::APP_NUMBER << 16) | (2U << 8) |
                       static_cast<uint32_t>(tether::wire::MsgId::data);
    hdr.session_id = 1042U;
    hdr.window_size = 64U;
    hdr.fragment_number = 3U;
    hdr.data_length = 128U;
    hdr.sequence_number = 17LL;
    hdr.next_expected_recv_seq = 18LL;
    hdr.timestamp_msec = 9876543210LL;

    if (!hdr.is_valid() || hdr.version() != 2U || hdr.msg_id() != tether::wire::MsgId::data) {
        return false;
    }
    if (std::strcmp(tether::wire::msg_name(hdr.msg_id()), "Data") != 0) return false;

    const auto encoded = tether::wire::encode(hdr);
    if (encoded.size() != tether::wire::HEADER_SIZE) return false;

    tether::wire::Header decoded{};
    if (tether::wire::decode(encoded.data(), encoded.size() - 1, decoded)) return false;
    if (!tether::wire::decode(encoded.data(), encoded.size(), decoded)) return false;
    return decoded.is_valid() && decoded.session_id == 1042U && decoded.sequence_number == 17LL &&
           decoded.next_expected_recv_seq == 18LL && decoded.timestamp_msec == 9876543210LL;
}

bool test_tether_proto_and_message_handover() {
    tether::wire::Message msg{};
    msg.tags["message_type_name"] = "request_game_client_connection_info";
    msg.tags["ics_endpoint"] = "127.0.0.1:443";
    msg.tags["epoch"] = "18";
    msg.data_string = "worker-east";
    msg.data_bytes = {0xDE, 0xAD, 0xBE, 0xEF};

    const auto wire_bytes = msg.encode();
    if (wire_bytes.empty()) return false;
    if (tether::wire::hex_dump(wire_bytes.data(), wire_bytes.size(), 8).empty()) return false;

    const auto roundtrip = tether::wire::Message::decode(wire_bytes.data(), wire_bytes.size());
    if (roundtrip.data_string != "worker-east" || roundtrip.data_bytes != msg.data_bytes) {
        return false;
    }
    if (roundtrip.tags.at("message_type_name") != "request_game_client_connection_info" ||
        roundtrip.tags.at("ics_endpoint") != "127.0.0.1:443" ||
        roundtrip.tags.at("epoch") != "18") {
        return false;
    }

    if (!aoc::ics_token::is_loopback_ipv4(0x7F000001U)) return false;
    if (aoc::ics_token::is_loopback_ipv4(0x0A000001U)) return false;
    const auto fallback = aoc::ics_token::client_tokens("", false);
    return !fallback.saved && !fallback.auth.empty() &&
           aoc::ics_token::format_age(std::chrono::seconds(125)) == "0h 2m";
}

bool test_monotonic_lease_epoch_handover() {
    if (world::is_newer_lease_epoch(10ULL, 10ULL)) return false;
    if (world::is_newer_lease_epoch(9ULL, 10ULL)) return false;
    if (!world::is_newer_lease_epoch(11ULL, 10ULL)) return false;

    using world::AuthorityState;
    if (world::complete_border_handover(AuthorityState::HandoverPending, 10ULL, 10ULL) != AuthorityState::HandoverPending) {
        return false;
    }
    if (world::complete_border_handover(AuthorityState::HandoverPending, 9ULL, 10ULL) != AuthorityState::HandoverPending) {
        return false;
    }
    if (world::complete_border_handover(AuthorityState::HandoverPending, 11ULL, 10ULL) != AuthorityState::RemoteGhost) {
        return false;
    }
    return world::complete_border_handover(AuthorityState::Local, 11ULL, 10ULL) == AuthorityState::Local;
}

int run_mesh_forward_header_12b_epoch() {
    if (!test_tether_wire_header_and_big_endian()) return 1;
    if (!test_tether_proto_and_message_handover()) return 1;
    if (!test_monotonic_lease_epoch_handover()) return 1;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* case_name = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            case_name = argv[++i];
        }
    }
    if (!case_name) {
        if (run_hysteresis_band_200m_authority() != 0) return 1;
        if (run_mesh_forward_header_12b_epoch() != 0) return 1;
        return 0;
    }
    if (std::strcmp(case_name, "hysteresis_band_200m_authority") == 0) {
        return run_hysteresis_band_200m_authority();
    }
    if (std::strcmp(case_name, "mesh_forward_header_12b_epoch") == 0) {
        return run_mesh_forward_header_12b_epoch();
    }
    std::fprintf(stderr, "Unknown --case %s\n", case_name);
    return 2;
}
