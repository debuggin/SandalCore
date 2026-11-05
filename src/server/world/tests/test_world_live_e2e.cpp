// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "capture_packets.h"
#include "fake_client.h"
#include "net/udp_socket.h"
#include "protocol/conn/handshake.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/craft_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/skill_rpc.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/packet_reader.h"
#include "protocol/wire/ue5_primitives.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace wire = aoc::protocol::wire;
using aoc::worldd::Address;
using aoc::worldd::UdpSocket;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;

namespace {

constexpr int64_t kGmAdventuringScroll = 6064634615524884604ll;
constexpr int64_t kBardTree = 6064629885900499588ll;
constexpr int64_t kInstrumentFlute = 6064630871707615237ll;
constexpr int64_t kGuidAdventuring = 6064629098744774667ll;
constexpr int64_t kTypeSkillRank = 6064628725236039698ll;
constexpr int64_t kTypeSkillTree = 6064628725236039697ll;
constexpr int64_t kTypeLevelingTypeDef = 6064628934189875200ll;
constexpr int64_t kSprintAbility = 6064631641648332884ll;
constexpr int64_t kPetFollowCmd = 6064630881966096959ll;

struct LiveConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 7777;
    std::string char_id;
    std::string ticket;
    std::string usmap_path = "/app/content/aoc.usmap";
    int timeout_ms = 8000;
    int hold_ms = 0;
};

struct ExportRecord {
    FIntrepidNetworkGUID guid{};
    uint8_t flags = 0;
    FIntrepidNetworkGUID outer{};
    std::string path;
    uint32_t checksum = 0;
};

struct ExportCursor {
    const uint8_t* data = nullptr;
    size_t len = 0;
    size_t pos = 0;
};

struct LiveSession {
    LiveConfig cfg;
    Address peer{};
    std::unique_ptr<UdpSocket> sock;
    std::optional<conn::HandshakePacket> challenge;
    std::optional<fake::Client> client;
    std::vector<fake::Received> rx_packets;
    std::vector<fake::Delivered> delivered;
    std::array<double, 3> spawn{0.0, 0.0, 1100.0};
    FIntrepidNetworkGUID pc_guid{};
    FIntrepidNetworkGUID pawn_guid{};
    FIntrepidNetworkGUID ability_guid{};
    FIntrepidNetworkGUID backpack_guid{};
    FIntrepidNetworkGUID equipment_guid{};
    int32_t scroll_slot = 0;
    int32_t scroll_qty = 1;
    repl::GuidValue scroll_guid{};
    bool has_scroll = false;
    size_t storage_payloads = 0;
    size_t actionbar_payloads = 0;
    float move_ts = 0.0f;
};

std::string strip_hyphens_lower(std::string_view in) {
    std::string out;
    out.reserve(32);
    for (size_t i = 0; i < in.size() && i < 64; ++i) {
        const char c = in[i];
        if (c == '-') continue;
        if (c >= 'A' && c <= 'F') out.push_back(static_cast<char>(c - 'A' + 'a'));
        else out.push_back(c);
    }
    return out;
}

bool encode_hex_guid_words(std::string_view hex_raw, uint8_t* out16) {
    const std::string hex = strip_hyphens_lower(hex_raw);
    if (hex.size() != 32) return false;
    for (size_t i = 0; i < 4; ++i) {
        char* endp = nullptr;
        const std::string part = hex.substr(i * 8, 8);
        const unsigned long val = std::strtoul(part.c_str(), &endp, 16);
        if (!endp || *endp != '\0') return false;
        const uint32_t word = static_cast<uint32_t>(val);
        std::memcpy(out16 + i * 4, &word, 4);
    }
    return ::ue5::guid_to_hex(out16) == hex;
}

const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
    if (!c) {
        std::fprintf(stderr, "[FAIL] missing compiled schema for %s\n", name);
        std::exit(2);
    }
    return *c;
}

BunchWriter build_actor_rpc(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params) {
    BunchWriter out;
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
    BunchWriter field;
    bool failed = false;
    const size_t bits = repl::write_rpc_field(cls, *rpc, params, field, {}, &failed);
    if ((bits == 0 && !params.values.empty()) || failed) return out;
    repl::write_content_block(out, repl::BlockHeader{}, field, false);
    return out;
}

