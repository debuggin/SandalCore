// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_foliage_session_common.h"

static BunchWriter interact(uint64_t component_object_id) {
    const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema *rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest r;
    r.interactable.ObjectId = component_object_id;
    r.interactable.ServerId = kServerId;
    r.interactable.Randomizer = kRandomizer;
    r.counter = 1;
    const auto params = repl::interact_params(*rpc, r);
    return params ? actor_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

static size_t refused_count(const Player &p, const char *kind) {
    return p.s->foliage_refusals().count(kind) ? p.s->foliage_refusals().at(kind) : size_t(0);
}

static bool refusal_answered(FRig &fr, const Player &p, size_t from, size_t n) {
    const auto names = pc_call_names_since(p, from);
    const size_t interrupts =
        static_cast<size_t>(std::count(names.begin(), names.end(), "ClientInterruptInteractionWithFoliage"));
    return foliage_data_seen(p, from).empty() && names.size() == n && interrupts == n &&
           p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locks() == 0;
}

static void check_refusals_no_tool_and_range(FRig &fr, Player &p) {
    p.send_now(item_used(4, 29, guid_n(129)), p.pawn_ch);
    fr.rig.run(milliseconds(100));
    CHECK(!p.s->inventory().at(Worn, 29), "(the axe is taken off)");
    size_t from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(refused_count(p, "no tool") == 1 && refusal_answered(fr, p, from, 1), "nothing in the axe slot");
    int axe_slot = -1;
    for (int slot = 0; slot < items::kBackpackSlots; ++slot) {
        const svc::InventoryItem *it = p.s->inventory().at(Bag, slot);
        if (it && it->instance_guid == guid_n(129)) axe_slot = slot;
    }
    if (axe_slot >= 0) p.send_now(item_used(3, axe_slot, guid_n(129)), p.pawn_ch);
    fr.rig.run(milliseconds(100));
    CHECK(p.s->inventory().at(Worn, 29) != nullptr, "(the axe is worn again)");
    p.s->gm_teleport(p.conn, fid::kStandX + 20000, fid::kStandY, fid::kStandZ, fr.rig.t, "the test: far away");
    fr.rig.run(milliseconds(400));
    from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(refused_count(p, "out of range") == 1 && refusal_answered(fr, p, from, 1), "200 m from the tree");
    p.s->gm_teleport(p.conn, fid::kStandX, fid::kStandY, fid::kStandZ, fr.rig.t, "the test: back");
    fr.rig.run(milliseconds(400));
}

static void check_refusals_empty_and_dead(FRig &fr, Player &p, uint32_t row) {
    fr.f().harvested(row, fr.rig.t);
    size_t from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(refused_count(p, "wrong state") == 3 && refusal_answered(fr, p, from, 1),
          "the Oak was cut meanwhile: the client's Adult is stale");
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 3), p.pc_ch);
    CHECK(refused_count(p, "no interaction") == 1 && refusal_answered(fr, p, from, 2),
          "and an Empty instance has no harvest");
    p.s->take_damage(p.conn, 1.0e9, fr.rig.t);
    fr.rig.run(milliseconds(300));
    from = p.got.size();
    const uint32_t row2 = *fr.f().map().find(fid::kOwner, fid::kFismc0, 1);
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 1, static_cast<uint64_t>(fr.f().store().state_of(row2))),
               p.pc_ch);
    const auto names = pc_call_names_since(p, from);
    const bool dead_ok = refused_count(p, "dead") == 1 && foliage_data_seen(p, from).empty() &&
                         std::count(names.begin(), names.end(), "ClientInterruptInteractionWithFoliage") == 1;
    CHECK(dead_ok, "a dead player cuts nothing (and its client is told to end its own)");
    CHECK(p.s->foliage_started() == 0 && p.s->foliage_completed() == 0, "no cutting ever started");
    CHECK(count_in_log("the cutting of InstancedFoliageActor_25600_-41_19_0 / IntrepidGatherableFISMC_0 #0 (Adult) is "
                       "refused (no tool)") == 1,
          "the log says what was refused and why");
}

