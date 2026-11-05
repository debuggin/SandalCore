// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "capture_leveling.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/leveling.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/skill_rpc.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <map>
#include <optional>
#include <set>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using namespace tsupport;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
namespace cap = capture_leveling;
using schema::PropType;
using schema::RepCommand;

static Bits bits_of(const cap::Text& hex, size_t n) { return bits_of_hex(hex.str().c_str(), n); }
static const schema::RpcSchema& rpc_of(const char* cls, const char* name) {
    const schema::RpcSchema* r = schema_named(cls).rpc(name);
    if (!r) { std::printf("  [FAIL] no RPC %s on %s\n", name, cls); std::exit(2); }
    return *r;
}
static CodecOptions stat_options() {
    CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}
static std::optional<RpcCall> read_call(const schema::RpcSchema& rpc, const Bits& bits, const CodecOptions& opts = {}) {
    return with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) -> std::optional<RpcCall> {
        (void)bytes; size_t pos = 0; auto p = read_rpc_params(rpc, d, n, pos, opts);
        return p ? std::optional<RpcCall>(RpcCall{&rpc, std::move(*p), true, 0, n}) : std::nullopt;
    });
}
static std::optional<Bits> write_call(const schema::RpcSchema& rpc, const RpcParams& params) {
    BunchWriter w; bool failed = false; write_rpc_params(rpc, params, w, {}, &failed);
    return failed ? std::nullopt : std::optional<Bits>(Bits(w));
}
static std::string name_text(const NameValue& n) {
    if (n.hardcoded) return "EName(" + num(n.index) + ")";
    return n.number == 0 ? n.text : n.text + "_" + snum(n.number - 1);
}
static std::string id_text(const DesignDataId& d) { return snum(d.guid) + "." + snum(d.type_id) + "." + name_text(d.name); }
static std::string value_text(const RepCommand& cmd, std::span<const RepCommand> table, const Value& v);
static std::string item_text(std::span<const RepCommand> cmds, std::span<const RepCommand> table, const ElementState& es) {
    std::vector<std::string> parts;
    for (size_t i = 0; i < cmds.size(); ++i) {
        const auto it = es.values.find(static_cast<uint16_t>(i + 1));
        parts.push_back(std::string(cmds[i].path) + "=" + (it == es.values.end() ? std::string("?") : value_text(cmds[i], table, it->second)));
    }
    return "{" + join(parts, ";") + "}";
}
static std::string value_text(const RepCommand& cmd, std::span<const RepCommand> table, const Value& v) {
    switch (cmd.type) {
        case PropType::Bool: return v.get_if<bool>() ? num(*v.get_if<bool>()) : "?";
        case PropType::Int8: case PropType::Int16: case PropType::Int32: case PropType::Int64: return v.get_if<int64_t>() ? snum(*v.get_if<int64_t>()) : "?";
        case PropType::UInt8: case PropType::UInt16: case PropType::UInt32: case PropType::UInt64: case PropType::Enum: return v.get_if<uint64_t>() ? num(*v.get_if<uint64_t>()) : "?";
        case PropType::Float: return v.get_if<float>() ? hexn(std::bit_cast<uint32_t>(*v.get_if<float>()), 8) : "?";
        case PropType::Double: return v.get_if<double>() ? hexn(std::bit_cast<uint64_t>(*v.get_if<double>()), 16) : "?";
        case PropType::Name: return v.get_if<NameValue>() ? name_text(*v.get_if<NameValue>()) : "?";
        case PropType::Str: return v.get_if<std::string>() ? *v.get_if<std::string>() : "?";
        case PropType::DynamicArray: {
            const ArrayValue* a = v.get_if<ArrayValue>();
            if (!a || cmd.elem_end > table.size()) return "?";
            const auto sub = table.subspan(cmd.elem_begin, cmd.elem_end - cmd.elem_begin);
            std::vector<std::string> parts;
            for (const ElementState& e : a->elements) parts.push_back(item_text(sub, table, e));
            return "[" + join(parts, ",") + "]";
        }
        default: return "?";
    }
}
static std::string dump_fast_field(const schema::ClassSchema& schema, const FastArrayField& f) {
    const auto layout = fast_array_layout(schema, f.field);
    std::vector<std::string> del, chg;
    for (const int32_t id : f.delta.deleted) del.push_back(snum(id));
    if (layout) for (const auto& c : f.delta.changed) chg.push_back(snum(c.id) + ":" + item_text(layout->element, layout->table, c.values));
    return std::string(schema.field(f.field)->name) + " key=" + snum(f.delta.array_key) + " base=" + snum(f.delta.base_key) + " del=[" + join(del, ",") + "] chg=[" + join(chg, ",") + "]";
}
static std::string dump_fast_fields(const schema::ClassSchema& schema, const std::vector<FastArrayField>& fields) {
    std::vector<std::string> parts;
    for (const FastArrayField& f : fields) parts.push_back(dump_fast_field(schema, f));
    return join(parts, " || ");
}
static std::string dump_rep_state(const schema::ClassSchema& schema, const RepState& state) {
    std::vector<std::string> parts;
    for (const auto& [handle, value] : state.values()) {
        const RepCommand* cmd = schema.command(handle);
        const ArrayValue* a = value.get_if<ArrayValue>();
        if (!cmd || !a || cmd->elem_end > schema.element_commands.size()) return "?";
        const auto sub = schema.element_commands.subspan(cmd->elem_begin, cmd->elem_end - cmd->elem_begin);
        std::vector<std::string> items;
        for (size_t i = 0; i < a->elements.size(); ++i) {
            if (a->elements[i].values.empty()) continue;
            std::vector<std::string> members;
            for (const auto& [local, v] : a->elements[i].values) {
                if (local < 1 || local > sub.size()) return "?";
                members.push_back(std::string(sub[local - 1].path) + "=" + value_text(sub[local - 1], schema.element_commands, v));
            }
            items.push_back(num(i) + ":{" + join(members, ";") + "}");
        }
        parts.push_back(std::string(cmd->path) + "=n" + num(a->elements.size()) + "{" + join(items, ",") + "}");
    }
    return join(parts, " ");
}
static std::string dump_build(const SkillBuild& b) {
    std::vector<std::string> parts;
    for (const SkillRankChange& c : b.changes) parts.push_back(id_text(c.rank) + "|" + id_text(c.tree) + "|" + id_text(c.leveling_type) + "|" + num(c.purchase));
    return "changes=" + num(b.changes.size()) + " [" + join(parts, ";") + "]";
}
static std::string dump_update(const SkillOnActionbar& u) {
    return "skill=" + snum(u.skill_id) + " tree=" + snum(u.skill_tree_id) + " rank=" + snum(u.rank_id) + " refund=" + num(u.is_refunding) + " hint=" + num(u.use_hint);
}
static std::string dump_use(const ActionbarUseData& u) {
    return num(u.use_type) + ":" + num(u.instigator) + ":" + snum(u.ability_id) + ":" + snum(u.skill_rank_id) + ":" + snum(u.pet_command_id) +
           ":" + snum(u.item_record_id) + ":" + hexn(u.item_guid.a, 8) + hexn(u.item_guid.b, 8) + hexn(u.item_guid.c, 8) +
           hexn(u.item_guid.d, 8) + ":" + num(u.item_rarity) + ":" + snum(u.item_quality) + ":" + num(u.combo_button);
}
static std::string dump_payload(const ActionbarPayload& p) {
    std::vector<std::string> parts;
    for (const ActionbarSlot& s : p.slots) parts.push_back(snum(s.bank_index) + ":" + snum(s.slot_index) + ":" + dump_use(s.use));
    return "auto=" + num(p.auto_granted) + " slots=" + num(p.slots.size()) + " [" + join(parts, ";") + "]";
}
static std::string dump_set_slot(const SetActionbarSlot& s) { return "bank=" + snum(s.bank_index) + " slot=" + snum(s.slot_index) + " use=" + dump_use(s.use); }
static std::optional<std::vector<FastArrayField>> read_fields(const schema::ClassSchema& s, const Bits& bits, const CodecOptions& opts) {
    return with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) { (void)bytes; size_t pos = 0; return read_fast_array_fields(s, d, n, pos, opts); });
}
static std::optional<Bits> rewrite_fields(const schema::ClassSchema& s, const std::vector<FastArrayField>& fields, const CodecOptions& opts) {
    FastArrays arrays; FastArrayShadows shadows;
    for (const FastArrayField& f : fields) {
        FastArrayState* st = arrays.get(s, f.field);
        if (!st || f.delta.array_key == f.delta.base_key) return std::nullopt;
        FastArrayShadow& sh = shadows.by_field[f.field];
        sh.array_key = f.delta.base_key;
        for (const auto& c : f.delta.changed)
            if (std::find(f.delta.deleted.begin(), f.delta.deleted.end(), c.id) != f.delta.deleted.end()) return std::nullopt;
        for (const int32_t id : f.delta.deleted) sh.items.emplace_back(id, 0);
        for (const auto& c : f.delta.changed)
            if (!st->insert(c.id, c.values) || !st->change(c.id, c.values)) return std::nullopt;
        st->set_array_key(f.delta.array_key);
    }
    BunchWriter w; bool failed = false;
    write_fast_array_fields(s, arrays, shadows, w, opts, &failed);
    return failed ? std::nullopt : std::optional<Bits>(Bits(w));
}
static std::optional<RepState> read_state(const schema::ClassSchema& s, const Bits& bits, size_t* end = nullptr) {
    return with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) {
        (void)bytes; size_t pos = 0; auto st = read_rep_block(s, d, n, pos, stat_options());
        if (end) *end = pos;
        return st;
    });
}
static std::optional<Bits> rewrite_state(const schema::ClassSchema& s, const RepState& now) {
    RepShadow shadow;
    for (const auto& [handle, value] : now.values()) {
        const ArrayValue* a = value.get_if<ArrayValue>();
        if (!a) return std::nullopt;
        ArrayValue old; old.elements.resize(a->elements.size() + 1);
        shadow.sent[handle] = Value(std::move(old));
    }
    BunchWriter w; bool failed = false;
    write_rep_block(s, now, shadow, w, stat_options(), &failed);
    return failed ? std::nullopt : std::optional<Bits>(Bits(w));
}
struct Block {
    std::optional<std::string> dump;
    std::optional<Bits> again;
    std::vector<FastArrayField> fields;
    std::optional<RepState> state;
    std::vector<RpcCall> calls;
    std::vector<SkillOnActionbar> updates;
};
static bool read_block(const cap::Block& b, Block& out, std::string* why) {
    const schema::ClassSchema& s = schema_named(b.cls);
    const Bits bits = bits_of(b.hex, b.bits);
    if (b.kind == cap::kFast) {
        auto fields = read_fields(s, bits, stat_options());
        if (!fields) { *why = "the field loop does not read"; return false; }
        out.fields = std::move(*fields); out.dump = dump_fast_fields(s, out.fields); out.again = rewrite_fields(s, out.fields, stat_options());
        return true;
    }
    if (b.kind == cap::kRep) {
        size_t end = 0; out.state = read_state(s, bits, &end);
        if (!out.state || end != bits.size()) { *why = "the RepLayout block does not read to its last bit"; return false; }
        out.dump = dump_rep_state(s, *out.state); out.again = rewrite_state(s, *out.state);
        return true;
    }
    if (b.kind == cap::kRpc) {
        auto calls = with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) { (void)bytes; size_t pos = 0; return read_rpc_fields(s, d, n, pos); });
        if (!calls) { *why = "the RPC field loop does not read"; return false; }
        std::vector<std::string> parts; BunchWriter w;
        for (const RpcCall& c : *calls) {
            const auto u = parse_update_skill_on_actionbar(c);
            if (!u) { *why = "an RPC is not a ClientUpdateSkillOnActionbar"; return false; }
            parts.push_back(dump_update(*u)); out.updates.push_back(*u);
            const auto built = update_skill_on_actionbar_params(*c.rpc, *u);
            if (!built || !(*built == c.params)) { *why = "the typed update builds other parameters"; return false; }
            bool failed = false; write_rpc_field(s, *c.rpc, c.params, w, {}, &failed);
            if (failed) { *why = "an RPC does not write"; return false; }
        }
        out.calls = std::move(*calls); out.dump = join(parts, " || "); out.again = Bits(w);
        return true;
    }
    *why = "unknown kind";
    return false;
}
static bool check_block(const cap::Block& b, Block& out, std::string* why) {
    if (!read_block(b, out, why)) return false;
    if (*out.dump != b.dump.str()) { *why = "dump differs:\n    " + *out.dump + "\n    " + b.dump.str(); return false; }
    if (!out.again || !(*out.again == bits_of(b.hex, b.bits))) { *why = "does not write back the same bits"; return false; }
    return true;
}
static bool all_first_sends(const std::vector<FastArrayField>& fields) {
    return std::all_of(fields.begin(), fields.end(), [](const FastArrayField& f) { return f.delta.base_key == 0 && f.delta.deleted.empty(); });
}

