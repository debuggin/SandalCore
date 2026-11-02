// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "capture_foliage.h"
#include "capture_gather.h"
#include "capture_in_place.h"
#include "capture_mount.h"
#include "protocol/repl/fname.h"
#include "test_support.h"
#include "test_worldd_foliage_session_common.h"

inline const char *g_path = nullptr;
inline const char *g_tsv = nullptr;

namespace fid {
inline constexpr int64_t kLumberjacking = 108752769996426ll;
inline constexpr double kOakX = -1032805.5, kOakY = 495823.7, kOakZ = 12846.0;
inline constexpr double kStandX = -1032839.01, kStandY = 495968.38, kStandZ = 12950.98;
inline constexpr const char *kOwner = "InstancedFoliageActor_25600_-41_19_0";
inline constexpr const char *kFismc0 = "IntrepidGatherableFISMC_0";
inline constexpr int64_t kOakLog = 109162169652924ll;
inline constexpr int64_t kCoal = 109162169652923ll;
} // namespace fid

inline svc::Character record_at(const char *id_hex, const char *name, double x, double y, double z) {
    svc::Character c = record_of(id_hex, name);
    c.has_position = true;
    c.position.x = x;
    c.position.y = y;
    c.position.z = z;
    return c;
}

struct FoliageSeen {
    size_t chain = 0;
    float duration = 0;
    double start_time = 0;
    int64_t instance = 0;
    std::string owner, component;
    uint64_t state = 0;
    int64_t tool_record = 0;
};

inline std::string name_text(const repl::Value *v) {
    const repl::NameValue *n = v ? v->get_if<repl::NameValue>() : nullptr;
    return n ? repl::fname_text(*n) : std::string();
}

inline void read_foliage_seen_block(size_t chain_idx, const fake::Delivered &d, const repl::BlockView &b,
                                    const emit::FIntrepidNetworkGUID &base_info, std::vector<FoliageSeen> &out) {
    if (b.header.is_actor || !b.header.has_rep_layout || !(b.header.object == base_info)) return;
    const schema::ClassSchema &info = class_named("UPlayerInfo");
    size_t pos = b.payload_pos;
    const auto st = repl::read_rep_block(info, d.content.data(), b.payload_pos + b.payload_bits, pos);
    if (!st) return;
    const repl::Value *dur = st->get(info, "FoliageData.Duration");
    if (!dur) return;
    FoliageSeen f;
    f.chain = chain_idx;
    f.duration = dur->get_if<float>() ? *dur->get_if<float>() : -1.0f;
    const repl::Value *t = st->get(info, "FoliageData.ServerStartTime");
    f.start_time = (t && t->get_if<double>()) ? *t->get_if<double>() : -1.0;
    const repl::Value *idx = st->get(info, "FoliageData.OriginalInstanceIndex");
    f.instance = (idx && idx->get_if<int64_t>()) ? *idx->get_if<int64_t>() : -2;
    f.owner = name_text(st->get(info, "FoliageData.FoliageName"));
    f.component = name_text(st->get(info, "FoliageData.FoliageComponentUUID"));
    const repl::Value *s = st->get(info, "FoliageData.FoliageState");
    f.state = (s && s->get_if<uint64_t>()) ? *s->get_if<uint64_t>() : 99;
    const repl::Value *tool = st->get(info, "FoliageData.ToolRecordId");
    f.tool_record = (tool && tool->get_if<int64_t>()) ? *tool->get_if<int64_t>() : -1;
    out.push_back(std::move(f));
}

inline std::vector<FoliageSeen> foliage_data_seen(const Player &p, size_t from) {
    std::vector<FoliageSeen> out;
    const bs::SessionGuids &g = p.s->bootstrap().setup().guids;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pawn_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto &b : *blocks) read_foliage_seen_block(i, p.got[i], b, g.base_character_info, out);
    }
    return out;
}

struct IfaSeen {
    size_t chain = 0;
    bool open = false, close = false;
    std::string owner;
    std::vector<std::pair<uint32_t, uint32_t>> changed;
    std::vector<int32_t> deleted;
    size_t components = 0;
};

