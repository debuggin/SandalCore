// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_mounts_ride_common.h"

using namespace aoc::worldd;
using namespace aoc::protocol;
using std::chrono::milliseconds;
namespace bs = aoc::worldd::bootstrap;

namespace {

void verify_roads_transition_and_dismount(Harness& h, Rider& r, uint32_t mch, const content::MountData& data) {
    for (int i = 0; i < 10 && h.s->ride()->road_tier() != 4; ++i) ride_step(h, r, 118.8, 0);
    CHECK(h.s->ride()->road_tier() == 4 && std::fabs(mult_of(*h.mount()).value_or(0) - 1.3) < 1e-6, "on the tier-4 crossing MoveSpeedMult is 1.3");
    const auto fx4 = items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects");
    CHECK(fx4.size() == 1 && data.road(4) && fx4[0].values.at(2).get_if<int64_t>() && *fx4[0].values.at(2).get_if<int64_t>() == data.road(4)->effect, "the tier-2 item is replaced by the tier-4's");
    for (int i = 0; i < 20; ++i) ride_step(h, r, 99, 0);
    CHECK(h.s->ride()->road_tier() == -1 && mult_of(*h.mount()).value_or(0) == 1.0 && items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects").empty(), "off the road the effect goes, MoveSpeedMult is 1");
    h.run(milliseconds(1200));
    CHECK(h.s->ride()->speed_limit(1, 1000) < 1500, "(the old limit is gone a second later)");
    size_t cur_effect_updates = 0;
    for (const fake::Delivered* d : chains_on(h, mch, 0)) {
        for (const auto& [id, o] : states_of(*d, h.mount()->actor)) {
            const schema::NetField* fe = o.schema ? o.schema->field("CurrentEffects") : nullptr;
            if (fe && o.arrays.find(fe->index)) ++cur_effect_updates;
        }
    }
    CHECK(cur_effect_updates >= 3, "the mount's channel carried the effects' items (added, replaced, removed)");
    get_off(h);
    h.run(milliseconds(100));
    CHECK(items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects").empty(), "nothing is left on the mount after the dismount");
}

void test_roads() {
    std::printf("roads\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    const world::WorldActor* m0 = h.mount();
    CHECK(m0 && h.s->mounts()->phase() == MountController::Phase::Out, "(the mount is out)");
    if (!m0) return;
    world::RoadMap::Polyline road;
    road.tier = 2;
    road.half_width_cm = 300;
    road.xy = {m0->x - 500, m0->y, m0->x + 3000, m0->y};
    CHECK(h.fx.world->roads().add(road), "(a road is added)");
    world::RoadMap::Polyline road4;
    road4.tier = 4;
    road4.half_width_cm = 300;
    road4.xy = {m0->x + 3000, m0->y - 3000, m0->x + 3000, m0->y + 3000};
    CHECK(h.fx.world->roads().add(road4), "(a second road is added)");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(the player rides)");
    if (!h.s->mounted()) return;
    const uint32_t mch = h.mount_ch();
    const content::MountData& data = h.fx.world->mounts()->data();
    Rider r = start_riding(h);
    const auto fx = items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects");
    CHECK(fx.size() == 1 && data.road(2) && fx[0].values.at(2).get_if<int64_t>() && *fx[0].values.at(2).get_if<int64_t>() == data.road(2)->effect, "the mount starts on the tier-2 road: Road_T2_Speed_Mount is on it");
    CHECK(fx[0].values.at(4).get_if<float>() && *fx[0].values.at(4).get_if<float>() > 3.0e38f, "the road's effect has no end (Duration FLT_MAX)");
    CHECK(std::fabs(mult_of(*h.mount()).value_or(0) - 1.2) < 1e-6 && h.s->ride()->road_tier() == 2, "MoveSpeedMult is 1.2 (990 -> 1,188 cm/s) [R: 959]");
    CHECK(std::fabs(h.s->ride()->speed_limit(1, 0) - 990 * 1.2 * 1.5) < 1.0, "the move limit is 990 x 1.2 x 1.5");
    const size_t refused = h.s->moves_refused();
    for (int i = 0; i < 20; ++i) ride_step(h, r, 118.8, 0);
    CHECK(h.s->moves_refused() == refused, "20 moves at 1,188 cm/s along the road are accepted");
    verify_roads_transition_and_dismount(h, r, mch, data);
}

void verify_mounted_desummon_order(int order) {
    Harness g(cyb_record());
    g.play();
    CHECK(ride_on(g), "(the player rides)");
    if (!g.s->mounted()) return;
    const size_t from = g.got.size();
    const svc::InventoryItem* item = g.s->inventory().at(Bag, kMountSlot);
    const std::string guid = item ? item->instance_guid : std::string();
    std::vector<fake::Bunch> bunches;
    bunches.push_back(g.client.actor(g.pawn_ch, item_used(3, kMountSlot, guid), true));
    if (order == 0) {
        bunches.push_back(g.client.actor(g.pc_ch, dismount_rpc(), true));
        bunches.push_back(g.client.actor(g.pc_ch, desummon_rpc(), true));
    } else {
        bunches.push_back(g.client.actor(g.pc_ch, desummon_rpc(), true));
        bunches.push_back(g.client.actor(g.pc_ch, dismount_rpc(), true));
    }
    g.client_sends(std::move(bunches));
    g.run(milliseconds(300));
    std::vector<std::string> seq;
    for (const fake::Delivered* d : chains_on(g, g.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) {
            if (c.rpc && (std::string(c.rpc->name) == "ClientRestart" || std::string(c.rpc->name) == "ClientSetCameraMode")) seq.push_back(c.rpc->name);
        }
    }
    const bool two = seq == std::vector<std::string>({"ClientRestart", "ClientSetCameraMode", "ClientRestart", "ClientSetCameraMode"});
    CHECK(two, order == 0 ? "dismount then desummon in one packet: two ClientRestarts and two camera modes (893 packet 142)" : "desummon then dismount in one packet: the same two (893)");
    CHECK(!g.s->mounted() && g.s->dismounts() == 1 && g.s->mounts()->phase() == MountController::Phase::Desummoning, "the rider is off and the mount dissolves");
    g.run(milliseconds(3600));
    CHECK(mount_count(g) == 0, "the mount is gone 3.2 s later");
}

void verify_mounted_desummon_orders() {
    for (const int order : {0, 1}) verify_mounted_desummon_order(order);
    Harness k(cyb_record());
    k.play();
    CHECK(ride_on(k), "(the player rides)");
    const size_t from = k.got.size();
    k.client_sends({k.client.actor(k.pc_ch, desummon_rpc(), true)});
    k.run(milliseconds(300));
    size_t restarts = 0;
    for (const fake::Delivered* d : chains_on(k, k.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) restarts += c.rpc && std::string(c.rpc->name) == "ClientRestart";
    }
    CHECK(restarts == 1 && !k.s->mounted() && k.s->mounts()->phase() == MountController::Phase::Desummoning, "a desummon of a ridden mount alone: one ClientRestart, the mount dissolves");
}

void test_repeats_and_desummon() {
    std::printf("getting on and off 16 times, and the mounted desummon\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    walk_up_to_mount(h);
    bool all = true;
    const uint32_t mch = h.mount_ch();
    for (int i = 0; i < 16; ++i) {
        get_on(h);
        h.run(milliseconds(120));
        all = all && h.s->mounted();
        Rider r = start_riding(h);
        for (int k = 0; k < 3; ++k) ride_step(h, r, 60, 0);
        get_off(h);
        h.run(milliseconds(120));
        all = all && !h.s->mounted();
        g_pawn_ts = 0.0f;
    }
    CHECK(all && h.s->mount_ups() == 16 && h.s->dismounts() == 16, "16 times on and off: every one takes");
    CHECK(!h.s->failed() && mount_count(h) == 1 && h.mount_ch() == mch, "the mount is the same one, on the same channel; the session lives");
    CHECK(h.s->mount_moves_acknowledged() == 48, "each ride's three moves were acknowledged (TimeStamp restarting each time)");
    verify_mounted_desummon_orders();
}

double pawn_health(const Harness& h) {
    const stats::StatValue* v = h.s->character_stats() ? h.s->character_stats()->find(content::kGuidHealth) : nullptr;
    return v ? v->value : -1;
}

void verify_mounted_death_and_lock(Harness& h, double rider_before) {
    const size_t from_death = h.got.size();
    const uint32_t mch = h.mount_ch();
    h.s->take_damage(h.conn, 10000.0, h.t);
    h.run(milliseconds(300));
    CHECK(h.s->mount_deaths() == 1 && !h.s->mounted() && h.s->dismounts() == 1, "the mount dies under its rider: the rider is dismounted");
    CHECK(pawn_health(h) == rider_before, "the rider's Health is still untouched");
    CHECK(h.mount() != nullptr && h.s->mounts()->phase() == MountController::Phase::Dying, "the dead mount is still there for a moment");
    const svc::InventoryItem* item = h.s->inventory().at(Bag, kMountSlot);
    const int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK(item && item->persisted_health && *item->persisted_health == 0.0f && item->lock_expiration >= now_unix + 290 && item->lock_expiration <= now_unix + 310,
          "the item keeps Health 0 and a lock of 300 s");
    size_t events = 0;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from_death)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) events += c.rpc && std::string(c.rpc->name) == "ClientReceiveStorageEvent";
    }
    CHECK(events == 2, "the client is told the item changed (a Remove and an Add)");
    CHECK(!h.sink.inventories.empty() && !h.s->inventory_dirty(), "the inventory is handed to the saver at once (a crash before the periodic save must not bring the mount back)");
    bool saved_dead = false;
    if (!h.sink.inventories.empty()) {
        for (const svc::InventoryItem& si : h.sink.inventories.back()) {
            if (si.container == Bag && si.slot == kMountSlot) saved_dead = si.persisted_health && *si.persisted_health == 0.0f && si.lock_expiration >= now_unix + 290;
        }
    }
    CHECK(saved_dead, "and what it holds is the mount item with Health 0 and its lock");
    h.run(milliseconds(1700));
    CHECK(mount_count(h) == 0 && h.s->mounts()->phase() == MountController::Phase::Idle, "the dead mount closes (Destroyed) 1.5 s later");
    bool closed = false;
    for (size_t i = from_death; i < h.got.size(); ++i) closed = closed || (h.got[i].channel == mch && h.got[i].close);
    CHECK(closed, "its channel is closed on the client");
    const size_t refused = h.s->mounts()->stats().refused;
    summon_with(h, kMountSlot, 31);
    h.run(milliseconds(1800));
    CHECK(h.s->mounts()->stats().refused == refused + 1 && mount_count(h) == 0, "a summon of the locked item is refused (the callout)");
    summon_with(h, kOtherMountSlot, 32);
    h.run(milliseconds(1900));
    CHECK(mount_count(h) == 1, "another mount item summons");
}

void test_mounted_combat() {
    std::printf("mounted combat: hits land on the mount, its death, the lock\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const double max_mount = h.s->ride()->max_health();
    const double mount_before = h.s->ride()->health();
    const double rider_before = pawn_health(h);
    CHECK(max_mount > 400 && max_mount < 600 && mount_before == max_mount, "(the mount's Health is Mount_Base_HP at the owner's level: 500 at level 1)");
    h.s->take_damage(h.conn, 120.0, h.t);
    h.run(milliseconds(100));
    CHECK(std::fabs(h.s->ride()->health() - (mount_before - 120)) < 0.01 && pawn_health(h) == rider_before, "120 damage comes off the mount's Health; the rider's is untouched");
    CHECK(h.s->mount_hits() == 1 && h.s->mounted(), "the rider stays on");
    const size_t from = h.got.size();
    combat::IncomingHit hit;
    hit.instigator = 0;
    hit.hit_guid = 6064632423136905040ll;
    hit.source_type = content::kSourceTypeAbility;
    hit.source_guid = 108436548376185ll;
    combat::ResolvedMod mod;
    mod.stat_mod_guid = 1;
    mod.stat_guid = content::kGuidHealth;
    mod.value = -30.0;
    hit.mods.push_back(mod);
    h.s->take_hit(h.conn, hit, h.t);
    h.run(milliseconds(200));
    CHECK(std::fabs(h.s->ride()->health() - (mount_before - 150)) < 0.01 && pawn_health(h) == rider_before, "an NPC's hit of 30 comes off the mount's Health too");
    size_t confirmations = 0;
    for (const fake::Delivered* d : chains_on(h, h.pc_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "AoCPlayerControllerBP_C")) confirmations += c.rpc && std::string(c.rpc->name) == "ClientHitConfirmationsReliable";
    }
    CHECK(confirmations == 1, "the client is sent the hit's confirmation (the floating number)");
    combat::IncomingHit miss = hit;
    miss.flags = stats::kHitMiss;
    h.s->take_hit(h.conn, miss, h.t);
    CHECK(std::fabs(h.s->ride()->health() - (mount_before - 150)) < 0.01, "a missed hit takes no Health");
    h.run(milliseconds(100));
    verify_mounted_death_and_lock(h, rider_before);
}

void test_health_comes_back_and_is_kept() {
    std::printf("a mount summoned again starts from the Health its item kept\n");
    svc::Character rec = cyb_record();
    for (svc::InventoryItem& it : rec.items) {
        if (it.container == Bag && it.slot == kMountSlot) it.persisted_health = 200.0f;
    }
    Harness h(rec);
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    CHECK(h.mount() && spawns::stat_value(*h.mount(), content::kGuidHealth).value_or(-1) == 200.0, "the mount starts at the item's Health (200 of 500)");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(150));
    h.s->take_damage(h.conn, 50.0, h.t);
    get_off(h);
    h.run(milliseconds(150));
    desummon_with(h, kMountSlot);
    h.run(milliseconds(300));
    const svc::InventoryItem* item = h.s->inventory().at(Bag, kMountSlot);
    CHECK(item && item->persisted_health && std::fabs(*item->persisted_health - 150.0f) < 0.01f, "desummoned, the item keeps the Health the mount had (150)");
    CHECK(item && item->lock_expiration == 0, "no lock: it did not die");
    h.run(milliseconds(3400));
    summon_with(h, kMountSlot, 41);
    h.run(milliseconds(1900));
    CHECK(h.mount() && spawns::stat_value(*h.mount(), content::kGuidHealth).value_or(-1) == 150.0, "summoned again it starts from 150");
    h.s->finish("the test");
    const svc::InventoryItem* after = h.s->inventory().at(Bag, kMountSlot);
    CHECK(after && after->persisted_health && *after->persisted_health == 150.0f, "(the session's end keeps the Health too)");
    CHECK(!h.sink.inventories.empty(), "and the inventory was handed to the saver");
}

void test_filter() {
    std::printf("the ability filter while mounted\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    const content::MountData& data = h.fx.world->mounts()->data();
    const content::MountAbilityRow* blink = nullptr;
    const content::MountAbilityRow* unstuck = nullptr;
    for (const content::MountAbilityRow& r : data.abilities) {
        if (r.name == "Mage_Blink") blink = &r;
        if (r.name == "General_Unstuck") unstuck = &r;
    }
    CHECK(blink && blink->auto_dismount && unstuck && unstuck->usable_while_mounted && !unstuck->auto_dismount, "(Mage_Blink dismounts, General_Unstuck is usable mounted, in the data)");
    if (!blink || !unstuck) return;
    const int64_t sprint = 6064629110265938944ll;
    const size_t ends = h.s->ability_ends_sent();
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, sprint, 51), true)});
    h.run(milliseconds(100));
    CHECK(h.s->mounted() && h.s->mount_triggers_refused() == 1 && h.s->ability_ends_sent() > ends, "an ability that is not usable while mounted is refused (ClientEndAbility) and the rider stays on");
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, unstuck->ability, 52), true)});
    h.run(milliseconds(100));
    CHECK(h.s->mounted() && h.s->mount_triggers_refused() == 1, "General_Unstuck is not refused by the filter and does not dismount");
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, blink->ability, 53), true)});
    h.run(milliseconds(150));
    CHECK(!h.s->mounted() && h.s->dismounts() == 1, "Mage_Blink (bAutoDismount) dismounts the rider first");
    const int64_t dash = h.fx.world->mounts()->dash_ability();
    const size_t refused = h.s->mount_triggers_refused();
    const size_t abilities_before = h.s->ability_triggers();
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, dash, 54), true)});
    h.run(milliseconds(100));
    CHECK(h.s->mount_triggers_refused() == refused + 1 && h.s->dashes_started() == 0 && h.s->ability_triggers() == abilities_before + 1, "the dash with nobody riding is refused and runs nowhere");
    const world::WorldActor* m = h.mount();
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, ability_component_of(*m), dash, 55), true)});
    h.run(milliseconds(100));
    CHECK(h.s->dashes_started() == 0, "(a dash on a mount nobody rides starts nothing)");
}

