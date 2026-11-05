#include "test_world_ability_session_common.h"

#include "combat_rig.h"
#include "session/player_session.h"
#include "spawns/area.h"

using namespace aoc::worldd;

void run_ability_session_pets_tests();

static std::vector<int32_t> ints(std::initializer_list<int32_t> xs) { return {xs}; }

static void test_sprint_drain(Run &r, double t_trigger) {
    r.play(0.3, true);
    CHECK(near(r.m.stamina_now(), 97.5, 0.001), "after 0.3 s the second: 97.5");
    r.play(9.7, true);
    CHECK(near(r.m.stamina_now(), 50.0, 1.3), "after 10 s of Sprint the bar is about half (50 within one charge)");
    CHECK(r.m.ends.empty() && r.rt().sprinting(), "(still sprinting)");
    r.play(10.5, true);
    CHECK(r.m.stamina_now() == 0.0f && !r.rt().sprinting(), "after 20.5 s the client's Stamina is 0 and the Sprint is over");
    CHECK(!r.m.has_ability(kSprint) && !r.m.has_effect(kSprintEffect) && r.m.stats[kMoveSpeedMult] == 1.0f && r.m.has_effect(kDisableRegenEffect), "the Sprint's ability item and effect are removed, MoveSpeedMult is 1.0 again and the DisableRegen effect is on (one update, as retail's reply)");
    CHECK(r.m.ends.size() == 1 && r.m.ends[0].second == 0, "the server ended instance 0 on the client: one ClientEndAbility(0)");
    CHECK(!r.m.end_before_removal, "it came after the update that removed the items (reliable, in order)");
    const double ended_at = r.m.ends.empty() ? 0 : r.m.ends[0].first;
    CHECK(ended_at - t_trigger >= 19.9 && ended_at - t_trigger <= 20.4, "19.75 s plus the 163 ms (within a tick or two) after the trigger");
    bool steps_ok = r.m.stamina.size() == 80;
    float prev = 100.0f;
    for (const auto &[at, v] : r.m.stamina) {
        steps_ok = steps_ok && near(prev - v, 1.25, 1e-6);
        prev = v;
    }
    CHECK(steps_ok, "the HUD saw 80 steps of 1.25, one per charge (98.75 ... 0)");
    CHECK(r.h.s->ability_ends_sent() == 1 && !r.h.s->abilities_failed() && r.m.unreadable == 0, "(one ClientEndAbility sent, nothing failed, every chain reads)");
}

static void test_sprint_regen(Run &r) {
    const size_t before = r.m.stamina.size();
    r.play(0.3, false);
    CHECK(r.m.stamina_now() == 0.0f && r.m.stamina.size() == before, "regeneration waits out a pause of 1 s after the Sprint");
    r.play(19.1, false);
    CHECK(r.m.stamina_now() >= 85.0f && r.m.stamina_now() < 100.0f, "then it gives 5 a second: not full 19.9 s after the end");
    CHECK(!r.m.has_effect(kDisableRegenEffect), "(the DisableRegen effect was removed when the pause was over)");
    r.play(1.3, false);
    CHECK(r.m.stamina_now() == 100.0f, "the bar is full again 20 s after the Sprint ended (a pause of 1 s, then 19 ticks and a bit)");
    bool regen_ok = true;
    for (size_t i = before + 1; i < r.m.stamina.size(); ++i) regen_ok = regen_ok && near(r.m.stamina[i].second - r.m.stamina[i - 1].second, 5.0, 1e-6);
    CHECK(regen_ok && r.m.stamina.size() - before == 20, "in 20 steps of 5");
    const size_t full = r.m.stamina.size();
    r.play(5.0, false);
    CHECK(r.m.stamina.size() == full && r.rt().stamina() == 100.0, "a full bar sends nothing more");
    CHECK(near(r.m.stamina_now(), r.rt().stamina(), 0), "(the client's view is the runtime's)");
}

static void test_sprint() {
    std::printf("Sprint: a full bar\n");
    Run r;
    CHECK(r.m.stamina_now() == 100.0f && r.m.abilities.empty() && r.m.effects.empty(), "joined: Stamina 100, no active ability, no effect");
    r.play(0.5, true);
    const double t_trigger = r.now();
    const int32_t id = r.trigger(kSprint);
    CHECK(id == 0, "(the first trigger of the session is ClientInstanceId 0)");
    CHECK(r.m.has_ability(kSprint) && r.m.has_effect(kSprintEffect) && r.m.chains_with_exports == 1, "the Sprint appears at once as an active ability and an effect, with the export of its montage (the first item that names it)");
    bool item_ok = !r.m.abilities.empty();
    for (const auto &[eid, a] : r.m.abilities) item_ok = item_ok && a.instance == 0 && a.has_montage && a.montage_group == "Sprint";
    CHECK(item_ok, "its item is instance 0 (the server's counter), with the Sprint montage in MontageGroup Sprint");
    CHECK(r.m.stats[kMoveSpeedMult] == 1.2f, "MoveSpeedMult is 1.2 on the client in the same update: 600 x 1.2 = 720 cm/s (the retail server sends it with the Sprint)");
    CHECK(near(r.m.stamina_now(), 98.75, 0.001), "the first charge comes with the activation: 98.75 on the client's HUD at once");
    test_sprint_drain(r, t_trigger);
    test_sprint_regen(r);
}

static void test_sprint_cancel() {
    std::printf("Sprint: a cancel\n");
    Run r;
    r.play(0.3, true);
    const int32_t id = r.trigger(kSprint);
    r.play(2.0, true);
    CHECK(near(r.m.stamina_now(), 88.75, 1.3), "2 s of Sprint: about 88.75");
    r.cancel(id);
    CHECK(!r.m.has_ability(kSprint) && !r.m.has_effect(kSprintEffect) && !r.rt().sprinting() && r.m.stats[kMoveSpeedMult] == 1.0f, "ServerNotifyHeldAbilityCancelled(0): the items are removed at once, MoveSpeedMult is 1.0 again and the Sprint is over");
    const float held = r.m.stamina_now();
    r.play(0.8, true);
    CHECK(r.m.stamina_now() == held && r.m.ends.empty(), "the player keeps moving: the drain has stopped, and no ClientEndAbility was sent (the client ended its own)");
    r.play(1.5, true);
    CHECK(r.m.stamina_now() > held, "after the pause the bar refills");
    r.cancel(id);
    r.cancel(99);
    r.play(0.2, true);
    CHECK(r.m.ends.empty() && r.m.abilities.empty() && r.m.effects.empty(), "a cancel of a finished or an unknown instance changes nothing and is not answered");
}