static void test_captured_blocks() {
    std::printf("captured XPTracker blocks: read, applied, written back bit for bit\n");
    size_t bad = 0, firsts = 0, typed_first = 0, updates = 0, trees = 0, max_ranks = 0;
    std::string first_why;
    std::set<std::string> map_names;
    for (const cap::Block& b : cap::kBlocks) {
        Block r;
        std::string why;
        if (!check_block(b, r, &why)) { ++bad; if (first_why.empty()) first_why = std::string(b.source) + ": " + why; continue; }
        const schema::ClassSchema& s = schema_named(b.cls);
        LevelingState ls;
        bool applied = true;
        for (const FastArrayField& f : r.fields) { applied = applied && ls.apply(f); map_names.insert(s.field(f.field)->name); }
        if (!applied) { ++bad; if (first_why.empty()) first_why = std::string(b.source) + ": a field does not apply"; continue; }
        for (const SkillTreePurchase& p : ls.purchases()) { ++trees; max_ranks = std::max(max_ranks, p.ranks.size()); }
        if (all_first_sends(r.fields)) {
            ++firsts;
            FastArrayShadows shadows;
            BunchWriter w;
            bool failed = false;
            ls.write(w, shadows, {}, &failed);
            if (!failed && Bits(w) == bits_of(b.hex, b.bits)) ++typed_first;
            else if (first_why.empty()) first_why = std::string(b.source) + ": the typed state does not write the first send back";
        } else ++updates;
    }
    CHECK(bad == 0, "every captured block reads to its last bit, applies, says what the Python reader says and writes back the same bits");
    if (!first_why.empty()) std::printf("    first: %s\n", first_why.c_str());
    CHECK(firsts == typed_first, "every first send (key against base 0) is written from the LevelingState's own items to the same bits");
    char msg[200];
    std::snprintf(msg, sizeof msg, "the sample: %zu blocks, %zu first sends and %zu updates; %zu tree lists, the longest %zu ranks", std::size(cap::kBlocks), firsts, updates, trees, max_ranks);
    CHECK(firsts >= 1 && updates >= 5 && max_ranks >= 25, msg);
    CHECK(map_names.count("ExperienceFastMap") && map_names.count("ExperienceDebtFastMap") && map_names.count("LevelingCertificationsFastMap") &&
              map_names.count("SkillTreePurchasesFastMap") && !map_names.count("SkillPointFastMap"),
          "four of the five maps occur; SkillPointFastMap never does (the client derives its skill points)");
}

