// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_foliage_session_helpers.h"

static void check_harvest_channel_start(FRig &fr, Player &p, uint32_t row, size_t from) {
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    const bool locked =
        p.s->cutting_instance() == row && p.s->foliage_started() == 1 && fr.f().store().locked_by(row) != 0;
    CHECK(locked, "the start: this player cuts the instance, and it is locked");
    CHECK(foliage_data_seen(p, from).empty(), "FoliageData is not sent with the start itself (116 ms later in retail)");
    fr.rig.run(milliseconds(160));
    const auto set = foliage_data_seen(p, from);
    const bool dur_ok = set.size() == 1 && set[0].duration > 14.0f && set[0].duration < 15.0f && set[0].instance == 0;
    const bool id_ok = dur_ok && set[0].owner == fid::kOwner && set[0].component == fid::kFismc0 && set[0].state == 2;
    CHECK(id_ok, "116 ms after the start the client was told FoliageData: 14.7 s (the base 15 s and the T1 axe's "
                 "speed: the retail capture's player had 14.71), the instance, its owner and component, state Adult");
    const bool tool_ok =
        !set.empty() && set[0].tool_record == p.s->inventory().at(Worn, 29)->item_record_id && set[0].start_time > 0.0;
    CHECK(tool_ok, "... with the axe's record and the start on the effects' clock");
    CHECK(channeling_seen(p, from).empty(), "the pawn's ChannelingData is not set for a tree: its channel is "
                                            "FoliageData's [R: 1428 and 1464 have no ChannelingData]");
    CHECK(pc_call_names_since(p, from).empty(), "nothing else is sent while the channel runs");
    CHECK(ifa_seen(p, from).empty(),
          "and the cell's state actor is not sent again (the client has held it since the login)");
}

static void check_harvest_result_frame(Player &p, size_t from, std::vector<IfaSeen> &cells_out) {
    const auto names = pc_call_names_since(p, from);
    const bool order_ok = names.size() >= 3 && names[0] == "ClientUpdateItemDurability" &&
                          names[1] == "ClientReceiveStorageEvent" && names[2] == "ClientConfirmMultiInteractFoliage";
    CHECK(order_ok, "the result frame: the tool's wear, the storage event, the confirm (the retail capture's packet "
                    "833, in its order)");
    const auto chains = pc_chain_calls(p, from);
    size_t confirm_chain = static_cast<size_t>(-1), durability_chain = static_cast<size_t>(-1);
    for (const auto &[chain, name] : chains) {
        if (name == "ClientConfirmMultiInteractFoliage") confirm_chain = chain;
        if (name == "ClientUpdateItemDurability") durability_chain = chain;
    }
    const size_t xp_chain = xp_chain_since(p, from);
    const auto cleared = foliage_data_seen(p, from);
    cells_out = ifa_seen(p, from);
    CHECK(xp_chain != static_cast<size_t>(-1) && xp_chain > confirm_chain && durability_chain < confirm_chain,
          "the XPTracker follows the confirm");
    const bool clr_ok = cleared.size() == 2 && cleared[1].duration == 0.0f && cleared[1].state == 3 &&
                        static_cast<uint32_t>(cleared[1].instance) == 0xFFFFFFFFu && cleared[1].owner.empty();
    CHECK(clr_ok, "FoliageData is cleared after the result: duration 0, index 0xFFFFFFFF, no name, state 3");
    const bool cell_ok = !cells_out.empty() && cells_out[0].open && cells_out[0].owner == fid::kOwner &&
                         cells_out[0].changed.size() == 1 && cells_out[0].changed[0] == std::make_pair(0u, 3u);
    CHECK(cell_ok, "the cell's state actor opened on this client with the instance Empty: index 0, state 3 (a first "
                   "look lists Empty as 11 only for a cell with older Empty instances: none yet)");
}

static void count_regrow_changes(const std::vector<IfaSeen> &grown, size_t &deletes, size_t &infancy,
                                 size_t &juvenile) {
    for (const IfaSeen &s : grown) {
        deletes += s.deleted.size();
        for (const auto &c : s.changed) {
            infancy += c.second == 0 ? 1 : 0;
            juvenile += c.second == 1 ? 1 : 0;
        }
    }
}

