// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_mounts_session_common.h"

using namespace aoc::worldd;
namespace cm = capture_mount_session;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using std::chrono::milliseconds;

struct OurSummon {
    std::optional<repl::ElementState> shown, ended;
    std::vector<repl::RpcCall> pc_calls;
    Saw saw;
};

static OurSummon run_our_summon(int slot) {
    OurSummon o;
    Harness h(cyb_record());
    h.play();
    const size_t from = h.got.size();
    const Clock::time_point req = h.t;
    summon_with(h, slot, 7);
    for (milliseconds done{0}; done < milliseconds(2200); done += milliseconds(20)) {
        o.saw = watch(h, req, milliseconds(20), o.saw);
        if (!o.shown) o.shown = our_item(h, true);
    }
    o.ended = our_item(h, false);
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch || h.got[i].open || h.got[i].close) continue;
        for (auto& c : pc_calls_of(h, h.got[i])) o.pc_calls.push_back(std::move(c));
    }
    return o;
}

static bool only_instance_values_differ(const repl::ElementState& a, const repl::ElementState& b, std::string* what = nullptr, bool prediction_flag = false) {
    const std::set<uint16_t> d = item_diff(a, b);
    bool ok = true;
    for (const uint16_t x : d) {
        if (x != 2 && x != 15 && x != 17 && !(prediction_flag && x == 9)) ok = false;
        if (what) *what += std::to_string(x) + " ";
    }
    return ok;
}

struct SummonGoldenStats {
    int item_lo = 100000, item_hi = 0, open_lo = 100000, open_hi = 0, end_lo = 100000, end_hi = 0, off_lo = 100000, off_hi = 0;
    size_t same_item = 0, same_end = 0, same_bar = 0, read = 0;
};

static bool check_golden_bar_calls(const OurSummon& kind_run, const cm::Block& clear, const cm::Block& bar) {
    const auto rc = retail_pc_calls(clear);
    const auto rb = retail_pc_calls(bar);
    bool ok = rc.size() == 1 && rb.size() == 1;
    size_t ours_clear = 0, ours_bar = 0;
    for (const repl::RpcCall& c : kind_run.pc_calls) {
        if (!c.rpc || !ok) continue;
        if (std::string(c.rpc->name) == "ClientClearActionbarsForInstigators") {
            ++ours_clear;
            ok = ok && c.params.values == rc[0].params.values;
        } else if (std::string(c.rpc->name) == "ClientReceiveActionbarSlotPayload") {
            ++ours_bar;
            ok = ok && c.params.values == rb[0].params.values;
        }
    }
    return ok && ours_clear == 1 && ours_bar == 1;
}

static void check_golden_summon_capture(uint16_t capture, const OurSummon& ours, const OurSummon& kind_run, SummonGoldenStats& st) {
    const cm::Block* item = retail(capture, "item");
    const cm::Block* end = retail(capture, "end");
    const cm::Block* clear = retail(capture, "bar_clear");
    const cm::Block* bar = retail(capture, "bar");
    const cm::Block* open = retail(capture, "open");
    const cm::Block* off = retail(capture, "off");
    if (!item || !end || !clear || !bar || !open || !off) return;
    st.item_lo = std::min(st.item_lo, item->ms);
    st.item_hi = std::max(st.item_hi, item->ms);
    st.open_lo = std::min(st.open_lo, open->ms);
    st.open_hi = std::max(st.open_hi, open->ms);
    st.end_lo = std::min(st.end_lo, end->ms);
    st.end_hi = std::max(st.end_hi, end->ms);
    const int off_after_open = off->ms - open->ms;
    st.off_lo = std::min(st.off_lo, off_after_open);
    st.off_hi = std::max(st.off_hi, off_after_open);
    const auto ri = load_block(*item);
    const auto re = load_block(*end);
    const auto ri_item = ri ? ability_item_of(*ri) : std::nullopt;
    const auto re_item = re ? ability_item_of(*re) : std::nullopt;
    if (!ri_item || !re_item) {
        CHECK(false, (std::string("the retail item and end of ") + std::to_string(capture) + " read").c_str());
        return;
    }
    ++st.read;
    const std::set<uint16_t> d = item_diff(*ri_item, *re_item);
    CHECK(d == std::set<uint16_t>({11, 15}) || (capture == 871 && d == std::set<uint16_t>({9, 11, 15})),
          (std::string("capture ") + std::to_string(capture) + ": its end differs from its item in the montage and the animation counter").c_str());
    std::string what;
    if (ours.shown && only_instance_values_differ(*ri_item, *ours.shown, &what)) ++st.same_item;
    else if (ours.shown) std::printf("      capture %u: the item differs in handles %s\n", capture, what.c_str());
    what.clear();
    if (ours.ended && only_instance_values_differ(*re_item, *ours.ended, &what, capture == 871)) ++st.same_end;
    else if (ours.ended) std::printf("      capture %u: the end differs in handles %s\n", capture, what.c_str());
    if (check_golden_bar_calls(kind_run, *clear, *bar)) ++st.same_bar;
}