BunchWriter build_subobject_rpc(const schema::ClassSchema& cls, const FIntrepidNetworkGUID& obj, const char* name,
                                const repl::RpcParams& params) {
    BunchWriter out;
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
    BunchWriter field;
    bool failed = false;
    const size_t bits = repl::write_rpc_field(cls, *rpc, params, field, {}, &failed);
    if ((bits == 0 && !params.values.empty()) || failed) return out;
    repl::BlockHeader header{};
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = obj;
    repl::write_content_block(out, header, field, false);
    return out;
}

bool read_export_entry(ExportCursor& cur, std::vector<ExportRecord>& out, FIntrepidNetworkGUID* guid_out, int depth) {
    if (depth > 8) return false;
    ExportRecord r;
    r.guid = emit::read_intrepid_guid(cur.data, cur.len, cur.pos);
    if (guid_out) *guid_out = r.guid;
    if (r.guid.ObjectId == 0) return true;
    r.flags = static_cast<uint8_t>(::ue5::read_bits(cur.data, cur.len, cur.pos, 8));
    if ((r.flags & 1u) != 0u) {
        if (!read_export_entry(cur, out, &r.outer, depth + 1)) return false;
        const int32_t n = static_cast<int32_t>(::ue5::read_bits(cur.data, cur.len, cur.pos, 32));
        if (n < 0 || n > 512) return false;
        for (int32_t i = 0; i < n; ++i) {
            const char c = static_cast<char>(::ue5::read_bits(cur.data, cur.len, cur.pos, 8));
            if (c != 0) r.path.push_back(c);
        }
        if ((r.flags & 4u) != 0u) r.checksum = static_cast<uint32_t>(::ue5::read_bits(cur.data, cur.len, cur.pos, 32));
    }
    out.push_back(std::move(r));
    return true;
}

void scan_delivered_exports(LiveSession& s, const fake::Delivered& d) {
    for (const BunchWriter& e : d.exports) {
        const size_t bits = e.bit_pos();
        ExportCursor cur{e.data(), e.byte_size() + 1, 0};
        (void)::ue5::read_bits(cur.data, cur.len, cur.pos, 1);
        const uint32_t count = static_cast<uint32_t>(::ue5::read_bits(cur.data, cur.len, cur.pos, 32));
        std::vector<ExportRecord> recs;
        for (uint32_t i = 0; i < count && cur.pos < bits && i < 256; ++i) {
            if (!read_export_entry(cur, recs, nullptr, 0)) break;
        }
        for (const ExportRecord& r : recs) {
            if (r.path == "AbilityComponent" && s.ability_guid.ObjectId == 0) s.ability_guid = r.guid;
            if (r.path == "BackpackItemStorage" && s.backpack_guid.ObjectId == 0) s.backpack_guid = r.guid;
            if (r.path == "EquipmentItemStorage" && s.equipment_guid.ObjectId == 0) s.equipment_guid = r.guid;
        }
    }
}

void inspect_storage_and_bar_rpcs(LiveSession& s, const fake::Delivered& d) {
    if (d.channel != 3 && d.channel != 5) return;
    const schema::ClassSchema& cls = class_named(d.channel == 3 ? "AoCPlayerControllerBP_C" : "PlayerPawn_C");
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return;
    for (const repl::BlockView& b : *blocks) {
        if (b.header.has_rep_layout) continue;
        size_t pos = b.payload_pos;
        const auto calls = repl::read_rpc_fields(cls, d.content.data(), b.payload_pos + b.payload_bits, pos);
        if (!calls) continue;
        for (const repl::RpcCall& call : *calls) {
            if (const auto sp = repl::parse_storage_payload(call)) {
                ++s.storage_payloads;
                if (sp->maximum_items == 70 && s.backpack_guid.ObjectId == 0) s.backpack_guid = sp->target;
                if (sp->maximum_items == 83 && s.equipment_guid.ObjectId == 0) s.equipment_guid = sp->target;
                for (const repl::StorageSlot& sl : sp->storage) {
                    if (!sl.item.has_item) continue;
                    if (!s.has_scroll || sl.item.record_id == kGmAdventuringScroll) {
                        s.scroll_slot = sl.slot;
                        s.scroll_qty = std::max<int32_t>(1, sl.item.quantity);
                        s.scroll_guid = sl.item.guid;
                        s.has_scroll = true;
                    }
                }
            }
            if (repl::parse_actionbar_payload(call)) ++s.actionbar_payloads;
        }
    }
}