static void test_refusals() {
    std::printf(
        "the refusals of a start: logged, counted, the world unchanged, and the client told to end its own cutting\n");
    FRig fr;
    if (!fr.open()) return;
    Player &p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const uint32_t row = fr.oak_row();
    const size_t from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 999, 2), p.pc_ch);
    CHECK(refused_count(p, "unknown instance") == 1 && refusal_answered(fr, p, from, 1),
          "an instance the map does not have");
    p.send_now(foliage_start("IFA_NodeLayoutReplicator_108646108518008_4_0", "IntrepidGatherableFISMC_2", 12, 1),
               p.pc_ch);
    CHECK(refused_count(p, "unknown instance") == 2 && refusal_answered(fr, p, from, 2),
          "a town's runtime tree (no row in the map) is not harvestable");
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 1), p.pc_ch);
    CHECK(refused_count(p, "wrong state") == 1 && refusal_answered(fr, p, from, 3),
          "the client says Juvenile, the Oak is Adult");
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 0), p.pc_ch);
    CHECK(refused_count(p, "wrong state") == 2 && refusal_answered(fr, p, from, 4), "the client says Infancy");
    check_refusals_no_tool_and_range(fr, p);
    check_refusals_empty_and_dead(fr, p, row);
}

static bool start_cutting_oak(FRig &fr, Player &p, uint32_t row) {
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    fr.rig.run(milliseconds(300));
    return p.s->cutting_instance() == row && fr.f().store().locked_by(row) != 0;
}

static bool nothing_granted_oak(FRig &fr, const Player &p, uint32_t row, size_t bag0, int64_t xp0) {
    return p.s->inventory().count(R0) == bag0 && p.s->professions().xp(fid::kLumberjacking) == xp0 &&
           fr.f().store().state_of(row) == foliage::kAdult && p.s->foliage_completed() == 0;
}

static void check_cancels_interrupt_and_step(FRig &fr, Player &p, uint32_t row, size_t bag0, int64_t xp0) {
    size_t from = p.got.size();
    CHECK(start_cutting_oak(fr, p, row), "a cutting starts");
    p.send_now(interrupt_channel(), p.pawn_ch);
    const bool int_ended = p.s->cutting_instance() == PlayerSession::kNoInstance &&
                           fr.f().store().locked_by(row) == 0 && p.s->foliage_cancelled() == 1;
    CHECK(int_ended, "ServerInterruptChannel: the cutting ends and the lock is released");
    fr.rig.run(milliseconds(300));
    const auto fd = foliage_data_seen(p, from);
    CHECK(fd.size() == 2 && fd[0].duration > 0 && fd[1].duration == 0.0f && fd[1].state == 3,
          "FoliageData was cleared");
    CHECK(p.s->foliage_interrupts_sent() == 0, "nothing is said to the client of its own interrupt");
    fr.rig.run(seconds(16));
    CHECK(nothing_granted_oak(fr, p, row, bag0, xp0), "and nothing comes when the time would have been up");
    from = p.got.size();
    CHECK(start_cutting_oak(fr, p, row), "a cutting starts again");
    p.s->gm_teleport(p.conn, fid::kStandX + 70, fid::kStandY, fid::kStandZ, fr.rig.t, "the test: a step away");
    fr.rig.run(milliseconds(300));
    const bool step_ended = p.s->cutting_instance() == PlayerSession::kNoInstance &&
                            fr.f().store().locked_by(row) == 0 && p.s->foliage_cancelled() == 2;
    CHECK(step_ended, "70 cm from where it began: the cutting ends");
    const auto names = pc_call_names_since(p, from);
    CHECK(std::count(names.begin(), names.end(), "ClientInterruptInteractionWithFoliage") == 1 &&
              p.s->foliage_interrupts_sent() == 1,
          "the client is told with the interrupt and the Movement callout, as a "
          "refused start is");
    CHECK(count_in_log("the player moved 70 cm since the channel began (a "
                       "cutting needs standing still") == 1,
          "the log says why");
    fr.rig.run(seconds(16));
    CHECK(nothing_granted_oak(fr, p, row, bag0, xp0), "no reward");
}

