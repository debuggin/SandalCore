// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_hunting_session_common.h"

#include "capture_hunting.h"
#include "gather/node_wire.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

using namespace aoc::worldd;
using std::chrono::milliseconds;
using std::chrono::seconds;
using svc::ItemContainer;
namespace chunt = capture_hunting;

static size_t xp_chain_since(const Player& p, size_t from) {
    const bs::SessionGuids& g = p.s->bootstrap().setup().guids;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor && b.header.object == g.xp_tracker) return i;
        }
    }
    return static_cast<size_t>(-1);
}

static void check_hunt_start_channel(HuntRig& h, uint64_t comp, uint32_t ch, size_t from) {
    Player& p = *h.p;
    Rig& rig = h.rig;
    combat::Aftermath& after = *rig.world->aftermath();
    h.interact_with(p, h.wolf);
    CHECK(p.s->harvesting_node() == h.wolf && p.s->harvests_started() == 1 && p.s->harvest_refusals().count("alive") == 0,
          "the interaction with the LIVING creature starts the hunt (nothing is refused as alive)");
    rig.run(milliseconds(300));
    CHECK(!dead_for_clients(rig.world->registry().find(h.wolf)) && p.s->kills_rewarded() == 0 && after.stats().piles == 0,
          "it is still alive while the bar runs; no kill's experience, no pile");
    const auto set = channeling_seen(p, from);
    CHECK(set.size() == 1 && set[0].second.active && set[0].second.timed && set[0].second.duration > 14.0f &&
              set[0].second.duration < 15.01f &&
              set[0].second.tool_record == p.s->inventory().at(Worn, 38)->item_record_id &&
              set[0].second.target.ObjectId == rig.world->registry().find(h.wolf)->actor.guid.ObjectId,
          "the pawn's ChannelingData: the corpse, the Bow, the channel's length (15 s at rating 0)");
    const CorpseSeen early = corpse_seen(p, comp, ch, from);
    CHECK(!early.statuses.empty() && early.statuses[0] == 0x08000001u,
          "the corpse's interactable was told: the status 0x08000001 (counter 1, channeling)");
    CHECK(early.statuses.size() >= 3 && ((early.statuses[1] >> 27) & 31u) == 3u &&
              ((early.statuses[2] >> 27) & 31u) == 5u,
          "then heartbeats whose counter goes up by 2 (3, 5 ...)");
}

static void check_hunt_start(HuntRig& h, uint64_t comp, uint32_t ch, size_t from) {
    check_hunt_start_channel(h, comp, ch, from);
    Player& p = *h.p;
    Rig& rig = h.rig;
    combat::Aftermath& after = *rig.world->aftermath();
    CHECK(rig.board().state_of(h.wolf) == gather::NodeState::Channeling && after.corpse_of(h.wolf) &&
              after.corpse_of(h.wolf)->living && after.corpse_of(h.wolf)->close_at > rig.now() + 14.0,
          "the board has the creature Channeling, and it stays for the channel (its record is a living one)");
    CHECK(count_in_log("starts hunting Hunting_Wolf") == 1 && count_in_log("a Novice Hunting kill, with") == 1,
          "the log: starts hunting, a Novice Hunting kill, with the Bow");
    h.interact_with(p, h.wolf);
    CHECK(p.s->harvest_refusals().count("already channeling") == 1 && p.s->harvests_started() == 1,
          "a replayed interaction changes nothing (already channeling)");
}

static void check_hunt_completion_chains(const Player& p, size_t from) {
    const auto chains = pc_chain_calls(p, from);
    size_t durability_chain = static_cast<size_t>(-1);
    size_t event_chain = static_cast<size_t>(-1);
    for (const auto& [chain, name] : chains) {
        if (name == "ClientUpdateItemDurability" && durability_chain == static_cast<size_t>(-1)) durability_chain = chain;
        if (name == "ClientReceiveStorageEvent" && event_chain == static_cast<size_t>(-1)) event_chain = chain;
    }
    const auto channeling = channeling_seen(p, from);
    const size_t xp_chain = xp_chain_since(p, from);
    CHECK(durability_chain != static_cast<size_t>(-1) && event_chain != static_cast<size_t>(-1) &&
              durability_chain <= event_chain,
          "the result frame: the Bow's wear, then the storage event(s)");
    CHECK(channeling.size() == 2 && channeling[1].second.flags == 0x106 && !channeling[1].second.active &&
              channeling[1].first > event_chain && channeling[1].first < xp_chain,
          "the pawn's ChannelingData is cleared after the items and before the XPTracker [R: 1457 packet 995]");
    CHECK(xp_chain != static_cast<size_t>(-1) && xp_chain > durability_chain,
          "and the XPTracker (Hunter XP) closes the frame");
}