void test_death_and_relog() {
    std::printf("the player's death and a relog while mounted\n");
    {
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h), "(the player rides)");
        if (!h.s->mounted()) return;
        h.s->set_current_value(content::kGuidHealth, 0.0f);
        h.run(milliseconds(300));
        CHECK(h.s->dead() && !h.s->mounted() && h.s->dismounts() == 1, "the player's death dismounts it");
        CHECK(h.s->mounts()->phase() == MountController::Phase::Desummoning, "and its mount is desummoned");
        h.run(milliseconds(3400));
        CHECK(mount_count(h) == 0, "the mount is gone 3.2 s later");
    }
    {
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h), "(the player rides)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        for (int i = 0; i < 20; ++i) ride_step(h, r, 99, 0);
        const GoodPosition mounted_at = h.s->movement().good();
        const GoodPosition saved = h.s->last_position();
        const double yaw = mounted_at.yaw_degrees * 3.14159265358979 / 180.0;
        CHECK(std::fabs(saved.x - (mounted_at.x + 54 * std::cos(yaw))) < 1.0 && std::fabs(saved.y - (mounted_at.y + 54 * std::sin(yaw))) < 1.0 && std::fabs(saved.z - (mounted_at.z + 41)) < 1.0,
              "while it rides, the position to save is 54 cm ahead of the mount, on its feet");
        h.s->finish("the test");
        CHECK(!h.sink.positions.empty() && std::fabs(h.sink.positions.back().x - saved.x) < 0.01 && std::fabs(h.sink.positions.back().z - saved.z) < 0.01,
              "the session's end hands that position to the saver: it logs in dismounted at the mount's last good position");
        CHECK(h.fx.world->registry().count(world::ActorKind::Mount) == 0, "(the mount left the world with the session)");
    }
}

