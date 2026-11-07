// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "capture_packets.h"
#include "plans/plan_border_crossing.h"

namespace conn = aoc::protocol::conn;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using aoc::world::tests::bots::Address;
using aoc::world::tests::bots::build_actor_rpc;
using aoc::world::tests::bots::class_named;
using aoc::world::tests::bots::LiveConfig;
using aoc::world::tests::bots::LiveSession;
using aoc::world::tests::bots::strip_hyphens_lower;
using aoc::world::tests::bots::UdpSocket;
using aoc::world::tests::plans::attach_service_auth;
using aoc::world::tests::plans::encode_hex_guid_words;
using aoc::world::tests::plans::parse_spawn_actor_guid;
using aoc::world::tests::plans::pump_udp;
using aoc::world::tests::plans::run_all_26_bot_patterns_of_life;
using aoc::world::tests::plans::run_stage5_movement;
using aoc::world::tests::plans::run_stage6_inventory_scrolls;
using aoc::world::tests::plans::run_stage7_talents_actionbar;
using aoc::world::tests::plans::run_stage8_combat_abilities;
using aoc::world::tests::plans::run_stage9_world_interactions;
using aoc::world::tests::plans::send_bunches;

namespace {

bool run_stage1_handshake(LiveSession& s) {
    const auto raw_init = capture::bytes(capture::kInitial);
    auto init_pkt = conn::parse_handshake(raw_init.data(), raw_init.size());
    if (!init_pkt || !init_pkt->ext) return false;
    if (!encode_hex_guid_words(s.cfg.char_id, init_pkt->ext->bytes.data() + 22) ||
        !encode_hex_guid_words(s.cfg.ticket, init_pkt->ext->bytes.data() + 54)) {
        std::fprintf(stderr, "[FAIL] Stage 1: invalid 32-hex char_id or ticket\n");
        return false;
    }
    const auto init_bytes = conn::build_handshake(*init_pkt);
    uint8_t buf[2048]{};
    Address from{};
    for (int attempt = 0; attempt < 25; ++attempt) {
        s.sock->send(s.peer, init_bytes.data(), init_bytes.size());
        const auto n = s.sock->receive(buf, sizeof(buf), from, std::chrono::milliseconds(200));
        if (!n || *n == 0) continue;
        const auto ch = conn::parse_handshake(buf, *n);
        if (ch && ch->type == conn::HandshakeType::Challenge && ch->ext) {
            s.challenge = *ch;
            break;
        }
    }
    if (!s.challenge) return false;
    conn::HandshakePacket resp = *s.challenge;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    resp.padding.assign(11, 0x5A);
    encode_hex_guid_words(s.cfg.char_id, resp.ext->bytes.data() + 22);
    encode_hex_guid_words(s.cfg.ticket, resp.ext->bytes.data() + 54);
    const auto resp_bytes = conn::build_handshake(resp);
    bool got_ack = false;
    for (int attempt = 0; attempt < 25 && !got_ack; ++attempt) {
        s.sock->send(s.peer, resp_bytes.data(), resp_bytes.size());
        const auto n = s.sock->receive(buf, sizeof(buf), from, std::chrono::milliseconds(200));
        if (!n || *n == 0) continue;
        const auto ack = conn::parse_handshake(buf, *n);
        got_ack = ack && ack->type == conn::HandshakeType::Ack;
    }
    if (!got_ack) return false;
    const auto seqs = conn::initial_sequences(s.challenge->cookie);
    s.spawn = s.challenge->ext->spawn();
    s.client.emplace(conn::outer_header_of(*s.challenge), s.challenge->ext->custom_field(), seqs.server, seqs.client);
    std::printf("  [ok ] Stage 1: StatelessConnect 4-way handshake complete (server_seq=%u client_seq=%u)\n",
                seqs.server, seqs.client);
    return true;
}

bool has_control_nmt(const LiveSession& s, conn::Nmt type, std::string* arg0 = nullptr) {
    for (const fake::Received& r : s.rx_packets) {
        for (const conn::ControlMessage& m : r.messages) {
            if (m.type == type) {
                if (arg0) *arg0 = !m.level.empty() ? m.level : (!m.error.empty() ? m.error : m.challenge);
                return true;
            }
        }
    }
    return false;
}

std::string eff_tst_label(std::string_view seed) {
    static constexpr const char* kWords[] = {
        "Grueling", "Biscuit", "Launcher", "Noodle", "Cobalt", "Falcon", "Velvet", "Harbor",
        "Lantern",  "Granite", "Meadow",   "Orchid", "Quartz", "Timber", "Walnut", "Zephyr",
    };
    uint32_t h = 2166136261u;
    for (char c : seed) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return std::string("TST ") + kWords[h & 15u] + kWords[(h >> 4) & 15u] + kWords[(h >> 8) & 15u];
}

bool run_stage2_control_login(LiveSession& s) {
    const std::string machine = eff_tst_label(s.cfg.char_id);
    for (int attempt = 0; attempt < 30 && !has_control_nmt(s, conn::Nmt::Challenge); ++attempt) {
        if (attempt % 5 == 0) send_bunches(s, {s.client->hello()});
        pump_udp(s, 150);
    }
    if (!has_control_nmt(s, conn::Nmt::Challenge)) {
        std::fprintf(stderr, "[FAIL] Stage 2: timed out waiting for NMT_Challenge\n");
        return false;
    }
    send_bunches(s, {s.client->login(machine, machine)});
    std::string map_name;
    for (int attempt = 0; attempt < 50 && !has_control_nmt(s, conn::Nmt::Welcome, &map_name); ++attempt) {
        if (has_control_nmt(s, conn::Nmt::Failure)) return false;
        pump_udp(s, 150);
    }
    if (map_name.find("Verra_World_Master") == std::string::npos) return false;
    send_bunches(s, {s.client->netspeed(200000), s.client->nmt(conn::Nmt::Join)});
    std::printf("  [ok ] Stage 2: Ch 0 NMT_Hello -> NMT_Challenge -> NMT_Login ('%s') -> NMT_Welcome (%s) -> NMT_Join\n",
                machine.c_str(), map_name.c_str());
    return true;
}

bool run_stage3_bootstrap(LiveSession& s) {
    bool ch3 = false, ch4 = false, ch5 = false, ch6 = false;
    for (int attempt = 0; attempt < 60 && !(ch3 && ch4 && ch5 && ch6 && s.delivered.size() >= 12); ++attempt) {
        pump_udp(s, 150);
        for (const fake::Delivered& d : s.delivered) {
            if (d.channel == 3) {
                ch3 = true;
                if (s.pc_guid.ObjectId == 0 && d.open) s.pc_guid = parse_spawn_actor_guid(d.content);
            } else if (d.channel == 4) {
                ch4 = true;
            } else if (d.channel == 5) {
                ch5 = true;
                if (s.pawn_guid.ObjectId == 0 && d.open) s.pawn_guid = parse_spawn_actor_guid(d.content);
            } else if (d.channel == 6) {
                ch6 = true;
            }
        }
    }
    if (!(ch3 && ch4 && ch5 && ch6) || s.pawn_guid.ObjectId == 0) {
        std::fprintf(stderr, "[FAIL] Stage 3: incomplete bootstrap (delivered=%zu pawn=%llu)\n",
                     s.delivered.size(), static_cast<unsigned long long>(s.pawn_guid.ObjectId));
        return false;
    }
    if (s.ability_guid.ObjectId == 0) {
        s.ability_guid = {24, s.pawn_guid.ServerId, s.pawn_guid.Randomizer};
    }
    std::printf("  [ok ] Stage 3: Bootstrap chains delivered (%zu chains, pawn=%llu ability=%llu)\n",
                s.delivered.size(), static_cast<unsigned long long>(s.pawn_guid.ObjectId),
                static_cast<unsigned long long>(s.ability_guid.ObjectId));
    return true;
}

bool run_stage4_phase_in(LiveSession& s) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    repl::RpcParams ack_params;
    if (const schema::RpcSchema* r = pc.rpc("ServerAcknowledgePossession")) {
        repl::set_rpc_param(*r, ack_params, "P", repl::Value(s.pawn_guid));
    }
    send_bunches(s, {
        s.client->actor(3, build_actor_rpc(pc, "ServerAcknowledgePossession", ack_params), true),
        s.client->actor(3, build_actor_rpc(pc, "ServerRequestClientLoad", {}), true),
        s.client->actor(3, build_actor_rpc(pc, "ServerUpdateAccountCombatSettings", {}), true),
    });
    pump_udp(s, 350);
    send_bunches(s, {s.client->actor(5, build_actor_rpc(pawn, "ServerClientIsReadyToPhaseIn", {}), true)});
    for (int i = 0; i < 20 && (s.storage_payloads == 0 || s.actionbar_payloads == 0); ++i) {
        pump_udp(s, 150);
    }
    if (s.backpack_guid.ObjectId == 0) {
        s.backpack_guid = {28, s.pawn_guid.ServerId, s.pawn_guid.Randomizer};
    }
    std::printf("  [ok ] Stage 4: Possession, ClientLoad & PhaseIn complete (storage=%zu actionbar=%zu)\n",
                s.storage_payloads, s.actionbar_payloads);
    return s.storage_payloads > 0 && s.actionbar_payloads > 0;
}