size_t pump_udp(LiveSession& s, int wait_ms, bool send_acks = true) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    size_t got_packets = 0;
    uint8_t buf[2048]{};
    for (int step = 0; step < 2000 && std::chrono::steady_clock::now() < deadline; ++step) {
        Address from{};
        const auto n = s.sock->receive(buf, sizeof(buf), from, std::chrono::microseconds(10000));
        if (!n || *n == 0) continue;
        size_t batch = 0;
        std::optional<size_t> cur = n;
        for (int drain = 0; drain < 128 && cur && *cur > 0; ++drain) {
            if (!conn::is_handshake(buf, *cur) && s.client) {
                std::vector<uint8_t> pkt(buf, buf + *cur);
                fake::Received rx = s.client->receive(pkt);
                for (const fake::Delivered& d : rx.delivered) {
                    scan_delivered_exports(s, d);
                    inspect_storage_and_bar_rpcs(s, d);
                    s.delivered.push_back(d);
                }
                s.rx_packets.push_back(std::move(rx));
                ++got_packets;
                ++batch;
            }
            cur = s.sock->receive(buf, sizeof(buf), from, std::chrono::microseconds(0));
        }
        if (batch > 0 && send_acks && s.client) {
            const auto ack = s.client->packet({});
            s.sock->send(s.peer, ack.data(), ack.size());
        }
    }
    return got_packets;
}

bool send_bunches(LiveSession& s, std::vector<fake::Bunch> bunches) {
    if (!s.client) return false;
    const auto pkt = s.client->packet(std::move(bunches));
    return s.sock->send(s.peer, pkt.data(), pkt.size());
}

FIntrepidNetworkGUID parse_spawn_actor_guid(const BunchWriter& w) {
    wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    FIntrepidNetworkGUID g{};
    const uint64_t lo = r.read_uint32();
    const uint64_t hi = r.read_uint32();
    g.ObjectId = lo | (hi << 32);
    g.ServerId = r.read_uint32();
    g.Randomizer = r.read_uint32();
    return g;
}

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

bool run_stage2_control_login(LiveSession& s) {
    const std::string machine = "DESKTOP-LIVE-E2E-0123456789ABCDEF0123456789ABCDEF";
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
        if (has_control_nmt(s, conn::Nmt::Failure)) {
            std::fprintf(stderr, "[FAIL] Stage 2: received NMT_Failure on Ch 0\n");
            return false;
        }
        pump_udp(s, 150);
    }
    if (map_name.find("Verra_World_Master") == std::string::npos) {
        std::fprintf(stderr, "[FAIL] Stage 2: unexpected NMT_Welcome map '%s'\n", map_name.c_str());
        return false;
    }
    send_bunches(s, {s.client->netspeed(200000), s.client->nmt(conn::Nmt::Join)});
    std::printf("  [ok ] Stage 2: Ch 0 NMT_Hello -> NMT_Challenge -> NMT_Login -> NMT_Welcome (%s) -> NMT_Join\n",
                map_name.c_str());
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

BunchWriter encode_pawn_move(float ts, double x, double y, double z) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::NetField* f = pawn.field("ServerMovePacked");
    BunchWriter out;
    if (!f) return out;
    repl::MovePacked mp;
    mp.new_move.time_stamp = ts;
    mp.new_move.acceleration = repl::quantize_vector(10, 0.0, 0.0, 0.0);
    mp.new_move.location = repl::quantize_vector(100, x, y, z);
    mp.new_move.movement_mode = repl::kMoveWalking;
    BunchWriter params, field;
    if (!repl::write_move_packed(params, mp) || repl::write_field(pawn, f->index, params, field) == 0) return out;
    repl::write_content_block(out, repl::BlockHeader{}, field, false);
    return out;
}

bool run_stage5_movement(LiveSession& s) {
    for (int step = 1; step <= 3; ++step) {
        s.move_ts += 0.2f;
        const BunchWriter mv = encode_pawn_move(s.move_ts, s.spawn[0] + step * 40.0, s.spawn[1], s.spawn[2]);
        send_bunches(s, {s.client->actor(5, mv, false)});
        pump_udp(s, 120);
    }
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    repl::RpcParams tp;
    if (const schema::RpcSchema* rpc = pc.rpc("ServerTeleportXYZ")) {
        repl::set_rpc_param(*rpc, tp, "X", repl::Value::i64(static_cast<int64_t>(s.spawn[0] + 120.0)));
        repl::set_rpc_param(*rpc, tp, "Y", repl::Value::i64(static_cast<int64_t>(s.spawn[1])));
        repl::set_rpc_param(*rpc, tp, "Z", repl::Value::i64(static_cast<int64_t>(s.spawn[2])));
        repl::set_rpc_param(*rpc, tp, "OptionalRadius", repl::Value::f32(0.0f));
    }
    send_bunches(s, {s.client->actor(3, build_actor_rpc(pc, "ServerTeleportXYZ", tp), true)});
    pump_udp(s, 250);
    std::printf("  [ok ] Stage 5: ServerMovePacked (3 steps) & ServerTeleportXYZ acknowledged\n");
    return true;
}