void test_other_mount_item() {
    std::printf("another mount item used while riding\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    summon_with(h, kOtherMountSlot, 61);
    h.run(milliseconds(300));
    CHECK(!h.s->mounted() && h.s->mounts()->phase() == MountController::Phase::Desummoning && h.s->mounts()->stats().summons == 1, "a different mount item: the rider is put off, the old mount is desummoned, nothing new is summoned");
    h.run(milliseconds(3400));
    CHECK(mount_count(h) == 0, "(the old one is gone)");
}

void test_flyers_and_aquatic_on_the_ground() {
    std::printf("a flying and an aquatic mount ridden on the ground\n");
    {
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(the player rides a flying mount)");
        if (!h.s->mounted()) return;
        const Ride* ride = h.s->ride();
        CHECK(std::fabs(ride->speed_limit(1, 0) - 990 * 1.5) < 1.0 && std::fabs(ride->speed_limit(5, 0) - 1290 * 1.5) < 1.0, "the ground limit is 990 x 1.5 and the flying limit (movement mode 5) 1,290 x 1.5");
        Rider r = start_riding(h);
        const size_t refused = h.s->moves_refused();
        for (int i = 0; i < 10; ++i) ride_step_mode(h, r, {70, 0, 100}, 5);
        CHECK(h.s->moves_refused() == refused, "a climb in flight mode (5) at 1,000 cm/s up and 700 along (3D 1,220) is taken (the flyer's own limits)");
        for (int i = 0; i < 10; ++i) ride_step_mode(h, r, {150, 0, 250}, 5);
        CHECK(h.s->moves_refused() > refused, "a climb at 2,500 cm/s up and 1,500 along (3D 2,915, past 1.5 x 1,290) is refused");
    }
    {
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h, kAquaticSlot), "(the player rides an aquatic mount)");
        if (!h.s->mounted()) return;
        const Ride* ride = h.s->ride();
        CHECK(ride->speed_limit(4, 0) > ride->speed_limit(1, 0) * 1.3, "its swimming limit (mode 4) is its water speed's, above its ground speed's");
    }
}

