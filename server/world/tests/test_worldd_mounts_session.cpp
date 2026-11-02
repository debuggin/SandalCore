// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_mounts_session_common.h"

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace cm = capture_mount_session;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using emit::BunchWriter;
using std::chrono::milliseconds;

static void test_summon_and_desummon() {
    std::printf("the summon and the desummon of a mount on a connection\n");
    Harness h(cyb_record());
    h.play();
    const size_t bag_before = h.s->inventory().count(Bag);
    const size_t applied_before = h.s->item_requests_applied(), refused_before = h.s->item_requests_refused();
    const size_t abilities_before = ability_items(h).size();
    const Clock::time_point req = h.t;
    const size_t got_before = h.got.size();
    summon_with(h, kMountSlot);
    CHECK(h.s->mount_item_uses() == 1 && h.s->mount_summon_requests() == 1, "the session saw the item used and the summon trigger");
    Saw saw = watch(h, req, milliseconds(2600));
    CHECK(saw.ability_item >= 0 && within(saw.ability_item, 0.08, 0.03), "the Summon_Mount ability item is on the pawn at about +80 ms");
    CHECK(within(saw.bar, 0.08, 0.04), "the bank-18 action bar goes out with it (ClientClearActionbarsForInstigators, then the slot)");
    CHECK(saw.open >= 0 && saw.open >= 0.10 && saw.open <= 0.30, "the mount's actor opens on a channel of its own (about +104 ms, the view's next pass)");
    CHECK(saw.mount_channel != 0 && !is_player_channel(h, saw.mount_channel) && saw.opens == 1, "one open, on a channel none of the player's four");
    CHECK(saw.summoning_off >= 0 && within(saw.summoning_off, 0.104 + 0.47, 0.05), "bIsSummoning goes off 0.47 s after the open");
    CHECK(saw.item_end >= 0 && within(saw.item_end, 1.541, 0.05), "the ability item ends (no montage) at +1.54 s");
    CHECK(mount_count(h) == 1 && h.s->mounts()->phase() == MountController::Phase::Out, "one mount is out");
    CHECK(saw.updates_on_mount >= 1, "the client got the bIsSummoning update on the mount's channel");
    CHECK(h.s->inventory().count(Bag) == bag_before && h.s->inventory().at(Bag, kMountSlot) != nullptr && h.s->item_requests_applied() == applied_before && h.s->item_requests_refused() == refused_before,
          "the mount item is still in the bag, the item request is neither applied nor refused");
    CHECK(abilities_before + 1 >= ability_items(h).size(), "(one ability item added)");
    const world::WorldActor* m = h.s->mounts()->mount();
    const auto& good = h.s->movement().good();
    const double yaw = good.yaw_degrees * 3.14159265358979 / 180.0;
    CHECK(m && std::hypot(m->x - (good.x + 575 * std::cos(yaw)), m->y - (good.y + 575 * std::sin(yaw))) < 1.0 && m->owner == static_cast<uint64_t>(h.s->bootstrap().setup().player_id),
          "it stands 575 cm in front of the player, its owner's");
    const Clock::time_point req2 = h.t;
    desummon_with(h, kMountSlot);
    CHECK(h.s->mount_desummon_requests() == 1 && h.s->mounts()->phase() == MountController::Phase::Desummoning, "the desummon begins");
    Saw init;
    init.mount_channel = saw.mount_channel;
    Saw gone = watch(h, req2, milliseconds(3600), init);
    CHECK(within(gone.flags, 0.1, 0.04), "bIsDesummoning goes on at about +0.1 s");
    CHECK(gone.close >= 0 && within(gone.close, 3.2, 0.06), "the channel is closed 3.2 s after the request");
    CHECK(mount_count(h) == 0 && h.s->mounts()->phase() == MountController::Phase::Idle, "the mount is gone from the world");
    CHECK(gone.updates_on_mount >= 10, "the client got the dissolve's flags and the run's ReplicatedMovement on the mount's channel");
    const Clock::time_point req3 = h.t;
    summon_with(h, kMountSlot, 9);
    Saw again = watch(h, req3, milliseconds(500));
    CHECK(again.open >= 0 && mount_count(h) == 1, "the item summons a new mount");
    (void)got_before;
}

