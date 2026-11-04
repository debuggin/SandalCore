#include "test_worldd_ability_session_common.h"
#include "combat_rig.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "spawns/area.h"
using namespace aoc::worldd;
static constexpr int64_t kCopper = 109162169652920ll, kOak = 109162169652924ll, kGlintT1 = 6064628753953390592ll;
static constexpr int64_t kMount = 107411158881912ll, kSanctusBag = 6064629382556287782ll;
static const char *kAftermathNote = "(plan E1, stage 3, part 2)";
static BunchWriter trigger_at_object(const bs::PlayerSetup &setup, int64_t guid, int32_t id, const emit::FIntrepidNetworkGUID &object) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    if (!rpc) return BunchWriter();
    repl::RpcParams p;
    repl::ArrayValue targets;
    repl::ElementState e;
    e.values[1] = repl::Value(object);
    targets.elements.push_back(std::move(e));
    repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (id != 0) repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1));
    repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{}));
    repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    return ability_rpc(setup.guids.ability, "ServerTriggerAbility", p);
}
static BunchWriter hit_request_at_object(const bs::PlayerSetup &setup, int64_t ability, int64_t targeting, int64_t hit, const emit::FIntrepidNetworkGUID &object) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("RequestHitVerification_Server");
    repl::HitRequest req;
    repl::HitComponentRef c;
    c.object = object;
    c.server_timestamp = 1.0f;
    req.components.push_back(c);
    req.ability_guid = ability;
    req.targeting_guid = targeting;
    req.hit_guid = hit;
    const auto params = rpc ? repl::hit_request_params(*rpc, req) : std::nullopt;
    return params ? ability_rpc(setup.guids.ability, "RequestHitVerification_Server", *params) : BunchWriter();
}
static BunchWriter actor_bunch(const schema::ClassSchema &cls, const char *name, const repl::RpcParams &params) {
    BunchWriter out, field;
    const schema::RpcSchema *rpc = cls.rpc(name);
    bool failed = false;
    if (!rpc || (repl::write_rpc_field(cls, *rpc, params, field, client_options(), &failed) == 0 && !params.values.empty()) || failed) return out;
    repl::write_content_block(out, repl::BlockHeader{}, field, false);
    return out;
}
static BunchWriter interact_bunch(uint64_t component) {
    const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema *rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest q{{component, kServerId, kRandomizer}, 1};
    const auto params = rpc ? repl::interact_params(*rpc, q) : std::nullopt;
    return params ? actor_bunch(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}
static BunchWriter loot_bunch(const std::vector<repl::ItemStorageEvent> &events) {
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema *rpc = pc.rpc("ServerRequestLootStorageEvents");
    repl::RpcParams p;
    repl::ArrayValue arr;
    for (const repl::ItemStorageEvent &e : events) {
        repl::ElementState es;
        es.values[1] = repl::Value(e);
        arr.elements.push_back(std::move(es));
    }
    if (!rpc || !repl::set_rpc_param(*rpc, p, "ItemEvents", repl::Value(std::move(arr)))) return BunchWriter();
    return actor_bunch(pc, "ServerRequestLootStorageEvents", p);
}
static repl::ItemStorageEvent take_event(const repl::LootPayload &w, size_t slot, const emit::FIntrepidNetworkGUID &container, const bs::PlayerSetup &setup) {
    repl::ItemStorageEvent e;
    e.type = static_cast<uint8_t>(repl::StorageEventType::Fill);
    e.move_source.object = container;
    e.item = w.data.storage[slot].item;
    e.move_source.slots.push_back({static_cast<int32_t>(slot), static_cast<int32_t>(e.item.quantity), e.item.guid});
    e.targets.push_back({setup.guids.backpack, {}});
    return e;
}
static BunchWriter respawn_choice_bunch(int64_t record) {
    const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema *rpc = pawn.rpc("SendServerRespawnChoice");
    repl::RpcParams p;
    return (rpc && repl::set_rpc_param(*rpc, p, "RespawnChoice", repl::Value::i64(record))) ? actor_bunch(pawn, "SendServerRespawnChoice", p) : BunchWriter();
}
static void send_on_pc(Run &r, const BunchWriter &b) {
    if (b.bit_pos() == 0) return;
    r.h.client_sends({r.h.client.actor(r.h.pc_ch, b, /*reliable=*/true)});
    r.h.flush();
    r.absorb();
}
static void send_on_pawn(Run &r, const BunchWriter &b) {
    if (b.bit_pos() != 0) r.send_reliable(b);
}
static void world_play(Run &r, double seconds) {
    const int n = static_cast<int>(std::llround(seconds * 10));
    for (int i = 0; i < n; ++i) {
        r.play(0.1, false);
        r.h.fx.world->tick_world(r.now());
    }
}
struct WolfPack {
    std::vector<uint64_t> wolves;
    uint64_t far_wolf = 0, next_actor_id = 5000000;
    bool ground = false;
    int first_level = 8;
};
static void spawn_wolves(PlayerWorld &w, WolfPack &pack, size_t count, int level, double aggro_first) {
    std::string err, path;
    auto nav = w.load_blueprints(NPC_MOTION_PATH, err) ? nav::AreaNav::from_file(navfix::build_file(), &err) : nullptr;
    pack.ground = nav && w.adopt_ground(std::move(nav), err);
    auto captured = pack.ground ? spawns::CapturedData::load(err) : std::nullopt;
    if (!captured) return;
    for (const auto &[p, row] : w.blueprints().rows()) {
        if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) path = p;
    }
    for (size_t i = 0; i < count; ++i) {
        const double x = kX + 120 + static_cast<double>(i % 7) * 70.0;
        const double y = kY + (i == 0 ? 0.0 : (i < 7 ? -220.0 : 220.0));
        if (const uint64_t id = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, pack.next_actor_id, path, x, y, kZ, i == 0 ? pack.first_level : level, i == 0 ? aggro_first : 1.0)) pack.wolves.push_back(id);
    }
    pack.far_wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, pack.next_actor_id, path, kX + 3000, kY, kZ, level, 1.0);
}
static void kill_npc(Run &r, uint64_t npc, uint64_t by) {
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    r.h.fx.world->combat()->apply_hit_to_npc(npc, by, kill, r.now());
}
static svc::InventoryItem stack_of(int64_t record, int32_t slot, int32_t quantity, uint32_t serial, svc::ItemContainer c = svc::ItemContainer::Backpack) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0x5000u + serial, 0x11u, 0x22u, 0x33u);
    return {c, slot, record, buf, quantity, 1, 0.0f, 1700000000000ll};
}
static svc::Character bard25_bagged() {
    svc::Character c = bard25();
    c.items.push_back(stack_of(kSanctusBag, 23, 1, 90001, svc::ItemContainer::Equipment));
    return c;
}
static int64_t held_of(const PlayerSession &s, int64_t record) {
    int64_t n = 0;
    for (const svc::InventoryItem &it : s.inventory().items()) {
        if (it.item_record_id == record) n += it.quantity;
    }
    return n;
}
static emit::FIntrepidNetworkGUID cylinder_of(uint64_t npc) { return {npc + 4, kServerId, kRandomizer}; }
static void swing_axe_at(Run &r, const bs::PlayerSetup &setup, const abilities::AbilityDef &axe, int64_t targeting, const emit::FIntrepidNetworkGUID &target) {
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_object(setup, axe.row.guid, id, target);
    r.send_trigger(axe.row.guid, id, &trig);
    world_play(r, 0.6);
    r.send_reliable(hit_request_at_object(setup, axe.row.guid, targeting, axe.hits[0].hit_guid, target));
    world_play(r, 0.6);
}
static void verify_whole_fight_aggro(Run &r, const bs::PlayerSetup &setup, uint64_t wolf, const abilities::AbilityDef &axe, int64_t targeting) {
    combat::CombatWorld &cw = *r.h.fx.world->combat();
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    r.h.fx.world->tick_world(r.now());
    swing_axe_at(r, setup, axe, targeting, cylinder_of(wolf));
    const double hp0 = cw.health_of(wolf);
    double bitten_at = -1;
    const float full = r.m.stats[content::kGuidMaxHealth];
    for (int i = 0; i < 80 && bitten_at < 0; ++i) {
        world_play(r, 0.1);
        if (r.m.stats[content::kGuidHealth] < full) bitten_at = r.now();
    }
    CHECK(bitten_at >= 0 && hp0 < cw.max_health_of(wolf) + 1, "the Wolf, hurt by the player, comes for it and bites");
    CHECK(r.h.s->kills_rewarded() == 0 && after.stats().deaths == 0, "nothing has died yet");
}
static uint64_t verify_whole_fight_corpse_parts(Run &r, uint64_t looted, size_t with_loot) {
    if (!looted) return 0;
    const combat::Corpse *corpse = r.h.fx.world->aftermath()->corpse_of(looted);
    const world::WorldActor *actor = r.h.fx.world->registry().find(looted);
    CHECK(corpse && actor && corpse->parts && corpse->remaining() == corpse->loot.size() && !corpse->loot.empty(), "the loot corpse holds its stacks");
    size_t big_updates = 0;
    for (const Mirror::ChannelEvent &e : r.m.other) big_updates += (!e.open && !e.close && e.bits > 1000) ? 1 : 0;
    CHECK(big_updates >= with_loot, "each corpse with loot sent its three new components to the client (updates of the NPCs' channels, over 1,000 bits)");
    return (corpse && actor && corpse->parts) ? looted : 0;
}
static uint64_t verify_whole_fight_kills(Run &r, const std::vector<uint64_t> &wolves, uint64_t pawn) {
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    const int64_t xp_before = r.h.s->progress().xp, debt_before = r.h.s->progress().debt;
    const int level_before = r.h.s->progress().level;
    kill_npc(r, wolves[0], pawn);
    world_play(r, 0.5);
    CHECK(r.h.s->kills_rewarded() == 1, "the killing blow: the session was given the kill's experience once");
    CHECK(r.h.s->progress().xp > xp_before || r.h.s->progress().level > level_before || r.h.s->progress().debt < debt_before, "the character's Adventuring experience grew (or paid down its debt: D1's rule)");
    CHECK(r.h.s->progress().debt <= debt_before, "(no debt from a kill)");
    CHECK(r.m.xp_updates >= 1, "the XPTracker was updated to the client");
    const combat::Corpse *c0 = after.corpse_of(wolves[0]);
    CHECK(c0 != nullptr && c0->rights.count(pawn) == 1, "the Wolf is a corpse and the player has the right to its loot");
    for (size_t i = 1; i < wolves.size(); ++i) kill_npc(r, wolves[i], pawn);
    world_play(r, 0.5);
    CHECK(r.h.s->kills_rewarded() == wolves.size() && after.stats().deaths == wolves.size(), "fourteen kills, fourteen rewards");
    uint64_t looted = 0;
    size_t with_loot = 0;
    for (const uint64_t w : wolves) {
        if (const combat::Corpse *c = after.corpse_of(w); c && c->parts) {
            ++with_loot;
            if (!looted) looted = w;
        }
    }
    CHECK(looted != 0, "some corpse has loot (the Wolves' tables give Glint about 38 times in a hundred)");
    CHECK(with_loot < wolves.size(), "and some have none");
    CHECK(r.m.unreadable == 0 && !r.h.s->abilities_failed(), "(every chain reads, the session is fine)");
    return verify_whole_fight_corpse_parts(r, looted, with_loot);
}
static bool loot_slot_matches(const repl::StorageSlot &s, const combat::LootStack &exp, int32_t idx) {
    if (!s.item.has_item || s.slot != idx || !s.check_loot_permission || s.loot_permission.size() != 1) return false;
    return s.item.record_id == static_cast<uint64_t>(exp.item) && s.item.quantity == static_cast<uint32_t>(exp.quantity) && s.item.quality == static_cast<uint32_t>(exp.quality);
}
static void verify_whole_fight_loot_window(Run &r, const bs::PlayerSetup &setup, uint64_t looted) {
    const combat::Corpse *corpse = r.h.fx.world->aftermath()->corpse_of(looted);
    const world::WorldActor *actor = r.h.fx.world->registry().find(looted);
    const std::vector<combat::LootStack> expect = corpse->loot;
    const size_t refused0 = r.h.s->loot_refused();
    send_on_pawn(r, interact_bunch(0xdeadbeefull));
    world_play(r, 0.2);
    CHECK(r.h.s->loot_opens() == 0 && r.m.loot_windows.empty(), "an interaction with a component that is none opens nothing");
    repl::LootPayload fake;
    fake.data.storage.resize(1);
    fake.data.storage[0].item = {true, static_cast<uint64_t>(kGlintT1), 3};
    send_on_pc(r, loot_bunch({take_event(fake, 0, corpse->parts->container_guid, setup)}));
    world_play(r, 0.2);
    CHECK(r.h.s->loot_refused() == refused0 + 1 && r.h.s->loot_taken() == 0 && r.m.storage_events.empty(), "a take with no window open is refused, nothing leaves the corpse");
    send_on_pawn(r, interact_bunch(corpse->parts->interactable));
    world_play(r, 0.3);
    CHECK(r.h.s->loot_opens() == 1 && r.m.loot_windows.size() == 1 && r.h.s->loot_actor() == looted, "interacting with the corpse's InteractableComponent opens the loot window");
    if (r.m.loot_windows.empty()) return;
    const repl::LootPayload win = r.m.loot_windows.back();
    CHECK(win.actor == actor->actor.guid, "the window names the corpse's actor (LootActor)");
    CHECK(win.data.storage.size() == expect.size() && win.data.client_retrieve_only, "one slot per stack, retrieve-only (retail: bClientRetrieveOnly 1)");
    bool shapes = win.data.storage.size() == expect.size();
    for (size_t i = 0; shapes && i < expect.size(); ++i) shapes = loot_slot_matches(win.data.storage[i], expect[i], static_cast<int32_t>(i));
    CHECK(shapes, "each slot is the rolled item, its quantity and quality, with the loot permission of the looting player (retail: bCheckItemLootPermission 1)");
    CHECK(win.allocation.default_method == 1 && win.allocation.by_rarity[0] == 0, "LootAllocationMethods: Default 1, the rest 0 (retail's)");
}
static void verify_whole_fight_hostile_loot(Run &r, const bs::PlayerSetup &setup, uint64_t looted, const repl::LootPayload win) {
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    const combat::Corpse *corpse = after.corpse_of(looted);
    const emit::FIntrepidNetworkGUID container_guid = corpse->parts->container_guid;
    const uint64_t interactable = corpse->parts->interactable;
    const size_t expect_n = corpse->loot.size();
    const auto refused_by = [&](const std::vector<repl::ItemStorageEvent> &evs, const char *what) {
        const size_t before = r.h.s->loot_refused();
        send_on_pc(r, loot_bunch(evs));
        world_play(r, 0.2);
        CHECK(r.h.s->loot_refused() == before + 1 && r.h.s->loot_taken() == 0 && r.m.storage_events.empty() && after.corpse_of(looted)->remaining() == expect_n, what);
    };
    const auto forged_ev = [&](auto tweak) {
        auto e = take_event(win, 0, container_guid, setup);
        tweak(e);
        return std::vector<repl::ItemStorageEvent>{e};
    };
    refused_by(forged_ev([](auto &e) { e.move_source.object.ObjectId += 2; }), "an event that names another container is refused");
    refused_by(forged_ev([](auto &e) { e.move_source.slots[0].slot = 99; }), "an event that names a slot that is none is refused");
    refused_by(forged_ev([](auto &e) { e.item.record_id += 1; }), "an event whose item is not the stack of its slot is refused");
    refused_by(forged_ev([](auto &e) { e.item.guid.a ^= 0xffu; }), "an event whose instance guid is forged is refused");
    repl::ItemStorageEvent light{.move_source = {.object = container_guid}, .type = static_cast<uint8_t>(repl::StorageEventType::Fill)};
    refused_by(std::vector<repl::ItemStorageEvent>(17, light), "seventeen events in one request are refused");
    refused_by({}, "a request with no events is refused");
    const size_t opens = r.h.s->loot_opens();
    for (int i = 0; i < 40; ++i) send_on_pawn(r, interact_bunch(interactable));
    CHECK(r.h.s->loot_opens() - opens <= PlayerSession::kLootBurst + 2, "forty interactions at once open the window at most the burst allows");
    const auto bad = forged_ev([](auto &e) { e.item.record_id += 1; });
    const size_t refused_before = r.h.s->loot_refused();
    for (int i = 0; i < 40; ++i) send_on_pc(r, loot_bunch(bad));
    CHECK(r.h.s->loot_refused() - refused_before <= PlayerSession::kLootBurst + 2 && r.h.s->loot_taken() == 0 && !r.h.s->abilities_failed(), "forty forged takes at once: the budget drops the rest, nothing is taken");
    world_play(r, 1.5);
    CHECK(r.h.s->loot_actor() == looted, "(the window is still the corpse's)");
}
static void verify_whole_fight_take_events(Run &r, uint64_t container_id, size_t expect_n, size_t events_before, size_t calls_before) {
    int removes = 0, adds = 0;
    size_t last_remove = 0, first_add = ~size_t{0};
    for (size_t i = events_before; i < r.m.storage_events.size(); ++i) {
        const repl::ItemStorageEvent &e = r.m.storage_events[i];
        if (e.type == static_cast<uint8_t>(repl::StorageEventType::Remove) && !e.targets.empty() && e.targets[0].object.ObjectId == container_id) {
            removes += static_cast<int>(e.targets[0].slots.size());
            last_remove = i;
        } else if (e.type == static_cast<uint8_t>(repl::StorageEventType::Add) && ++adds == 1) {
            first_add = i;
        }
    }
    CHECK(removes == static_cast<int>(expect_n) && adds >= 1, "the client was told: the stacks removed from the loot container, then added to the bag [retail's order]");
    CHECK(first_add != ~size_t{0} && first_add > last_remove, "(the removals come before the adds)");
    int listener_at = -1, last_event_at = -1;
    for (size_t i = calls_before; i < r.m.pc_calls.size(); ++i) {
        if (r.m.pc_calls[i] == "ClientRemoveListenerFromStorage") listener_at = static_cast<int>(i);
        if (r.m.pc_calls[i] == "ClientReceiveStorageEvent") last_event_at = static_cast<int>(i);
    }
    CHECK(r.m.listeners_removed == 1 && listener_at > last_event_at && r.h.s->loot_actor() == 0, "the window is closed with ClientRemoveListenerFromStorage after the last event, once");
}
static bool verify_whole_fight_real_take(Run &r, const bs::PlayerSetup &setup, uint64_t looted, std::vector<repl::ItemStorageEvent> &out_all) {
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    const combat::Corpse *corpse = after.corpse_of(looted);
    const uint64_t container_id = corpse->parts->container;
    const emit::FIntrepidNetworkGUID container_guid = corpse->parts->container_guid;
    const std::vector<combat::LootStack> expect = corpse->loot;
    const repl::LootPayload win2 = r.m.loot_windows.back();
    for (size_t i = 0; i < expect.size(); ++i) out_all.push_back(take_event(win2, i, container_guid, setup));
    repl::ItemStorageEvent forged = take_event(win2, 0, container_guid, setup);
    forged.item.record_id += 1;
    out_all.insert(out_all.begin(), forged);
    out_all.push_back(take_event(win2, 0, container_guid, setup));
    std::map<int64_t, int64_t> held_before, want;
    for (const combat::LootStack &s : expect) {
        held_before[s.item] = held_of(*r.h.s, s.item);
        want[s.item] += s.quantity;
    }
    const size_t events_before = r.m.storage_events.size(), calls_before = r.m.pc_calls.size();
    send_on_pc(r, loot_bunch(out_all));
    world_play(r, 0.3);
    CHECK(r.h.s->loot_taken() == expect.size() && after.corpse_of(looted) && after.corpse_of(looted)->remaining() == 0, "taking every slot empties the corpse");
    bool grew = true;
    for (const auto &[rec_id, n] : want) grew = grew && held_of(*r.h.s, rec_id) == held_before[rec_id] + n;
    CHECK(grew, "the bag holds what the corpse did (every stack's quantity added)");
    verify_whole_fight_take_events(r, container_id, expect.size(), events_before, calls_before);
    return grew;
}
static void verify_bare_corpse_close(Run &r, const std::vector<uint64_t> &wolves, uint64_t looted, double t_died) {
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    world::ActorRegistry &reg = r.h.fx.world->registry();
    uint64_t bare = 0;
    for (const uint64_t w : wolves) {
        const combat::Corpse *c = after.corpse_of(w);
        if (w != looted && c && !c->parts) bare = w;
    }
    CHECK(bare != 0 && reg.find(bare) != nullptr, "a corpse with no loot is still there 5 s after its death");
    world_play(r, 11.0);
    CHECK(bare != 0 && reg.find(bare) == nullptr && r.now() - t_died >= 15.0, "and it is closed 15 s after its death [R: 15.0 s]");
}
static void verify_whole_fight_corpse_close(Run &r, const std::vector<uint64_t> &wolves, uint64_t looted, double t_died, const std::vector<repl::ItemStorageEvent> &all) {
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    world::ActorRegistry &reg = r.h.fx.world->registry();
    const size_t expect_n = after.corpse_of(looted)->loot.size(), refused1 = r.h.s->loot_refused();
    const double t_take = r.now() - 0.3;
    send_on_pc(r, loot_bunch(all));
    world_play(r, 0.2);
    CHECK(r.h.s->loot_taken() == expect_n && r.h.s->loot_refused() == refused1 + 1, "the same request again: refused (the window is gone), no second helping");
    double closed_at = -1;
    for (int i = 0; i < 70 && closed_at < 0; ++i) {
        world_play(r, 0.1);
        if (!reg.find(looted)) closed_at = r.now() - t_take;
    }
    CHECK(closed_at >= 3.8 && closed_at <= 4.6, "the emptied corpse is closed about 4 s later [R: 3.97 to 4.08 s]");
    world_play(r, 0.5);
    bool closed_to_client = false;
    for (const Mirror::ChannelEvent &e : r.m.other) closed_to_client = closed_to_client || (e.close && e.at >= t_take);
    CHECK(closed_to_client, "its channel is closed to the client (Destroyed)");
    verify_bare_corpse_close(r, wolves, looted, t_died);
}
static void verify_whole_fight_hostile_hits(Run &r, const bs::PlayerSetup &setup, const WolfPack &pack, uint64_t looted, const abilities::AbilityDef &axe) {
    combat::CombatWorld &cw = *r.h.fx.world->combat();
    int64_t targeting = 0;
    for (const content::TargetingRow &t : axe.targetings) {
        if (t.key == axe.hits[0].key) targeting = t.guid;
    }
    const uint64_t dead = pack.wolves[0] == looted ? pack.wolves[1] : pack.wolves[0];
    const size_t rewards = r.h.s->kills_rewarded();
    const float my_hp = r.m.stats[content::kGuidHealth];
    swing_axe_at(r, setup, axe, targeting, cylinder_of(dead));
    CHECK(r.h.s->kills_rewarded() == rewards && cw.health_of(dead) <= 0, "a hit on a dead creature changes nothing and pays nothing a second time");
    const double far_hp = cw.health_of(pack.far_wolf);
    swing_axe_at(r, setup, axe, targeting, cylinder_of(pack.far_wolf));
    CHECK(cw.health_of(pack.far_wolf) == far_hp, "a hit on a Wolf 30 m away (out of the axe's range) takes none of its Health");
    emit::FIntrepidNetworkGUID forged = cylinder_of(pack.wolves[3]);
    forged.ObjectId += 0x7777777;
    swing_axe_at(r, setup, axe, targeting, forged);
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "a trigger and a hit on an object that is no actor: the session is fine");
    swing_axe_at(r, setup, axe, targeting, setup.guids.collision_cylinder);
    CHECK(r.m.stats[content::kGuidHealth] >= my_hp - 0.5f && !r.h.s->dead(), "an axe swing at the player's own cylinder (not a hostile target) hurts nobody");
    CHECK(r.h.s->kills_rewarded() == rewards, "(no kill came of any of them)");
}
static void test_e1_whole_fight() {
    std::printf("E1 stage 3, part 2: a whole fight through the session (aggro, hits, kill, experience, loot, corpse) %s\n", kAftermathNote);
    const svc::Character rec = bard25_bagged();
    WolfPack pack;
    Run r(0, true, &rec, [&](PlayerWorld &w) { spawn_wolves(w, pack, 14, 24, 800); });
    CHECK(pack.ground && pack.wolves.size() == 14 && pack.far_wolf != 0 && r.h.fx.world->combat() && r.h.fx.world->aftermath(),
          "a world with ground, fourteen Wolves of level 24 around the player (the first one aggressive), one 30 m away, and the aftermath");
    if (!pack.ground || pack.wolves.size() != 14 || !r.h.fx.world->aftermath()) return;
    r.play(0.5, false);
    send_on_pawn(r, actor_bunch(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}));
    world_play(r, 1.0);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const abilities::AbilityDef *axe = r.h.fx.world->ability_catalog()->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe != nullptr && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    int64_t targeting = 0;
    for (const content::TargetingRow &t : axe->targetings) {
        if (t.key == axe->hits[0].key) targeting = t.guid;
    }
    r.h.s->teach_ability(axe->row.guid);
    verify_whole_fight_aggro(r, setup, pack.wolves[0], *axe, targeting);
    const uint64_t looted = verify_whole_fight_kills(r, pack.wolves, setup.guids.pawn.ObjectId);
    if (!looted) return;
    const double t_died = r.h.fx.world->aftermath()->corpse_of(looted)->died_at;
    verify_whole_fight_loot_window(r, setup, looted);
    if (r.m.loot_windows.empty()) return;
    verify_whole_fight_hostile_loot(r, setup, looted, r.m.loot_windows.back());
    std::vector<repl::ItemStorageEvent> all;
    verify_whole_fight_real_take(r, setup, looted, all);
    verify_whole_fight_corpse_close(r, pack.wolves, looted, t_died, all);
    verify_whole_fight_hostile_hits(r, setup, pack, looted, *axe);
}
static void verify_full_bag_loot_refused(Run &r, const bs::PlayerSetup &setup, uint64_t looted) {
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    const combat::Corpse *c = after.corpse_of(looted);
    const size_t stacks = c->loot.size();
    send_on_pawn(r, interact_bunch(c->parts->interactable));
    world_play(r, 0.3);
    CHECK(r.m.loot_windows.size() == 1, "the window opens");
    if (r.m.loot_windows.empty()) return;
    std::vector<repl::ItemStorageEvent> all;
    for (size_t i = 0; i < stacks; ++i) all.push_back(take_event(r.m.loot_windows.back(), i, c->parts->container_guid, setup));
    send_on_pc(r, loot_bunch(all));
    world_play(r, 0.3);
    CHECK(r.h.s->loot_taken() == 0 && after.corpse_of(looted)->remaining() == stacks, "taking with no room takes nothing: every stack stays in the corpse");
    CHECK(r.m.storage_events.empty() && r.m.listeners_removed == 0 && r.h.s->loot_actor() == looted, "the client was told of no removal, the window stays open");
    CHECK(r.h.s->inventory().count(svc::ItemContainer::Backpack) == 70 && r.h.s->inventory().count(svc::ItemContainer::Resource0) == 24 && !r.h.s->abilities_failed(), "the bags are as they were");
}
static void test_e1_loot_with_full_bag() {
    std::printf("E1 stage 3, part 2: a full bag leaves the loot in the corpse %s\n", kAftermathNote);
    svc::Character rec = bard25_bagged();
    for (int i = 0; i < 24; ++i) rec.items.push_back(stack_of(kGlintT1, i, 10, static_cast<uint32_t>(i), svc::ItemContainer::Resource0));
    for (int i = 0; i < 70; ++i) rec.items.push_back(stack_of(kCopper, i, 20, 100 + static_cast<uint32_t>(i)));
    WolfPack pack;
    Run r(0, true, &rec, [&](PlayerWorld &w) { spawn_wolves(w, pack, 14, 24, 0); });
    CHECK(pack.ground && pack.wolves.size() == 14 && r.h.fx.world->aftermath(), "a world with Wolves and a character whose bag is full");
    if (pack.wolves.size() != 14) return;
    CHECK(r.h.s->inventory().count(svc::ItemContainer::Backpack) == 70 && r.h.s->inventory().count(svc::ItemContainer::Resource0) == 24, "the Backpack holds seventy stacks and Resource0 24: no room anywhere");
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    r.play(0.5, false);
    send_on_pawn(r, actor_bunch(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}));
    world_play(r, 1.0);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    uint64_t looted = 0;
    for (const uint64_t w : pack.wolves) {
        kill_npc(r, w, setup.guids.pawn.ObjectId);
        world_play(r, 0.2);
        const combat::Corpse *c = after.corpse_of(w);
        if (c && c->parts && !looted) looted = w;
    }
    CHECK(looted != 0, "a kill leaves loot");
    if (looted) verify_full_bag_loot_refused(r, setup, looted);
}
static double verify_player_death_frame(Run &r, uint64_t wolf, uint64_t pawn, int64_t debt0) {
    combat::PlayerPort *port = r.h.fx.world->combat()->find_player(pawn);
    CHECK(port != nullptr, "the combat world knows the player");
    if (!port) return -1.0;
    const double t_death = r.now();
    combat::IncomingHit lethal{.instigator = wolf, .hit_guid = 1, .mods = {{1, content::kGuidHealth, -1.0e9}}};
    port->receive_hit(lethal, t_death);
    r.ts = 0.0f;
    world_play(r, 0.6);
    CHECK(r.h.s->dead() && r.h.s->deaths() == 1, "the session reads the Health at 0: the player is dead");
    CHECK(r.m.stats[content::kGuidHealth] <= 0.0f && r.m.stats[content::kGuidMana] <= 0.0f, "the client's Health and Mana are 0");
    CHECK(r.m.death_locations.size() == 1 && std::fabs(r.m.death_locations[0][0] - kX) < 100 && std::fabs(r.m.death_locations[0][1] - kY) < 100, "ClientSetLastDeathLocation names where the player fell");
    size_t restarts = 0, cams = 0;
    for (const std::string &n : r.m.pc_calls) {
        restarts += n == "ClientRestart" ? 1 : 0;
        cams += n == "ClientSetCameraMode" ? 1 : 0;
    }
    CHECK(restarts == 2 && cams == 2, "the PlayerController got ClientRestart and ClientSetCameraMode twice (the retail death frame)");
    CHECK(r.h.s->progress().debt > debt0 && r.m.xp_updates >= 1, "the death put experience debt on the character and the client's XPTracker says so");
    CHECK(held_of(*r.h.s, kCopper) == 10 && held_of(*r.h.s, kOak) == 3 && held_of(*r.h.s, kGlintT1) == 2 && held_of(*r.h.s, kMount) == 1,
          "about half of each stack of materials is gone (10 copper, 3 oak, 2 Glint certificates), the mount is not a material");
    return t_death;
}
static void verify_dead_player_ignored(Run &r, uint64_t wolf, const combat::Corpse *pc, double t_death) {
    bool opened = false;
    for (const Mirror::ChannelEvent &e : r.m.other) opened = opened || (e.open && e.at >= t_death);
    CHECK(opened, "the client was sent the pile's actor");
    world_play(r, 3.0);
    const auto ai = r.h.fx.world->combat()->ai_state(wolf);
    CHECK(ai.has_value() && *ai != npc::ai::State::Attack && *ai != npc::ai::State::Chase, "the Wolf that killed the player stops attacking it");
    const size_t opens0 = r.h.s->loot_opens();
    if (pc && pc->parts) send_on_pawn(r, interact_bunch(pc->parts->interactable));
    world_play(r, 0.2);
    CHECK(r.h.s->loot_opens() == opens0, "a dead player's interaction is ignored");
    r.send_trigger(kSprint, r.next_id++);
    world_play(r, 0.5);
    CHECK(!r.m.has_ability(kSprint), "a dead player's trigger starts nothing");
}
static uint64_t verify_player_death_pile_and_ai(Run &r, uint64_t wolf, double t_death) {
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    const uint64_t pile = r.h.s->pile_actor();
    const world::WorldActor *pa = pile ? r.h.fx.world->registry().find(pile) : nullptr;
    CHECK(pa != nullptr && pa->kind == world::ActorKind::Pile && std::hypot(pa->x - kX, pa->y - kY) < 100, "a loot pile stands where the player fell");
    const combat::Corpse *pc = pile ? after.corpse_of(pile) : nullptr;
    int64_t pile_copper = 0, pile_oak = 0;
    if (pc) {
        for (const combat::LootStack &s : pc->loot) {
            pile_copper += s.item == kCopper ? s.quantity : 0;
            pile_oak += s.item == kOak ? s.quantity : 0;
        }
    }
    CHECK(pc != nullptr && pc->is_pile && pile_copper == 10 && pile_oak == 4, "the pile holds the other half (10 copper, 4 oak, 3 Glint certificates)");
    verify_dead_player_ignored(r, wolf, pc, t_death);
    return pile;
}
static bool verify_player_respawn_and_clock(Run &r, int64_t &out_record) {
    send_on_pawn(r, actor_bunch(class_named("PlayerPawn_C"), "RequestServerRespawnChoices", {}));
    world_play(r, 0.3);
    CHECK(r.m.choices.size() == 1 && r.m.choices[0].closest.size() == 1 && r.m.choices[0].home == r.m.choices[0].closest[0] && !r.m.choices[0].previous_invalid,
          "RequestServerRespawnChoices is answered with the places the player may come back (one) and the home");
    if (r.m.choices.empty() || r.m.choices[0].closest.empty()) return false;
    out_record = r.m.choices[0].closest[0];
    send_on_pawn(r, respawn_choice_bunch(out_record + 12345));
    world_play(r, 0.3);
    CHECK(r.h.s->dead() && r.m.choices.size() == 2 && r.m.choices[1].previous_invalid, "a choice the server did not offer: the player stays dead and the choices come again, marked invalid");
    send_on_pawn(r, respawn_choice_bunch(0));
    world_play(r, 0.3);
    CHECK(r.h.s->dead() && r.h.s->respawns() == 0, "a choice of 0: still dead");
    send_on_pawn(r, respawn_choice_bunch(out_record));
    world_play(r, 0.5);
    CHECK(!r.h.s->dead() && r.h.s->respawns() == 1, "the offered choice brings the player back");
    const PlayerWorld::RespawnSpot spot = r.h.fx.world->respawn_spot(kX, kY);
    const GoodPosition &good = r.h.s->movement().good();
    CHECK(std::fabs(good.x - spot.x) < 1 && std::fabs(good.y - spot.y) < 1, "at the respawn point (the login spot when no shrine is nearer in the area)");
    CHECK(r.m.stats[content::kGuidHealth] >= r.m.stats[content::kGuidMaxHealth] - 0.5f && r.m.stats[content::kGuidMana] >= r.m.stats[content::kGuidMaxMana] - 0.5f, "with full Health and Mana");
    const size_t acked = r.h.s->moves_acknowledged(), stale = r.h.s->moves_stale();
    for (int i = 0; i < 10; ++i) {
        r.ts += 0.1f;
        repl::NetworkMove mv = move_at(r.ts, spot.x, true);
        mv.location = repl::quantize_vector(100, spot.x + 10.0 * (i + 1), spot.y, spot.z);
        r.h.client_sends({r.h.client.actor(r.h.pawn_ch, client_move(mv), /*reliable=*/false)});
        r.h.step(milliseconds(100));
        r.absorb();
    }
    CHECK(r.h.s->moves_acknowledged() >= acked + 9 && r.h.s->moves_stale() == stale, "the moves after the respawn, at the restarted clock's TimeStamps, are acknowledged (the death forgot the old clock)");
    CHECK(r.h.s->movement().good().time_stamp < 10.0f && r.h.s->movement().good().x > spot.x + 50.0, "and the player walks on from the shrine on the new clock: the last good move is the client's (TimeStamp under 10 s, "
                                                                                                     "not over 100), not the shrine's");
    return true;
}
static void verify_player_pile_recovery(Run &r, const bs::PlayerSetup &setup, uint64_t pile, int64_t record) {
    combat::Aftermath &after = *r.h.fx.world->aftermath();
    const world::WorldActor *pa = pile ? r.h.fx.world->registry().find(pile) : nullptr;
    const combat::Corpse *pc = pile ? after.corpse_of(pile) : nullptr;
    const PlayerWorld::RespawnSpot spot = r.h.fx.world->respawn_spot(kX, kY);
    const size_t choices_n = r.m.choices.size();
    send_on_pawn(r, respawn_choice_bunch(record));
    send_on_pawn(r, actor_bunch(class_named("PlayerPawn_C"), "RequestServerRespawnChoices", {}));
    world_play(r, 0.3);
    CHECK(r.h.s->respawns() == 1 && r.m.choices.size() == choices_n, "choices of a player who is alive are ignored");
    if (pc && pc->parts && pa && std::hypot(spot.x - pa->x, spot.y - pa->y) < PlayerSession::kInteractRangeCm - 100) {
        const size_t windows = r.m.loot_windows.size();
        send_on_pawn(r, interact_bunch(pc->parts->interactable));
        world_play(r, 0.3);
        CHECK(r.m.loot_windows.size() == windows + 1, "the pile's window opens for its owner");
        if (r.m.loot_windows.size() == windows + 1) {
            const repl::LootPayload w = r.m.loot_windows.back();
            std::vector<repl::ItemStorageEvent> all;
            for (size_t i = 0; i < w.data.storage.size(); ++i) all.push_back(take_event(w, i, pc->parts->container_guid, setup));
            send_on_pc(r, loot_bunch(all));
            world_play(r, 0.3);
            CHECK(held_of(*r.h.s, kCopper) == 20 && held_of(*r.h.s, kOak) == 7 && held_of(*r.h.s, kGlintT1) == 5, "taking the pile gives back the materials (20 copper, 7 oak, 5 Glint)");
            CHECK(r.m.listeners_removed == 1 && after.corpse_of(pile) && after.corpse_of(pile)->emptied, "the emptied pile closes its window and is marked emptied");
        }
    } else {
        CHECK(false, "(the respawn point is within reach of the pile)");
    }
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(the session is fine, every chain reads)");
}
static void test_e1_player_death() {
    std::printf("E1 stage 3, part 2: the player's death and respawn through the session %s\n", kAftermathNote);
    svc::Character rec = bard25_bagged();
    rec.items.push_back(stack_of(kCopper, 0, 20, 1));
    rec.items.push_back(stack_of(kOak, 1, 7, 2));
    rec.items.push_back(stack_of(kGlintT1, 2, 5, 3));
    rec.items.push_back(stack_of(kMount, 3, 1, 4));
    WolfPack pack;
    Run r(0, true, &rec, [&](PlayerWorld &w) { spawn_wolves(w, pack, 2, 24, 800); });
    CHECK(pack.ground && pack.wolves.size() == 2 && r.h.fx.world->aftermath(), "a world with a Wolf at the player's side and a character with copper, oak and Glint in the bag");
    if (!pack.ground || pack.wolves.size() != 2 || !r.h.fx.world->aftermath()) return;
    r.play(0.5, false);
    send_on_pawn(r, actor_bunch(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}));
    world_play(r, 1.0);
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const int64_t debt0 = r.h.s->progress().debt;
    CHECK(held_of(*r.h.s, kCopper) == 20 && held_of(*r.h.s, kOak) == 7 && held_of(*r.h.s, kGlintT1) == 5 && held_of(*r.h.s, kMount) == 1, "the bag holds 20 copper, 7 oak, 5 Glint and a mount");
    CHECK(r.h.s->movement().good().has_time_stamp && r.h.s->movement().good().time_stamp > 100.0f, "(the client's movement clock is past 100 s: that is the checker's last good TimeStamp)");
    const double t_death = verify_player_death_frame(r, pack.wolves[0], setup.guids.pawn.ObjectId, debt0);
    if (t_death < 0.0) return;
    const uint64_t pile = verify_player_death_pile_and_ai(r, pack.wolves[0], t_death);
    int64_t record = 0;
    if (!verify_player_respawn_and_clock(r, record)) return;
    verify_player_pile_recovery(r, setup, pile, record);
}
static void test_e1_death_without_loss() {
    std::printf("E1 stage 3, part 2: with --no-death-loss a death costs experience only %s\n", kAftermathNote);
    svc::Character rec = bard25();
    rec.items.push_back(stack_of(kCopper, 0, 20, 1));
    WolfPack pack;
    Run r(0, true, &rec, [&](PlayerWorld &w) {
        combat::AftermathConfig cfg = w.aftermath_config();
        cfg.death_loss = false;
        w.set_aftermath_config(cfg);
        spawn_wolves(w, pack, 1, 24, 0);
    });
    if (!pack.ground || pack.wolves.empty() || !r.h.fx.world->combat()) {
        CHECK(false, "the world");
        return;
    }
    r.play(0.5, false);
    combat::PlayerPort *port = r.h.fx.world->combat()->find_player(r.h.s->bootstrap().setup().guids.pawn.ObjectId);
    combat::IncomingHit lethal{.instigator = pack.wolves[0], .hit_guid = 1, .mods = {{1, content::kGuidHealth, -1.0e9}}};
    if (port) port->receive_hit(lethal, r.now());
    world_play(r, 0.6);
    CHECK(r.h.s->dead() && r.h.s->progress().debt > 0, "the player died with debt");
    CHECK(held_of(*r.h.s, kCopper) == 20 && r.h.s->pile_actor() == 0, "the copper is all there and no pile stands");
}
int main() {
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    if (ability_rig::real_db_present()) {
        test_e1_whole_fight();
        test_e1_loot_with_full_bag();
        test_e1_player_death();
        test_e1_death_without_loss();
    } else {
        std::printf("no content.db at %s: the plan D2 session tests are skipped\n", ability_rig::real_db_path().c_str());
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