struct CaseStats {
    size_t bad = 0, replies = 0, respecs = 0, swaps = 0, updates_total = 0, purchases_total = 0, rep_blocks = 0, effect_blocks = 0, tree_checks = 0;
    std::string first_why;
    void fail(const cap::Case& c, const std::string& why) { ++bad; if (first_why.empty()) first_why = std::string(c.name) + " (" + c.source + "): " + why; }
};
static void verify_case_replies(const cap::Case& c, const SkillBuild& build, CaseStats& st) {
    size_t updates = 0;
    std::optional<std::vector<DesignDataId>> ranks;
    bool tree_seen = false;
    for (size_t i = 0; i < c.reply_count; ++i) {
        const cap::Block& b = cap::kReplyBlocks[c.first_reply + i];
        Block r;
        std::string why;
        if (!check_block(b, r, &why)) { st.fail(c, std::string(b.source) + " " + b.cls + ": " + why); continue; }
        ++st.replies;
        st.rep_blocks += b.kind == cap::kRep ? 1 : 0;
        if (std::string(b.cls) == "UAoCStatsComponent") ++st.effect_blocks;
        for (const SkillOnActionbar& u : r.updates) {
            ++updates;
            if (u.skill_tree_id != build.changes[0].tree.guid || u.is_refunding || !u.use_hint || u.rank_id != 0)
                st.fail(c, "a ClientUpdateSkillOnActionbar that does not match the request's tree");
        }
        if (std::string(b.cls) == "ULevelingComponent") {
            LevelingState ls;
            for (const FastArrayField& f : r.fields) ls.apply(f);
            ranks = ls.purchased_ranks(build.changes[0].tree);
            tree_seen = tree_seen || ranks.has_value();
        }
    }
    st.updates_total += updates;
    if (updates != build.purchases()) st.fail(c, "ClientUpdateSkillOnActionbar calls: " + std::to_string(updates) + " for " + std::to_string(build.purchases()) + " purchases");
    if (tree_seen) {
        ++st.tree_checks;
        for (const SkillRankChange& ch : build.changes) {
            const bool has = ranks && std::find(ranks->begin(), ranks->end(), ch.rank) != ranks->end();
            if (has != ch.purchase) st.fail(c, ch.purchase ? "a bought rank is not in the tree's list" : "a refunded rank is still in the tree's list");
        }
    }
}
static void test_captured_cases() {
    std::printf("captured purchases, refunds, swaps and respecs, the request and the whole answer\n");
    const schema::RpcSchema& rpc = rpc_of("PlayerPawn_C", "ServerPurchaseSkillBuild");
    CaseStats st;
    for (const cap::Case& c : cap::kCases) {
        const Bits bits = bits_of(c.request_hex, c.request_bits);
        const auto call = read_call(rpc, bits);
        const auto build = call ? parse_purchase_skill_build(*call) : std::nullopt;
        if (!build) { st.fail(c, "the request does not read"); continue; }
        if (dump_build(*build) != c.request_dump.str()) { st.fail(c, "request dump differs:\n    " + dump_build(*build) + "\n    " + c.request_dump.str()); continue; }
        const auto again = write_call(rpc, call->params);
        const auto built = purchase_skill_build_params(rpc, *build);
        if (!again || !(*again == bits)) { st.fail(c, "the request does not write back the same bits"); continue; }
        if (!built || !(*built == call->params)) { st.fail(c, "the typed request builds other parameters"); continue; }
        st.respecs += build->refund_only() && build->changes.size() > 1 ? 1 : 0;
        st.swaps += build->purchases() > 0 && build->refunds() > 0 ? 1 : 0;
        st.purchases_total += build->purchases();
        verify_case_replies(c, *build, st);
    }
    CHECK(st.bad == 0, "every request and reply block reads, parses, writes back the same bits and says what the Python reader says; each answer fits its request");
    if (!st.first_why.empty()) std::printf("    first: %s\n", st.first_why.c_str());
    char msg[240];
    std::snprintf(msg, sizeof msg, "%zu cases: %zu reply blocks, %zu purchases answered by %zu ClientUpdateSkillOnActionbar, %zu respecs, %zu swap, %zu tree lists checked",
                  std::size(cap::kCases), st.replies, st.purchases_total, st.updates_total, st.respecs, st.swaps, st.tree_checks);
    CHECK(st.replies == std::size(cap::kReplyBlocks) && st.updates_total == st.purchases_total && st.respecs >= 5 && st.swaps >= 1, msg);
    std::printf("    %zu RepLayout blocks and %zu StatsComponent blocks in the answers\n", st.rep_blocks, st.effect_blocks);
    CHECK(st.rep_blocks == 9 && st.effect_blocks == 8, "the answers include RepLayout array updates of the AbilityComponent and CurrentEffects items of the StatsComponent (A7's writers' forms)");
}