static void check_refusals_locked_and_invalid() {
    {
        Harness h(cyb_record());
        h.play();
        const Clock::time_point req = h.t;
        summon_with(h, kLockedSlot, 11);
        Saw saw = watch(h, req, milliseconds(4500));
        CHECK(saw.open < 0 && mount_count(h) == 0, "a mount item under lock summons nothing");
        CHECK(saw.ability_item >= 0 && within(saw.ability_item, 0.105, 0.04) && saw.item_end >= 0 && within(saw.item_end, 1.6, 0.05), "the ability item shows at +105 ms and ends at +1.6 s");
        CHECK(saw.callout >= 0 && within(saw.callout, 0.105, 0.04), "the callout is on the PlayerController");
        CHECK(saw.bar < 0, "no mount bar");
        CHECK(h.s->bootstrap().callouts() == 0, "the callout is taken off again (3 s later)");
        CHECK(h.s->mounts()->stats().refused == 1, "counted");
    }
    {
        Harness h(cyb_record());
        h.play();
        size_t ends_before = h.s->ability_ends_sent();
        const svc::InventoryItem* sword = h.s->inventory().at(Bag, kSwordSlot);
        h.client_sends({h.client.actor(h.pawn_ch, summon_trigger(h, 21, kSwordSlot, sword), true)});
        h.settle();
        h.client_sends({h.client.actor(h.pawn_ch, summon_trigger(h, 22, 40, nullptr), true)});
        h.settle();
        h.client_sends({h.client.actor(h.pawn_ch, summon_trigger(h, 23, kMountSlot, h.s->inventory().at(Bag, kMountSlot), 4), true)});
        h.settle();
        h.client_sends({h.client.actor(h.pawn_ch, summon_trigger(h, 24, -1, nullptr), true)});
        h.settle();
        CHECK(mount_count(h) == 0 && h.s->mounts()->stats().summons == 0, "a trigger that names a sword, an empty slot, the equipment or no item makes no mount");
        CHECK(h.s->ability_ends_sent() >= ends_before + 4, "each is answered with ClientEndAbility (the client's own cast is ended)");
        h.client_sends({h.client.actor(h.pc_ch, desummon_rpc(), true)});
        h.settle();
        CHECK(mount_count(h) == 0 && h.s->mounts()->phase() == MountController::Phase::Idle, "ServerDesummonMount with no mount does nothing");
    }
}

static void check_refusals_spam_and_second_mount() {
    {
        Harness h(cyb_record());
        h.play();
        size_t max_mounts = 0;
        for (int i = 0; i < 30; ++i) {
            summon_with(h, kMountSlot, 100 + i);
            h.step(milliseconds(10));
            max_mounts = std::max(max_mounts, mount_count(h));
        }
        for (int i = 0; i < 400; ++i) {
            h.step();
            max_mounts = std::max(max_mounts, mount_count(h));
        }
        CHECK(max_mounts <= 1 && !h.s->failed(), "30 summons in 0.3 s never make two mounts and do not fail the session");
        CHECK(h.s->requests_dropped() > 0 || h.s->mount_requests_ignored() > 0, "the budget and the one-mount rule dropped the rest");
        for (int i = 0; i < 30; ++i) {
            h.client_sends({h.client.actor(h.pc_ch, desummon_rpc(), true)});
            h.step(milliseconds(10));
        }
        h.run(milliseconds(4000));
        CHECK(mount_count(h) == 0 && !h.s->failed() && h.s->mounts()->phase() == MountController::Phase::Idle, "30 desummons settle: no mount, no failure");
    }
    {
        Harness h(cyb_record());
        h.play();
        summon_with(h, kMountSlot, 31);
        h.run(milliseconds(1800));
        CHECK(mount_count(h) == 1, "(a mount is out)");
        summon_with(h, kOtherMountSlot, 32);
        h.run(milliseconds(3800));
        CHECK(mount_count(h) == 0 && h.s->mounts()->stats().summons == 1, "a trigger for another mount item while one is out desummons the old and summons nothing");
    }
}

static void test_refusals_and_hostile() {
    std::printf("refusals and hostile requests\n");
    check_refusals_locked_and_invalid();
    check_refusals_spam_and_second_mount();
}