void test_hostile_other_player_mount() {
    Harness h(cyb_record());
    h.play();
    const spawns::MountBook& book = *h.fx.world->mounts();
    const content::MountRow* row = nullptr;
    for (const content::MountRow& r : book.data().mounts) {
        if (r.item == g_mount_record) row = &r;
    }
    spawns::MountParams p;
    p.player_id = 999;
    p.owner_guid = repl::GuidValue{1, 2, 3, 4};
    p.owner_name = "other";
    p.owner_level = 20;
    const GoodPosition g = h.s->movement().good();
    p.x = g.x + 200;
    p.y = g.y;
    p.z = g.z - 41;
    std::string error;
    auto made = row ? book.make(*row, p, [&] { return h.fx.world->mint_guid(); }, error) : std::nullopt;
    CHECK(made.has_value(), "(another player's mount is made)");
    if (made) {
        const emit::FIntrepidNetworkGUID comp = component_of(made->actor, made->actor.npc.interact_component);
        CHECK(h.fx.world->registry().add(std::move(made->actor)), "(it is added to the world)");
        h.run(milliseconds(300));
        h.client_sends({h.client.actor(h.pawn_ch, interact_with(comp), true)});
        h.run(milliseconds(100));
        CHECK(!h.s->mounted() && h.s->mount_interactions_refused() >= 1, "getting on another player's mount does nothing (the connection does not even have it)");
    }
}