static void test_actionbar_payloads() {
    const schema::RpcSchema& rpc = rpc_of("AoCPlayerControllerBP_C", "ClientReceiveActionbarSlotPayload");
    size_t bad = 0, max_slots = 0, auto_granted = 0;
    std::set<int32_t> banks;
    std::set<unsigned> types, instigators;
    std::string first_why;
    for (const cap::Call& c : cap::kPayloads) {
        const Bits bits = bits_of(c.hex, c.bits);
        std::string why;
        const auto call = read_call(rpc, bits);
        const auto p = call ? parse_actionbar_payload(*call) : std::nullopt;
        if (!p) why = "does not read or parse";
        else if (dump_payload(*p) != c.dump.str()) why = "dump differs";
        else if (const auto again = write_call(rpc, call->params); !again || !(*again == bits)) why = "does not write back the same bits";
        else if (const auto built = actionbar_payload_params(rpc, *p); !built || !(*built == call->params)) why = "the typed form builds other parameters";
        if (!why.empty()) { ++bad; if (first_why.empty()) first_why = std::string(c.source) + ": " + why; continue; }
        max_slots = std::max(max_slots, p->slots.size());
        auto_granted += p->auto_granted;
        for (const ActionbarSlot& s : p->slots) { banks.insert(s.bank_index); types.insert(s.use.use_type); instigators.insert(s.use.instigator); }
    }
    CHECK(bad == 0, "every captured payload reads, parses, builds again and writes back the same bits, and says what the Python reader says");
    if (bad) std::printf("    first: %s\n", first_why.c_str());
    CHECK(max_slots == 494 && auto_granted >= 1 && banks.count(0) && banks.count(4) && banks.count(16) && banks.count(19) && types.count(ActionbarUseType::Ability) &&
              types.count(ActionbarUseType::Skill) && types.count(ActionbarUseType::Item) && types.count(ActionbarUseType::SwapWeapons) &&
              types.count(ActionbarUseType::PetCommand) && types.count(ActionbarUseType::Dismount) && instigators.count(ActionbarInstigator::Mount),
          "the sample reaches 494 slots, banks 0 to 19, every use type of the bar and the mount instigator, and an auto-granted slot");
}
static void test_actionbar_set_slots_and_emit() {
    const schema::RpcSchema& rpc = rpc_of("AoCPlayerControllerBP_C", "ServerSetActionbarSlotUseData");
    size_t bad = 0, cleared = 0;
    std::string first_why;
    for (const cap::Call& c : cap::kSetSlots) {
        const Bits bits = bits_of(c.hex, c.bits);
        std::string why;
        const auto call = read_call(rpc, bits);
        const auto s = call ? parse_set_actionbar_slot(*call) : std::nullopt;
        if (!s) why = "does not read or parse";
        else if (dump_set_slot(*s) != c.dump.str()) why = "dump differs";
        else if (const auto again = write_call(rpc, call->params); !again || !(*again == bits)) why = "does not write back the same bits";
        else if (const auto built = set_actionbar_slot_params(rpc, *s); !built || !(*built == call->params)) why = "the typed form builds other parameters";
        if (!why.empty()) { ++bad; if (first_why.empty()) first_why = std::string(c.source) + ": " + why; continue; }
        cleared += s->use.use_type == ActionbarUseType::None ? 1 : 0;
    }
    CHECK(bad == 0, "all 6 ServerSetActionbarSlotUseData calls read, parse, build again and write back the same bits");
    if (bad) std::printf("    first: %s\n", first_why.c_str());
    CHECK(cleared == 1 && std::size(cap::kSetSlots) == 6, "one of them clears a slot (UseType 0); a bank or slot of 0 is not on the wire");
    const schema::ClassSchema& pc_schema = schema_named("AoCPlayerControllerBP_C");
    Actor pc;
    pc.guid = FIntrepidNetworkGUID{588076, 60, 0};
    pc.obj.schema = &pc_schema;
    ExportAcks acks;
    ActionbarPayload payload{{{0, 0, {ActionbarUseType::Skill, 0, 0, 6064629978367333452ll, 0, 0, {}, 0, 0, 0}},
                              {4, 1, {ActionbarUseType::Ability, 0, 6064632582576340992ll, 0, 0, 0, {}, 0, 0, 0}}}, true};
    const auto e = client_receive_actionbar_slot_payload(pc, payload, acks);
    CHECK(e && e->reliable, "ClientReceiveActionbarSlotPayload is reliable (function flags 0x01020cc0)");
    if (e) {
        const auto blocks = read_content_blocks(e->payload.data(), e->payload.bit_pos(), 0, true);
        size_t pos = blocks && !blocks->empty() ? (*blocks)[0].payload_pos : 0;
        const auto calls = blocks && !blocks->empty() ? read_rpc_fields(pc_schema, e->payload.data(), pos + (*blocks)[0].payload_bits, pos) : std::nullopt;
        CHECK(calls && calls->size() == 1 && parse_actionbar_payload((*calls)[0]) == payload, "two slots built by hand come back through the field loop equal");
    }
    SkillOnActionbar update{6064629978368971853ll, 108850029942393ll, 0, false, true};
    const auto eu = client_update_skill_on_actionbar(pc, update, acks);
    CHECK(eu && eu->reliable, "ClientUpdateSkillOnActionbar is reliable");
    if (eu) {
        const auto blocks = read_content_blocks(eu->payload.data(), eu->payload.bit_pos(), 0, true);
        size_t pos = blocks && !blocks->empty() ? (*blocks)[0].payload_pos : 0;
        const auto calls = blocks && !blocks->empty() ? read_rpc_fields(pc_schema, eu->payload.data(), pos + (*blocks)[0].payload_bits, pos) : std::nullopt;
        CHECK(calls && calls->size() == 1 && parse_update_skill_on_actionbar((*calls)[0]) == update, "and the skill update (133 bits: RankId left at its default) reads back equal");
        CHECK(blocks && blocks->size() == 1 && (*blocks)[0].header.is_actor, "in one block of the PlayerController");
    }
    Actor bare;
    bare.obj.schema = &schema_named("PlayerPawn_C");
    CHECK(!client_update_skill_on_actionbar(bare, update, acks) && !client_receive_actionbar_slot_payload(bare, payload, acks), "an actor whose class has neither RPC gets nothing");
}
static void test_actionbar() {
    std::printf("the action bar: payloads the server sent, slots the client set\n");
    test_actionbar_payloads();
    test_actionbar_set_slots_and_emit();
}