static void test_cancel_by_damage() {
    std::printf("a damaging hit during the cast\n");
    Harness h(cyb_record());
    h.play();
    const Clock::time_point req = h.t;
    summon_with(h, kMountSlot, 41);
    h.step();
    h.step();
    h.step();
    Connection& c = h.conn;
    h.s->take_damage(c, 10.0, h.t);
    Saw saw = watch(h, req, milliseconds(1500));
    CHECK(saw.open < 0 && mount_count(h) == 0, "a hit before the open: no mount");
    CHECK(h.s->mounts()->stats().cancelled == 1 && h.s->mounts()->phase() == MountController::Phase::Idle, "the summon is cancelled");
    const Clock::time_point req2 = h.t;
    summon_with(h, kMountSlot, 42);
    watch(h, req2, milliseconds(1800));
    h.s->take_damage(c, 10.0, h.t);
    h.run(milliseconds(500));
    CHECK(mount_count(h) == 1 && h.s->mounts()->stats().cancelled == 1, "a hit after the cast leaves the mount");
    summon_with(h, kMountSlot, 43);
    h.run(milliseconds(3800));
    CHECK(mount_count(h) == 0, "(the first mount is gone)");
    const size_t desummons_before = h.s->mounts()->stats().desummons, cancelled_before = h.s->mounts()->stats().cancelled;
    summon_with(h, kMountSlot, 44);
    h.run(milliseconds(300));
    CHECK(mount_count(h) == 1, "(the mount has appeared)");
    for (int i = 0; i < 12; ++i) {
        h.s->take_damage(c, 1.0, h.t);
        h.run(milliseconds(500));
    }
    CHECK(mount_count(h) == 1 && h.s->mounts()->phase() == MountController::Phase::Out && h.s->mounts()->stats().cancelled == cancelled_before && h.s->mounts()->stats().desummons == desummons_before,
          "hits during and after the cast, for 6 s: the mount that appeared stays, nothing is cancelled or desummoned");
}