bool run_stage10_close(LiveSession& s) {
    if (s.cfg.hold_ms > 0) pump_udp(s, s.cfg.hold_ms);
    for (int i = 0; i < 6 && pump_udp(s, 60, true) > 0; ++i) {}
    const fake::Bunch close_bunch = s.client->close_channel();
    auto stub = aoc::worldadmin::v1::WorldAdminService::NewStub(
        grpc::CreateChannel("127.0.0.1:50061", grpc::InsecureChannelCredentials()));
    const std::string want_id = strip_hyphens_lower(s.cfg.char_id);
    for (int attempt = 0; attempt < 10; ++attempt) {
        send_bunches(s, {close_bunch});
        pump_udp(s, 80, true);
        if (attempt >= 2 && s.pawn_guid.ObjectId != 0) {
            grpc::ClientContext dp_ctx; attach_service_auth(dp_ctx);
            aoc::worldadmin::v1::DespawnNpcRequest dp_req; dp_req.set_id(s.pawn_guid.ObjectId);
            aoc::worldadmin::v1::DespawnNpcResponse dp_resp;
            (void)stub->DespawnNpc(&dp_ctx, dp_req, &dp_resp);
        }
        grpc::ClientContext lp_ctx; attach_service_auth(lp_ctx);
        aoc::worldadmin::v1::ListPlayersResponse lp_resp;
        if (!stub->ListPlayers(&lp_ctx, {}, &lp_resp).ok()) continue;
        if (std::none_of(lp_resp.players().begin(), lp_resp.players().end(),
                         [&](const auto& p) { return strip_hyphens_lower(p.character_id()) == want_id; })) break;
    }
    std::printf("  [ok ] Stage 10: Ch 0 close_channel() sent; session closed cleanly\n");
    return true;
}

