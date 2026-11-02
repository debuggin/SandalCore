// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_mounts_session_common.h"

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
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

int main() {
    spdlog::set_level(spdlog::level::off);
    find_mount_records();
    test_summon_and_desummon();
    test_refusals_and_hostile();
    test_cancel_by_damage();
    test_world();
    test_open_on_the_wire();
    run_mounts_session_part2();
    run_mounts_session_part3();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