static void check_hunt_completion(HuntRig& h, size_t from, float wear_before, int64_t xp_before) {
    Player& p = *h.p;
    Rig& rig = h.rig;
    CHECK(run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20)),
          "the hunt completes by the clock, about 15 s after");
    rig.run(milliseconds(300));
    check_hunt_completion_chains(p, from);
    const svc::InventoryItem* bow = p.s->inventory().at(Worn, 38);
    CHECK(bow && bow->durability == wear_before - 1.0f, "the Bow lost 1 durability");
    const int64_t gained = p.s->professions().xp(hid::kHunting) - xp_before;
    CHECK(gained >= 160 && gained % 80 == 0, "Hunter XP of 160 for a Common item, 80 more for each better rarity");
    const int64_t carcass = 6064630406019547147ll;
    const int64_t trainable = 6064629326302543894ll;
    int64_t held = 0;
    int64_t in_bag = 0;
    for (const svc::InventoryItem& it : p.s->inventory().items()) {
        if (it.item_record_id != carcass && it.item_record_id != trainable) continue;
        if (it.container == R0 || it.container == Bag) held += it.quantity;
        if (it.container == Bag) in_bag += it.quantity;
    }
    CHECK(held >= 1 && in_bag == 0, "a carcass or a trainable is in Resource0 (and none in the Backpack)");
    CHECK(count_in_log("hunted Hunting_Wolf") == 1 || count_in_log("[hunt] cyb: harvested Hunting_Wolf") == 1,
          "the log: harvested Hunting_Wolf ...");
}

static void check_hunt_done_wire(const Player& p, uint64_t comp, uint32_t ch, size_t from) {
    const CorpseSeen done = corpse_seen(p, comp, ch, from);
    CHECK(!done.statuses.empty() && (done.statuses.back() & 7u) == 4u && done.statuses.back() >> 27 != 0,
          "the corpse's last update has the status of phase 4 (done)");
    bool can_ok = !done.can_input.empty();
    for (const auto& c : done.can_input) can_ok = can_ok && c.size() == 4;
    CHECK(can_ok && done.can_input.back()[0] && !*done.can_input.back()[0],
          "CanInputActivate was told with four entries, the first false (never an empty array)");
    CHECK(done.last_states.size() == 4 && done.last_states[0] == 0 && done.last_states[1] == 0 &&
              done.last_states[2] == 0 && done.last_states[3] == 0,
          "and all four interaction states 0");
}

static void check_hunt_done_update(HuntRig& h, uint64_t comp, uint32_t ch, size_t from) {
    check_hunt_done_wire(*h.p, comp, ch, from);
    Player& p = *h.p;
    Rig& rig = h.rig;
    combat::Aftermath& after = *rig.world->aftermath();
    CHECK(!gather::node_gathered(*rig.world->registry().find(h.wolf)) &&
              rig.board().state_of(h.wolf) == gather::NodeState::Gathered,
          "the creature is Gathered for the board and has no bGathered");
    CHECK(dead_for_clients(rig.world->registry().find(h.wolf)) && after.corpse_of(h.wolf) &&
              !after.corpse_of(h.wolf)->living && after.corpse_of(h.wolf)->hunted,
          "the done update killed it for its clients (bIsDead), and its record is a hunted one now");
    CHECK(p.s->harvesting_node() == 0 && p.s->loot_opens() == 0,
          "this player hunts nothing now; no loot window was ever opened");
    h.interact_with(p, h.wolf);
    CHECK(p.s->harvest_refusals().count("corpse hunted") == 1 && p.s->harvests_started() == 1,
          "the corpse is hunted already: refused");
    CHECK(after.corpse_of(h.wolf) && after.corpse_of(h.wolf)->hunted &&
              after.corpse_of(h.wolf)->close_at <= rig.now() + 4.0 + 0.5,
          "the corpse goes at most 4 s after the hunt");
    rig.run(seconds(5));
    CHECK(rig.world->registry().find(h.wolf) == nullptr && after.corpse_of(h.wolf) == nullptr &&
              rig.board().state_of(h.wolf) == gather::NodeState::Available,
          "5 s later the corpse is gone, and the board forgot it");
}