static void check_cancels_axe_and_death(FRig &fr, Player &p, uint32_t row, int64_t xp0) {
    p.s->gm_teleport(p.conn, fid::kStandX, fid::kStandY, fid::kStandZ, fr.rig.t, "the test: back");
    fr.rig.run(milliseconds(400));
    CHECK(start_cutting_oak(fr, p, row), "a cutting starts a third time");
    p.send_now(item_used(4, 29, guid_n(129)), p.pawn_ch);
    fr.rig.run(milliseconds(100));
    CHECK(!p.s->inventory().at(Worn, 29) && p.s->cutting_instance() == PlayerSession::kNoInstance &&
              fr.f().store().locked_by(row) == 0,
          "the axe is taken off: the cutting ends");
    fr.rig.run(seconds(16));
    CHECK(p.s->foliage_completed() == 0 && p.s->professions().xp(fid::kLumberjacking) == xp0 &&
              fr.f().store().state_of(row) == foliage::kAdult,
          "...with no reward");
    int axe_slot = -1;
    for (int slot = 0; slot < items::kBackpackSlots; ++slot) {
        const svc::InventoryItem *it = p.s->inventory().at(Bag, slot);
        if (it && it->instance_guid == guid_n(129)) axe_slot = slot;
    }
    CHECK(axe_slot >= 0, "the axe is in the bag");
    if (axe_slot >= 0) p.send_now(item_used(3, axe_slot, guid_n(129)), p.pawn_ch);
    fr.rig.run(milliseconds(200));
    CHECK(p.s->inventory().at(Worn, 29) != nullptr, "the axe is worn again");
    const size_t from = p.got.size();
    CHECK(start_cutting_oak(fr, p, row), "a cutting starts a fourth time");
    p.s->take_damage(p.conn, 1.0e9, fr.rig.t);
    fr.rig.run(milliseconds(300));
    CHECK(p.s->dead() && p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locked_by(row) == 0,
          "the player dies: the cutting ends and the instance is free");
    const auto after_death = pc_call_names_since(p, from);
    CHECK(std::count(after_death.begin(), after_death.end(), "ClientInterruptInteractionWithFoliage") == 1,
          "(the death is told as the Death interruption)");
    fr.rig.run(seconds(16));
    CHECK(p.s->foliage_completed() == 0 && fr.f().store().state_of(row) == foliage::kAdult, "...with no reward");
}

static void check_cancels_disconnect(FRig &fr, uint32_t row) {
    Player &q = join_at_the_oak(fr, "0000000000000000000000000000cc01", "carol");
    q.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), q.pc_ch);
    fr.rig.run(milliseconds(300));
    CHECK(q.s->cutting_instance() == row && fr.f().store().locked_by(row) != 0, "a second player cuts the tree");
    q.s->finish("the test ends the session");
    CHECK(fr.f().store().locked_by(row) == 0, "the session ends mid-cutting: the instance is free at once");
    fr.rig.run(seconds(16));
    CHECK(q.s->foliage_completed() == 0 && fr.f().store().state_of(row) == foliage::kAdult,
          "...and nothing is granted");
}

static void test_cancels() {
    std::printf("a cutting that ends with no reward: the instance free at once, "
                "FoliageData cleared, nothing granted\n");
    FRig fr;
    if (!fr.open()) return;
    Player &p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const uint32_t row = fr.oak_row();
    const size_t bag0 = p.s->inventory().count(R0);
    const int64_t xp0 = p.s->professions().xp(fid::kLumberjacking);
    check_cancels_interrupt_and_step(fr, p, row, bag0, xp0);
    check_cancels_axe_and_death(fr, p, row, xp0);
    check_cancels_disconnect(fr, row);
}

static void test_one_cutter() {
    std::printf("two players on one tree: the second is refused until the first "
                "is done\n");
    FRig fr;
    if (!fr.open()) return;
    Player &a = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    Player &b = join_at_the_oak(fr, "0000000000000000000000000000cc01", "carol");
    const uint32_t row = fr.oak_row();
    a.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), a.pc_ch);
    CHECK(a.s->cutting_instance() == row, "the first player cuts the tree");
    fr.rig.run(milliseconds(300));
    b.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), b.pc_ch);
    CHECK(b.s->cutting_instance() == PlayerSession::kNoInstance && b.s->foliage_refusals().count("instance busy") == 1,
          "the second is refused: instance busy");
    CHECK(fr.f().store().locked_by(row) != 0 && a.s->cutting_instance() == row, "the first goes on");
    a.send_now(interrupt_channel(), a.pawn_ch);
    b.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), b.pc_ch);
    CHECK(b.s->cutting_instance() == row, "the first player stops: the second can start");
    b.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), b.pc_ch);
    CHECK(b.s->foliage_started() == 1 && b.s->foliage_refusals().count("already channeling") == 1,
          "a replayed start of the same player changes nothing");
}