static void test_sprint_standing() {
    std::printf("Sprint: not moving\n");
    Run r;
    r.play(0.3, false);
    r.trigger(kSprint);
    r.play(3.0, false);
    CHECK(r.rt().sprinting() && r.m.stamina_now() == 100.0f && r.m.stamina.empty(), "a Sprint held while the player stands still costs nothing");
    r.play(2.0, true);
    CHECK(r.m.stamina_now() < 100.0f && r.m.stamina_now() > 85.0f, "the moves that carry an input charge it");
    r.play(0.5, false);
    const float at_stop = r.m.stamina_now();
    r.play(2.0, false);
    CHECK(r.m.stamina_now() >= at_stop, "standing again, the drain stops (regeneration is not blocked while standing)");
}

static void test_dodge_exhaustion(Run &r) {
    const int32_t third = r.trigger(kDodge);
    CHECK(r.m.stamina_now() == 0.0f && third == 2, "after the cooldown a second Dodge is allowed: 50 left, so 0");
    r.play(0.9, false);
    const size_t ends = r.m.ends.size();
    const int32_t fourth = r.trigger(kDodge);
    CHECK(r.m.ends.size() == ends + 1 && r.m.ends.back().second == fourth && r.m.stamina_now() == 0.0f, "a Dodge with less than 50 Stamina is refused with ClientEndAbility");
    CHECK(r.m.stamina.size() == 2 && r.m.stamina[0].second == 50.0f && r.m.stamina[1].second == 0.0f, "(the HUD saw 50 and 0, nothing else yet)");
    r.play(0.5, false);
    CHECK(r.m.stamina_now() == 0.0f, "regeneration stays off for 1.5 s after a Dodge");
    r.play(1.7, false);
    CHECK(r.m.stamina_now() >= 5.0f, "and then it is back");
    CHECK(!r.m.end_before_removal && !r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}

static void test_dodge() {
    std::printf("Dodge\n");
    Run r;
    r.play(0.5, false);
    const int32_t first = r.trigger(kDodge);
    CHECK(r.m.stamina_now() == 50.0f, "a Dodge costs 50, and the HUD sees it at once");
    CHECK(r.m.has_ability(kDodge) && r.m.has_effect(kEvasionEffect) && r.m.cooldowns.size() == 1 && r.m.cooldowns.begin()->second == kDodge, "one update: the Dodge's ability item, its Evasion effect and its cooldown");
    r.play(0.3, false);
    const int32_t second = r.trigger(kDodge);
    CHECK(r.m.end_ids() == std::vector<int32_t>{second} && r.m.stamina_now() == 50.0f, "a second Dodge 0.3 s later is in the cooldown: ClientEndAbility(its id), Stamina unchanged");
    CHECK(r.rt().refused() == 1 && first == 0 && second == 1, "(counted as refused)");
    r.play(0.3, false);
    CHECK(!r.m.has_effect(kEvasionEffect), "the Evasion effect ends after 0.5 s");
    CHECK(r.m.cooldowns.size() == 1, "(the cooldown is still on)");
    r.play(0.2, false);
    CHECK(r.m.cooldowns.empty() && !r.m.has_ability(kDodge), "the cooldown and the ability item go after 0.65 s");
    test_dodge_exhaustion(r);
}

static void test_rest_cancel_and_retrigger(Run &r, int32_t id) {
    r.cancel(id);
    bool jumped = false;
    for (const auto &[eid, a] : r.m.abilities) jumped = a.may_jump && a.montage_position == 5.82606792f && a.animation_counter == 1;
    CHECK(jumped && r.m.has_effect(kRestEffect), "a cancel: the montage jumps to its end section (the effect is still on)");
    r.play(0.3, false);
    CHECK(!r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 0.6f && r.m.stats[kManaRegen] == 0.6f, "a moment later the effect goes and the regeneration is back to 0.6");
    CHECK(r.m.ends.empty() && r.m.stamina_now() == 100.0f, "(no ClientEndAbility, Stamina untouched)");
    r.trigger(kRest);
    CHECK(r.m.chains_with_exports == 1, "a second Rest does not export the montage again: the first export was acknowledged");
}

static void test_rest() {
    std::printf("Rest\n");
    Run r;
    r.play(0.5, false);
    const size_t chains = r.m.chains;
    const int32_t id = r.trigger(kRest);
    CHECK(r.m.has_ability(kRest) && r.m.chains == chains + 1 && r.m.chains_with_exports == 1, "Rest: one update, with the export of its montage (the client has to learn the path)");
    bool item_ok = false;
    for (const auto &[eid, a] : r.m.abilities) item_ok = a.ability == kRest && a.instance == id && a.has_montage && a.has_target && a.animation_counter == 0;
    CHECK(item_ok, "its item: the Rest montage, the caster as target, AnimationCounter 0");
    CHECK(!r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 0.6f, "the effect is not there during the sit-down");
    r.play(2.2, false);
    CHECK(r.m.has_effect(kRestEffect) && r.m.stats[kHealthRegen] == 2.5f && r.m.stats[kManaRegen] == 2.5f, "after the sit-down the Rest effect appears and Health and Mana regeneration are 2.5");
    test_rest_cancel_and_retrigger(r, id);
}

static BunchWriter flipped(const BunchWriter &in, size_t bit) {
    std::vector<uint8_t> bytes(in.data(), in.data() + in.byte_size());
    bytes[bit >> 3] ^= static_cast<uint8_t>(1u << (bit & 7));
    BunchWriter out;
    out.write_bit_range(bytes.data(), 0, in.bit_pos());
    return out;
}

static void test_dodge_during_sprint() {
    Run r;
    r.play(0.5, true);
    const int32_t sprint = r.trigger(kSprint);
    r.play(1.0, true);
    const float at_dodge = r.m.stamina_now();
    r.trigger(kDodge);
    CHECK(r.m.has_ability(kSprint) && r.m.has_effect(kSprintEffect) && r.m.has_ability(kDodge) && r.m.has_effect(kEvasionEffect) && r.m.end_ids().empty(), "a Dodge does not end a Sprint: both are on, and no ClientEndAbility went out");
    CHECK(r.m.stats[kMoveSpeedMult] == 1.2f && near(r.m.stamina_now(), at_dodge - 50.0, 1.3), "MoveSpeedMult stays 1.2, and the Dodge cost 50 on top of the Sprint's steps");
    const float after_dodge = r.m.stamina_now();
    r.play(2.0, true);
    CHECK(r.rt().sprinting() && r.m.has_effect(kSprintEffect) && r.m.stats[kMoveSpeedMult] == 1.2f && r.m.stamina_now() < after_dodge - 5.0f, "the Sprint goes on through the Dodge: its effect, its speed and its drain (1.25 every 0.25 s)");
    r.cancel(sprint);
    CHECK(!r.rt().sprinting() && !r.m.has_effect(kSprintEffect) && r.m.stats[kMoveSpeedMult] == 1.0f && r.m.end_ids().empty(), "the release the client sends afterwards (its own id) ends it as usual: MoveSpeedMult back to 1.0, still no ClientEndAbility");
    CHECK(!r.m.end_before_removal && !r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}

static void test_rest_during_sprint() {
    Run r;
    r.play(0.5, true);
    const int32_t sprint = r.trigger(kSprint);
    r.play(0.5, true);
    r.trigger(kRest);
    CHECK(!r.rt().sprinting() && r.rt().resting() && r.m.end_ids() == std::vector<int32_t>{sprint} && !r.m.has_effect(kSprintEffect) && r.m.stats[kMoveSpeedMult] == 1.0f && r.m.has_ability(kRest), "a Rest ends the Sprint and tells the client: ClientEndAbility(the Sprint's id), MoveSpeedMult back to 1.0 (no retail capture has a Rest)");
    CHECK(!r.m.end_before_removal, "(after the update that removed the Sprint's items)");
}

static void test_sprint_and_dodge() {
    std::printf("a Dodge in the middle of a Sprint, a Rest after one\n");
    test_dodge_during_sprint();
    test_rest_during_sprint();
}

static void test_instance_ids() {
    std::printf("ServerInstanceId is the server's counter, ClientEndAbility the client's id\n");
    Run r;
    r.play(0.5, true);
    const int32_t refused = r.trigger(kScepterCombo);
    const int32_t sprint = r.trigger(kSprint);
    CHECK(refused == 0 && sprint == 1, "(the client counted a refused trigger: the Sprint is ClientInstanceId 1)");
    bool item_ok = false;
    for (const auto &[eid, a] : r.m.abilities) item_ok = a.ability == kSprint && a.instance == 0;
    CHECK(item_ok, "its item says ServerInstanceId 0: the server's own count, the refused trigger took none");
    r.cancel(0);
    CHECK(r.rt().sprinting() && r.m.has_effect(kSprintEffect), "a cancel of 0 is the client's id 0 (the refused trigger), not the instance number: the Sprint runs on");
    r.play(21.0, true);
    CHECK(!r.rt().sprinting() && r.m.end_ids() == ints({refused, sprint}) && !r.m.end_before_removal, "the Sprint that runs out is ended with ClientEndAbility(1), the client's id (not the instance's 0), after the refusal's ClientEndAbility(0)");
}

static void test_dodge_dash() {
    std::printf("the Dodge's dash against the move checker\n");
    Run control;
    control.play(0.5, true);
    for (int i = 0; i < 8; ++i) control.tick_at(2800);
    CHECK(control.h.s->moves_refused() > 0, "(without a Dodge, 8 moves at 2,800 cm/s are refused: over the limit of 2,135)");
    Run r;
    r.play(0.5, true);
    r.trigger(kDodge);
    for (int i = 0; i < 6; ++i) r.tick_at(2800);
    CHECK(r.h.s->moves_refused() == 0 && r.h.s->corrections_sent() == 0, "after a Dodge the same speed is accepted for the length of the dash: no move refused, no correction");
    r.play(1.0, true);
    for (int i = 0; i < 8; ++i) r.tick_at(2800);
    CHECK(r.h.s->moves_refused() > 0, "once the window is over the limit is 2,135 again: the same speed is refused");
    Run too_fast;
    too_fast.play(0.5, true);
    too_fast.trigger(kDodge);
    for (int i = 0; i < 6; ++i) too_fast.tick_at(4500);
    CHECK(too_fast.h.s->moves_refused() > 0, "and not even a Dodge allows 4,500 cm/s");
}

static void test_ability_waits_for_join_settle(Run &r) {
    r.h.release();
    r.trigger(kDodge);
    CHECK(r.h.s->join_pending() > 0 && r.h.s->ability_updates_sent() == 0 && r.h.s->ability_ends_sent() == 0, "an acknowledgement and a trigger in one datagram: the window has room, but the join's chains still go first (nothing of the abilities is sent)");
    r.h.settle();
    r.play(0.5, true);
    CHECK(r.h.s->join_pending() == 0 && r.h.s->ability_updates_sent() >= 1 && r.h.s->ability_ends_sent() == 1 && r.m.has_ability(kSprint) && r.m.has_effect(kSprintEffect) && r.m.end_ids() == std::vector<int32_t>{1}, "once the join is out the client gets the Sprint and then the refusal, both");
}

static void test_ability_waits_for_the_join() {
    std::printf("an ability before the join is on the wire\n");
    Run r(248);
    CHECK(r.h.s->join_pending() > 0, "(the join is queued behind the PlayerController's full window)");
    r.trigger(kSprint);
    for (int i = 0; i < 5; ++i) r.tick(true);
    CHECK(r.h.s->join_pending() > 0 && r.h.s->ability_updates_sent() == 0 && r.h.s->ability_ends_sent() == 0 && !r.h.s->abilities_failed(), "the Sprint is started, but no ability update goes out while the join waits");
    r.trigger(kScepterCombo);
    CHECK(r.h.s->ability_updates_sent() == 0 && r.h.s->ability_ends_sent() == 0 && r.h.s->ability_ends_pending() == 1, "and a refusal's ClientEndAbility waits too");
    test_ability_waits_for_join_settle(r);
}

static void test_ability_update_waits_release(Run &r) {
    r.h.release();
    r.play(1.0, true);
    CHECK(r.h.s->ability_updates_sent() >= 1 && r.m.has_ability(kSprint) && r.m.has_effect(kSprintEffect) && r.m.stats[kMoveSpeedMult] == 1.2f, "once the client acknowledges, the update goes out: the Sprint, its effect and MoveSpeedMult");
    bool decreasing = !r.m.stamina.empty();
    for (size_t i = 1; i < r.m.stamina.size(); ++i) decreasing = decreasing && r.m.stamina[i].second < r.m.stamina[i - 1].second;
    CHECK(decreasing && near(r.m.stamina_now(), r.rt().stamina(), 1.3) && r.m.unreadable == 0, "with the Stamina as it was by then, the later steps one by one in order, and the client within a charge of the runtime");
}

static void test_ability_update_waits_for_the_window() {
    std::printf("an ability update waits for a full window\n");
    Run r;
    r.play(0.5, true);
    const size_t room = Connection::kMaxUnackedReliable - r.h.conn.unacked_reliable(r.h.pawn_ch);
    r.h.hold = true;
    r.h.filler(r.h.pawn_ch, room);
    CHECK(r.h.conn.unacked_reliable(r.h.pawn_ch) == Connection::kMaxUnackedReliable, "(the pawn channel's window is full)");
    r.trigger(kSprint);
    r.play(1.5, true);
    CHECK(r.h.s->ability_updates_sent() == 0 && !r.m.has_ability(kSprint) && !r.h.s->abilities_failed() && r.rt().sprinting(), "the Sprint runs on the server, and its update waits for the window (nothing is dropped, nothing fails)");
    test_ability_update_waits_release(r);
}

static size_t count_lines(const std::string &text, const char *a, const char *b) {
    size_t n = 0, pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) ++n;
        pos = end + 1;
    }
    return n;
}