static void test_a_hunt() {
    std::printf("a creature from its living interactable to the corpse's end: a Hunting_Wolf, Novice, the Bow\n");
    HuntRig h;
    if (!h.open()) return;
    Player& p = *h.p;
    Rig& rig = h.rig;
    const world::WorldActor* body = rig.world->registry().find(h.wolf);
    CHECK(body && body->actor.subobjects.size() == 7 && body->npc.interact_def != 0 &&
              rig.g().hunt_kind(body->npc.interact_def) != nullptr,
          "the Wolf has the eight blocks of a huntable and its hunting kind");
    const uint64_t comp = rig.component_of(h.wolf);
    const uint32_t ch = p.s->actor_view() ? p.s->actor_view()->channel_of(h.wolf) : 0;
    CHECK(ch != 0, "the client has the Wolf open");
    CHECK(p.s->inventory().at(Worn, 38) != nullptr, "the character wears a T1 Bow in slot 38");
    const float wear_before = p.s->inventory().at(Worn, 38)->durability;
    const int64_t xp_before = p.s->professions().xp(hid::kHunting);

    h.settle();
    combat::Aftermath& after = *rig.world->aftermath();
    CHECK(after.corpse_of(h.wolf) == nullptr && !dead_for_clients(rig.world->registry().find(h.wolf)) &&
              after.stats().deaths == 0,
          "the Wolf is alive: no corpse, no record, not dead for its clients");

    const size_t from = p.got.size();
    check_hunt_start(h, comp, ch, from);
    check_hunt_completion(h, from, wear_before, xp_before);
    check_hunt_done_update(h, comp, ch, from);
}

struct SeenBlock {
    uint64_t object = 0;
    bool is_actor = false;
    bool rep = false;
    size_t bits = 0;
    std::string hex;
};

static std::string hex_of_bits(const uint8_t* data, size_t pos, size_t bits) {
    static const char* const kHex = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < bits; i += 8) {
        unsigned byte = 0;
        for (size_t k = 0; k < 8 && i + k < bits; ++k) {
            const size_t at = pos + i + k;
            if ((data[at >> 3] >> (at & 7)) & 1u) byte |= 1u << k;
        }
        out += kHex[byte >> 4];
        out += kHex[byte & 15];
    }
    return out;
}

struct ChannelLog {
    std::vector<std::vector<SeenBlock>> chains;
    std::vector<uint32_t> closes;
};

static ChannelLog channel_log(const Player& p, uint32_t channel, size_t from) {
    ChannelLog out;
    for (size_t i = from; i < p.got.size(); ++i) {
        const auto& d = p.got[i];
        if (d.channel != channel || d.open) continue;
        if (d.close) {
            out.closes.push_back(d.close_reason);
            continue;
        }
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) continue;
        std::vector<SeenBlock> chain;
        for (const auto& b : *blocks) {
            SeenBlock s;
            s.object = b.header.object.ObjectId;
            s.is_actor = b.header.is_actor;
            s.rep = b.header.has_rep_layout;
            s.bits = b.payload_bits;
            s.hex = hex_of_bits(d.content.data(), b.payload_pos, b.payload_bits);
            chain.push_back(std::move(s));
        }
        out.chains.push_back(std::move(chain));
    }
    return out;
}

static unsigned hex_byte(const std::string& hex, size_t index) {
    if (2 * index + 1 >= hex.size()) return 0;
    return static_cast<unsigned>(std::stoul(hex.substr(2 * index, 2), nullptr, 16));
}

static repl::CodecOptions stat_options() {
    repl::CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}

struct StatsDelta {
    bool ok = false;
    int32_t key = 0;
    int32_t base = 0;
    int32_t id = 0;
    size_t changed = 0;
    size_t deleted = 0;
    int64_t record = 0;
    uint32_t value = 99;
};

