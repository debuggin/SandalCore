// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_mounts_session_common.h"

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
    run_mounts_session_goldens_tests();
    test_item_abilities();
    test_item_ability_arrives_with_the_item();
    test_item_abilities_in_a_real_world();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