static void check_harvest_regrowth(FRig &fr, Player &p, Player &other, uint32_t row, size_t other_from) {
    const size_t regrow_from = p.got.size();
    fr.rig.run(seconds(40));
    const auto grown = ifa_seen(p, regrow_from);
    size_t deletes = 0, infancy = 0, juvenile = 0;
    count_regrow_changes(grown, deletes, infancy, juvenile);
    CHECK(fr.f().store().state_of(row) == foliage::kAdult && !fr.f().store().deviates(row),
          "after 39 s (x0.01) the Oak is whole: Adult, painted");
    CHECK(infancy == 1 && juvenile == 1 && deletes == 1, "the client was told Infancy, then Juvenile, then the "
                                                         "instance was deleted from the list (a delete, not a state)");
    const auto last = ifa_seen(other, other_from);
    size_t other_deletes = 0;
    for (const IfaSeen &s : last) other_deletes += s.deleted.size();
    CHECK(other_deletes == 1, "the second client was told the delete as well");
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(p.s->cutting_instance() == row && p.s->foliage_started() == 2, "a regrown tree is cut again");
}

static void test_a_harvest() {
    std::printf("a tree from the start to its return: the retail capture's Oak\n");
    FRig fr;
    if (!fr.open()) return;
    Player &p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    Player &other = join_at_the_oak(fr, "0000000000000000000000000000cc01", "carol");
    fr.rig.run(seconds(2));
    const uint32_t row = fr.oak_row();
    CHECK(fr.f().store().state_of(row) == foliage::kAdult && p.s->cutting_instance() == PlayerSession::kNoInstance,
          "the Oak is Adult and nobody cuts it");
    CHECK(p.s->inventory().at(Worn, 29) != nullptr, "the character wears a T1 axe in slot 29");
    const float wear_before = p.s->inventory().at(Worn, 29)->durability;
    const int64_t xp_before = p.s->professions().xp(fid::kLumberjacking);
    const size_t from = p.got.size(), other_from = other.got.size();
    check_harvest_channel_start(fr, p, row, from);
    CHECK(run_until(
              fr.rig, [&] { return p.s->foliage_completed() == 1; }, seconds(20)),
          "the channel completes by the clock, about 15 s after");
    fr.rig.run(milliseconds(400));
    std::vector<IfaSeen> cells;
    check_harvest_result_frame(p, from, cells);
    CHECK(p.s->inventory().count(R0) > 0, "the logs are in Resource0");
    const svc::InventoryItem *axe = p.s->inventory().at(Worn, 29);
    CHECK(axe && axe->durability == wear_before - 1.0f, "the axe lost 1 durability");
    CHECK(p.s->professions().xp(fid::kLumberjacking) > xp_before, "Lumberjacking XP came");
    const bool free_again = fr.f().store().state_of(row) == foliage::kEmpty && fr.f().store().locked_by(row) == 0 &&
                            p.s->cutting_instance() == PlayerSession::kNoInstance;
    CHECK(free_again, "the instance is Empty and free again; this player cuts nothing");
    const auto theirs = ifa_seen(other, other_from);
    const bool theirs_ok = !theirs.empty() && theirs[0].open && theirs[0].owner == fid::kOwner &&
                           theirs[0].changed.size() == 1 && theirs[0].changed[0] == std::make_pair(0u, 3u);
    CHECK(theirs_ok, "a SECOND client near the tree was sent the same state actor with the instance Empty");
    bool closed = false;
    for (const IfaSeen &s : cells) closed = closed || s.close;
    CHECK(closed, "and the state actor is closed with Dormancy again (retail: 265 ms after the open)");
    check_harvest_regrowth(fr, p, other, row, other_from);
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

static void check_unnamed_call_first_bunch(FRig &fr, Player &p, const BunchWriter &call, const BunchWriter &start,
                                           size_t unread_before) {
    const uint32_t row = fr.oak_row();
    size_t from = p.got.size();
    p.send_now(joined({&call, &start}), p.pc_ch);
    CHECK(p.s->foliage_started() == 1 && p.s->cutting_instance() == row && fr.f().store().locked_by(row) != 0,
          "the call and the start in one bunch: the start reaches the foliage handler, this player cuts the Oak "
          "(before: the whole bunch was dropped, the start with it)");
    CHECK(count_in_log("does not read: ignored") == unread_before, "no 'does not read' line for that bunch");
    CHECK(count_in_log("names by path (\"UnmappedComponent_0\"") == 1,
          "and the name is said once, not again for the same call");
    CHECK(count_in_log("held 1 calls named by path and 1 others (ServerStartInteractionWithFoliage)") == 1,
          "a bunch that shares the call with another is said, with the other's name (the next run's log shows whether "
          "a start ever does)");
    fr.rig.run(milliseconds(160));
    const auto set = foliage_data_seen(p, from);
    const bool fd_ok = set.size() == 1 && set[0].duration > 14.0f && set[0].duration < 15.0f &&
                       set[0].owner == fid::kOwner && set[0].component == fid::kFismc0 && set[0].state == 2;
    CHECK(fd_ok, "116 ms after, the client is told FoliageData: the channel runs (14.7 s, the Oak, Adult)");
    const size_t sent_before = p.s->foliage_interrupts_sent();
    from = p.got.size();
    p.send_now(joined({&call, &start}), p.pc_ch);
    const bool second_ok = p.s->foliage_refusals().count("already channeling") == 1 &&
                           p.s->foliage_interrupts_sent() == sent_before && pc_call_names_since(p, from).empty();
    CHECK(second_ok, "a second start while the channel runs: refused (already channeling), and the client's running "
                     "channel is left alone (no interrupt)");
}

static void check_unnamed_call_many_bunches(FRig &fr, Player &p, const BunchWriter &call, const BunchWriter &start,
                                            size_t unread_before) {
    const uint32_t row = fr.oak_row();
    const auto free_the_player = [&] {
        p.send_now(interrupt_channel(), p.pawn_ch);
        fr.rig.run(milliseconds(300));
    };
    free_the_player();
    CHECK(p.s->cutting_instance() == PlayerSession::kNoInstance && fr.f().store().locked_by(row) == 0,
          "(the client ends the channel)");
    p.send_now(joined({&start, &call}), p.pc_ch);
    CHECK(p.s->foliage_started() == 2 && p.s->cutting_instance() == row,
          "the start, then the call: the start reaches the handler");
    free_the_player();
    BunchWriter many;
    for (int i = 0; i < 13; ++i) many.write_bit_range(call.data(), 0, call.bit_pos());
    CHECK(many.bit_pos() == 7150, "(thirteen of the calls are 7150 bits, a size the logs had)");
    p.send_now(joined({&many, &start}), p.pc_ch);
    CHECK(p.s->foliage_started() == 3 && p.s->cutting_instance() == row,
          "thirteen calls and the start behind them: the start reaches the handler");
    CHECK(count_in_log("does not read: ignored") == unread_before &&
              count_in_log("names by path (\"UnmappedComponent_0\"") == 1,
          "still no 'does not read', and the name is still said once");
    free_the_player();
}

static void check_unnamed_call_refusal_and_garbage(FRig &fr, Player &p) {
    size_t from = p.got.size();
    const size_t callouts_before = p.s->bootstrap().callouts();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 999, 2), p.pc_ch);
    fr.rig.run(milliseconds(200));
    const auto after_refusal = pc_call_names_since(p, from);
    const bool ref_ok =
        p.s->foliage_refusals().count("unknown instance") == 1 &&
        std::count(after_refusal.begin(), after_refusal.end(), "ClientInterruptInteractionWithFoliage") == 1 &&
        after_refusal.size() == 1 && p.s->bootstrap().callouts() == callouts_before;
    CHECK(ref_ok, "a refused start (an instance the map does not have): ClientInterruptInteractionWithFoliage, once, "
                  "and no callout (before: silent, the client stayed locked)");
    from = p.got.size();
    p.send_now(foliage_start(fid::kOwner, fid::kFismc0, 0, 2), p.pc_ch);
    CHECK(p.s->cutting_instance() == fr.oak_row() && p.s->foliage_started() == 4, "(a fourth cutting starts)");
    p.send_now(item_used(4, 29, guid_n(129)), p.pawn_ch);
    fr.rig.run(milliseconds(300));
    const auto after_cancel = pc_call_names_since(p, from);
    const bool cancel_ok =
        !p.s->inventory().at(Worn, 29) && p.s->cutting_instance() == PlayerSession::kNoInstance &&
        p.s->foliage_cancelled() >= 1 && foliage_data_seen(p, from).empty() &&
        std::count(after_cancel.begin(), after_cancel.end(), "ClientInterruptInteractionWithFoliage") == 1 &&
        p.s->bootstrap().callouts() == callouts_before;
    CHECK(cancel_ok, "the axe taken off within 116 ms of the start: no FoliageData was ever sent, so the client is "
                     "told with the interrupt, once, and no callout");
    const size_t unread_mid = count_in_log("does not read: ignored");
    const auto garbage = [](int words) {
        BunchWriter g;
        for (int i = 0; i < words; ++i) g.write_bits(0xFFFFFFFFu, 32);
        return g;
    };
    for (int i = 0; i < 4; ++i) p.send_now(garbage(12), p.pc_ch);
    CHECK(count_in_log("does not read: ignored") == unread_mid + 3,
          "garbage that does not read: the first three of a session are said (with their bytes), the fourth of the "
          "same kind is not");
    p.send_now(garbage(13), p.pc_ch);
    CHECK(count_in_log("does not read: ignored") == unread_mid + 4,
          "and a bunch of another kind (its size) is said even after the budget of three: a new kind is not hidden by "
          "the old ones");
    p.s->finish("the test ends the session");
    CHECK(count_in_log("17 calls on subobjects it names by path (skipped, in 5 bunches, 1 names)") == 1,
          "the session's summary says how many calls on subobjects named by path were skipped, in how many bunches, "
          "with how many names");
    CHECK(count_in_log("the client's calls by name (") == 1 && count_in_log("ServerStartInteractionWithFoliage 6") == 1,
          "and the client's calls by name: the six starts that reached the handler are counted (a start worldd never "
          "logged is told from one that never arrived)");
}