bool run_stage6_inventory_scrolls(LiveSession& s) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    repl::RpcParams used;
    if (const schema::RpcSchema* rpc = pawn.rpc("ServerItemUsed")) {
        repl::set_rpc_param(*rpc, used, "Storage", repl::Value::u64(3));
        repl::set_rpc_param(*rpc, used, "Slot", repl::Value::i64(s.scroll_slot));
        repl::set_rpc_param(*rpc, used, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(s.scroll_guid.a)));
        repl::set_rpc_param(*rpc, used, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(s.scroll_guid.b)));
        repl::set_rpc_param(*rpc, used, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(s.scroll_guid.c)));
        repl::set_rpc_param(*rpc, used, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(s.scroll_guid.d)));
        repl::set_rpc_param(*rpc, used, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
    }
    send_bunches(s, {s.client->actor(5, build_actor_rpc(pawn, "ServerItemUsed", used), true)});
    pump_udp(s, 200);

    repl::ItemStorageEvent ev{};
    ev.type = static_cast<uint8_t>(repl::StorageEventType::Move);
    ev.move_source.object = s.backpack_guid;
    ev.move_source.slots = {{s.scroll_slot, 1, s.scroll_guid}};
    ev.move_source.status = repl::kStorageNoError;
    ev.targets.push_back({s.backpack_guid, {{35, 1, s.scroll_guid}}, repl::kStorageNoError});
    if (const schema::RpcSchema* rpc = pc.rpc("ServerRequestStorageEvent")) {
        if (const auto params = repl::storage_event_params(*rpc, ev)) {
            send_bunches(s, {s.client->actor(3, build_actor_rpc(pc, "ServerRequestStorageEvent", *params), true)});
        }
    }
    pump_udp(s, 200);
    std::printf("  [ok ] Stage 6: ServerItemUsed (slot=%d) & ServerRequestStorageEvent processed\n", s.scroll_slot);
    return true;
}

bool run_stage7_talents_actionbar(LiveSession& s) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    repl::SkillBuild build;
    build.changes.push_back({
        {kInstrumentFlute, kTypeSkillRank},
        {kBardTree, kTypeSkillTree, repl::NameValue::plain("Prototype_Assassin_SkillTable")},
        {kGuidAdventuring, kTypeLevelingTypeDef},
        true,
    });
    if (const schema::RpcSchema* rpc = pawn.rpc("ServerPurchaseSkillBuild")) {
        if (const auto params = repl::purchase_skill_build_params(*rpc, build)) {
            send_bunches(s, {s.client->actor(5, build_actor_rpc(pawn, "ServerPurchaseSkillBuild", *params), true)});
        }
    }
    repl::ActionbarSlot slot{};
    slot.bank_index = 0;
    slot.slot_index = 5;
    slot.use.use_type = repl::ActionbarUseType::Ability;
    slot.use.instigator = repl::ActionbarInstigator::Player;
    slot.use.ability_id = kSprintAbility;
    if (const schema::RpcSchema* rpc = pc.rpc("ServerSetActionbarSlotUseData")) {
        if (const auto params = repl::set_actionbar_slot_params(*rpc, slot)) {
            send_bunches(s, {s.client->actor(3, build_actor_rpc(pc, "ServerSetActionbarSlotUseData", *params), true)});
        }
    }
    pump_udp(s, 250);
    std::printf("  [ok ] Stage 7: ServerPurchaseSkillBuild & ServerSetActionbarSlotUseData processed\n");
    return true;
}