void test_hostile_interaction_and_summon_spam() {
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const world::WorldActor* m = h.mount();
    const size_t ignored = h.s->interactions_ignored();
    h.client_sends({h.client.actor(h.pawn_ch, interact_with(component_of(*m, m->npc.interact_component)), true)});
    h.run(milliseconds(100));
    CHECK(h.s->mounted() && h.s->mount_ups() == 1 && h.s->interactions_ignored() == ignored + 1, "an interaction while mounted is ignored (it does not get on twice)");
    for (int i = 0; i < 40; ++i) {
        get_off(h);
        h.step(milliseconds(5));
    }
    CHECK(!h.s->mounted() && h.s->dismounts() == 1 && !h.s->failed(), "40 ServerDismountInteractions in 0.2 s dismount once and do no harm");
    for (int i = 0; i < 40; ++i) {
        get_on(h);
        h.step(milliseconds(5));
        get_off(h);
        h.step(milliseconds(5));
    }
    CHECK(!h.s->failed() && h.s->mount_ups() <= 1 + 10, "40 mount/dismount pairs in 0.4 s: the budget holds the rest back, the session lives");
    h.run(milliseconds(1000));
    if (!h.s->mounted()) {
        get_on(h);
        h.run(milliseconds(200));
    }
    for (int i = 0; i < 30; ++i) {
        summon_with(h, kMountSlot, 200 + i);
        h.client_sends({h.client.actor(h.pc_ch, desummon_rpc(), true)});
        h.step(milliseconds(10));
    }
    h.run(milliseconds(4500));
    CHECK(!h.s->failed() && !h.s->mounted() && mount_count(h) <= 1, "summon and desummon spam while riding settles: no mount ridden, at most one mount, no failure");
}

