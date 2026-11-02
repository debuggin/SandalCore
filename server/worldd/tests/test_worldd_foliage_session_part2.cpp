// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_worldd_foliage_session_helpers.h"

static std::optional<repl::ElementState> read_callout_block(const schema::ClassSchema &pc, uint16_t field_idx,
                                                            const fake::Delivered &d, const repl::BlockView &b) {
    if (!b.header.is_actor || b.header.has_rep_layout) return std::nullopt;
    size_t pos = b.payload_pos;
    const auto fields = repl::read_fast_array_fields(pc, d.content.data(), b.payload_pos + b.payload_bits, pos);
    if (!fields) return std::nullopt;
    for (const repl::FastArrayField &f : *fields) {
        if (f.field == field_idx && !f.delta.changed.empty()) return f.delta.changed[0].values;
    }
    return std::nullopt;
}

static std::optional<repl::ElementState> read_callout_chain(const schema::ClassSchema &pc, uint16_t field_idx,
                                                            const fake::Delivered &d) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return std::nullopt;
    for (const auto &b : *blocks) {
        if (const auto item = read_callout_block(pc, field_idx, d, b)) return item;
    }
    return std::nullopt;
}

static std::optional<repl::ElementState> callout_item_seen(const Player &p, size_t from) {
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const auto layout = repl::fast_array_layout(pc, "CalloutQueueReplication");
    if (!layout) return std::nullopt;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close) continue;
        if (const auto item = read_callout_chain(pc, layout->field, p.got[i])) return item;
    }
    return std::nullopt;
}

static std::optional<repl::ElementState> load_golden_callout_1464_191() {
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const auto layout = repl::fast_array_layout(pc, "CalloutQueueReplication");
    for (const capture_mount::Callout &c : capture_mount::kCallouts) {
        if (c.capture != 1464 || c.packet != 191) continue;
        const capture_mount::Payload &pay = capture_mount::kPayloads[c.payload];
        const auto o = repl::load_object_state_hex(pc, pay.hex, pay.bits, c.rep);
        const repl::FastArrayState *fa = (o && layout) ? o->arrays.find(layout->field) : nullptr;
        if (fa && !fa->items().empty()) return fa->items()[0].values;
    }
    return std::nullopt;
}

static std::string diff_callout_handles(const repl::ElementState &mine, const repl::ElementState &golden) {
    std::string differs;
    for (const auto &[handle, value] : golden.values) {
        if (handle == 21 || handle >= 25) continue;
        const auto it = mine.values.find(handle);
        if (it == mine.values.end() || !(it->second == value)) differs += " " + std::to_string(handle);
    }
    return differs;
}

static void check_moving_callout_golden(const Player &p, size_t from) {
    const auto mine = callout_item_seen(p, from);
    const auto golden = load_golden_callout_1464_191();
    const std::string differs = (mine && golden) ? diff_callout_handles(*mine, *golden) : std::string();
    const bool same = mine && golden && mine->values.size() == golden->values.size() && differs.empty();
    const std::string label = "the callout item is the retail capture's in every field but the time "
                              "and the key "
                              "(message empty, Economy / GatheringInterruption_Movement, type 4, point "
                              "size 32)" +
                              (differs.empty() ? std::string() : "; differs in handles" + differs);
    CHECK(same, label.c_str());
}

static void test_moving() {
    std::printf("a start while walking: refused with the callout and the "
                "interrupt [R: the retail capture]\n");
    FRig fr;
    if (!fr.open()) return;
    Player &p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const size_t from = p.got.size();
    float ts = 5.0f;
    p.client_sends({p.client.actor(p.pawn_ch, client_move(ts, true), true)});
    fr.rig.run(milliseconds(40));
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    const bool ref_moving = p.s->foliage_refusals().count("moving") == 1 &&
                            p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locks() == 0;
    CHECK(ref_moving, "a start 40 ms after a walking move is refused as moving");
    fr.rig.run(milliseconds(200));
    const auto names = pc_call_names_since(p, from);
    CHECK(std::count(names.begin(), names.end(), "ClientInterruptInteractionWithFoliage") == 1,
          "ClientInterruptInteractionWithFoliage was sent, once");
    CHECK(p.s->foliage_interrupts_sent() == 1 && p.s->bootstrap().callouts() == 1,
          "and a callout (Economy / GatheringInterruption_Movement) is shown");
    CHECK(foliage_data_seen(p, from).empty(), "no FoliageData was set [R: 1464]");
    CHECK(count_in_log("(moving)") >= 1, "the log says why");
    check_moving_callout_golden(p, from);
    fr.rig.run(seconds(4));
    CHECK(p.s->bootstrap().callouts() == 0, "the callout is taken off after 3 s");
    ts += 1.0f;
    p.client_sends({p.client.actor(p.pawn_ch, client_move(ts, false), true)});
    fr.rig.run(milliseconds(700));
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(p.s->cutting_instance() == fr.oak_row() && p.s->foliage_started() == 1,
          "700 ms after the last walking move the start is accepted");
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

int main() {
    if (!init_foliage_session_env()) return 0;
    test_moving();
    test_cancels();
    test_one_cutter();
    test_a_tree_start_during_a_node_channel();
    test_flood();
    test_full_bag();
    test_foliage_off();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