static void test_ability_log_drift() {
    Run r;
    r.play(0.5, true);
    const int32_t id = r.trigger(kSprint);
    r.play(0.3, true);
    bs::AbilityChange gone;
    gone.effects_removed.push_back(0);
    CHECK(const_cast<bs::PlayerBootstrap &>(r.h.s->bootstrap()).apply_ability_change(gone), "(the test removes the Sprint's effect from the pawn behind the runtime's back)");
    g_log.str("");
    r.cancel(id);
    spdlog::default_logger()->flush();
    const std::string text = g_log.str();
    CHECK(r.h.s->abilities_failed() && text.find("abilities are off for this session") != std::string::npos && text.find("the pawn lists 1 ability item(s)") != std::string::npos && text.find("[warning]") != std::string::npos, "the runtime ends the Sprint, the pawn does not take it: abilities are off, and a warning says what the pawn still lists");
    const size_t updates = r.h.s->ability_updates_sent();
    r.trigger(kDodge);
    r.play(0.5, true);
    CHECK(r.h.s->ability_updates_sent() == updates && r.m.stamina_now() > 90.0f && !r.m.has_ability(kDodge) && r.h.s->ability_triggers() == 1, "afterwards abilities are not run: a Dodge costs nothing, adds nothing and is not even counted");
}