static DesignDataId adventuring() { return DesignDataId::named(108651085518458ll, 5949882024684912047ll, "Adventuring"); }
static DesignDataId weapon_line() { return DesignDataId::named(108651085518459ll, 5949882024684912047ll, "Weapon_Ranged_Longbow"); }
static DesignDataId tree(const char* name = "Ranger_SkillTable") { return DesignDataId::named(108850029942393ll, 297887639323616934ll, name); }
static DesignDataId rank(int64_t guid) { DesignDataId d; d.guid = guid; d.type_id = 4847957796014348780ll; return d; }
static std::optional<std::vector<FastArrayField>> read_written(const LevelingState& ls, const Bits& bits) { return read_fields(ls.schema(), bits, {}); }

static void test_leveling_state_erase_and_bounds(LevelingState& a, LevelingState& b, FastArrayShadows& shadows) {
    bool failed = false;
    CHECK(a.set_purchased_ranks(tree(), {rank(1)}) && a.erase_purchased_ranks(tree("Bard_SkillTable")) && !a.erase_purchased_ranks(tree("Bard_SkillTable")) &&
              a.erase_experience(weapon_line()) && !a.erase_experience(weapon_line()),
          "a list shrinks, a tree and an XP entry are erased (a second erase finds nothing)");
    BunchWriter w3; a.write(w3, shadows, {}, &failed);
    const auto f3 = read_written(a, Bits(w3));
    bool applied3 = f3.has_value() && !failed;
    size_t deleted = 0;
    if (f3) for (const FastArrayField& f : *f3) { deleted += f.delta.deleted.size(); applied3 = applied3 && b.apply(f); }
    CHECK(applied3 && deleted == 2 && b.purchases() == a.purchases() && b.experience() == a.experience() && b.purchases().size() == 1,
          "the deletes travel as the IDs of the items and the second state drops them");
    const auto before = a.purchases();
    CHECK(!a.set_purchased_ranks(tree(), std::vector<DesignDataId>(kMaxTreeRanks + 1, rank(5))) && a.purchases() == before, "a rank list over the bound is refused and nothing changes");
    CHECK(!a.apply(FastArrayField{0, {}}), "a field that is not one of the five maps does not apply");
    CHECK(!a.apply(FastArrayField{999, {}}), "nor does a field beyond the class");
    FastArrayShadows s2; LevelingState c; c.set_experience(adventuring(), 5);
    BunchWriter wc, wc2; c.write(wc, s2); c.set_experience(adventuring(), 5);
    CHECK(c.write(wc2, s2) == 0, "setting a value to what it already is changes nothing");
}
static void test_leveling_state() {
    std::printf("LevelingState: entries, deltas and a second state applying them\n");
    const std::vector<DesignDataId> r123 = {rank(1), rank(2), rank(3)}, r1234 = {rank(1), rank(2), rank(3), rank(4)}, r9 = {rank(9)};
    LevelingState a, b;
    CHECK(a.experience().empty() && a.purchases().empty() && !a.purchased_ranks(tree()), "a new state is empty");
    CHECK(a.set_experience(adventuring(), 7762172) && a.set_experience(weapon_line(), 1294402) && a.set_experience_debt(adventuring(), 0) &&
              a.set_certification(DesignDataId::named(1, 2, "Mining"), 108906339653240ll) && a.set_skill_points(adventuring(), 24) &&
              a.set_purchased_ranks(tree(), r123) && a.set_purchased_ranks(tree("Bard_SkillTable"), r9),
          "entries are set in each of the five maps");
    CHECK(a.experience().size() == 2 && a.experience(adventuring()) == 7762172 && a.experience(weapon_line()) == 1294402 && !a.experience(tree()) &&
              a.experience_debt(adventuring()) == 0 && a.skill_points(adventuring()) == 24 && a.certification(DesignDataId::named(1, 2, "Mining")) == 108906339653240ll &&
              a.purchased_ranks(tree()) == r123 && a.purchases().size() == 2,
          "and read back by key");
    FastArrayShadows shadows; BunchWriter w1, nothing, w2; bool failed = false;
    const size_t n1 = a.write(w1, shadows, {}, &failed);
    CHECK(n1 > 0 && !failed && n1 == w1.bit_pos(), "the first write sends all five maps");
    const auto f1 = read_written(a, Bits(w1));
    CHECK(f1 && f1->size() == 5, "five fields, ascending");
    bool applied = f1.has_value();
    if (f1) for (const FastArrayField& f : *f1) applied = applied && b.apply(f) && f.delta.base_key == 0;
    CHECK(applied && b.experience() == a.experience() && b.skill_points() == a.skill_points() && b.certifications() == a.certifications() &&
              b.purchases() == a.purchases() && b.experience_debt() == a.experience_debt(),
          "a second state applies them and holds the same entries in the same order");
    CHECK(a.write(nothing, shadows) == 0 && nothing.bit_pos() == 0, "a second write of the unchanged state sends nothing");
    CHECK(a.set_experience(adventuring(), 7762173) && a.set_purchased_ranks(tree(), r1234), "an XP value and a tree's list change");
    a.write(w2, shadows, {}, &failed);
    const auto f2 = read_written(a, Bits(w2));
    CHECK(!failed && f2 && f2->size() == 2 && (*f2)[0].delta.changed.size() == 1 && (*f2)[1].delta.changed.size() == 1 &&
              (*f2)[0].delta.base_key == (*f1)[0].delta.array_key && (*f2)[1].delta.base_key == (*f1)[4].delta.array_key,
          "the update has the XP map and the purchases, one item each, against the keys of the first send");
    bool applied2 = f2.has_value();
    if (f2) for (const FastArrayField& f : *f2) applied2 = applied2 && b.apply(f);
    CHECK(applied2 && b.experience(adventuring()) == 7762173 && b.purchased_ranks(tree()) == a.purchased_ranks(tree()) && b.purchases() == a.purchases(), "and the second state follows");
    test_leveling_state_erase_and_bounds(a, b, shadows);
}