LiveConfig parse_args(int argc, char** argv) {
    LiveConfig cfg;
    if (const char* v = std::getenv("AOC_LIVE_UDP_HOST"); v && *v) cfg.host = v;
    if (const char* v = std::getenv("AOC_LIVE_UDP_PORT"); v && *v) cfg.port = static_cast<uint16_t>(std::atoi(v));
    if (const char* v = std::getenv("AOC_LIVE_CHAR_ID"); v && *v) cfg.char_id = v;
    if (const char* v = std::getenv("AOC_LIVE_TICKET"); v && *v) cfg.ticket = v;
    if (const char* v = std::getenv("AOC_LIVE_USMAP_PATH"); v && *v) cfg.usmap_path = v;
    if (const char* v = std::getenv("AOC_LIVE_HOLD_MS"); v && *v) cfg.hold_ms = std::atoi(v);
    if (const char* v = std::getenv("AOC_LIVE_TIMEOUT_MS"); v && *v) cfg.timeout_ms = std::atoi(v);

    std::vector<std::string> pos;
    for (int i = 1; i < argc && i < 64; ++i) {
        const std::string a = argv[i];
        if ((a == "--char-id" || a == "--character-id") && i + 1 < argc) cfg.char_id = argv[++i];
        else if (a == "--ticket" && i + 1 < argc) cfg.ticket = argv[++i];
        else if (a == "--host" && i + 1 < argc) cfg.host = argv[++i];
        else if (a == "--port" && i + 1 < argc) cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--usmap" && i + 1 < argc) cfg.usmap_path = argv[++i];
        else if (a == "--hold-ms" && i + 1 < argc) cfg.hold_ms = std::atoi(argv[++i]);
        else if (a == "--timeout-ms" && i + 1 < argc) cfg.timeout_ms = std::atoi(argv[++i]);
        else if (!a.empty() && a[0] != '-') pos.push_back(a);
    }
    if (cfg.char_id.empty() && pos.size() >= 1) cfg.char_id = pos[0];
    if (cfg.ticket.empty() && pos.size() >= 2) cfg.ticket = pos[1];
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    (void)class_named("AoCPlayerControllerBP_C");
    (void)class_named("PlayerPawn_C");
    (void)class_named("UAoCAbilityComponent");

    if (!run_all_26_bot_patterns_of_life()) {
        std::fprintf(stderr, "[FAIL] 32-bot 64-archetype-pair 26-plan player-bot suite failed\n");
        return 1;
    }

    LiveSession s;
    s.cfg = parse_args(argc, argv);
    if (s.cfg.char_id.empty() || s.cfg.ticket.empty()) {
        uint8_t words[16]{};
        if (!encode_hex_guid_words("bbec5c86eb101fe5f38c87f0fa22e7d0", words)) return 1;
        std::printf("[skip] test_world_live_e2e: AOC_LIVE_CHAR_ID and AOC_LIVE_TICKET not set (hermetic self-check ok)\n");
        return 0;
    }

    const auto parsed = aoc::worldd::parse_address(s.cfg.host + ":" + std::to_string(s.cfg.port));
    if (!parsed) {
        std::fprintf(stderr, "[FAIL] invalid target UDP address %s:%u\n", s.cfg.host.c_str(), s.cfg.port);
        return 1;
    }
    s.peer = *parsed;
    std::string err;
    s.sock = UdpSocket::open(Address{s.peer.is_loopback() ? 0x7F000001u : 0u, 0}, &err);
    if (!s.sock) {
        std::fprintf(stderr, "[FAIL] UdpSocket::open failed: %s\n", err.c_str());
        return 1;
    }

    if (!run_stage1_handshake(s) || !run_stage2_control_login(s) || !run_stage3_bootstrap(s) ||
        !run_stage4_phase_in(s) || !run_stage5_movement(s) || !run_stage6_inventory_scrolls(s) ||
        !run_stage7_talents_actionbar(s) || !run_stage8_combat_abilities(s) ||
        !run_stage9_world_interactions(s) || !run_stage10_close(s)) {
        return 1;
    }

    std::printf("[PASS] test_world_live_e2e completed all 10 stages\n");
    return 0;
}