static void test_ability_log() {
    std::printf("the ability log\n");
    Run r;
    r.play(0.3, true);
    g_log.str("");
    for (int i = 0; i < 60; ++i) {
        const int32_t id = r.trigger(kSprint);
        r.play(0.1, true);
        r.cancel(id);
        r.play(0.1, true);
    }
    spdlog::default_logger()->flush();
    const std::string text = g_log.str();
    const size_t info = count_lines(text, "[info]", "(Stamina ");
    const size_t debug = count_lines(text, "[debug]", "the Sprint");
    CHECK(info == PlayerSession::kLoggedAbilityTransitions && debug >= 20, "120 transitions: the first 100 are at info (one line each), the rest at debug");
    CHECK(r.h.s->ability_triggers() == 60 && r.h.s->ability_triggers_dropped() == 0 && !r.h.s->abilities_failed(), "(all 60 triggers were handled)");
    test_ability_log_drift();
}

static void test_refusals_corrupted() {
    Run r;
    r.play(0.5, false);
    const BunchWriter good = trigger_bunch(r.h.ability_guid(), kNotRun, 4);
    const auto resolver = [&](const emit::FIntrepidNetworkGUID &g) -> const schema::ClassSchema * {
        return g == r.h.ability_guid() ? &class_named("UAoCAbilityComponent") : nullptr;
    };
    const auto parsed = repl::read_client_bunch(class_named("PlayerPawn_C"), resolver, good.data(), good.bit_pos(), client_options());
    const bool found = parsed && parsed->rpcs.size() == 1 && parsed->rpcs[0].call.decoded;
    CHECK(found, "(the trigger of an ability world does not run reads whole)");
    if (!found) return;
    const size_t bit = parsed->rpcs[0].call.param_pos + parsed->rpcs[0].call.param_bits - 6;
    const BunchWriter bad = flipped(good, bit);
    const auto reread = repl::read_client_bunch(class_named("PlayerPawn_C"), resolver, bad.data(), bad.bit_pos(), client_options());
    CHECK(reread && reread->rpcs.size() == 1 && !reread->rpcs[0].call.decoded, "(with the empty item's bit set it does not decode)");
    g_log.str("");
    r.send_reliable(bad);
    CHECK(r.m.end_ids() == std::vector<int32_t>{4}, "an ability world does not run, with a parameter that does not read: ClientEndAbility(4), from its first two parameters");
    const BunchWriter sprint = trigger_bunch(r.h.ability_guid(), kSprint, 5);
    const auto sp = repl::read_client_bunch(class_named("PlayerPawn_C"), resolver, sprint.data(), sprint.bit_pos(), client_options());
    const size_t sbit = sp && sp->rpcs.size() == 1 ? sp->rpcs[0].call.param_pos + sp->rpcs[0].call.param_bits - 6 : 0;
    r.send_reliable(flipped(sprint, sbit));
    CHECK(!r.rt().sprinting() && !r.m.has_ability(kSprint) && r.m.end_ids() == std::vector<int32_t>{4}, "a Sprint that does not read is not started: half a call is not acted on, and there is nothing to answer");
    spdlog::default_logger()->flush();
    CHECK(g_log.str().find("does not read") != std::string::npos, "and it is logged");
}