inline size_t new_actor_prefix_bits(const emit::BunchWriter &content) {
    aoc::protocol::wire::PacketReader r(content.data(), (content.bit_pos() + 7) / 8, content.bit_pos());
    r.set_pos(384);
    if (r.read_bit()) {
        if (r.read_bit()) {
            if (!repl::read_quantized_vector(r)) return 0;
        } else {
            r.read_bits(64);
            r.read_bits(64);
            r.read_bits(64);
        }
    }
    r.read_bit();
    r.read_bit();
    r.read_bit();
    return r.pos();
}

inline void read_ifa_open_owner(const fake::Delivered &d, std::map<uint32_t, std::string> &channels) {
    channels.erase(d.channel);
    const size_t prefix = new_actor_prefix_bits(d.content);
    const auto blocks =
        prefix ? repl::read_content_blocks(d.content.data(), d.content.bit_pos(), prefix) : std::nullopt;
    if (!blocks || blocks->empty() || !(*blocks)[0].header.is_actor || !(*blocks)[0].header.has_rep_layout) return;
    const schema::ClassSchema &ifa = class_named("AIntrepidStateIFA");
    size_t pos = (*blocks)[0].payload_pos;
    const auto st =
        repl::read_rep_block(ifa, d.content.data(), (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, pos);
    const repl::Value *name = st ? st->get(ifa, "FoliageName") : nullptr;
    if (name) channels[d.channel] = name_text(name);
}

inline void read_ifa_comp_field(const repl::FastArrayField &f, IfaSeen &s) {
    for (const int32_t d : f.delta.deleted) s.deleted.push_back(d);
    for (const auto &ch2 : f.delta.changed) {
        const auto st1 = ch2.values.values.find(1);
        const auto key = ch2.values.values.find(2);
        if (st1 == ch2.values.values.end() || key == ch2.values.values.end()) continue;
        if (!st1->second.get_if<uint64_t>() || !key->second.get_if<uint64_t>()) continue;
        s.changed.emplace_back(static_cast<uint32_t>(*key->second.get_if<uint64_t>()),
                               static_cast<uint32_t>(*st1->second.get_if<uint64_t>()));
    }
}

inline void read_ifa_comp_block(const fake::Delivered &d, const repl::BlockView &b, IfaSeen &s) {
    if (b.header.is_actor) return;
    ++s.components;
    const schema::ClassSchema &comp = class_named("UIntrepidFoliageStateComponent");
    size_t pos = b.payload_pos;
    const size_t end = b.payload_pos + b.payload_bits;
    if (b.header.has_rep_layout && !repl::read_rep_block(comp, d.content.data(), end, pos)) return;
    if (pos >= end) return;
    const auto fields = repl::read_fast_array_fields(comp, d.content.data(), end, pos);
    if (!fields) return;
    for (const repl::FastArrayField &f : *fields) read_ifa_comp_field(f, s);
}

inline void read_ifa_blocks(const fake::Delivered &d, IfaSeen &s) {
    if (d.close) return;
    const auto blocks =
        repl::read_content_blocks(d.content.data(), d.content.bit_pos(), d.open ? new_actor_prefix_bits(d.content) : 0);
    if (!blocks) return;
    for (const auto &b : *blocks) read_ifa_comp_block(d, b, s);
}

inline std::vector<IfaSeen> ifa_seen(const Player &p, size_t from) {
    std::vector<IfaSeen> out;
    std::map<uint32_t, std::string> channels;
    for (size_t i = from; i < p.got.size(); ++i) {
        const fake::Delivered &d = p.got[i];
        if (d.open) read_ifa_open_owner(d, channels);
        const auto it = channels.find(d.channel);
        if (it == channels.end()) continue;
        IfaSeen s;
        s.chain = i;
        s.open = d.open;
        s.close = d.close;
        s.owner = it->second;
        read_ifa_blocks(d, s);
        out.push_back(std::move(s));
        if (d.close) channels.erase(d.channel);
    }
    return out;
}

inline BunchWriter client_move(float ts, bool moving, double x = fid::kStandX, double y = fid::kStandY,
                               double z = fid::kStandZ) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, moving ? 100 : 0, 0, 0);
    m.location = repl::quantize_vector(100, x, y, z);
    m.movement_mode = repl::kMoveWalking;
    m.input_bits = moving ? 1 : 0;
    const schema::ClassSchema &cls = class_named("PlayerPawn_C");
    const schema::NetField *f = cls.field("ServerMovePacked");
    BunchWriter out;
    repl::MovePacked mp;
    mp.new_move = m;
    BunchWriter params;
    BunchWriter field;
    if (!f || !repl::write_move_packed(params, mp) || repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

inline BunchWriter foliage_start(const char *owner, const char *component, uint32_t index, uint64_t state) {
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema *rpc = pc.rpc("ServerStartInteractionWithFoliage");
    repl::RpcParams p;
    if (!rpc) return BunchWriter();
    if (index != 0) repl::set_rpc_param(*rpc, p, "OriginalInstanceIndex", repl::Value::i64(index));
    repl::set_rpc_param(*rpc, p, "FoliageName", repl::Value(repl::fname_parse(owner)));
    repl::set_rpc_param(*rpc, p, "FoliageComponentUUID", repl::Value(repl::fname_parse(component)));
    repl::set_rpc_param(*rpc, p, "FoliageState", repl::Value::u64(state));
    return actor_rpc(pc, "ServerStartInteractionWithFoliage", p);
}

inline std::vector<std::string> pc_call_names_since(const Player &p, size_t from) {
    std::vector<std::string> out;
    for (const auto &[chain, name] : pc_chain_calls(p, from)) out.push_back(name);
    return out;
}

struct FRig {
    Rig rig;
    bool open(double regrow_mult = 0.01, bool all_cells = true) {
        if (!rig.open(g_path)) return false;
        std::string error;
        foliage::Config cfg;
        cfg.map_path = g_tsv;
        cfg.regrow_mult = regrow_mult;
        cfg.all_cells = all_cells;
        if (!rig.world->start_foliage(cfg, error)) {
            std::printf("  [FAIL] foliage does not start: %s\n", error.c_str());
            return false;
        }
        return true;
    }
    foliage::Foliage &f() { return *rig.world->foliage(); }
    uint32_t oak_row() { return *f().map().find(fid::kOwner, fid::kFismc0, 0); }
};

inline Player &join_at_the_oak(FRig &fr, const char *id_hex, const char *name) {
    Player &p = fr.rig.add_player(record_at(id_hex, name, fid::kStandX, fid::kStandY, fid::kStandZ));
    play(p);
    return p;
}

inline BunchWriter retail_oak_start() {
    for (const capture_foliage::Start &s : capture_foliage::kStarts) {
        if (s.capture != 1428 || s.packet != 53) continue;
        const std::vector<uint8_t> bytes = tsupport::bits_of_hex(s.field.hex, s.field.bits).bytes();
        BunchWriter payload;
        payload.write_bit_range(bytes.data(), 0, s.field.bits);
        BunchWriter out;
        repl::write_content_block(out, repl::BlockHeader{}, payload,
                                  /*server_form=*/false);
        return out;
    }
    return BunchWriter();
}

inline BunchWriter joined(std::initializer_list<const BunchWriter *> parts) {
    BunchWriter out;
    for (const BunchWriter *p : parts) out.write_bit_range(p->data(), 0, p->bit_pos());
    return out;
}

using ClientName = std::pair<std::string, int32_t>;

inline std::optional<ClientName> client_name(const repl::Value *v) {
    const repl::NameValue *n = v ? v->get_if<repl::NameValue>() : nullptr;
    if (!n || n->hardcoded) return std::nullopt;
    return ClientName{n->text, n->number};
}

inline ClientName client_fname(const std::string &s) {
    const size_t us = s.rfind('_');
    if (us == std::string::npos || us == 0 || us + 1 >= s.size()) return {s, 0};
    const std::string tail = s.substr(us + 1);
    const bool bad_len = tail.size() > 9 || (tail.size() > 1 && tail[0] == '0');
    if (bad_len || tail.find_first_not_of("0123456789") != std::string::npos) return {s, 0};
    return {s.substr(0, us), std::stoi(tail) + 1};
}

struct ClientFoliage {
    struct Cell {
        std::map<ClientName, bool> components;
        std::map<ClientName, std::map<uint32_t, uint32_t>> states;
    };
    struct Open {
        size_t chain = 0;
        ClientName name;
        std::vector<tsupport::Bits> blocks;
        size_t with_states = 0;
        std::vector<std::pair<uint32_t, uint32_t>> changed;
        size_t deleted = 0;
        int channel = 0;
    };
    std::map<ClientName, Cell> lookup;
    std::map<uint32_t, ClientName> channels;
    std::map<uint64_t, std::pair<ClientName, ClientName>> object_names;
    std::vector<Open> opens;
    size_t closes_dormancy = 0, closes_destroyed = 0, closes_relevancy = 0, closes_other = 0;
    size_t updates = 0;

    void absorb_comp_fast_array(const repl::FastArrayField &f, uint64_t obj_id, Cell &cell, Open &open) {
        open.deleted += f.delta.deleted.size();
        const auto known = object_names.find(obj_id);
        for (const auto &item : f.delta.changed) {
            const auto s1 = item.values.values.find(1);
            const auto key = item.values.values.find(2);
            if (s1 == item.values.values.end() || key == item.values.values.end()) continue;
            if (!s1->second.get_if<uint64_t>() || !key->second.get_if<uint64_t>()) continue;
            const uint32_t idx = static_cast<uint32_t>(*key->second.get_if<uint64_t>());
            const uint32_t st = static_cast<uint32_t>(*s1->second.get_if<uint64_t>());
            open.changed.emplace_back(idx, st);
            if (known != object_names.end()) cell.states[known->second.second][idx] = st;
        }
    }

    bool absorb_comp_rep(const fake::Delivered &d, const repl::BlockView &b, size_t &p2, Cell &cell, const Open &open) {
        if (!b.header.has_rep_layout) return true;
        const schema::ClassSchema &comp = class_named("UIntrepidFoliageStateComponent");
        const auto c = repl::read_rep_block(comp, d.content.data(), b.payload_pos + b.payload_bits, p2);
        if (!c) return false;
        const auto fismc = client_name(c->get(comp, "FoliageComponentUUID"));
        const repl::Value *init = c->get(comp, "bInitialized");
        if (fismc) {
            cell.components[*fismc] = init && init->get_if<bool>() && *init->get_if<bool>();
            object_names[b.header.object.ObjectId] = {open.name, *fismc};
        }
        return true;
    }

    void absorb_comp_block(const fake::Delivered &d, const repl::BlockView &b, Cell &cell, Open &open) {
        if (b.header.is_actor) return;
        size_t p2 = b.payload_pos;
        const size_t end = b.payload_pos + b.payload_bits;
        if (!absorb_comp_rep(d, b, p2, cell, open) || p2 >= end) return;
        const schema::ClassSchema &comp = class_named("UIntrepidFoliageStateComponent");
        const auto fields = repl::read_fast_array_fields(comp, d.content.data(), end, p2);
        if (!fields) return;
        ++open.with_states;
        for (const repl::FastArrayField &f : *fields) absorb_comp_fast_array(f, b.header.object.ObjectId, cell, open);
    }

    void absorb_open(size_t chain_idx, const fake::Delivered &d) {
        channels.erase(d.channel);
        const size_t prefix = new_actor_prefix_bits(d.content);
        const auto blocks =
            prefix ? repl::read_content_blocks(d.content.data(), d.content.bit_pos(), prefix) : std::nullopt;
        if (!blocks || blocks->empty() || !(*blocks)[0].header.is_actor || !(*blocks)[0].header.has_rep_layout) return;
        const schema::ClassSchema &ifa = class_named("AIntrepidStateIFA");
        size_t pos = (*blocks)[0].payload_pos;
        const auto st =
            repl::read_rep_block(ifa, d.content.data(), (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, pos);
        const auto name = st ? client_name(st->get(ifa, "FoliageName")) : std::nullopt;
        if (!name) return;
        channels[d.channel] = *name;
        Open open;
        open.chain = chain_idx;
        open.name = *name;
        open.channel = static_cast<int>(d.channel);
        const tsupport::Bits all(d.content.bytes(), d.content.bit_pos());
        for (const auto &b : *blocks) open.blocks.push_back(all.slice(b.payload_pos, b.payload_bits));
        Cell &cell = lookup[*name];
        for (size_t k = 1; k < blocks->size(); ++k) absorb_comp_block(d, (*blocks)[k], cell, open);
        opens.push_back(std::move(open));
    }

    void absorb_close(const fake::Delivered &d) {
        const auto held = channels.find(d.channel);
        if (held == channels.end()) return;
        switch (d.close_reason) {
        case 1:
            ++closes_dormancy;
            break;
        case 0:
            ++closes_destroyed;
            lookup.erase(held->second);
            break;
        case 3:
            ++closes_relevancy;
            lookup.erase(held->second);
            break;
        default:
            ++closes_other;
            lookup.erase(held->second);
            break;
        }
        channels.erase(held);
    }

    void absorb(const Player &p, size_t from) {
        for (size_t i = from; i < p.got.size(); ++i) {
            const fake::Delivered &d = p.got[i];
            if (d.open) {
                absorb_open(i, d);
            } else if (!d.close && channels.count(d.channel)) {
                ++updates;
            }
            if (d.close) absorb_close(d);
        }
    }

    bool resolves(const std::string &ifa, const std::string &fismc) const {
        const auto cell = lookup.find(client_fname(ifa));
        if (cell == lookup.end()) return false;
        const auto comp = cell->second.components.find(client_fname(fismc));
        return comp != cell->second.components.end() && comp->second;
    }
    size_t closes() const { return closes_dormancy + closes_destroyed + closes_relevancy + closes_other; }
};

struct Spot {
    const char *what;
    double x, y, z;
    const char *def;
};

inline std::optional<uint32_t> nearest_row(const foliage::FoliageMap &map, const Spot &s, double *distance) {
    std::optional<uint32_t> best;
    double least = 1e18;
    for (uint32_t r = 0; r < map.rows().size(); ++r) {
        const auto &row = map.rows()[r];
        if (map.def_names()[row.def] != s.def) continue;
        const double d = std::hypot(row.x - s.x, row.y - s.y);
        if (d < least) {
            least = d;
            best = r;
        }
    }
    *distance = least;
    return best;
}

inline bool client_resolves(const ClientFoliage &client, const foliage::FoliageMap &map, uint32_t row) {
    const auto &r = map.rows()[row];
    return client.resolves(map.owners()[r.owner].name, map.owners()[r.owner].components[r.component]);
}

inline bool init_foliage_session_env() {
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    const char *path = db_path();
    if (!path) {
        std::printf("[skip] test_worldd_foliage_session: no content.db (set "
                    "AOC_CONTENT_DB, or build "
                    "dist/content/content.db)\n");
        return false;
    }
#ifdef FOLIAGE_TSV_PATH
    if (fs::exists(FOLIAGE_TSV_PATH)) g_tsv = FOLIAGE_TSV_PATH;
#endif
    if (const char *env = std::getenv("AOC_FOLIAGE_MAP"); env && *env) g_tsv = env;
    if (!g_tsv) {
        std::printf("[skip] test_worldd_foliage_session: no "
                    "dist/content/region_foliage.tsv\n");
        return false;
    }
    g_path = path;
    std::printf("content.db: %s\nmap: %s\n", path, g_tsv);
    return true;
}
