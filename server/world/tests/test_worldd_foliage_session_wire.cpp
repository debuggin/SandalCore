// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_foliage_session_common.h"

#include "capture_foliage.h"
#include "capture_in_place.h"
#include "capture_mount.h"
#include "protocol/repl/move_packed.h"
#include "test_support.h"

namespace {

struct IfaSeen {
    size_t chain = 0;
    bool open = false, close = false;
    std::string owner;
    std::vector<std::pair<uint32_t, uint32_t>> changed;
    std::vector<int32_t> deleted;
    size_t components = 0;
};

void read_ifa_open_owner(const fake::Delivered &d, std::map<uint32_t, std::string> &channels) {
    channels.erase(d.channel);
    const size_t prefix = new_actor_prefix_bits(d.content);
    const auto blocks =
        prefix ? repl::read_content_blocks(d.content.data(), d.content.bit_pos(), prefix) : std::nullopt;
    if (!blocks || blocks->empty() || !(*blocks)[0].header.is_actor || !(*blocks)[0].header.has_rep_layout) return;
    const schema::ClassSchema &ifa = class_named("AIntrepidStateIFA");
    size_t pos = (*blocks)[0].payload_pos;
    const auto st =
        repl::read_rep_block(ifa, d.content.data(), (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, pos);
    const repl::Value *name = st ? st->get(ifa, "FoliageName") : nullptr;
    if (name) channels[d.channel] = name_text(name);
}

void read_ifa_comp_field(const repl::FastArrayField &f, IfaSeen &s) {
    for (const int32_t d : f.delta.deleted) s.deleted.push_back(d);
    for (const auto &ch2 : f.delta.changed) {
        const auto st1 = ch2.values.values.find(1);
        const auto key = ch2.values.values.find(2);
        if (st1 == ch2.values.values.end() || key == ch2.values.values.end()) continue;
        if (!st1->second.get_if<uint64_t>() || !key->second.get_if<uint64_t>()) continue;
        s.changed.emplace_back(static_cast<uint32_t>(*key->second.get_if<uint64_t>()),
                               static_cast<uint32_t>(*st1->second.get_if<uint64_t>()));
    }
}

void read_ifa_comp_block(const fake::Delivered &d, const repl::BlockView &b, IfaSeen &s) {
    if (b.header.is_actor) return;
    ++s.components;
    const schema::ClassSchema &comp = class_named("UIntrepidFoliageStateComponent");
    size_t pos = b.payload_pos;
    const size_t end = b.payload_pos + b.payload_bits;
    if (b.header.has_rep_layout && !repl::read_rep_block(comp, d.content.data(), end, pos)) return;
    if (pos >= end) return;
    const auto fields = repl::read_fast_array_fields(comp, d.content.data(), end, pos);
    if (!fields) return;
    for (const repl::FastArrayField &f : *fields) read_ifa_comp_field(f, s);
}

void read_ifa_blocks(const fake::Delivered &d, IfaSeen &s) {
    if (d.close) return;
    const auto blocks =
        repl::read_content_blocks(d.content.data(), d.content.bit_pos(), d.open ? new_actor_prefix_bits(d.content) : 0);
    if (!blocks) return;
    for (const auto &b : *blocks) read_ifa_comp_block(d, b, s);
}

std::vector<IfaSeen> ifa_seen(const Player &p, size_t from) {
    std::vector<IfaSeen> out;
    std::map<uint32_t, std::string> channels;
    for (size_t i = from; i < p.got.size(); ++i) {
        const fake::Delivered &d = p.got[i];
        if (d.open) read_ifa_open_owner(d, channels);
        const auto it = channels.find(d.channel);
        if (it == channels.end()) continue;
        IfaSeen s;
        s.chain = i;
        s.open = d.open;
        s.close = d.close;
        s.owner = it->second;
        read_ifa_blocks(d, s);
        out.push_back(std::move(s));
        if (d.close) channels.erase(d.channel);
    }
    return out;
}

bool has_xp_tracker_block(const std::vector<repl::BlockView> &blocks, const emit::FIntrepidNetworkGUID &xp_tracker) {
    for (const auto &b : blocks) {
        if (!b.header.is_actor && b.header.object == xp_tracker) return true;
    }
    return false;
}

size_t xp_chain_since(const Player &p, size_t from) {
    const bs::SessionGuids &g = p.s->bootstrap().setup().guids;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (blocks && has_xp_tracker_block(*blocks, g.xp_tracker)) return i;
    }
    return static_cast<size_t>(-1);
}

BunchWriter retail_oak_start() {
    for (const capture_foliage::Start &s : capture_foliage::kStarts) {
        if (s.capture != 1428 || s.packet != 53) continue;
        const std::vector<uint8_t> bytes = tsupport::bits_of_hex(s.field.hex, s.field.bits).bytes();
        BunchWriter payload;
        payload.write_bit_range(bytes.data(), 0, s.field.bits);
        BunchWriter out;
        repl::write_content_block(out, repl::BlockHeader{}, payload, /*server_form=*/false);
        return out;
    }
    return BunchWriter();
}

BunchWriter joined(std::initializer_list<const BunchWriter *> parts) {
    BunchWriter out;
    for (const BunchWriter *p : parts) out.write_bit_range(p->data(), 0, p->bit_pos());
    return out;
}

BunchWriter client_move(float ts, bool moving, double x = fid::kStandX, double y = fid::kStandY,
                        double z = fid::kStandZ) {
    repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = repl::quantize_vector(10, moving ? 100 : 0, 0, 0);
    m.location = repl::quantize_vector(100, x, y, z);
    m.movement_mode = repl::kMoveWalking;
    m.input_bits = moving ? 1 : 0;
    const schema::ClassSchema &cls = class_named("PlayerPawn_C");
    const schema::NetField *f = cls.field("ServerMovePacked");
    BunchWriter out;
    repl::MovePacked mp;
    mp.new_move = m;
    BunchWriter params;
    BunchWriter field;
    if (!f || !repl::write_move_packed(params, mp) || repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

void check_harvest_channel_start(FRig &fr, Player &p, uint32_t row, size_t from) {
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

void check_harvest_result_frame(Player &p, size_t from, std::vector<IfaSeen> &cells_out) {
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

void count_regrow_changes(const std::vector<IfaSeen> &grown, size_t &deletes, size_t &infancy, size_t &juvenile) {
    for (const IfaSeen &s : grown) {
        deletes += s.deleted.size();
        for (const auto &c : s.changed) {
            infancy += c.second == 0 ? 1 : 0;
            juvenile += c.second == 1 ? 1 : 0;
        }
    }
}

void check_harvest_regrowth(FRig &fr, Player &p, Player &other, uint32_t row, size_t other_from) {
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

void test_a_harvest() {
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

void check_unnamed_call_first_bunch(FRig &fr, Player &p, const BunchWriter &call, const BunchWriter &start,
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

void check_unnamed_call_many_bunches(FRig &fr, Player &p, const BunchWriter &call, const BunchWriter &start,
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

void check_unnamed_call_refusal_and_garbage(FRig &fr, Player &p) {
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

void test_start_beside_an_unnamed_call() {
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

std::optional<repl::ElementState> read_callout_block(const schema::ClassSchema &pc, uint16_t field_idx,
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

std::optional<repl::ElementState> read_callout_chain(const schema::ClassSchema &pc, uint16_t field_idx,
                                                     const fake::Delivered &d) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return std::nullopt;
    for (const auto &b : *blocks) {
        if (const auto item = read_callout_block(pc, field_idx, d, b)) return item;
    }
    return std::nullopt;
}

std::optional<repl::ElementState> callout_item_seen(const Player &p, size_t from) {
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const auto layout = repl::fast_array_layout(pc, "CalloutQueueReplication");
    if (!layout) return std::nullopt;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close) continue;
        if (const auto item = read_callout_chain(pc, layout->field, p.got[i])) return item;
    }
    return std::nullopt;
}

std::optional<repl::ElementState> load_golden_callout_1464_191() {
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

std::string diff_callout_handles(const repl::ElementState &mine, const repl::ElementState &golden) {
    std::string differs;
    for (const auto &[handle, value] : golden.values) {
        if (handle == 21 || handle >= 25) continue;
        const auto it = mine.values.find(handle);
        if (it == mine.values.end() || !(it->second == value)) differs += " " + std::to_string(handle);
    }
    return differs;
}

void check_moving_callout_golden(const Player &p, size_t from) {
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

void test_moving() {
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

} // namespace

void run_foliage_unnamed_call_test() {
    test_start_beside_an_unnamed_call();
}

void run_foliage_session_wire_tests() {
    test_a_harvest();
    test_start_beside_an_unnamed_call();
    test_moving();
}