static void test_refusals_finished_and_budget() {
    {
        Run r;
        r.play(0.5, false);
        r.trigger(kSprint);
        r.h.s->finish("the test");
        const size_t steps = r.m.stamina.size();
        r.trigger(kScepterCombo);
        r.cancel(0);
        r.play(1.0, true);
        CHECK(r.m.stamina.size() == steps && r.m.ends.empty() && r.m.has_ability(kSprint) && r.h.s->ability_triggers() == 1, "after finish() a trigger and a cancel are not acted on (the Sprint is neither ended nor answered) and its drain stops");
    }
    {
        Run r;
        r.play(0.5, false);
        for (int i = 0; i < 20; ++i) r.trigger(kScepterCombo);
        CHECK(r.h.s->ability_triggers() == 8 && r.h.s->ability_triggers_dropped() == 12 && r.m.ends.size() == 8, "20 triggers in an instant: 8 handled (and answered), 12 dropped for the budget and counted");
        r.play(1.0, false);
        r.trigger(kScepterCombo);
        CHECK(r.h.s->ability_triggers() == 9 && r.m.ends.size() == 9, "a second later the budget allows more");
    }
}

static void test_refusals() {
    std::printf("what is refused\n");
    {
        Run r;
        r.play(0.5, false);
        const int32_t a = r.trigger(kScepterCombo);
        const int32_t b = r.trigger(kQuickWit);
        CHECK(r.m.end_ids() == ints({a, b}) && r.m.stamina_now() == 100.0f && r.m.abilities.empty() && r.m.effects.empty(), "abilities world does not run (a combo, QuickWit) are answered with ClientEndAbility(their ids) and nothing else");
        CHECK(r.h.s->ability_triggers() == 2 && r.h.s->ability_updates_sent() == 0 && r.h.s->ability_ends_sent() == 2, "(two triggers handled, no update, two ends)");
    }
    test_refusals_corrupted();
    test_refusals_finished_and_budget();
}

static void test_laggy_client() {
    std::printf("a client whose acknowledgements are late\n");
    Run r;
    r.play(0.5, false);
    r.h.hold = true;
    const int32_t id = r.trigger(kRest);
    r.play(1.0, false);
    r.cancel(id);
    r.play(0.5, false);
    CHECK(r.m.abilities.empty() && r.h.s->ability_updates_sent() == 2, "(two updates went out while the client heard nothing: the Rest and its cancel)");
    r.h.release();
    r.play(1.0, false);
    CHECK(r.m.chains_with_exports == 2, "the montage's export rides in both updates: the first was not acknowledged when the second was built");
    bool jumped = false;
    for (const auto &[eid, a] : r.m.abilities) jumped = a.ability == kRest && a.may_jump && a.animation_counter == 1;
    CHECK(jumped && r.m.unreadable == 0 && !r.h.s->abilities_failed(), "the client reads both, in order: the Rest, then its jump to the end section");
    r.trigger(kRest);
    CHECK(r.m.chains_with_exports == 2, "once the export is acknowledged it is not sent again");
    Run s;
    s.play(0.3, true);
    s.trigger(kSprint);
    s.h.hold = true;
    s.play(3.0, true);
    CHECK(s.m.stamina_now() == 98.75f, "(nothing reached the client for 3 s but the activation's charge)");
    s.h.release();
    s.play(0.3, true);
    CHECK(near(s.m.stamina_now(), static_cast<float>(s.rt().stamina()), 1.3) && s.m.has_ability(kSprint) && s.m.stamina.size() >= 12 && s.m.unreadable == 0, "released: the client sees the Sprint and every step the server took, and is within a charge of the runtime");
}

static BunchWriter trigger_at_value(const bs::PlayerSetup &setup, int64_t guid, int32_t id, const repl::Value &target, int anim_index = 0) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(guid));
    if (ok && id != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(id));
    if (ok && anim_index != 0) ok = repl::set_rpc_param(*rpc, p, "AnimationIndex", repl::Value::i64(anim_index));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    if (ok) {
        repl::ArrayValue targets;
        repl::ElementState e;
        e.values[1] = target;
        targets.elements.push_back(std::move(e));
        ok = repl::set_rpc_param(*rpc, p, "Targets", repl::Value(std::move(targets)));
    }
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(-1)) && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(repl::ItemInstance{})) && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(24));
    return ok ? ability_rpc(setup.guids.ability, "ServerTriggerAbility", p) : BunchWriter();
}

static repl::InlineObject inline_cylinder_of(const world::WorldActor &wa) {
    repl::InlineObject cyl;
    cyl.flags = 5; cyl.outer = wa.actor.guid; cyl.path = "CollisionCylinder"; cyl.checksum = 0xb876932f;
    return cyl;
}

struct WolfWorld {
    uint64_t wolf = 0, next_actor_id = 5000000;
    bool ground = false;
    int level = 8, extra = 0, extra_level = 0;
    double ahead = 120;
    std::vector<uint64_t> wolves;
    void prepare(PlayerWorld &w) {
        std::string err;
        if (!w.load_blueprints(NPC_MOTION_PATH, err)) return;
        auto nav = nav::AreaNav::from_file(navfix::build_file(), &err);
        ground = nav && w.adopt_ground(std::move(nav), err);
        auto captured = spawns::CapturedData::load(err);
        if (!ground || !captured) return;
        std::string path;
        for (const auto &[p, row] : w.blueprints().rows()) if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) path = p;
        wolf = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + ahead, kY, kZ, level, 800);
        for (int i = 0; i < extra; ++i) {
            const uint64_t id = combat_rig::spawn_npc(w.registry(), w.npcs(), *captured, next_actor_id, path, kX + ahead, kY + 100 * (i + 1), kZ, extra_level > 0 ? extra_level : level, 800);
            if (id != 0) wolves.push_back(id);
        }
    }
};

