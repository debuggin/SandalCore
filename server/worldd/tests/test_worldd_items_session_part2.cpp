// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_items_session_helpers.h"
using namespace aoc::worldd;
struct FakeLogins : LoginQueue {
    std::vector<std::pair<uint64_t, LoginRequest>> submitted;
    void submit(uint64_t id, LoginRequest r) override {
        submitted.emplace_back(id, std::move(r));
    }
};
struct Rig {
    CookieSigner signer{counting_random};
    FakeLogins logins;
    std::vector<std::pair<Address, std::vector<uint8_t>>> sent;
    Endpoint ep;
    explicit Rig(EndpointConfig cfg) : ep(std::move(cfg), signer, logins, [this](const Address &a, const std::vector<uint8_t> &p) { sent.emplace_back(a, p); }, counting_random, t0) {
    }
    std::vector<std::vector<uint8_t>> take(const Address &a) {
        std::vector<std::vector<uint8_t>> out;
        std::vector<std::pair<Address, std::vector<uint8_t>>> rest;
        for (auto &[to, p] : sent) {
            if (to == a)
                out.push_back(p);
            else
                rest.emplace_back(to, p);
        }
        sent.swap(rest);
        return out;
    }
};
struct Joined {
    std::optional<conn::HandshakePacket> challenge;
    std::optional<fake::Client> client;
    uint64_t session = 0;
    std::vector<fake::Delivered> got;
};
static Joined handshake(Rig &rig, const Address &addr, Clock::time_point now) {
    Joined j;
    rig.take(addr);
    const auto initial = capture::bytes(capture::kInitial);
    rig.ep.on_datagram(addr, initial.data(), initial.size(), now);
    auto out = rig.take(addr);
    if (out.size() != 1)
        return j;
    j.challenge = conn::parse_handshake(out[0].data(), out[0].size());
    conn::HandshakePacket resp = *j.challenge;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    resp.padding.assign(11, 0x5A);
    const auto bytes = conn::build_handshake(resp);
    rig.ep.on_datagram(addr, bytes.data(), bytes.size(), now);
    out = rig.take(addr);
    if (out.size() != 1)
        return j;
    const auto seqs = conn::initial_sequences(j.challenge->cookie);
    j.client.emplace(conn::outer_header_of(*j.challenge), j.challenge->ext->custom_field(), seqs.server, seqs.client);
    j.session = rig.logins.submitted.empty() ? 0 : rig.logins.submitted.back().first;
    return j;
}
static void drain(Rig &rig, Joined &j, const Address &addr) {
    for (const auto &p : rig.take(addr)) {
        const auto r = j.client->receive(p);
        for (const auto &d : r.delivered) j.got.push_back(d);
    }
}
static void exchange(Rig &rig, Joined &j, const Address &addr, std::vector<fake::Bunch> bunches, Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    drain(rig, j, addr);
}
static void log_in(Rig &rig, Joined &j, const Address &addr, const svc::Character &rec, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(j.session, r);
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login(kMachine, kMachine)}, now + milliseconds(2));
}
static void join_world(Rig &rig, Joined &j, const Address &addr, const svc::Character &rec, Clock::time_point now) {
    log_in(rig, j, addr, rec, now);
    exchange(rig, j, addr, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)}, now + milliseconds(40));
}
struct Endpoints {
    Fixture fx;
    FakeCharacters api;
    std::unique_ptr<CharacterSaver> saver;
    EndpointConfig cfg;
    std::unique_ptr<Rig> rig;
    Endpoints() {
        saver = std::make_unique<CharacterSaver>(api, quick());
        fx.world->set_position_sink(saver.get());
        cfg.players = fx.world.get();
        rig = std::make_unique<Rig>(cfg);
    }
    ~Endpoints() {
        rig.reset();
        saver.reset();
    }
    std::vector<FakeCharacters::Inv> saves() {
        saver->flush(seconds(3));
        return api.saved_inventories();
    }
    void use(Joined &j, const Address &addr, int storage, int slot, Clock::time_point now) {
        const PlayerSession *p = rig->ep.player_at(addr);
        const uint32_t ch = p ? p->channel_of(bs::Role::Pawn) : 0;
        exchange(*rig, j, addr, {j.client->actor(ch, item_used(storage, slot, guid_n(102)), true), j.client->actor(ch, item_activate(storage, slot, guid_n(102)), true)}, now);
    }
};
static bool has_chest_in_bag(const std::vector<svc::InventoryItem> &items, int slot = 4) {
    for (const auto &it : items)
        if (it.item_record_id == fi::kChest && it.container == Bag && it.slot == slot)
            return true;
    return false;
}
static bool has_chest_worn(const std::vector<svc::InventoryItem> &items) {
    for (const auto &it : items)
        if (it.item_record_id == fi::kChest && it.container == Worn && it.slot == 2)
            return true;
    return false;
}
static void test_endpoint_saves_quit_and_timeout() {
    {
        Endpoints e;
        Rig &rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        e.use(j, kA, 4, 2, t0 + milliseconds(100));
        const PlayerSession *p = rig.ep.player_at(kA);
        CHECK(p && p->item_requests_applied() == 1 && p->inventory().at(Bag, 4) && e.api.inventory_count() == 0,
              "(a player in the world takes the chest off, in a datagram that holds ServerItemUsed and ServerActivateItem: one unequip, nothing saved yet)");
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + milliseconds(200));
        rig.ep.tick(t0 + milliseconds(250));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && s[0].id == kCyb && s[0].items.size() == 17 && has_chest_in_bag(s[0].items) && e.saver->inventories_saved() == 1,
              "the client quits: the character service gets the whole inventory, once: 17 items, the chest in the bag");
    }
    {
        Endpoints e;
        Rig &rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + milliseconds(200));
        rig.ep.tick(t0 + milliseconds(250));
        CHECK(rig.ep.session_count() == 0 && e.saves().empty() && e.api.inventory_call_count() == 0, "a client that joins and quits without touching an item saves no inventory");
    }
    {
        Endpoints e;
        Rig &rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        e.use(j, kA, 4, 2, t0 + milliseconds(100));
        rig.ep.tick(t0 + seconds(30));
        CHECK(rig.ep.session_count() == 1 && e.saves().empty(), "(30 s of silence: still there, nothing saved)");
        rig.ep.tick(t0 + seconds(70));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && has_chest_in_bag(s[0].items), "the client times out: its inventory is saved");
        g_log.str("");
    }
}
static void test_endpoint_saves_second_login_ok() {
    {
        Endpoints e;
        Rig &rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        e.use(a, kA, 4, 2, t0 + milliseconds(100));
        Joined b = handshake(rig, kB, t0 + milliseconds(200));
        join_world(rig, b, kB, cyb_record(), t0 + milliseconds(210));
        const PlayerSession *pb = rig.ep.player_at(kB);
        CHECK(pb && pb->inventory().at(Bag, 4) && pb->inventory().at(Bag, 4)->item_record_id == fi::kChest && !pb->inventory().at(Worn, 2),
              "the new session starts from the old one's items (the chest in the bag), not from the record the login read");
        CHECK(e.saves().size() == 1 && has_chest_in_bag(e.saves()[0].items), "and the old session's inventory was handed to the saver when the new one took over");
        rig.ep.tick(t0 + seconds(3));
        CHECK(rig.ep.session_count() == 1 && e.saves().size() == 1, "A's removal saves nothing more, and B saves nothing yet");
        const PlayerSession *p2 = rig.ep.player_at(kB);
        (void)p2;
        e.use(b, kB, 3, 4, t0 + seconds(4));
        exchange(rig, b, kB, {b.client->close_channel()}, t0 + seconds(5));
        rig.ep.tick(t0 + seconds(6));
        const auto s = e.saves();
        CHECK(s.size() == 2 && has_chest_in_bag(s[0].items) && has_chest_worn(s[1].items), "B's own inventory lands after A's: the chest in the bag, then worn");
    }
}
static void test_endpoint_saves_second_login_unavailable() {
    {
        Endpoints e;
        Rig &rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        e.use(a, kA, 4, 2, t0 + milliseconds(100));
        e.api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Unavailable};
        Joined b = handshake(rig, kB, t0 + milliseconds(200));
        join_world(rig, b, kB, cyb_record(), t0 + milliseconds(210));
        e.saver->flush(seconds(3));
        CHECK(e.api.inventory_call_count() == 3 && e.api.inventory_count() == 0 && e.saver->failed() == 1, "(A's final save: three Unavailable answers, given up on; the service holds nothing of the chest in the bag)");
        const PlayerSession *pb = rig.ep.player_at(kB);
        CHECK(pb && pb->inventory().at(Bag, 4) && pb->inventory_changed() && !pb->inventory_dirty() && pb->inventories_handed_over() == 0,
              "B started from items that differ from the record's: marked changed this session (and nothing handed over yet)");
        rig.ep.tick(t0 + seconds(3));
        exchange(rig, b, kB, {b.client->close_channel()}, t0 + seconds(5));
        rig.ep.tick(t0 + seconds(6));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && has_chest_in_bag(s[0].items) && s[0].items.size() == 17,
              "B quits having touched nothing: the carried inventory is saved again, and the service has it (17 items, the chest in the bag)");
    }
}
static void test_endpoint_saves_unchanged_and_shutdown() {
    {
        Endpoints e;
        Rig &rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        Joined b = handshake(rig, kB, t0 + milliseconds(200));
        join_world(rig, b, kB, cyb_record(), t0 + milliseconds(210));
        const PlayerSession *pb = rig.ep.player_at(kB);
        CHECK(pb && !pb->inventory_changed() && !pb->inventory_dirty(), "a second login over an unchanged inventory is not marked changed");
        rig.ep.tick(t0 + seconds(3));
        exchange(rig, b, kB, {b.client->close_channel()}, t0 + seconds(5));
        rig.ep.tick(t0 + seconds(6));
        CHECK(rig.ep.session_count() == 0 && e.saves().empty() && e.api.inventory_call_count() == 0, "and neither session saves an inventory");
    }
    {
        Endpoints e;
        Rig &rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        e.use(a, kA, 4, 2, t0 + milliseconds(100));
        rig.ep.kick_all(login_text::kShuttingDown, t0 + milliseconds(200));
        rig.ep.finish_players("worldd is stopping");
        const auto s = e.saves();
        CHECK(s.size() == 1 && has_chest_in_bag(s[0].items), "worldd stops: finish_players saves the open session's inventory");
        rig.ep.tick(t0 + seconds(5));
        CHECK(rig.ep.session_count() == 0 && e.saves().size() == 1, "and the session's removal afterwards saves nothing again");
    }
}
static void test_endpoint_saves_periodic_and_errors() {
    {
        Endpoints e;
        Rig &rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        e.use(j, kA, 4, 2, t0 + milliseconds(100));
        size_t saves_at_30 = 0;
        for (int sec = 1; sec <= 65; ++sec) {
            exchange(rig, j, kA, {}, t0 + seconds(sec));
            if (sec == 30)
                saves_at_30 = e.api.inventory_count() + e.saver->queued();
        }
        e.saver->flush(seconds(3));
        const auto s = e.api.saved_inventories();
        CHECK(saves_at_30 == 0 && s.size() == 1 && has_chest_in_bag(s[0].items), "an inventory that changed is saved once at about 60 s, not before, and not again while nothing changes");
    }
    {
        Endpoints e;
        Rig &rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        e.use(j, kA, 4, 2, t0 + milliseconds(100));
        e.api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Unavailable};
        for (int sec = 1; sec <= 65; ++sec) exchange(rig, j, kA, {}, t0 + seconds(sec));
        e.saver->flush(seconds(3));
        CHECK(e.api.inventory_call_count() == 3 && e.api.inventory_count() == 0 && e.saver->failed() == 1, "(the 60 s save: three Unavailable answers, given up on; the service holds nothing)");
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + seconds(66));
        rig.ep.tick(t0 + seconds(67));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && has_chest_in_bag(s[0].items), "the player quits standing still: the end of the session hands the inventory over again, and the service has it");
    }
    {
        Endpoints e;
        Rig &rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        e.use(j, kA, 4, 2, t0 + milliseconds(100));
        e.api.inventory_script = {svc::CallStatus::Refused};
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + milliseconds(200));
        rig.ep.tick(t0 + milliseconds(250));
        e.saver->flush(seconds(3));
        spdlog::default_logger()->flush();
        CHECK(e.api.inventory_call_count() == 1 && e.saver->failed() == 1 && g_log.str().find("could not be saved") != std::string::npos, "a refused inventory is one call, counted and logged: no retry");
        g_log.str("");
    }
}
static void test_endpoint_saves() {
    std::printf("through the endpoint: quit, timeout, a second login, shutdown and the 60 s tick save the inventory\n");
    test_endpoint_saves_quit_and_timeout();
    test_endpoint_saves_second_login_ok();
    test_endpoint_saves_second_login_unavailable();
    test_endpoint_saves_unchanged_and_shutdown();
    test_endpoint_saves_periodic_and_errors();
}
static void test_item_trigger() {
    std::printf("ServerTriggerAbility that names an item\n");
    constexpr int64_t kQuickWit = 6064630736949673984ll;
    Harness h;
    const emit::FIntrepidNetworkGUID ability = h.s->bootstrap().setup().guids.ability;
    const svc::InventoryItem *scepter = h.s->inventory().at(Worn, 15);
    CHECK(scepter != nullptr, "(the Bard wears the Scepter)");
    if (!scepter)
        return;
    const BunchWriter bunch = trigger_with_item(ability, kQuickWit, 3, items::to_instance(*scepter));
    CHECK(bunch.bit_pos() > 0, "(the trigger with the Scepter as its item writes)");
    const auto resolver = [&](const emit::FIntrepidNetworkGUID &g) -> const schema::ClassSchema * { return g == ability ? &class_named("UAoCAbilityComponent") : nullptr; };
    const auto parsed = repl::read_client_bunch(class_named("PlayerPawn_C"), resolver, bunch.data(), bunch.bit_pos(), client_options());
    const bool found = parsed && parsed->rpcs.size() == 1 && parsed->rpcs[0].call.decoded;
    CHECK(found, "it reads whole: the item is decoded, the call is not half-read any more");
    if (found) {
        const auto t = repl::parse_trigger_ability(parsed->rpcs[0].call);
        CHECK(t && t->ability_guid == kQuickWit && t->client_instance_id == 3 && t->item_slot == 15 && t->item_location == 4, "with the ability, the client's id, the item's slot 15 and its location 4");
    }
    h.request(bunch, h.pawn_ch);
    CHECK(h.s->ability_triggers() == 1 && h.s->ability_ends_sent() == 1 && !h.s->failed(), "the session handles it as any trigger: an ability worldd does not run is answered with ClientEndAbility");
    CHECK(h.s->item_requests_applied() == 0 && h.s->item_requests_refused() == 0, "(and it is no item request)");
}
static void test_real_content_db_chest_swap(Harness &h) {
    const size_t events = h.mirror.events.size();
    h.request_both(item_used(4, 2, guid_n(102)), item_activate(4, 2, guid_n(102)));
    CHECK(h.mirror.events.size() == events + 2 && h.s->inventory().at(Bag, 1) && h.s->inventory().at(Bag, 1)->item_record_id == fi::kChest && h.mirror.appearance.size() == 3 && mirror_matches(h) && gear_matches(h),
          "the chest comes off (the starter armor's bCanEquip is 0 in the design data and is not a rule): an Add to bag slot 1 and a Remove, the look loses a piece");
    h.step(milliseconds(150));
    h.request_both(item_used(3, 1, guid_n(102)), item_activate(3, 1, guid_n(102)));
    CHECK(h.s->inventory().at(Worn, 2) && !h.s->inventory().at(Bag, 1) && h.mirror.appearance.size() == 4 && mirror_matches(h) && gear_matches(h), "and goes on again, in Chest");
    h.step(milliseconds(150));
}
static void test_real_content_db_sword_swap(Harness &h) {
    const size_t before = h.mirror.events.size();
    h.request(item_used(3, 0, guid_n(200)), h.pawn_ch);
    CHECK(h.mirror.events.size() == before + 6 && h.s->inventory().at(Worn, 15) && h.s->inventory().at(Worn, 15)->item_record_id == fi::kSword2H && !h.s->inventory().at(Worn, 16) && mirror_matches(h) && gear_matches(h),
          "the real two-hander takes Primary and Secondary: six events, the Scepter and the Focus in the bag");
    CHECK(h.mirror.unreadable == 0 && !h.s->failed(), "everything the server sent reads");
}
static void test_real_content_db() {
    const char *real = std::getenv("AOC_CONTENT_DB");
    if (!real || !*real) {
        std::printf("the real content.db: skipped (AOC_CONTENT_DB is not set)\n");
        return;
    }
    std::printf("the real content.db (contentc's item and item_equip_slot rows)\n");
    svc::Character c = cyb_record();
    std::vector<svc::InventoryItem> items;
    for (const auto &it : c.items)
        if (it.container == Worn)
            items.push_back(it);
    items.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    c.items = items;
    Harness h(c, true, /*real_db=*/true);
    CHECK(h.mirror.unreadable == 0 && mirror_matches(h) && gear_matches(h) && h.mirror.held.size() == 23 && h.mirror.appearance.size() == 4, "the join: storages, HeldItems and the four AppearanceIDs are the kit's");
    test_real_content_db_chest_swap(h);
    test_real_content_db_sword_swap(h);
}
static std::vector<svc::InventoryItem> bard_all_in_bag() {
    std::vector<svc::InventoryItem> out;
    int slot = 0;
    for (auto it : bard_items()) {
        if (it.container != Worn)
            continue;
        it.container = Bag;
        it.slot = slot++;
        out.push_back(it);
    }
    return out;
}
static std::string describe_gear(const Harness &h) {
    std::string s = "held:";
    for (const auto &[k, r] : h.mirror.held_by_key())
        if (r)
            s += " " + std::to_string(k);
    s += " | appearance " + std::to_string(h.mirror.appearance.size()) + " | held_key " + std::to_string(h.mirror.held_key) + " updates " + std::to_string(h.mirror.held_updates.size());
    return s;
}
static void test_held_items_window_worn() {
    Harness h;
    const int key0 = h.mirror.held_key;
    CHECK(key0 > 0 && h.mirror.held.size() == 23, "(the join gave the client HeldItems at its initial key)");
    const size_t room = Connection::kMaxUnackedReliable - h.conn.unacked_reliable(h.pawn_ch);
    clog_channel(h, h.pawn_ch, room);
    CHECK(h.conn.unacked_reliable(h.pawn_ch) == Connection::kMaxUnackedReliable, "(the pawn channel's window is full)");
    h.client_sends({h.client.actor(h.pawn_ch, item_used(4, 2, guid_n(102)), true)});
    CHECK(h.s->item_requests_applied() == 1 && h.s->replies_pending() == 1 && h.mirror.held_key == key0, "the chest comes off: the events went out on the PlayerController's channel, the pawn's update waits for the window");
    h.settle();
    CHECK(h.s->replies_pending() == 0 && !h.s->failed(), "once the client acknowledges, the update goes out");
    CHECK(h.mirror.held_key == key0 + 23, (std::string("the HeldItems key moved once, by 23: ") + std::to_string(h.mirror.held_key - key0)).c_str());
    CHECK(mirror_matches(h) && gear_matches(h) && h.mirror.unreadable == 0, "and the client's storages and gear are the inventory's (the chest in the bag, not shown)");
    size_t held_updates_after = 0;
    for (const auto &u : h.mirror.held_updates) held_updates_after += u.first > key0 ? 1 : 0;
    CHECK(held_updates_after == 1, "exactly one HeldItems update reached the client");
}
static svc::InventoryItem find_bag_chest(const Harness &h) {
    svc::InventoryItem chest;
    for (const auto &it : h.s->inventory().items()) {
        if (it.item_record_id == fi::kChest) {
            chest = it;
        }
    }
    return chest;
}
static bool naked_chest_equip_settled_ok(const Harness &h) {
    return h.s->replies_pending() == 0 && h.s->item_requests_applied() == 1 && h.mirror.held.size() == 23 && h.mirror.unreadable == 0 && mirror_matches(h) && gear_matches(h);
}
static void equip_chest_from_naked_join(bool clog, int &key, size_t &updates, bool &fine) {
    Harness h(record_with(bard_all_in_bag()));
    if (!h.mirror.held.empty() || !h.mirror.held_updates.empty()) {
        fine = false;
    }
    if (clog) {
        clog_channel(h, h.pawn_ch, Connection::kMaxUnackedReliable - h.conn.unacked_reliable(h.pawn_ch));
    }
    const svc::InventoryItem chest = find_bag_chest(h);
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, chest.slot, chest.instance_guid), true)});
    if (clog && h.s->replies_pending() != 1) {
        fine = false;
    }
    h.settle();
    key = h.mirror.held_key;
    updates = h.mirror.held_updates.size();
    fine = fine && naked_chest_equip_settled_ok(h);
}
static void test_held_items_window_naked() {
    int free_key = 0, waited_key = 0;
    size_t free_updates = 0, waited_updates = 0;
    bool free_ok = true, waited_ok = true;
    equip_chest_from_naked_join(false, free_key, free_updates, free_ok);
    equip_chest_from_naked_join(true, waited_key, waited_updates, waited_ok);
    CHECK(free_ok && waited_ok, "the chest goes on after a join with nothing worn, with and without a full window: the client has HeldItems (23 entries) and the same storages and gear");
    CHECK(free_updates == 1 && waited_updates == 1 && waited_key == free_key && free_key > 0,
          (std::string("one update, and the same array key as without the wait: ") + std::to_string(waited_key) + " against " + std::to_string(free_key)).c_str());
}
static void test_held_items_update_waits_for_the_window() {
    std::printf("the HeldItems update waits for the pawn channel's window\n");
    test_held_items_window_worn();
    test_held_items_window_naked();
}
static void test_review_naked_join_then_equip() {
    std::printf("review: join with nothing worn, then equip every item (HeldItems was never sent at the join)\n");
    Harness h(record_with(bard_all_in_bag()));
    CHECK(h.mirror.unreadable == 0, "the join reads");
    CHECK(mirror_matches(h), "the join: the client's storages are the inventory");
    const auto start = h.s->inventory().items();
    int n = 0;
    bool all_match = true;
    for (const auto &it : start) {
        if (it.container != Bag)
            continue;
        const svc::InventoryItem *now = h.s->inventory().at(Bag, it.slot);
        if (!now || now->instance_guid != it.instance_guid)
            continue;
        h.request(item_used(3, it.slot, it.instance_guid), h.pawn_ch);
        ++n;
        if (!(mirror_matches(h) && gear_matches(h))) {
            all_match = false;
            std::printf("    equip %d (record %lld slot %d): client view differs; %s; unreadable=%zu\n", n, static_cast<long long>(it.item_record_id), it.slot, describe_gear(h).c_str(), h.mirror.unreadable);
            break;
        }
    }
    CHECK(n == 13 && h.s->inventory().count(Worn) == 13, "all 13 were worn");
    CHECK(all_match, "after every equip from a naked join the client's storages and gear equal the inventory");
    CHECK(h.mirror.unreadable == 0, "everything the server sent reads");
}
static bool unequip_all_worn_items(Harness &h) {
    std::vector<svc::InventoryItem> worn;
    for (const auto &it : h.s->inventory().items()) {
        if (it.container == Worn)
            worn.push_back(it);
    }
    for (const auto &it : worn) {
        h.request(item_used(4, it.slot, it.instance_guid), h.pawn_ch);
        if (!(mirror_matches(h) && gear_matches(h))) {
            std::printf("    unequip of slot %d: client differs\n", it.slot);
            return false;
        }
    }
    return true;
}
static bool equip_all_bag_items(Harness &h) {
    const auto bag = h.s->inventory().items();
    for (const auto &it : bag) {
        if (it.container != Bag)
            continue;
        const svc::InventoryItem *now = h.s->inventory().at(Bag, it.slot);
        if (!now || now->instance_guid != it.instance_guid)
            continue;
        h.request(item_used(3, it.slot, it.instance_guid), h.pawn_ch);
        if (!(mirror_matches(h) && gear_matches(h))) {
            std::printf("    equip of slot %d: client differs\n", it.slot);
            return false;
        }
    }
    return true;
}
static void test_review_unequip_everything_and_back() {
    std::printf("review: take everything off, put it all on again, twice\n");
    Harness h;
    bool ok = true;
    for (int round = 0; round < 2 && ok; ++round) {
        ok = unequip_all_worn_items(h) && equip_all_bag_items(h);
    }
    CHECK(ok, "the client's view equals the inventory through two rounds of everything off and on");
    CHECK(h.mirror.unreadable == 0, "everything the server sent reads");
}
static void test_review_full_bag() {
    std::printf("review: a full bag: unequip refused, a two-hander over two items refused, nothing changes\n");
    std::vector<svc::InventoryItem> items;
    for (auto it : bard_items())
        if (it.container == Worn)
            items.push_back(it);
    for (int s = 0; s < 70; ++s) items.push_back(make_item(Bag, s, s == 0 ? fi::kSword2H : fi::kPotion, 1000 + s));
    Harness h(record_with(items));
    CHECK(mirror_matches(h), "the join");
    const auto before = h.s->inventory().items();
    h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
    CHECK(h.s->inventory().items() == before && h.s->inventory().at(Worn, 2) && mirror_matches(h), "unequip with a full bag refused, nothing changed, the client agrees");
    h.request(item_used(3, 0, guid_n(1000)), h.pawn_ch);
    CHECK(h.s->inventory().items() == before && h.s->inventory().at(Bag, 0) && h.s->inventory().at(Worn, 15)->item_record_id == fi::kScepter && mirror_matches(h),
          "a two-hander that needs two bag slots with one free is refused, nothing changed");
    CHECK(h.mirror.unreadable == 0 && !h.s->failed(), "everything the server sent reads");
}
static void test_review_empty_character() {
    std::printf("review: a character with no items: every request is refused and the session lives\n");
    Harness h(record_with({}));
    h.request(item_used(3, 0, guid_n(1)), h.pawn_ch);
    h.request(item_used(4, 2, guid_n(1)), h.pawn_ch);
    h.request(bag_move(h, 0, 1, guid_n(1), 5), h.pc_ch);
    CHECK(!h.s->failed() && h.s->item_requests_applied() == 0 && h.s->inventory().size() == 0, "refused, nothing applied");
    h.s->finish("probe");
    CHECK(h.sink.inventories.empty(), "an empty inventory is never handed to the saver (it would be refused)");
}
static void test_review_saves_after_equip() {
    std::printf("review: the periodic save and the end-of-session save carry the whole set\n");
    Harness h;
    h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
    h.step(seconds(61));
    CHECK(h.sink.inventories.size() == 1 && h.sink.inventories[0].items.size() == 17, "one save of 17 items at the 60 s tick");
    h.request(item_used(3, 4, guid_n(102)), h.pawn_ch);
    h.s->finish("probe");
    CHECK(h.sink.inventories.size() == 2 && h.sink.inventories[1].items.size() == 17 && h.sink.inventories[1].items != h.sink.inventories[0].items, "the final save is the latest");
    h.s->finish("again");
    CHECK(h.sink.inventories.size() == 2, "finish is idempotent");
    const size_t n = h.sink.inventories.size();
    h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
    h.step(seconds(61));
    CHECK(h.sink.inventories.size() == n, "a request after finish is not saved (a new session carries what it has)");
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    test_endpoint_saves();
    test_item_trigger();
    test_held_items_update_waits_for_the_window();
    test_review_naked_join_then_equip();
    test_review_unequip_everything_and_back();
    test_review_full_bag();
    test_review_empty_character();
    test_review_saves_after_equip();
    test_real_content_db();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