static void test_world() {
    std::printf("the mount belongs to its owner and leaves with the session\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot, 51);
    h.run(milliseconds(1800));
    const world::ActorRegistry& reg = h.fx.world->registry();
    const world::WorldActor* m = h.s->mounts()->mount();
    CHECK(m && m->kind == world::ActorKind::Mount && m->owner != 0 && m->never_dormant && reg.count(world::ActorKind::Mount) == 1, "the registry holds one Mount actor with an owner, never dormant");
    const uint64_t id = m ? m->id : 0;
    CHECK(m && m->owner == static_cast<uint64_t>(h.s->bootstrap().setup().player_id), "its owner is this player: another player's connection would not be sent it");
    h.s->finish("the test");
    CHECK(reg.find(id) == nullptr && reg.count(world::ActorKind::Mount) == 0, "the mount leaves the world when its owner's session ends");
}

static bool bits_contain_text(const emit::BunchWriter& w, const std::string& text) {
    if (text.empty()) return false;
    const size_t nbits = w.bit_pos();
    const uint8_t* d = w.data();
    for (size_t shift = 0; shift < 8; ++shift) {
        std::string bytes;
        for (size_t bit = shift; bit + 8 <= nbits; bit += 8) {
            const size_t i = bit / 8, o = bit % 8;
            uint8_t b = static_cast<uint8_t>(d[i] >> o);
            if (o != 0) b = static_cast<uint8_t>(b | (d[i + 1] << (8 - o)));
            bytes.push_back(static_cast<char>(b));
        }
        if (bytes.find(text) != std::string::npos) return true;
    }
    return false;
}

static void check_open_exports_and_alignments(const Harness& h, const world::WorldActor& m, const fake::Delivered& open) {
    CHECK(!open.exports.empty(), "the open carries its exports (the archetype and the subobjects' names)");
    {
        const content::MountRow* row = h.s->mounts()->book().find(h.s->mounts()->item_record());
        const std::string anim = row ? h.s->mounts()->book().rider_anim_of(*row) : std::string();
        const size_t dot = anim.rfind('.');
        const std::string package = dot == std::string::npos ? std::string() : anim.substr(0, dot);
        const std::string klass = dot == std::string::npos ? std::string() : anim.substr(dot + 1);
        bool has_class = false, has_package = false;
        for (const emit::BunchWriter& x : open.exports) {
            has_class = has_class || bits_contain_text(x, klass);
            has_package = has_package || bits_contain_text(x, package);
        }
        CHECK(!klass.empty() && has_class && has_package, "the export bunches name the rider's AnimBP class and its package");
    }
    {
        const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
        const repl::Subobject* pa = h.s->bootstrap().pawn().find(g.alignment);
        const repl::Value* pv = pa ? pa->obj.state.get(*pa->obj.schema, "Alignments") : nullptr;
        const auto* pawn_set = pv ? pv->get_if<repl::AlignmentContainer>() : nullptr;
        const repl::Subobject* ma = spawns::sub_of(m, "UAlignmentComponent");
        const repl::Value* mv = ma ? ma->obj.state.get(*ma->obj.schema, "InheritedAlignments") : nullptr;
        const auto* mount_set = mv ? mv->get_if<repl::AlignmentContainer>() : nullptr;
        CHECK(pawn_set && mount_set && *pawn_set == *mount_set, "the mount's InheritedAlignments are the pawn's Alignments, instance for instance");
        bool flagged = false;
        if (mount_set) {
            for (const auto& i : mount_set->instances) {
                flagged = flagged || i.key == 6064629443707731976ull || i.key == 6064631051897143296ull || i.key == 6064630560782155776ull;
            }
        }
        CHECK(mount_set && !flagged, "(and so hold no Combatant, no node and no guild)");
    }
}

static void test_open_on_the_wire() {
    std::printf("the open the client received\n");
    Harness h(cyb_record());
    h.play();
    const size_t from = h.got.size();
    summon_with(h, kMountSlot, 61);
    h.run(milliseconds(500));
    const world::WorldActor* m = h.s->mounts()->mount();
    const fake::Delivered* open = nullptr;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].open && !is_player_channel(h, h.got[i].channel)) open = &h.got[i];
    }
    CHECK(open != nullptr && m != nullptr, "an open reached the client");
    if (!open || !m) return;
    check_open_exports_and_alignments(h, *m, *open);
    const auto blocks = repl::read_content_blocks(open->content.data(), open->content.bit_pos(), 0);
    size_t start = 0;
    std::optional<std::vector<repl::BlockView>> found;
    for (start = 0; start < 2500 && !found; ++start) {
        auto got = repl::read_content_blocks(open->content.data(), open->content.bit_pos(), start);
        if (got && got->size() == m->actor.subobjects.size() + 1 && !got->empty() && (*got)[0].header.is_actor) found = std::move(got);
    }
    CHECK(found.has_value(), "its content reads as the actor's block and one block per subobject (9)");
    (void)blocks;
}

static void find_mount_records() {
    std::string error;
    auto real = content::ContentDb::open(real_db_path(), error);
    const auto data = real ? real->mount_data(error) : std::nullopt;
    if (!data) {
        std::printf("  [FAIL] the real content.db has no mount tables (%s): %s\n", real_db_path().c_str(), error.c_str());
        std::exit(2);
    }
    for (const content::MountRow& r : data->mounts) {
        if (r.item_name == "Mount_Grounded_TerrorBird_0") g_mount_record = r.item;
        if (r.item_name == "Mount_Grounded_Horse_0") g_other_mount_record = r.item;
        if (r.kind == "aquatic" && !g_aquatic_record) g_aquatic_record = r.item;
        if (r.kind == "flying" && !g_flying_record) g_flying_record = r.item;
    }
}

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

static void find_callout_in_bunch(const fake::Delivered& d, std::optional<repl::ElementState>& our_callout) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return;
    for (const repl::BlockView& b : *blocks) {
        if (!b.header.is_actor) continue;
        BunchWriter w;
        w.write_bit_range(d.content.data(), b.payload_pos, b.payload_bits);
        auto st = repl::load_object_state(class_named("AoCPlayerControllerBP_C"), w.data(), b.payload_bits, b.header.has_rep_layout);
        if (!st) continue;
        const schema::NetField* f = st->schema->field("CalloutQueueReplication");
        const repl::FastArrayState* a = f ? st->arrays.find(f->index) : nullptr;
        if (a && !a->items().empty()) our_callout = a->items().back().values;
    }
}