struct TimedHit { double at = 0; std::string hit, source; repl::HitInstance raw; };

struct FlightCast {
    const char *ability = "";
    int anim = 0, extra = 0, wolf_level = 25, extra_level = 0;
    double ahead = 1500, seconds = 3.0, bar = 0, then_at = 0;
    int64_t branch = 0;
    const char *then = "";
};

struct FlightEnd { std::string log; std::vector<double> wolf_health; int charges_left = -1; };

static std::vector<double> flight_leaves(const std::string &log, const std::string &who) {
    std::vector<double> out;
    std::istringstream in(log);
    for (std::string line; std::getline(in, line);) {
        const size_t at = line.find(who);
        if (at == std::string::npos) continue;
        const size_t lv = line.find(" leaves ", at);
        if (lv != std::string::npos) out.push_back(std::strtod(line.c_str() + lv + 8, nullptr));
    }
    return out;
}

static void send_branch_verification(Run &r, const bs::PlayerSetup &setup, int64_t branch, int32_t id) {
    const schema::RpcSchema *rpc = class_named("UAoCAbilityComponent").rpc("ServerRequestBranchVerification");
    repl::RpcParams bp;
    bool ok = rpc && repl::set_rpc_param(*rpc, bp, "Request.Branch.BranchGuid", repl::Value::i64(branch));
    ok = ok && repl::set_rpc_param(*rpc, bp, "Request.Branch.MontageIndex", repl::Value::i64(0));
    ok = ok && repl::set_rpc_param(*rpc, bp, "Request.Branch.MeshYaw", repl::Value::u64(0));
    ok = ok && repl::set_rpc_param(*rpc, bp, "Request.Branch.Instigator", repl::Value(setup.guids.pawn));
    ok = ok && repl::set_rpc_param(*rpc, bp, "ClientInstanceId", repl::Value::i64(id));
    if (ok) r.send_reliable(ability_rpc(setup.guids.ability, "ServerRequestBranchVerification", bp));
}

static void collect_flight_hits(const Run &r, size_t before, double base, std::vector<TimedHit> &out) {
    const abilities::Catalog *cat = r.h.fx.world->ability_catalog();
    for (size_t i = before + out.size(); i < r.m.hits.size(); ++i) {
        const repl::HitInstance &h = r.m.hits[i];
        const abilities::HitDef *hd = cat ? cat->hit(h.record_guid) : nullptr;
        const content::ProjectileRow *pr = cat ? cat->projectile(h.source_record_guid) : nullptr;
        out.push_back(TimedHit{r.now() - base, hd ? hd->row.name : "", pr ? pr->name : "", h});
    }
}

static void step_flight_cast(Run &r, const FlightCast &fc, const repl::InlineObject &cyl, int32_t id, std::vector<TimedHit> &out) {
    const bs::PlayerSetup setup = r.h.s->bootstrap().setup();
    const abilities::Catalog *cat = r.h.fx.world->ability_catalog();
    const abilities::AbilityDef *then_def = (cat && fc.then[0] != '\0') ? cat->ability_named(fc.then) : nullptr;
    const size_t before = r.m.hits.size();
    const double base = r.now();
    bool requested = false, then_sent = false;
    for (int i = 0; i < static_cast<int>(fc.seconds / 0.05); ++i) {
        if (then_def && !then_sent && (i + 1) * 0.05 >= fc.then_at - 1e-9) {
            then_sent = true;
            const int32_t then_id = r.next_id++;
            const BunchWriter then_trig = trigger_at_value(setup, then_def->row.guid, then_id, repl::Value(cyl), 0);
            r.send_trigger(then_def->row.guid, then_id, &then_trig);
        }
        if (fc.branch != 0 && !requested && (i + 1) * 0.05 >= fc.bar - 1e-9) {
            requested = true;
            send_branch_verification(r, setup, fc.branch, id);
        }
        r.tick(false, std::chrono::milliseconds(50));
        r.h.fx.world->tick_world(r.now());
        collect_flight_hits(r, before, base, out);
    }
}

static void record_flight_end(const Run &r, const WolfWorld &ww, int64_t ability_guid, FlightEnd *end) {
    if (!end) return;
    spdlog::default_logger()->flush();
    end->log = g_log.str();
    if (r.h.s->pipeline()) end->charges_left = r.h.s->pipeline()->charges_left(ability_guid);
    const combat::CombatWorld &cw = *r.h.fx.world->combat();
    end->wolf_health.push_back(cw.health_of(ww.wolf));
    for (const uint64_t w : ww.wolves) end->wolf_health.push_back(cw.health_of(w));
}

static bool run_flight_cast(const svc::Character &rec, const FlightCast &fc, std::vector<TimedHit> &out, FlightEnd *end = nullptr) {
    g_log.str("");
    WolfWorld ww{0, 5000000, false, fc.wolf_level, fc.extra, fc.extra_level, fc.ahead, {}};
    Run r(0, true, &rec, [&](PlayerWorld &w) { ww.prepare(w); });
    if (!ww.ground || ww.wolf == 0 || !r.h.fx.world->combat()) return false;
    r.play(0.5, false);
    const world::WorldActor *wa = r.h.fx.world->registry().find(ww.wolf);
    const abilities::Catalog *cat = r.h.fx.world->ability_catalog();
    const abilities::AbilityDef *def = cat ? cat->ability_named(fc.ability) : nullptr;
    const abilities::AbilityDef *then_def = (cat && fc.then[0] != '\0') ? cat->ability_named(fc.then) : nullptr;
    if (!wa || !def || (fc.then[0] != '\0' && !then_def)) return false;
    r.h.s->teach_ability(def->row.guid);
    if (then_def) r.h.s->teach_ability(then_def->row.guid);
    r.h.fx.world->tick_world(r.now());
    const repl::InlineObject cyl = inline_cylinder_of(*wa);
    const int32_t id = r.next_id++;
    const BunchWriter trig = trigger_at_value(r.h.s->bootstrap().setup(), def->row.guid, id, repl::Value(cyl), fc.anim);
    r.send_trigger(def->row.guid, id, &trig);
    step_flight_cast(r, fc, cyl, id, out);
    record_flight_end(r, ww, def->row.guid, end);
    return true;
}