static void test_golden_summon() {
    std::printf("golden: the summon of the retail captures\n");
    const OurSummon by_kind[3] = {run_our_summon(kMountSlot), run_our_summon(kAquaticSlot), run_our_summon(kFlyingSlot)};
    const OurSummon& ours = by_kind[0];
    CHECK(ours.shown.has_value() && ours.ended.has_value() && by_kind[1].shown.has_value() && by_kind[2].shown.has_value(),
          "worldd shows the Summon_Mount item and ends it, for a ground, an aquatic and a flying mount");
    SummonGoldenStats st;
    for (const uint16_t capture : {911, 896, 898, 871}) {
        const OurSummon& kind_run = capture == 898 ? by_kind[1] : capture == 871 ? by_kind[2] : by_kind[0];
        check_golden_summon_capture(capture, ours, kind_run, st);
    }
    CHECK(st.read == 4, "the four retail summons read");
    CHECK(st.same_item == 4, "the Summon_Mount item worldd shows is retail's but for the instance, the animation counter and the instigator (4 of 4)");
    CHECK(st.same_end == 4, "the item worldd ends is retail's end item but for the same three (and 871's prediction flag) (4 of 4)");
    CHECK(st.same_bar == 4, "the bank-18 bar is retail's two calls with retail's values (4 of 4)");
    std::printf("    retail: the item at +%d to +%d ms, the open at +%d to +%d ms, bIsSummoning off %d to %d ms after the open, the end at +%d to +%d ms\n",
                st.item_lo, st.item_hi, st.open_lo, st.open_hi, st.off_lo, st.off_hi, st.end_lo, st.end_hi);
    CHECK(ours.saw.ability_item * 1000 >= st.item_lo - 25 && ours.saw.ability_item * 1000 <= st.item_hi + 25, "worldd's item comes within retail's range");
    CHECK(ours.saw.open * 1000 >= st.open_lo - 30 && ours.saw.open * 1000 <= st.open_hi + 120,
          "the open comes within retail's range (plus the view's pass: a connection looks for new actors every 67 ms)");
    const double off_after = (ours.saw.summoning_off - ours.saw.open) * 1000;
    CHECK(off_after >= st.off_lo - 40 && off_after <= st.off_hi + 40, "bIsSummoning goes off within retail's range after the open");
    CHECK(ours.saw.item_end * 1000 >= st.end_lo - 60 && ours.saw.item_end * 1000 <= st.end_hi + 60, "the end of the cast is within retail's range");
}

static const repl::ObjectState* find_subobject_state(const world::WorldActor& m, uint64_t obj_id) {
    for (const repl::Subobject& s : m.actor.subobjects) {
        if (s.guid.ObjectId == obj_id) return &s.obj;
    }
    return nullptr;
}

static std::vector<std::pair<uint64_t, std::set<uint16_t>>> handles_of(const fake::Delivered& d, const world::WorldActor& m) {
    std::vector<std::pair<uint64_t, std::set<uint16_t>>> out;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return out;
    for (const repl::BlockView& b : *blocks) {
        const uint64_t id = b.header.is_actor ? 0 : b.header.object.ObjectId;
        const repl::ObjectState* obj = b.header.is_actor ? &m.actor.obj : find_subobject_state(m, id);
        if (!obj || !b.header.has_rep_layout) continue;
        size_t pos = b.payload_pos;
        const auto st = repl::read_rep_block(*obj->schema, d.content.data(), b.payload_pos + b.payload_bits, pos, obj->opts);
        if (!st) continue;
        std::set<uint16_t> hs;
        for (const auto& [handle, v] : st->values()) hs.insert(handle);
        out.emplace_back(id, std::move(hs));
    }
    return out;
}