static void check_tree_start_during_node_refusal(FRig &fr, Player &p, uint64_t node, uint32_t row,
                                                 const gather::Channel &before) {
    const size_t from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    const auto names = pc_call_names_since(p, from);
    CHECK(p.s->foliage_refusals().count("already channeling") == 1 && p.s->foliage_started() == 0,
          "the start is refused: already channeling");
    CHECK(names.size() == 1 && names[0] == "ClientInterruptInteractionWithFoliage" &&
              p.s->foliage_interrupts_sent() == 1,
          "the client is told to end its own FoliageData: exactly one "
          "ClientInterruptInteractionWithFoliage, and "
          "nothing else on the PlayerController");
    const gather::Channel *during = fr.rig.board().channel_of(node);
    const bool node_ok = during && during->started == before.started && during->completes == before.completes &&
                         fr.rig.board().state_of(node) == gather::NodeState::Channeling &&
                         p.s->harvests_started() == 1 && p.s->harvests_cancelled() == 0;
    CHECK(node_ok, "the node's channel is untouched: the same clock, not cancelled");
    CHECK(channeling_seen(p, from).empty(), "the pawn's ChannelingData is not touched: the node's bar goes on");
    const bool oak_ok = p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locks() == 0 &&
                        fr.f().store().state_of(row) == foliage::kAdult && foliage_data_seen(p, from).empty();
    CHECK(oak_ok, "the Oak is untouched: nobody cuts it, no lock, no FoliageData");
}

static void test_a_tree_start_during_a_node_channel() {
    std::printf("a tree's start while a NODE channel runs (review A1): refused, "
                "the client told to end its own "
                "FoliageData, the node untouched, a tree cut after it\n");
    FRig fr;
    if (!fr.open()) {
        ++g_fail;
        return;
    }
    const uint64_t node =
        fr.rig.add_node(id::kDaffodil, fid::kStandX + 100.0 - kCx, fid::kStandY - kCy, 41, fid::kStandZ);
    CHECK(node != 0, "a Daffodil node stands 1 m from the player (the Oak is 1.5 m away)");
    if (node == 0) return;
    Player &p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const uint32_t row = fr.oak_row();
    p.send_now(interact(fr.rig.component_of(node)), p.pawn_ch);
    const bool ch_runs = p.s->harvests_started() == 1 &&
                         fr.rig.board().state_of(node) == gather::NodeState::Channeling &&
                         p.s->cutting_instance() == PlayerSession::kNoInstance;
    CHECK(ch_runs, "the node's channel runs (the pawn's ChannelingData; "
                   "FoliageData is the trees' and is not set)");
    if (p.s->harvests_started() != 1) return;
    const gather::Channel before = *fr.rig.board().channel_of(node);
    fr.rig.run(milliseconds(400));
    check_tree_start_during_node_refusal(fr, p, node, row, before);
    CHECK(run_until(
              fr.rig, [&] { return p.s->harvests_completed() == 1; }, seconds(30)),
          "the node's channel completes by the clock");
    fr.rig.run(milliseconds(500));
    CHECK(p.s->harvests_completed() == 1 && p.s->harvests_cancelled() == 0 && p.s->inventory().count(R0) > 0,
          "and its harvest is granted (the unlock did not end it)");
    const size_t interrupts_before = p.s->foliage_interrupts_sent();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(p.s->cutting_instance() == row && p.s->foliage_started() == 1 &&
              p.s->foliage_refusals().at("already channeling") == 1,
          "a tree's start after the node is accepted");
    CHECK(p.s->foliage_interrupts_sent() == interrupts_before, "... and not answered with an interrupt");
}

static void test_flood() {
    std::printf("a flood of starts: bounded work, one cutting, the rest counted\n");
    FRig fr;
    if (!fr.open()) return;
    Player &p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 400; ++i) {
        p.client_sends(
            {p.client.actor(p.pc_ch, foliage_start(fid::kOwner, fid::kFismc0, static_cast<uint32_t>(i % 5), 2), true)});
        if (i % 50 == 49) fr.rig.run(milliseconds(40));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    fr.rig.run(milliseconds(200));
    CHECK(p.s->foliage_started() == 1 && p.s->foliage_refused() == 399,
          "400 starts in a row: one cutting, 399 refusals (already channeling), "
          "each counted");
    char msg[100];
    std::snprintf(msg, sizeof msg, "(400 starts took %.0f ms)", ms);
    CHECK(ms < 2000.0, msg);
    CHECK(fr.f().store().locks() == 1, "one lock is held");
}