static void test_flight_firebolt(const svc::Character &mage) {
    for (const int anim : {0, 1, 2, 3, -1}) {
        FlightCast fc{"Mage_Firebolt", anim};
        fc.seconds = 2.0;
        std::vector<TimedHit> hits;
        const bool ok = run_flight_cast(mage, fc, hits);
        CHECK(ok, "a world with ground and a Wolf for the Firebolt");
        if (!ok) return;
        int landed = 0;
        std::set<std::string> carried;
        for (const TimedHit &h : hits) {
            if (h.hit != "Mage_FireballCombo_Projectile_Hit") continue;
            ++landed;
            carried.insert(h.source);
        }
        const std::string want = anim == 1 ? "Mage_FireballCombo_Projectile_L" : "Mage_FireballCombo_Projectile_R";
        CHECK(landed == 1, (std::string("Firebolt, montage ") + std::to_string(anim) + ": one Hit confirmation, not one for each hand").c_str());
        CHECK(carried == std::set<std::string>{want}, (std::string("Firebolt, montage ") + std::to_string(anim) + ": the Hit came from " + want).c_str());
    }
}

static void test_flight_frostbolt(const svc::Character &mage) {
    {
        FlightCast fc{"Mage_Frostbolt"};
        fc.seconds = 4.5;
        fc.branch = 6064628890262700041ll;
        fc.bar = 2.2;
        std::vector<TimedHit> hits;
        const bool ok = run_flight_cast(mage, fc, hits);
        CHECK(ok, "a world with ground and a Wolf for the Frostbolt");
        if (!ok) return;
        int bolt = 0, shard = 0;
        for (const TimedHit &h : hits) {
            if (h.hit == "Mage_Frostbolt_Hit") ++bolt;
            if (h.hit == "Mage_Frostbolt_Hit_Shard") ++shard;
        }
        CHECK(bolt == 1 && shard == 0, "Frostbolt: the bolt's Hit lands once and the shard's Hit does not land with it");
    }
    {
        FlightCast fc{"Mage_Frostbolt"};
        fc.seconds = 6.5;
        std::vector<TimedHit> hits;
        const bool ok = run_flight_cast(mage, fc, hits);
        CHECK(ok, "a world with ground and a Wolf for the Frostbolt without a request");
        if (!ok) return;
        int bolt = 0, shard = 0;
        for (const TimedHit &h : hits) {
            if (h.hit == "Mage_Frostbolt_Hit") ++bolt;
            if (h.hit == "Mage_Frostbolt_Hit_Shard") ++shard;
        }
        CHECK(bolt == 1 && shard == 0, "Frostbolt, the bar's end never asked for (the deadline): the bolt's Hit lands once and the shard's does not");
    }
}

static void test_flight_salvo_distance() {
    const svc::Character ranger = class25(1, "Ranger", 17751);
    std::vector<TimedHit> near_hits, far_hits;
    FlightCast fc{"Ranger_Salvo"};
    fc.ahead = 500;
    bool ok = run_flight_cast(ranger, fc, near_hits);
    fc.ahead = 2500;
    ok = ok && run_flight_cast(ranger, fc, far_hits);
    CHECK(ok, "a world with ground and a Wolf for the Salvo");
    if (!ok) return;
    std::vector<double> near_at, far_at;
    for (const TimedHit &h : near_hits) if (h.hit == "Ranger_Salvo") near_at.push_back(h.at);
    for (const TimedHit &h : far_hits) if (h.hit == "Ranger_Salvo") far_at.push_back(h.at);
    CHECK(near_at.size() >= 4 && near_at.size() == far_at.size(), "Salvo: the same number of arrows land at 5 and at 25 m");
    if (!near_at.empty() && !far_at.empty()) {
        const double gap = far_at.front() - near_at.front();
        std::printf("  (the first arrow lands %.2f s after the trigger at 5 m and %.2f s at 25 m)\n", near_at.front(), far_at.front());
        CHECK(gap > 0.2 && gap < 0.4, "Salvo: the first arrow lands about 0.29 s later at 25 m than at 5 m (its flight)");
    }
}

static void test_flight_salvo_slow_and_interrupted() {
    {
        svc::Character slow = class25(1, "Ranger", 17751);
        slow.level = 15;
        FlightCast fc{"Ranger_Salvo"};
        fc.seconds = 2.6;
        std::vector<TimedHit> hits;
        FlightEnd end;
        const bool ok = run_flight_cast(slow, fc, hits, &end);
        CHECK(ok, "a world with ground and a Wolf for the Salvo at a slow rate");
        if (!ok) return;
        const std::vector<double> arrows = flight_leaves(end.log, "Ranger_Salvo: arrow ");
        int landed = 0;
        for (const TimedHit &h : hits) landed += (h.hit == "Ranger_Salvo" && !h.raw.targets.empty()) ? 1 : 0;
        std::printf("  (the slow Salvo: %zu arrows, the first %.3f s after the trigger; %d hits; %d charges left)\n", arrows.size(), arrows.empty() ? 0.0 : arrows.front(), landed, end.charges_left);
        CHECK(!arrows.empty() && arrows.front() > 0.33 && arrows.front() < 0.42, "Salvo at a rate of 0.79: the first arrow leaves about 0.37 s after the trigger (0.2943 s of montage over 0.79)");
        CHECK(end.charges_left == 0 && arrows.size() == 6 && landed == 6, "Salvo at a rate of 0.79: six shots paid, six arrows left and six hits landed");
    }
    {
        const svc::Character ranger = class25(1, "Ranger", 17751);
        FlightCast fc{"Ranger_Salvo"};
        fc.then = "Ranger_PowerShot";
        fc.then_at = 0.7;
        fc.seconds = 2.8;
        std::vector<TimedHit> hits;
        FlightEnd end;
        const bool ok = run_flight_cast(ranger, fc, hits, &end);
        CHECK(ok, "a world with ground and a Wolf for the Salvo and a second ability");
        if (!ok) return;
        const std::vector<double> arrows = flight_leaves(end.log, "Ranger_Salvo: arrow ");
        int landed = 0;
        for (const TimedHit &h : hits) landed += (h.hit == "Ranger_Salvo" && !h.raw.targets.empty()) ? 1 : 0;
        const int paid = 6 - end.charges_left;
        std::printf("  (the Salvo cut by a second ability: %d shots paid, %zu arrows (%.3f s after the trigger for the first), %d hits)\n", paid, arrows.size(), arrows.empty() ? 0.0 : arrows.front(), landed);
        CHECK(paid >= 1 && paid < 6, "Salvo with another ability pressed in the middle: the loop ended early, not all six shots were paid");
        CHECK(paid == static_cast<int>(arrows.size()) && static_cast<int>(arrows.size()) == landed, "... every shot paid sent its arrow and landed its hit, and no arrow left or landed unpaid");
    }
}