static std::set<uint16_t> handles_in(const repl::ObjectState& s) {
    std::set<uint16_t> out;
    for (const auto& [h, v] : s.state.values()) out.insert(h);
    return out;
}

struct DesummonSeen {
    std::set<uint16_t> flags, combat, run, flags_off, input_off;
    int flags_ms = 0, run_ms = 0, off_ms = 0, input_off_ms = 0, close_ms = 0;
};

static std::map<uint16_t, DesummonSeen> collect_retail_desummon_seen() {
    std::map<uint16_t, DesummonSeen> retail_seen;
    for (const uint16_t capture : {874, 893, 898}) {
        DesummonSeen s;
        const cm::Block* b[] = {retail(capture, "flags"), retail(capture, "combat"), retail(capture, "run"),
                                retail(capture, "flags_off"), retail(capture, "input_off"), retail(capture, "close")};
        bool all = true;
        for (const cm::Block* x : b) all = all && x != nullptr;
        if (!all) continue;
        const auto flags = load_block(*b[0]);
        const auto combat = load_block(*b[1]);
        const auto run = load_block(*b[2]);
        const auto off = load_block(*b[3]);
        const auto in_off = load_block(*b[4]);
        CHECK(flags && combat && run && off && in_off, (std::string("retail's blocks of ") + std::to_string(capture) + " read").c_str());
        if (!(flags && combat && run && off && in_off)) continue;
        s.flags = handles_in(*flags);
        s.combat = handles_in(*combat);
        s.run = handles_in(*run);
        s.flags_off = handles_in(*off);
        s.input_off = handles_in(*in_off);
        s.flags_ms = b[0]->ms;
        s.run_ms = b[2]->ms;
        s.off_ms = b[3]->ms;
        s.input_off_ms = b[4]->ms;
        s.close_ms = b[5]->ms;
        retail_seen[capture] = s;
        const repl::Value* d = flags->state.get(*flags->schema, "bIsDesummoning");
        const repl::Value* t = flags->state.get(*flags->schema, "DissolveTimer");
        const repl::Value* inv = combat->state.get(*combat->schema, "bInvulnerable");
        const repl::Value* nt = combat->state.get(*combat->schema, "bNotTargetable");
        const repl::Value* in = run->state.get(*run->schema, "CurrentMovementInput");
        CHECK(d && d->get_if<bool>() && *d->get_if<bool>() && t && t->get_if<float>() && *t->get_if<float>() > 0.02f && *t->get_if<float>() < 0.06f &&
                  inv && inv->get_if<bool>() && *inv->get_if<bool>() && nt && nt->get_if<bool>() && *nt->get_if<bool>() && in && in->get_if<repl::PlayerMovementInput>() &&
                  in->get_if<repl::PlayerMovementInput>()->bits != 0,
              (std::string("capture ") + std::to_string(capture) + ": bIsDesummoning, a short DissolveTimer, invulnerable, untargetable, the input forward").c_str());
    }
    return retail_seen;
}

struct OurDesummon {
    double flags = -1, run = -1, off = -1, input_off = -1, close = -1;
    std::set<uint16_t> flags_h, combat_h, run_h, off_h, input_off_h;
};

static bool is_subobject_schema(const world::WorldActor& ref, uint64_t id, const char* schema_name) {
    for (const repl::Subobject& s : ref.actor.subobjects) {
        if (s.guid.ObjectId == id) return std::string(s.obj.schema->name) == schema_name;
    }
    return false;
}

static void record_desummon_handle_block(const world::WorldActor& ref, uint64_t id, const std::set<uint16_t>& hs, double now, OurDesummon& ours) {
    const bool is_actor = id == 0;
    const bool mount_comp = !is_actor && is_subobject_schema(ref, id, "UMountComponent");
    const bool combat_comp = !is_actor && is_subobject_schema(ref, id, "UNPCCombatInfo");
    const schema::ClassSchema& mc = class_named("UMountComponent");
    const uint16_t desum = static_cast<uint16_t>(mc.command("bIsDesummoning")->handle);
    const uint16_t move_in = static_cast<uint16_t>(class_named("ANPCCharacter").command("CurrentMovementInput")->handle);
    if (mount_comp && hs.count(desum) && ours.flags < 0) {
        ours.flags = now;
        ours.flags_h = hs;
    } else if (mount_comp && ours.flags >= 0 && ours.off < 0 && hs.count(desum)) {
        ours.off = now;
        ours.off_h = hs;
    } else if (combat_comp && ours.combat_h.empty()) {
        ours.combat_h = hs;
    } else if (is_actor && hs.count(move_in)) {
        if (ours.run < 0) {
            ours.run = now;
            ours.run_h = hs;
        } else if (ours.input_off < 0) {
            ours.input_off = now;
            ours.input_off_h = hs;
        }
    }
}