static StatsDelta stats_delta_of(const std::string& hex, size_t bits) {
    StatsDelta out;
    std::vector<uint8_t> bytes((bits + 7) / 8);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(hex_byte(hex, i));
    size_t pos = 0;
    const auto fields =
        repl::read_fast_array_fields(class_named("UAoCStatsComponent"), bytes.data(), bits, pos, stat_options());
    if (!fields || fields->size() != 1) return out;
    const repl::FastArrayDelta& d = (*fields)[0].delta;
    out.ok = true;
    out.key = d.array_key;
    out.base = d.base_key;
    out.changed = d.changed.size();
    out.deleted = d.deleted.size();
    if (d.changed.size() == 1) {
        out.id = d.changed[0].id;
        const auto it = d.changed[0].values.values.find(1);
        const repl::StatInt32Inst* s =
            (it == d.changed[0].values.values.end()) ? nullptr : it->second.get_if<repl::StatInt32Inst>();
        if (s) {
            out.record = s->record;
            out.value = s->value;
        }
    }
    return out;
}

static bool is_health_zero_delta(const StatsDelta& d) {
    return d.ok && d.changed == 1 && d.deleted == 0 && d.key == d.base + 1 &&
           d.record == content::kGuidHealth && d.value == 0u;
}

static size_t count_differing_bits(const std::string& a, const std::string& b, size_t bits) {
    size_t diff = 0;
    for (size_t i = 0; i < bits; ++i) {
        if (((hex_byte(a, i >> 3) >> (i & 7)) & 1u) != ((hex_byte(b, i >> 3) >> (i & 7)) & 1u)) ++diff;
    }
    return diff;
}

static void check_done_chain_blocks(const std::vector<SeenBlock>& done_chain, uint64_t comp) {
    const SeenBlock& dc = done_chain[0];
    const SeenBlock& ds = done_chain[1];
    const SeenBlock& di = done_chain[2];
    CHECK(dc.object < ds.object && ds.object < di.object && di.object == comp,
          "... in the order of their object ids, the interactable last [R: 1456: 436126 436130 436136]");
    const chunt::DoneBlocks& retail = chunt::kDoneBlocks[0];
    CHECK(dc.rep && dc.bits == retail.combat.bits && dc.hex == retail.combat.hex,
          "the CombatInfo block is the capture's to the bit: 18 bits, bIsDead 1 [R: 1456 1458 1178 1179; 1457's 27 also clears bNotTargetable]");
    bool stats_same = true;
    for (const chunt::DoneBlocks& d : chunt::kDoneBlocks) {
        stats_same = stats_same && std::string(d.stats.hex) == retail.stats.hex && d.stats.bits == retail.stats.bits;
    }
    CHECK(stats_same, "(the five captured hunts write the same stats block: 278 bits, the Health item to 0)");
    const StatsDelta ours = stats_delta_of(ds.hex, ds.bits);
    const StatsDelta theirs = stats_delta_of(retail.stats.hex, retail.stats.bits);
    std::printf("      (the Health item is id %d, the array key %d -> %d; the capture's: id %d, key %d -> %d)\n",
                ours.id, ours.base, ours.key, theirs.id, theirs.base, theirs.key);
    CHECK(!ds.rep && ds.bits == retail.stats.bits && is_health_zero_delta(ours) && is_health_zero_delta(theirs),
          "the stats block is the capture's: 278 bits, one StatRepInt32EveryoneProxy delta that changes the one Health item to 0 [R: 1456 1457 1458 1178 1179]");
    const size_t limit = std::min(ds.bits, static_cast<size_t>(retail.stats.bits));
    CHECK(count_differing_bits(ds.hex, retail.stats.hex, limit) <= 6,
          "(only the id and the two keys differ from the capture's block: a few bits)");
    CHECK(di.rep && di.bits == chunt::kRun1457Done.bits,
          "the interactable's block is the done update's 370 bits (its content: test_corpse_wire, to the bit)");
}