void test_hostile_bad_moves_and_junk() {
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h), "(the player rides)");
    if (!h.s->mounted()) return;
    const uint32_t mch = h.mount_ch();
    Rider r = start_riding(h);
    ride_step(h, r, 0, 0);
    const GoodPosition good = h.s->movement().good();
    move_mount(h, move_at(0.5f, r.x + 500000, r.y, r.z));
    move_mount(h, move_at(0.01f, r.x, r.y, r.z));
    move_mount(h, move_at(std::nanf(""), r.x, r.y, r.z));
    CHECK(std::fabs(h.s->movement().good().x - good.x) < 1.0 && std::fabs(h.mount()->x - good.x) < 1.0 && !h.s->failed(), "a far move, a TimeStamp that goes back and a NaN leave the mount where it was");
    emit::BunchWriter junk;
    for (int i = 0; i < 40; ++i) junk.write_bits(0x5a5a5a5a, 32);
    h.client_sends({h.client.actor(mch, junk, false)});
    h.run(milliseconds(100));
    CHECK(!h.s->failed() && h.s->mounted(), "a bunch of junk on the mount's channel is ignored");
}

void test_hostile() {
    std::printf("hostile requests\n");
    test_hostile_other_player_mount();
    test_hostile_interaction_and_summon_spam();
    test_hostile_bad_moves_and_junk();
}