static OurDesummon collect_our_desummon_seen(Harness& h, uint64_t channel, const world::WorldActor& snapshot, Clock::time_point req, size_t from) {
    OurDesummon ours;
    size_t scanned = from;
    for (milliseconds done{0}; done < milliseconds(3600); done += milliseconds(20)) {
        h.step();
        const double now = h.since(req);
        for (; scanned < h.got.size(); ++scanned) {
            const fake::Delivered& d = h.got[scanned];
            if (d.channel != channel) continue;
            if (d.close) {
                if (ours.close < 0) ours.close = now;
                continue;
            }
            const world::WorldActor* cur = h.s->mounts()->mount();
            const world::WorldActor& ref = cur ? *cur : snapshot;
            for (const auto& [id, hs] : handles_of(d, ref)) record_desummon_handle_block(ref, id, hs, now, ours);
        }
    }
    return ours;
}

static void test_golden_desummon() {
    std::printf("golden: the desummon of the retail captures\n");
    std::map<uint16_t, DesummonSeen> retail_seen = collect_retail_desummon_seen();
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot, 7);
    h.run(milliseconds(2000));
    const world::WorldActor* m = h.s->mounts()->mount();
    if (!m) {
        CHECK(false, "(a mount is out)");
        return;
    }
    const uint64_t channel = h.s->actor_view()->channel_of(m->id);
    const world::WorldActor snapshot = *m;
    const Clock::time_point req = h.t;
    const size_t from = h.got.size();
    desummon_with(h, kMountSlot);
    const OurDesummon ours = collect_our_desummon_seen(h, channel, snapshot, req, from);
    const DesummonSeen* s874 = retail_seen.count(874) ? &retail_seen[874] : nullptr;
    CHECK(s874 && ours.flags_h == s874->flags && ours.combat_h == s874->combat && ours.off_h == s874->flags_off,
          "the flags update has the handles retail's has (bIsDesummoning and DissolveTimer; invulnerable and untargetable; bIsDesummoning off)");
    const auto show = [](const std::set<uint16_t>& hs) {
        std::string out;
        for (const uint16_t x : hs) out += std::to_string(x) + " ";
        return out;
    };
    if (s874) {
        std::printf("      run: ours %s| retail %s; input off: ours %s| retail %s\n", show(ours.run_h).c_str(), show(s874->run).c_str(), show(ours.input_off_h).c_str(), show(s874->input_off).c_str());
        std::printf("      times: flags %.3f run %.3f off %.3f input_off %.3f close %.3f\n", ours.flags, ours.run, ours.off, ours.input_off, ours.close);
    }
    CHECK(s874 && ours.run_h == s874->run && ours.input_off_h == s874->input_off, "the run's updates carry the handles retail's do (the movement, its timestamp, the input)");
    int lo[5] = {100000, 100000, 100000, 100000, 100000}, hi[5] = {0, 0, 0, 0, 0};
    for (const auto& [capture, s] : retail_seen) {
        const int v[5] = {s.flags_ms, s.run_ms, s.off_ms, s.input_off_ms, s.close_ms};
        for (int i = 0; i < 5; ++i) {
            lo[i] = std::min(lo[i], v[i]);
            hi[i] = std::max(hi[i], v[i]);
        }
    }
    const double mine[5] = {ours.flags, ours.run, ours.off, ours.input_off, ours.close};
    const char* what[5] = {"bIsDesummoning on", "the run", "bIsDesummoning off", "the input off", "the close"};
    for (int i = 0; i < 5; ++i) {
        const double slack = i == 0 || i == 1 ? 110 : 80;
        CHECK(mine[i] >= 0 && mine[i] * 1000 >= lo[i] - slack && mine[i] * 1000 <= hi[i] + slack,
              (std::string(what[i]) + " comes within retail's range (" + std::to_string(lo[i]) + " to " + std::to_string(hi[i]) + " ms)").c_str());
    }
}

void run_mounts_session_part2() {
    test_golden_summon();
    test_golden_desummon();
}