static void test_resend_and_ranged() {
    std::printf("a tree list sent again and ServerSetUseRangedWeapon\n");
    const std::vector<DesignDataId> r12 = {rank(1), rank(2)};
    LevelingState a, b; a.set_purchased_ranks(tree(), r12);
    FastArrayShadows shadows; BunchWriter w1, quiet, w2;
    a.write(w1, shadows);
    const auto f1 = read_written(a, Bits(w1));
    CHECK(a.write(quiet, shadows) == 0, "(the first write is sent; the state is then quiet)");
    CHECK(a.set_purchased_ranks(tree(), r12) && a.write(quiet, shadows) == 0, "setting the same list again sends nothing");
    CHECK(a.resend_purchased_ranks(tree()) && !a.resend_purchased_ranks(tree("Bard_SkillTable")), "resend_purchased_ranks marks a tree that has an item and says no for one that has none");
    bool failed = false;
    CHECK(a.write(w2, shadows, {}, &failed) > 0 && !failed, "...and the next write carries it");
    const auto f2 = read_written(a, Bits(w2));
    bool applied = f1.has_value() && f2.has_value();
    if (f1) for (const FastArrayField& f : *f1) applied = applied && b.apply(f);
    CHECK(f2 && f2->size() == 1 && (*f2)[0].delta.changed.size() == 1 && (*f2)[0].delta.deleted.empty() && f1 && (*f2)[0].delta.base_key == (*f1)[0].delta.array_key && applied,
          "one item, changed (not added, not deleted), against the key of the first send");
    if (f2) for (const FastArrayField& f : *f2) applied = applied && b.apply(f);
    CHECK(applied && b.purchased_ranks(tree()) == r12, "a client that applies it holds the same list");
    const schema::RpcSchema& rpc = rpc_of("PlayerPawn_C", "ServerSetUseRangedWeapon");
    for (const bool ranged : {true, false}) {
        const auto params = set_use_ranged_weapon_params(rpc, ranged);
        const auto bits = params ? write_call(rpc, *params) : std::nullopt;
        const auto call = bits ? read_call(rpc, *bits) : std::nullopt;
        const auto back = call ? parse_set_use_ranged_weapon(*call) : std::nullopt;
        CHECK(bits && bits->size() > 0 && call && back == ranged, ranged ? "ServerSetUseRangedWeapon(true) writes and reads back" : "ServerSetUseRangedWeapon(false) writes and reads back");
    }
    const schema::RpcSchema& other_rpc = rpc_of("PlayerPawn_C", "ServerPurchaseSkillBuild");
    CHECK(!set_use_ranged_weapon_params(other_rpc, true), "the builder refuses another RPC's schema");
    CHECK(!parse_set_use_ranged_weapon(RpcCall{&other_rpc, {}, true, 0, 0}) && !parse_set_use_ranged_weapon(RpcCall{}), "the parser refuses another RPC and a call that was not decoded");
}