static void test_flight_discordance() {
    const svc::Character bard = bard25();
    {
        FlightCast fc{"Bard_Misc_Discordance", 0, 1};
        fc.seconds = 5.0;
        std::vector<TimedHit> hits;
        const bool ok = run_flight_cast(bard, fc, hits);
        CHECK(ok, "a world with ground and two Wolves for the Discordance");
        if (!ok) return;
        const TimedHit *first = nullptr, *child = nullptr;
        for (const TimedHit &h : hits) {
            if (!first && h.hit == "Bard_Misc_Discordance_Hit1" && !h.raw.targets.empty()) first = &h;
            if (!child && h.hit == "Bard_Misc_Discordance_Bounce1" && !h.raw.targets.empty()) child = &h;
        }
        CHECK(first != nullptr && child != nullptr, "the note's Hit1 and its bounce's child hit are both confirmed");
        if (first && child) {
            CHECK(child->raw.source.ObjectId != 0 && child->raw.source == first->raw.targets[0].hit_comp, "the bounce's child hit names the enemy the note landed on as its Source");
            CHECK(!(child->raw.targets[0].hit_comp == first->raw.targets[0].hit_comp), "... and the other Wolf as its target");
        }
    }
    {
        FlightCast fc{"Bard_Misc_Discordance", 0, 1, 1, 25};
        fc.seconds = 5.0;
        std::vector<TimedHit> hits;
        FlightEnd end;
        const bool ok = run_flight_cast(bard, fc, hits, &end);
        CHECK(ok, "a world with ground and two weak Wolves for the Discordance");
        if (!ok) return;
        int notes = 0, bounces = 0;
        for (const TimedHit &h : hits) {
            if (h.raw.targets.empty()) continue;
            if (h.hit == "Bard_Misc_Discordance_Hit1") ++notes;
            if (h.hit == "Bard_Misc_Discordance_Bounce1") ++bounces;
        }
        std::printf("  (the weak Wolf: %.1f Health left; %d note Hit1 confirmed, %d first bounces; the sturdy Wolf: %.1f Health left)\n", end.wolf_health.empty() ? -1.0 : end.wolf_health[0], notes, bounces, end.wolf_health.size() > 1 ? end.wolf_health[1] : -1.0);
        CHECK(end.wolf_health.size() == 2 && end.wolf_health[0] <= 0 && end.wolf_health[1] > 0, "the notes killed the weak Wolf and the sturdy Wolf lived through the bounces");
        CHECK(notes >= 1 && notes < 3 && bounces == notes, "every note that landed fired one bounce, and the note that landed on the body confirmed nothing and fired none");
    }
}

static void test_flight_throw() {
    const svc::Character rogue = class25(5, "Rogue", 17752);
    FlightCast fc{"Rogue_Throw", 0, 1};
    fc.ahead = 1200;
    std::vector<TimedHit> hits;
    FlightEnd end;
    const bool ok = run_flight_cast(rogue, fc, hits, &end);
    CHECK(ok, "a world with ground and two Wolves for the Throw");
    if (!ok) return;
    int bounces = 0;
    const TimedHit *child = nullptr;
    for (const TimedHit &h : hits) {
        if (h.hit != "Rogue_Throw_Bounce" || h.raw.targets.empty()) continue;
        ++bounces;
        if (!child) child = &h;
    }
    std::printf("  (the Throw at two Wolves: %d bounce child hits; %zu bounces left in the log)\n", bounces, flight_leaves(end.log, "Rogue_Throw: bounce ").size());
    CHECK(bounces == 1, "the Throw at two Wolves bounces once: one child hit is confirmed");
    if (child) CHECK(child->raw.source.ObjectId != 0 && !(child->raw.targets[0].hit_comp == child->raw.source), "... it names the first Wolf as its Source and the second Wolf as its target");
}

static void test_flight_leftovers() {
    std::printf("Flight leftovers: one projectile's hits a cast, the bounce's Source, the Salvo's arrows by distance\n");
    const svc::Character mage = class25(8, "Mage", 17750);
    test_flight_firebolt(mage);
    test_flight_frostbolt(mage);
    test_flight_salvo_distance();
    test_flight_salvo_slow_and_interrupted();
    test_flight_discordance();
    test_flight_throw();
}

int main() {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log);
    auto logger = std::make_shared<spdlog::logger>("test", sink);
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    test_sprint();
    test_sprint_cancel();
    test_sprint_standing();
    test_dodge();
    test_rest();
    test_sprint_and_dodge();
    test_instance_ids();
    test_dodge_dash();
    test_ability_waits_for_the_join();
    test_ability_update_waits_for_the_window();
    test_ability_log();
    test_refusals();
    test_laggy_client();
    if (ability_rig::real_db_present()) test_flight_leftovers();
    run_ability_session_pets_tests();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