static std::optional<repl::ElementState> find_callout_in_delivered(const Harness& h, size_t from) {
    std::optional<repl::ElementState> our_callout;
    for (size_t i = from; i < h.got.size(); ++i) {
        const fake::Delivered& d = h.got[i];
        if (d.channel != h.pc_ch || d.open || d.close) continue;
        find_callout_in_bunch(d, our_callout);
    }
    return our_callout;
}

static void check_golden_refusal_capture(uint16_t capture) {
    const cm::Block* item = retail(capture, "item");
    const cm::Block* end = retail(capture, "end");
    const cm::Block* callout = retail(capture, "callout");
    const cm::Block* off = cm::find(capture, "callout_off");
    if (!item || !end || !callout) return;
    const auto ri = load_block(*item);
    const auto rc = load_block(*callout);
    const auto ri_item = ri ? ability_item_of(*ri) : std::nullopt;
    CHECK(ri_item && rc, (std::string("retail's item and callout of ") + std::to_string(capture) + " read").c_str());
    if (!ri_item || !rc) return;
    Harness h(cyb_record());
    h.play();
    const size_t from = h.got.size();
    summon_with(h, kLockedSlot, 11);
    std::optional<repl::ElementState> shown;
    for (milliseconds done{0}; done < milliseconds(1200); done += milliseconds(20)) {
        h.step();
        if (!shown) shown = our_item(h, true);
    }
    const std::optional<repl::ElementState> our_callout = find_callout_in_delivered(h, from);
    CHECK(shown.has_value(), "worldd shows the item of the refused summon");
    if (shown) {
        const std::set<uint16_t> d = item_diff(*ri_item, *shown);
        const std::set<uint16_t> allowed_item = {2, 15, 17};
        CHECK(std::includes(allowed_item.begin(), allowed_item.end(), d.begin(), d.end()),
              (std::string("the refusal's item is retail's ") + std::to_string(capture) + " but for the instance, the counter and the instigator").c_str());
    }
    const schema::NetField* f = rc->schema->field("CalloutQueueReplication");
    const repl::FastArrayState* ra = f ? rc->arrays.find(f->index) : nullptr;
    CHECK(ra && !ra->items().empty() && our_callout.has_value(), "worldd sends a callout, and so did retail");
    if (ra && !ra->items().empty() && our_callout) {
        const std::set<uint16_t> d = item_diff(ra->items().back().values, *our_callout);
        const std::set<uint16_t> allowed = {21, 25, 26, 27, 28};
        bool subset = true;
        std::string s;
        for (const uint16_t x : d) {
            subset = subset && allowed.count(x);
            s += std::to_string(x) + " ";
        }
        CHECK(subset, (std::string("the callout is retail's but for its key and creation time (differing: ") + s + ")").c_str());
    }
    (void)off;
}

static void test_golden_refusal() {
    std::printf("golden: the refused summon of the retail captures\n");
    for (const uint16_t capture : {876, 870}) check_golden_refusal_capture(capture);
    Harness h(cyb_record());
    h.play();
    const Clock::time_point req = h.t;
    summon_with(h, kLockedSlot, 11);
    const Saw saw = watch(h, req, milliseconds(4500));
    const cm::Block* a = cm::find(876, "item");
    const cm::Block* b = cm::find(870, "item");
    const cm::Block* e1 = cm::find(876, "end");
    const cm::Block* e2 = cm::find(870, "end");
    CHECK(a && b && e1 && e2 && saw.ability_item * 1000 >= std::min(a->ms, b->ms) - 40 && saw.ability_item * 1000 <= std::max(a->ms, b->ms) + 40,
          "the item comes within retail's range (+95 to +116 ms)");
    CHECK(a && b && e1 && e2 && saw.item_end * 1000 >= std::min(e1->ms, e2->ms) - 50 && saw.item_end * 1000 <= std::max(e1->ms, e2->ms) + 50,
          "the end comes within retail's range (+1.58 to +1.62 s)");
}

struct ClientLists {
    std::vector<int64_t> known, records;
    size_t updates = 0;
};