static void test_refusals() {
    std::printf("hostile input, a field at a time\n");
    const schema::RpcSchema& build_rpc = rpc_of("PlayerPawn_C", "ServerPurchaseSkillBuild");
    SkillBuild big{std::vector<SkillRankChange>(kMaxSkillBuildChanges + 1)};
    CHECK(!purchase_skill_build_params(build_rpc, big), "a build of 1,025 changes is not built");
    big.changes.resize(kMaxSkillBuildChanges);
    const auto ok = purchase_skill_build_params(build_rpc, big);
    CHECK(ok.has_value(), "1,024 are");
    if (ok) {
        RpcCall call{&build_rpc, *ok, true, 0, 0};
        CHECK(parse_purchase_skill_build(call) && parse_purchase_skill_build(call)->changes.size() == kMaxSkillBuildChanges, "and read back");
        const RepCommand* cmd = rpc_command(build_rpc, "BuildChanges.AllocationChanges");
        ArrayValue a; a.elements.resize(kMaxSkillBuildChanges + 1);
        const auto elems = build_rpc.element_commands.subspan(cmd->elem_begin, cmd->elem_end - cmd->elem_begin);
        for (ElementState& e : a.elements)
            for (size_t i = 0; i < elems.size(); ++i) {
                if (elems[i].type == PropType::Int64) e.values[static_cast<uint16_t>(i + 1)] = Value::i64(0);
                else if (elems[i].type == PropType::Name) e.values[static_cast<uint16_t>(i + 1)] = Value(NameValue::hardcoded_name(0));
                else if (elems[i].type == PropType::Bool) e.values[static_cast<uint16_t>(i + 1)] = Value::boolean(false);
            }
        call.params.values[cmd->handle] = Value(std::move(a));
        CHECK(!parse_purchase_skill_build(call), "a decoded call of 1,025 changes is refused by the parser");
    }
    const RpcCall none;
    CHECK(!parse_purchase_skill_build(none) && !parse_update_skill_on_actionbar(none) && !parse_actionbar_payload(none) && !parse_set_actionbar_slot(none), "an empty call parses as nothing");
    const schema::RpcSchema& payload_rpc = rpc_of("AoCPlayerControllerBP_C", "ClientReceiveActionbarSlotPayload");
    ActionbarPayload p{{{0, 0, {16, 0, 0, 0, 0, 0, {}, 0, 0, 0}}}, false};
    CHECK(!actionbar_payload_params(payload_rpc, p), "a use type of 16 does not fit its 4 bits");
    p.slots[0].use.use_type = 15;
    p.slots[0].use.instigator = 8;
    CHECK(!actionbar_payload_params(payload_rpc, p), "an instigator of 8 does not fit its 3 bits");
    p.slots[0].use.instigator = 7;
    p.slots[0].use.combo_button = 16;
    CHECK(!actionbar_payload_params(payload_rpc, p), "a combo button of 16 does not fit its 4 bits");
    p.slots[0].use.combo_button = 15;
    CHECK(actionbar_payload_params(payload_rpc, p).has_value(), "the largest of each do");
    p.slots.assign(kMaxActionbarSlots + 1, ActionbarSlot{});
    CHECK(!actionbar_payload_params(payload_rpc, p), "1,025 slots are not built");
    CHECK(!update_skill_on_actionbar_params(payload_rpc, SkillOnActionbar{}) && !purchase_skill_build_params(payload_rpc, SkillBuild{}) &&
              !set_actionbar_slot_params(payload_rpc, SetActionbarSlot{}) && !actionbar_payload_params(build_rpc, ActionbarPayload{}),
          "each builder refuses another RPC's schema entry");
}

enum class FuzzWhat { Fast, Rep, Rpc, Update, Build, Payload, SetSlot };
struct FuzzSeed { FuzzWhat what; const char* cls; const schema::RpcSchema* rpc; Bits bits; };
struct FuzzStats { size_t reads = 0, decoded = 0, rewritten = 0, same = 0, typed = 0; std::map<int, std::array<size_t, 3>> by_kind; };