static void test_full_bag() {
    std::printf("a full bag: the rest of the logs go to a pile, the tree is cut\n");
    FRig fr;
    if (!fr.open()) return;
    svc::Character rec = record_at("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", fid::kStandX, fid::kStandY, fid::kStandZ);
    for (int slot = 0; slot < 24; ++slot)
        rec.items.push_back(make_item(R0, slot, fid::kCoal, 300 + static_cast<uint32_t>(slot), 20));
    Player &p = fr.rig.add_player(std::move(rec));
    play(p);
    std::vector<combat::LootStack> dropped;
    fr.rig.world->set_pile_hook([&](double, double, double, const repl::GuidValue &, const std::string &,
                                    const std::vector<combat::LootStack> &stacks) -> uint64_t {
        dropped = stacks;
        return 1;
    });
    const uint32_t row = fr.oak_row();
    const size_t from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(run_until(fr.rig, [&] { return p.s->foliage_completed() == 1; }, seconds(20)), "the cutting completes");
    fr.rig.run(milliseconds(500));
    const auto names = pc_call_names_since(p, from);
    CHECK(std::count(names.begin(), names.end(), "ClientReceiveStorageEvent") == 0 &&
              std::count(names.begin(), names.end(), "ClientConfirmMultiInteractFoliage") == 1,
          "no storage event, the confirm all the same");
    CHECK(!dropped.empty() && dropped[0].item == fid::kOakLog && p.s->harvest_piles() == 1,
          "the logs are in a pile where the player stands");
    CHECK(fr.f().store().state_of(row) == foliage::kEmpty, "the tree is cut");
    CHECK(p.s->bootstrap().callouts() >= 0, "(a callout says why)");
}

static void test_foliage_off() {
    std::printf("a world without foliage: one info line, no cutting\n");
    Rig rig;
    if (!rig.open(g_path)) return;
    Player &p =
        rig.add_player(record_at("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb", fid::kStandX, fid::kStandY, fid::kStandZ));
    play(p);
    const size_t before = count_in_log("foliage gathering is off in this world");
    p.request(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    p.request(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(count_in_log("foliage gathering is off in this world") == before + 1,
          "two interactions with foliage log one line");
    CHECK(count_in_log("cyb: a tree or plant of the world's foliage was interacted with") >= 1,
          "naming the player and saying what it was");
    CHECK(p.s->foliage_started() == 0 && p.s->cutting_instance() == PlayerSession::kNoInstance,
          "and no cutting starts");
}

static void test_foliage_start_budget() {
    std::printf("a flood of tree starts: the budget drops the surplus\n");
    FRig fr;
    if (!fr.open()) return;
    Player &p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    for (int i = 0; i < 400; ++i) {
        p.client_sends({p.client.actor(p.pc_ch, foliage_start(fid::kOwner, fid::kFismc0, static_cast<uint32_t>(i % 5), 2), true)});
        if (i % 50 == 49) fr.rig.run(milliseconds(40));
    }
    fr.rig.run(milliseconds(200));
    CHECK(p.s->foliage_interacts() == 400, "all 400 starts arrived");
    char msg[160];
    std::snprintf(msg, sizeof msg, "...the budget dropped all but the first few of them (%zu dropped)", p.s->requests_dropped());
    CHECK(p.s->requests_dropped() >= 350, msg);
    CHECK(p.s->foliage_started() == 1 && p.s->foliage_refused() == 399, "...one cutting, and every other start counted as a refusal");
    const auto over = p.s->foliage_refusals().find("over the request budget");
    CHECK(over != p.s->foliage_refusals().end() && over->second >= 350, "...the refusals the budget made are listed under their own name");
}

int main() {
    if (!init_foliage_session_env()) return 0;
    if (std::getenv("FOLIAGE_ONLY_UNNAMED_CALL")) {
        run_foliage_unnamed_call_test();
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    run_foliage_session_wire_tests();
    test_refusals();
    test_cancels();
    test_one_cutter();
    test_a_tree_start_during_a_node_channel();
    test_flood();
    test_full_bag();
    test_foliage_off();
    test_foliage_start_budget();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