static void test_start_beside_an_unnamed_call() {
    std::printf("T1: a tree's start in the bunch of the client's 550-bit call on a subobject it names by path reaches "
                "the foliage handler\n");
    FRig fr;
    if (!fr.open()) return;
    Player &p = join_at_the_oak(fr, "bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
    const BunchWriter call = capture_in_place::block();
    const BunchWriter start = retail_oak_start();
    CHECK(call.bit_pos() == capture_in_place::kBlockBits && start.bit_pos() > 0,
          "(the client's 550-bit call, and the retail start of the Oak)");
    const size_t unread_before = count_in_log("does not read: ignored");
    const size_t from = p.got.size();
    p.send_now(call, p.pc_ch);
    CHECK(p.s->foliage_started() == 0 && pc_call_names_since(p, from).empty() &&
              count_in_log("does not read: ignored") == unread_before,
          "the call alone: read and skipped, nothing starts, nothing is sent, no 'does not read'");
    CHECK(count_in_log("names by path (\"UnmappedComponent_0\", outer 17025330/60/") == 1,
          "the log says what it was: the path it names and the outer's GUID");
    CHECK(count_in_log(
              "the bunch is reliable, 550 bits, and starts 0400000000000000000000000000000014c8240f04000000") == 1,
          "and whether the bunch was reliable, its size, and its bytes: the 24 the real client's log held come out the "
          "same");
    check_unnamed_call_first_bunch(fr, p, call, start, unread_before);
    check_unnamed_call_many_bunches(fr, p, call, start, unread_before);
    check_unnamed_call_refusal_and_garbage(fr, p);
}

int main() {
    if (!init_foliage_session_env()) return 0;
    if (std::getenv("FOLIAGE_ONLY_UNNAMED_CALL")) {
        test_start_beside_an_unnamed_call();
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    test_a_harvest();
    test_refusals();
    test_start_beside_an_unnamed_call();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