static bool apply_ability_array_field(const repl::RepState& st, const schema::ClassSchema& cls, const char* name, std::vector<int64_t>& list) {
    const repl::Value* val = st.get(cls, name);
    const repl::ArrayValue* a = val ? val->get_if<repl::ArrayValue>() : nullptr;
    if (!a) return false;
    list.resize(a->elements.size());
    for (size_t i = 0; i < a->elements.size(); ++i) {
        const auto it = a->elements[i].values.find(1);
        if (it != a->elements[i].values.end() && it->second.get_if<int64_t>()) list[i] = *it->second.get_if<int64_t>();
    }
    return true;
}

static void apply_delivered_client_lists(const Harness& h, const schema::ClassSchema& cls, const fake::Delivered& d, ClientLists& out) {
    if (d.channel != h.pawn_ch || d.open || d.close || d.content.bit_pos() <= 8) return;
    const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!views) return;
    for (const repl::BlockView& v : *views) {
        if (!(v.header.object == h.s->bootstrap().setup().guids.ability) || !v.header.has_rep_layout) continue;
        size_t p = v.payload_pos;
        const auto st = repl::read_rep_block(cls, d.content.data(), v.payload_pos + v.payload_bits, p);
        if (!st) continue;
        const bool c1 = apply_ability_array_field(*st, cls, "KnownAbilities", out.known);
        const bool c2 = apply_ability_array_field(*st, cls, "ReplicatedRecordsToLoad", out.records);
        if (c1 || c2) ++out.updates;
    }
}

static ClientLists client_lists(const Harness& h) {
    ClientLists out;
    const repl::Subobject* ab = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.ability);
    if (!ab) return out;
    for (const fake::Delivered& d : h.got) apply_delivered_client_lists(h, *ab->obj.schema, d, out);
    return out;
}

static constexpr int64_t kSummonMountRecord = 108436548376185ll;

static size_t times_in(const std::vector<int64_t>& v, int64_t x) { return static_cast<size_t>(std::count(v.begin(), v.end(), x)); }

static bool use_mount_item_like_retail(Harness& h, int slot, int32_t client_instance = 7) {
    const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    const std::string guid = item ? item->instance_guid : std::string();
    if (times_in(client_lists(h).records, kSummonMountRecord) == 0) {
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid), true)});
        return false;
    }
    summon_with(h, slot, client_instance);
    return true;
}

static std::vector<svc::InventoryItem> kit_without_mounts() {
    std::vector<svc::InventoryItem> out;
    for (svc::InventoryItem& it : kit()) {
        if (it.item_record_id == g_mount_record || it.item_record_id == g_other_mount_record || it.item_record_id == g_aquatic_record || it.item_record_id == g_flying_record) continue;
        out.push_back(std::move(it));
    }
    return out;
}

static void test_item_abilities() {
    std::printf("the pawn's ability records for the mount items it carries: the client needs Summon_Mount loaded to send the trigger\n");
    std::vector<int64_t> with_mounts;
    {
        Harness h(cyb_record());
        h.play();
        const ClientLists l = client_lists(h);
        with_mounts = l.records;
        const std::vector<int64_t> tail(bs::static_known_tail().begin(), bs::static_known_tail().end());
        CHECK(times_in(l.records, kSummonMountRecord) == 1, "a character with mounts in the bag has Summon_Mount in ReplicatedRecordsToLoad, once");
        CHECK(times_in(l.known, kSummonMountRecord) == 0, "and not in KnownAbilities (retail's login: 92 records with it, 75 known without it)");
        CHECK(l.records.size() >= tail.size() && std::equal(tail.begin(), tail.end(), l.records.end() - static_cast<std::ptrdiff_t>(tail.size())),
              "the static tail still ends the records (the item's ability comes before it, as in retail's login)");
        CHECK(l.records == h.s->record_abilities() && l.known == h.s->known_abilities(), "the client's lists are the session's");
        const Clock::time_point req = h.t;
        CHECK(use_mount_item_like_retail(h, kMountSlot), "the client has the ability loaded: it sends ServerTriggerAbility with ServerItemUsed");
        const Saw saw = watch(h, req, milliseconds(2600));
        CHECK(h.s->mount_summon_requests() == 1 && saw.open >= 0 && mount_count(h) == 1 && saw.ability_item >= 0,
              "the summon starts on the client's trigger: the ability item, then the mount's open");
    }
    {
        Harness h(cyb_record(kit_without_mounts()));
        h.play();
        const ClientLists l = client_lists(h);
        std::vector<int64_t> expected = with_mounts;
        expected.erase(std::remove(expected.begin(), expected.end(), kSummonMountRecord), expected.end());
        CHECK(!l.records.empty() && times_in(l.records, kSummonMountRecord) == 0, "a character with no mount item does not load Summon_Mount");
        CHECK(l.records == h.s->record_abilities() && l.records == expected, "and its records are the other character's but for that one");
    }
}