static void check_end_of_hunt_pawn_and_quiet(
    const Player& p, uint64_t wolf_guid, uint32_t ch, size_t from, size_t after_done) {
    const ChannelLog pawn = channel_log(p, p.pawn_ch, from);
    size_t blocks_on_pawn = 0;
    std::vector<size_t> pawn_bits;
    for (const auto& chain : pawn.chains) {
        for (const SeenBlock& b : chain) {
            if (b.is_actor) continue;
            ++blocks_on_pawn;
            pawn_bits.push_back(b.bits);
        }
    }
    CHECK(blocks_on_pawn == 2 && pawn_bits.size() == 2 && pawn_bits[0] == 593 && pawn_bits[1] == 305,
          "the pawn gets two component blocks in the whole run: ChannelingData's start (593 bits) and its clear (305 bits) [R: 1457 packets 134 and 995; nothing else, no FoliageData]");
    const auto channeling = channeling_seen(p, from);
    CHECK(channeling.size() == 2 && channeling[1].second.flags == 0x106 && !channeling[1].second.active &&
              !channeling[1].second.timed && channeling[1].second.target.ObjectId == wolf_guid,
          "the clear is the retail one (flags 0x106, not active, not timed) and names the creature");
    size_t pawn_after = 0;
    for (const auto& chain : channel_log(p, p.pawn_ch, after_done).chains) {
        for (const SeenBlock& b : chain) pawn_after += b.is_actor ? 0 : 1;
    }
    CHECK(pawn_after == 0, "no component block of the pawn follows the done packet");
    size_t creature_after = 0;
    for (const auto& chain : channel_log(p, ch, after_done).chains) creature_after += chain.size();
    CHECK(creature_after == 0, "no block of the creature follows the done packet (it is only closed)");
}

static void check_creature_heartbeat_chains(const ChannelLog& creature, uint64_t comp) {
    size_t other_chains = 0;
    size_t odd_chains = 0;
    for (const auto& chain : creature.chains) {
        if (chain.size() != 1) continue;
        ++other_chains;
        const bool expected_beat = chain[0].bits == 418 || chain[0].bits == 217 || chain[0].bits == 81;
        if (!(chain[0].object == comp && chain[0].rep && expected_beat)) ++odd_chains;
    }
    CHECK(other_chains >= 100 && odd_chains == 0,
          "every other chain of the creature is the interactable's start (418 bits), its first beat (217) or a heartbeat (81 bits) [R: 1457]");
    CHECK(creature.closes.size() == 1 && creature.closes[0] == 0, "and 4 s later the channel closes once, Destroyed");
}

static void test_end_of_hunt() {
    std::printf("T1: the end of a hunt against the five captured hunts (1456 1457 1458 1178 1179): the creature's done packet, the pawn's clear, what follows\n");
    HuntRig h;
    if (!h.open()) return;
    Player& p = *h.p;
    Rig& rig = h.rig;
    const uint32_t ch = p.s->actor_view() ? p.s->actor_view()->channel_of(h.wolf) : 0;
    const world::WorldActor* body = rig.world->registry().find(h.wolf);
    const uint64_t comp = rig.component_of(h.wolf);
    CHECK(ch != 0 && body, "the client has the Wolf open");
    if (!ch || !body) return;
    h.settle();
    const size_t from = p.got.size();
    h.interact_with(p, h.wolf);
    CHECK(run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20)), "the hunt completes by the clock");
    rig.run(milliseconds(300));
    const size_t after_done = p.got.size();
    rig.run(seconds(6));

    const ChannelLog creature = channel_log(p, ch, from);
    const std::vector<SeenBlock>* done_chain = nullptr;
    size_t done_chains = 0;
    for (const auto& chain : creature.chains) {
        if (chain.size() > 1) {
            ++done_chains;
            done_chain = &chain;
        }
    }
    CHECK(done_chains == 1 && done_chain && done_chain->size() == 3,
          "the creature's done packet is one chain of three blocks (as the captures' CombatInfo, stats, interactable)");
    if (!done_chain || done_chain->size() != 3) return;
    check_done_chain_blocks(*done_chain, comp);
    check_creature_heartbeat_chains(creature, comp);
    check_end_of_hunt_pawn_and_quiet(p, body->actor.guid.ObjectId, ch, from, after_done);
}

int main() {
    {
        auto logger = std::make_shared<spdlog::logger>(
            "test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
        logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger);
    }
    const char* path = db_path();
    if (!path) {
        std::printf("[skip] test_world_hunting_session: no content.db (set AOC_CONTENT_DB, or build dist/content/content.db)\n");
        return 0;
    }
    g_path = path;
    std::printf("content.db: %s\n", path);
    if (std::getenv("HUNT_ONLY_END")) {
        test_end_of_hunt();
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    test_a_hunt();
    run_hunting_session_refusals();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