bool call_names_instance(const repl::RpcCall& c, int32_t instance) {
    for (const auto& [handle, v] : c.params.values) {
        const auto* i = v.get_if<int64_t>();
        const auto* u = v.get_if<uint64_t>();
        if ((i && *i == instance) || (u && *u == static_cast<uint64_t>(instance))) return true;
    }
    return false;
}

size_t pawn_ends(const Harness& h, size_t from, int32_t instance, bool* named) {
    size_t n = 0;
    if (named) *named = false;
    for (const fake::Delivered* d : chains_on(h, h.pawn_ch, from)) {
        for (const repl::RpcCall& c : calls_of(*d, "UAoCAbilityComponent")) {
            if (!c.rpc || std::string(c.rpc->name) != "ClientEndAbility") continue;
            ++n;
            if (named && call_names_instance(c, instance)) *named = true;
        }
    }
    return n;
}

void test_sprint_does_not_ride() {
    std::printf("a Sprint does not ride: it ends at mount-up, nothing drains the mounted rider, the pawn's state is back at dismount (stage 3 fix 2)\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot);
    h.run(milliseconds(1900));
    CHECK(h.mount() != nullptr && h.s->mounts()->phase() == MountController::Phase::Out, "(the mount is out)");
    if (!h.mount()) return;
    const bs::SessionGuids& g = h.s->bootstrap().setup().guids;
    constexpr int64_t kMoveSpeedMult = 109228544644729ll;
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, content::kGuidSprint, 31), true)});
    h.run(milliseconds(100));
    const AbilityRuntime* ab = h.s->abilities();
    CHECK(ab && ab->sprinting(), "(the player sprints)");
    if (!ab) return;
    walk_up_to_mount(h);
    CHECK(ab->sprinting() && ab->stamina() < ab->max_stamina() && ab->stat(kMoveSpeedMult) > 1.1, "(it drains Stamina while the player moves, and MoveSpeedMult is up)");
    const size_t from = h.got.size();
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(the player is on the mount)");
    bool named = false;
    const size_t ends = pawn_ends(h, from, 31, &named);
    CHECK(!ab->sprinting() && ends >= 1 && named, "getting on ends the Sprint the way a Sprint ends: ClientEndAbility to the client's instance, on the pawn's AbilityComponent");
    CHECK(ab->stat(kMoveSpeedMult) == 1.0, "MoveSpeedMult is back to 1.0 on the pawn (the Sprint's effect is gone)");
    CHECK(ab->regen_pause_until() < 0, "no regeneration pause is put on a rider (the exertion effect is not applied at mount-up)");
    h.client_sends({h.client.actor(h.pc_ch, possession_ack(h.mount()->actor.guid), true)});
    h.run(milliseconds(100));

    const double at_mount = ab->stamina();
    Rider r = start_riding(h);
    for (int i = 0; i < 40; ++i) ride_step(h, r, 60, 0);
    CHECK(h.s->mounted() && ab->stamina() >= at_mount, "4 s of riding take no Stamina");
    const size_t from2 = h.got.size();
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, content::kGuidSprint, 32), true)});
    h.run(milliseconds(100));
    named = false;
    pawn_ends(h, from2, 32, &named);
    CHECK(!ab->sprinting() && named && h.s->mounted(), "a Sprint asked for while mounted is refused (ClientEndAbility) and does not start");
    for (int i = 0; i < 20; ++i) ride_step(h, r, 60, 0);
    CHECK(ab->stamina() >= at_mount, "(and the moves that follow take no Stamina)");

    get_off(h);
    h.run(milliseconds(200));
    CHECK(!h.s->mounted() && !ab->mounted(), "(the player is on foot)");
    h.client_sends({h.client.actor(h.pawn_ch, trigger_on(h, g.ability, content::kGuidSprint, 33), true)});
    h.run(milliseconds(100));
    CHECK(ab->sprinting(), "on foot again the Sprint starts: the normal state is back");
}

} // namespace

void run_mounts_ride_part2() {
    test_roads();
    test_repeats_and_desummon();
    test_mounted_combat();
    test_health_comes_back_and_is_kept();
    test_filter();
    test_death_and_relog();
    test_other_mount_item();
    test_flyers_and_aquatic_on_the_ground();
    test_sprint_does_not_ride();
    test_hostile();
}