static std::optional<std::string> fuzz_say_call(FuzzWhat what, const RpcCall& c) {
    if (what == FuzzWhat::Build) { const auto b = parse_purchase_skill_build(c); return b ? std::optional<std::string>(dump_build(*b)) : std::nullopt; }
    if (what == FuzzWhat::Payload) { const auto p = parse_actionbar_payload(c); return p ? std::optional<std::string>(dump_payload(*p)) : std::nullopt; }
    const auto u = parse_set_actionbar_slot(c);
    return u ? std::optional<std::string>(dump_set_slot(*u)) : std::nullopt;
}
static void fuzz_step_blocks(const FuzzSeed& s, const schema::ClassSchema& cls, const Bits& m, FuzzStats& st) {
    if (s.what == FuzzWhat::Fast) {
        const auto fields = read_fields(cls, m, stat_options());
        if (!fields) return;
        ++st.decoded;
        if (std::string(s.cls) == "ULevelingComponent") {
            LevelingState ls;
            bool all = true;
            for (const FastArrayField& f : *fields) all = all && ls.apply(f);
            if (all) { ++st.typed; (void)ls.purchases(); (void)ls.experience(); }
        }
        const auto out = rewrite_fields(cls, *fields, stat_options());
        if (!out) return;
        ++st.rewritten;
        const auto second = read_fields(cls, *out, stat_options());
        if (second && second->size() == fields->size() && dump_fast_fields(cls, *second) == dump_fast_fields(cls, *fields)) ++st.same;
        return;
    }
    size_t end = 0;
    const auto rep = read_state(cls, m, &end);
    if (!rep || end != m.size()) return;
    ++st.decoded;
    const auto out = rewrite_state(cls, *rep);
    if (!out) return;
    ++st.rewritten;
    if (const auto second = read_state(cls, *out); second && *second == *rep) ++st.same;
}
static void fuzz_step_rpcs(const FuzzSeed& s, const schema::ClassSchema& cls, const Bits& m, FuzzStats& st) {
    if (s.what == FuzzWhat::Update) {
        const auto calls = with_exact(m, [&](const uint8_t* d, size_t bytes, size_t n) { (void)bytes; size_t pos = 0; return read_rpc_fields(cls, d, n, pos); });
        if (!calls) return;
        ++st.decoded;
        BunchWriter w;
        bool failed = false;
        std::vector<std::optional<SkillOnActionbar>> first;
        for (const RpcCall& c : *calls) {
            if (!c.decoded) continue;
            first.push_back(parse_update_skill_on_actionbar(c));
            st.typed += first.back() ? 1 : 0;
            write_rpc_field(cls, *c.rpc, c.params, w, {}, &failed);
        }
        if (failed) return;
        ++st.rewritten;
        const Bits out(w);
        const auto second = with_exact(out, [&](const uint8_t* d, size_t bytes, size_t n) { (void)bytes; size_t pos = 0; return read_rpc_fields(cls, d, n, pos); });
        bool equal = second && second->size() == first.size();
        for (size_t i = 0; equal && i < first.size(); ++i) equal = (*second)[i].decoded && parse_update_skill_on_actionbar((*second)[i]) == first[i];
        if (equal) ++st.same;
        return;
    }
    const auto call = read_call(*s.rpc, m);
    if (!call) return;
    ++st.decoded;
    const auto said = fuzz_say_call(s.what, *call);
    if (said) {
        ++st.typed;
        std::optional<RpcParams> built;
        if (s.what == FuzzWhat::Build) built = purchase_skill_build_params(*s.rpc, *parse_purchase_skill_build(*call));
        else if (s.what == FuzzWhat::Payload) built = actionbar_payload_params(*s.rpc, *parse_actionbar_payload(*call));
        else built = set_actionbar_slot_params(*s.rpc, *parse_set_actionbar_slot(*call));
        RpcCall again = *call;
        again.params = built ? *built : RpcParams{};
        if (!built || fuzz_say_call(s.what, again) != said) std::printf("    a call builds other parameters\n");
    }
    if (const auto out = write_call(*s.rpc, call->params)) {
        ++st.rewritten;
        if (const auto second = read_call(*s.rpc, *out); second && fuzz_say_call(s.what, *second) == said) ++st.same;
    }
}
static void fuzz_run_one(const FuzzSeed& s, const Bits& m, FuzzStats& st) {
    ++st.reads;
    const size_t d0 = st.decoded, r0 = st.rewritten, s0 = st.same;
    const schema::ClassSchema& cls = schema_named(s.cls);
    if (s.what == FuzzWhat::Fast || s.what == FuzzWhat::Rep) fuzz_step_blocks(s, cls, m, st);
    else fuzz_step_rpcs(s, cls, m, st);
    auto& bk = st.by_kind[static_cast<int>(s.what)];
    bk[0] += st.decoded - d0;
    bk[1] += st.rewritten - r0;
    bk[2] += st.same - s0;
}
static void fuzz_mutate_seed(const FuzzSeed& s, Rng& rng, FuzzStats& st) {
    const size_t n = s.bits.size();
    if (n == 0) return;
    const int scale = n > 100000 ? 4 : (n > 20000 ? 12 : 40);
    fuzz_run_one(s, s.bits, st);
    for (int round = 0; round < scale * 2; ++round) {
        Bits m = s.bits;
        const int flips = 1 + static_cast<int>(rng.below(4));
        for (int i = 0; i < flips; ++i) { const size_t at = rng.below(n); m.b[at] = !m.b[at]; }
        fuzz_run_one(s, m, st);
    }
    for (int round = 0; round < scale; ++round) fuzz_run_one(s, s.bits.slice(0, 1 + rng.below(n)), st);
    for (int round = 0; round < scale; ++round) {
        Bits m = s.bits;
        for (size_t bit = rng.below(n); bit < n; ++bit) if (rng.next() & 1) m.b[bit] = !m.b[bit];
        fuzz_run_one(s, m, st);
    }
    for (int round = 0; round < scale; ++round) {
        static const uint64_t words[] = {0, 1, 2, 0xff, 0xffff, 0x7fffffff, 0x80000000u, 0xffffffffu, 0xffffffffffffffffull};
        Bits m = s.bits;
        const size_t at = rng.below(n);
        const uint64_t w = words[rng.below(std::size(words))];
        for (int i = 0; i < 32 && at + i < n; ++i) m.b[at + i] = (w >> i) & 1;
        fuzz_run_one(s, m, st);
    }
}
static void test_fuzz() {
    std::printf("hostile blocks and payloads, a seeded fuzz of the captures\n");
    std::vector<FuzzSeed> seeds;
    for (const cap::Block& b : cap::kBlocks) seeds.push_back({FuzzWhat::Fast, b.cls, nullptr, bits_of(b.hex, b.bits)});
    for (const cap::Block& b : cap::kReplyBlocks)
        seeds.push_back({b.kind == cap::kFast ? FuzzWhat::Fast : (b.kind == cap::kRep ? FuzzWhat::Rep : FuzzWhat::Update), b.cls, nullptr, bits_of(b.hex, b.bits)});
    for (const cap::Case& c : cap::kCases) seeds.push_back({FuzzWhat::Build, "PlayerPawn_C", &rpc_of("PlayerPawn_C", "ServerPurchaseSkillBuild"), bits_of(c.request_hex, c.request_bits)});
    for (const cap::Call& c : cap::kPayloads) seeds.push_back({FuzzWhat::Payload, "AoCPlayerControllerBP_C", &rpc_of("AoCPlayerControllerBP_C", c.rpc), bits_of(c.hex, c.bits)});
    for (const cap::Call& c : cap::kSetSlots) seeds.push_back({FuzzWhat::SetSlot, "AoCPlayerControllerBP_C", &rpc_of("AoCPlayerControllerBP_C", c.rpc), bits_of(c.hex, c.bits)});
    Rng rng(0x9e3779b97f4a7c15ull);
    FuzzStats st;
    const auto start = std::chrono::steady_clock::now();
    for (const FuzzSeed& s : seeds) fuzz_mutate_seed(s, rng, st);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("    %zu reads, %zu decoded, %zu typed, %zu written back, %zu reading back equal, %.2f s\n", st.reads, st.decoded, st.typed, st.rewritten, st.same, secs);
    for (const auto& [k, v] : st.by_kind) std::printf("      kind %d: %zu decoded, %zu written back, %zu equal\n", k, v[0], v[1], v[2]);
    CHECK(st.reads > 10000 && st.decoded > 0 && st.same == st.rewritten, "mutated blocks and calls: none crashes, and whatever decodes and writes back reads equal");
    CHECK(st.rewritten * 2 > st.decoded, "and most of what decodes can be written back");
    CHECK(secs < 120, "and none takes long");
}

int main() {
    test_captured_blocks();
    test_captured_cases();
    test_actionbar();
    test_leveling_state();
    test_resend_and_ranged();
    test_refusals();
    test_fuzz();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