static void test_item_abilities_in_a_real_world() {
    std::printf("the same on the real content.db's world (its talents catalog: the production path of the join's lists)\n");
    Harness h(cyb_record(), true);
    if (!h.fx.world->ready() || !h.fx.world->talents()) {
        std::printf("  [skip] the real world is not ready or has no talents: %s\n", h.fx.world->content_problem().c_str());
        return;
    }
    h.play();
    const ClientLists l = client_lists(h);
    const std::vector<int64_t> tail(bs::static_known_tail().begin(), bs::static_known_tail().end());
    CHECK(times_in(l.records, kSummonMountRecord) == 1 && times_in(l.known, kSummonMountRecord) == 0, "Summon_Mount is in the records once and in no KnownAbilities");
    CHECK(l.records.size() >= tail.size() && std::equal(tail.begin(), tail.end(), l.records.end() - static_cast<std::ptrdiff_t>(tail.size())), "before the static tail");
    const Clock::time_point req = h.t;
    CHECK(use_mount_item_like_retail(h, kMountSlot), "the client sends the trigger with the item use");
    const Saw saw = watch(h, req, milliseconds(2600));
    CHECK(saw.open >= 0 && mount_count(h) == 1, "and the mount is summoned");
}

struct NoItemCatalog : items::ItemCatalog {
    std::optional<items::ItemInfo> info(int64_t) const override { return std::nullopt; }
};

static void test_item_ability_arrives_with_the_item() {
    std::printf("a mount item that reaches the bag during the session brings its ability record right after it (retail 821: the record is appended)\n");
    Harness h(cyb_record(kit_without_mounts()));
    h.play();
    const ClientLists before = client_lists(h);
    CHECK(times_in(before.records, kSummonMountRecord) == 0, "the bag holds no mount: no Summon_Mount");
    items::Inventory& inv = const_cast<items::Inventory&>(h.s->inventory());
    svc::InventoryItem fresh = make_item(Bag, 0, g_mount_record, 900);
    fresh.slot = inv.first_free(Bag);
    const int slot = fresh.slot;
    CHECK(inv.grant({fresh}, NoItemCatalog()).ok, "the mount item is granted into the bag");
    h.run(milliseconds(300));
    const ClientLists after = client_lists(h);
    CHECK(times_in(after.records, kSummonMountRecord) == 1 && after.records.size() == before.records.size() + 1 && after.records.back() == kSummonMountRecord,
          "Summon_Mount is appended to the client's records, once");
    CHECK(after.updates == before.updates + 1 && after.known == before.known, "in one update, and KnownAbilities does not change");
    CHECK(after.records == h.s->record_abilities(), "the client's records are the session's");
    svc::InventoryItem second = make_item(Bag, 0, g_other_mount_record, 901);
    second.slot = inv.first_free(Bag);
    CHECK(inv.grant({second}, NoItemCatalog()).ok, "a second mount item is granted");
    h.run(milliseconds(300));
    const ClientLists again = client_lists(h);
    CHECK(again.updates == after.updates && again.records == after.records, "a second mount item adds no record and sends no update");
    const Clock::time_point req = h.t;
    CHECK(use_mount_item_like_retail(h, slot), "the client sends ServerTriggerAbility with the item use");
    const Saw saw = watch(h, req, milliseconds(2600));
    CHECK(saw.open >= 0 && mount_count(h) == 1 && h.s->mount_summon_requests() == 1, "and the mount is summoned");
}

int main() {
    spdlog::set_level(spdlog::level::off);
    find_mount_records();
    test_summon_and_desummon();
    test_refusals_and_hostile();
    test_cancel_by_damage();
    test_world();
    test_open_on_the_wire();
    test_golden_summon();
    test_golden_desummon();
    test_golden_refusal();
    test_item_abilities();
    test_item_ability_arrives_with_the_item();
    test_item_abilities_in_a_real_world();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