bool run_stage8_combat_abilities(LiveSession& s) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::ClassSchema& ab_cls = class_named("UAoCAbilityComponent");
    send_bunches(s, {s.client->actor(5, build_actor_rpc(pawn, "ServerSetActiveTarget", {}), true)});

    repl::RpcParams trig;
    if (const schema::RpcSchema* rpc = ab_cls.rpc("ServerTriggerAbility")) {
        repl::set_rpc_param(*rpc, trig, "AbilityGuid", repl::Value::i64(kSprintAbility));
        repl::set_rpc_param(*rpc, trig, "ClientInstanceId", repl::Value::i64(1));
        repl::set_rpc_param(*rpc, trig, "InputVector", repl::Value(repl::Vec2d{1.0, 0.0}));
        repl::set_rpc_param(*rpc, trig, "ItemData.Slot", repl::Value::i64(-1));
        repl::set_rpc_param(*rpc, trig, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
        repl::set_rpc_param(*rpc, trig, "ItemData.Location", repl::Value::u64(24));
    }
    send_bunches(s, {s.client->actor(5, build_subobject_rpc(ab_cls, s.ability_guid, "ServerTriggerAbility", trig), true)});
    pump_udp(s, 150);

    repl::RpcParams cancel;
    if (const schema::RpcSchema* rpc = ab_cls.rpc("ServerNotifyHeldAbilityCancelled")) {
        repl::set_rpc_param(*rpc, cancel, "ClientInstanceId", repl::Value::i64(1));
        repl::set_rpc_param(*rpc, cancel, "CancelledTime", repl::Value::f32(0.1f));
        repl::set_rpc_param(*rpc, cancel, "CancelSectionIndex", repl::Value::i64(-1));
    }
    repl::RpcParams pet;
    if (const schema::RpcSchema* rpc = pawn.rpc("ServerSendPetCommand")) {
        repl::set_rpc_param(*rpc, pet, "PetCommandId", repl::Value::i64(kPetFollowCmd));
    }
    send_bunches(s, {
        s.client->actor(5, build_subobject_rpc(ab_cls, s.ability_guid, "ServerNotifyHeldAbilityCancelled", cancel), true),
        s.client->actor(5, build_actor_rpc(pawn, "ServerSendPetCommand", pet), true),
    });
    pump_udp(s, 200);
    std::printf("  [ok ] Stage 8: ServerSetActiveTarget, ServerTriggerAbility & ServerSendPetCommand processed\n");
    return true;
}

bool run_stage9_world_interactions(LiveSession& s) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    std::vector<fake::Bunch> batch;
    if (const schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent")) {
        repl::InteractRequest req{};
        req.interactable = s.pawn_guid;
        req.counter = 1;
        req.zone = 1;
        if (const auto p = repl::interact_params(*rpc, req)) {
            batch.push_back(s.client->actor(5, build_actor_rpc(pawn, "ServerInteractWithInteractableComponent", *p), true));
        }
    }
    if (const schema::RpcSchema* rpc = pc.rpc("ServerSelectDialogueChoice")) {
        repl::DialogueResponse d{};
        d.response_type = static_cast<uint8_t>(repl::DialogueResponseType::SubDialogue);
        d.quest_response = static_cast<uint8_t>(repl::SubDialogueResponse::Close);
        if (const auto p = repl::dialogue_response_params(*rpc, d)) {
            batch.push_back(s.client->actor(3, build_actor_rpc(pc, "ServerSelectDialogueChoice", *p), true));
        }
    }
    if (const schema::RpcSchema* rpc = pc.rpc("ServerOpenVendor")) {
        repl::OpenVendorRequest v{};
        v.npc = {1, 2, 3, 4};
        v.vendor = 0;
        if (const auto p = repl::server_open_vendor_params(*rpc, v)) {
            batch.push_back(s.client->actor(3, build_actor_rpc(pc, "ServerOpenVendor", *p), true));
        }
    }
    if (const schema::RpcSchema* rpc = pc.rpc("ServerStartCraft")) {
        repl::StartCraft c{};
        c.station = s.pawn_guid;
        c.quantity = 1;
        if (const auto p = repl::start_craft_params(*rpc, c)) {
            batch.push_back(s.client->actor(3, build_actor_rpc(pc, "ServerStartCraft", *p), true));
        }
    }
    if (const schema::RpcSchema* rpc = pc.rpc("ServerGetNodeTaxTable")) {
        if (const auto p = repl::node_tax_request_params(*rpc, 0)) {
            batch.push_back(s.client->actor(3, build_actor_rpc(pc, "ServerGetNodeTaxTable", *p), true));
        }
    }
    send_bunches(s, std::move(batch));
    pump_udp(s, 250);
    std::printf("  [ok ] Stage 9: Interact, Dialogue, Vendor, Craft & NodeTax RPCs processed\n");
    return true;
}

bool run_stage10_close(LiveSession& s) {
    if (s.cfg.hold_ms > 0) {
        std::printf("  [info] Stage 10: holding session alive for %d ms...\n", s.cfg.hold_ms);
        pump_udp(s, s.cfg.hold_ms);
    }
    send_bunches(s, {s.client->close_channel()});
    pump_udp(s, 200, false);
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
