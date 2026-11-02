#include "test_worldd_ability_session_common.h"
#include "capture_blocks.h"
#include "session/player_session.h"
#include "spawns/area.h"
#include "world/relevancy.h"
using namespace aoc::worldd;
static void send_ready_to_phase_in(Run &r) {
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
}
static void test_e1_sprint_with_world() {
    std::printf("E1: a Sprint after the phase-in, with the living area around "
                "the player\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    std::string error;
    CHECK(r.h.fx.world->load_area(spawns::AreaConfig{}, error), ("the living area loads (" + error + ")").c_str());
    send_ready_to_phase_in(r);
    r.play(3.0, true);
    CHECK(r.h.s->actor_view() && r.h.s->actor_view()->count(world::ClientState::Awake) > 20, "(the world's actors are open on the client)");
    CHECK(r.m.stats.count(kStaminaRegeneration) != 0 && near(r.m.stats[kStaminaRegeneration], 5.0, 1e-6), "joined: the client's StaminaRegeneration is 5");
    const float full = r.m.stamina_now();
    const size_t before = r.m.stamina.size();
    const int32_t id = r.trigger(kSprint);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "the Sprint's first packet sets StaminaRegeneration to 0 on the client");
    r.play(5.0, true);
    CHECK(near(full - r.m.stamina_now(), 26.25, 2.6), "5 s of Sprint while moving: the client's Stamina fell by about 25 (5 "
                                                      "a second)");
    bool only_down = r.m.stamina.size() > before + 15;
    for (size_t i = before + 1; i < r.m.stamina.size(); ++i) { only_down = only_down && near(r.m.stamina[i - 1].second - r.m.stamina[i].second, 1.25, 1e-6); }
    CHECK(only_down, "...in steps of 1.25, each one an item the client received");
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "StaminaRegeneration stayed 0 the whole time");
    r.cancel(id);
    r.play(0.5, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "the Sprint is released: the pause keeps StaminaRegeneration at 0");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}
static void test_e1_clock_resync() {
    std::printf("E1 stage 3 fix: ClientSetTime again after the phase-in and "
                "every 120 s\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    const size_t joined = r.m.set_times.size();
    r.play(2.0, false);
    CHECK(r.m.set_times.size() == joined, "nothing more before the phase-in");
    send_ready_to_phase_in(r);
    r.play(0.5, false);
    CHECK(r.m.set_times.size() == joined, "not with the phase-in itself (the client's frames still hitch)");
    r.play(1.0, false);
    CHECK(r.m.set_times.size() == joined + 1 && near(r.m.set_times.back(), bs::effect_clock(r.now()), 1.6), "1 s after the phase-in: ClientSetTime again, with the world time of "
                                                                                                            "its send on the synchronized clock");
    r.play(4.0, false);
    CHECK(r.m.set_times.size() == joined + 2 && near(r.m.set_times.back(), bs::effect_clock(r.now()), 1.6), "...and again 5 s after it");
    r.play(60.0, false);
    CHECK(r.m.set_times.size() == joined + 2, "...then not for a while");
    r.play(61.0, false);
    std::printf("  (%zu sent since the join, the last %.2f s behind the clock)\n", r.m.set_times.size() - joined, r.m.set_times.empty() ? 0.0 : bs::effect_clock(r.now()) - r.m.set_times.back());
    CHECK(r.m.set_times.size() == joined + 3 && near(r.m.set_times.back(), bs::effect_clock(r.now()), 2.0), "...then every 120 s, as retail (pkt#23 and every 120.0 s after)");
    CHECK(r.m.unreadable == 0, "(every chain reads)");
}
static void test_e1_sprint_under_load() {
    std::printf("E1: a Sprint while the client is busy with the world's actors\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    std::string error;
    CHECK(r.h.fx.world->load_area(spawns::AreaConfig{}, error), "the living area loads");
    send_ready_to_phase_in(r);
    r.play(0.3, true);
    r.h.hold = true;
    size_t peak_flight = 0;
    const int32_t id = r.trigger(kSprint);
    for (int i = 0; i < 30; ++i) {
        r.tick(true);
        peak_flight = std::max(peak_flight, r.h.conn.packets_in_flight());
    }
    CHECK(peak_flight <= 140, "the connection never has more than about a hundred packets in flight "
                              "while the client acknowledges nothing (the window is 256)");
    CHECK(r.h.conn.unreliable_dropped() == 0, "no unreliable chain of the pawn was dropped from the queue");
    CHECK(r.h.s->actor_view() && r.h.s->actor_view()->stats().throttled > 0, "the actor system held back (throttled passes)");
    r.h.release();
    r.h.settle();
    r.absorb();
    r.play(1.0, true);
    CHECK(r.m.stats[kStaminaRegeneration] == 0.0f, "once the client is back: StaminaRegeneration is 0 on it");
    CHECK(near(r.m.stamina_now(), r.rt().stamina(), 1.3), "...and its Stamina is the runtime's (every drop item arrived)");
    r.cancel(id);
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "(nothing failed)");
}
static void run_loss_session_steps(Run &r, size_t &wraps, size_t &cycles, size_t &bad_cycles) {
    uint16_t last_seq = r.h.conn.notify().out_seq();
    for (int i = 0; i < 24000; ++i) {
        r.ts += 0.016f;
        r.h.client_sends({r.h.client.actor(r.h.pawn_ch, client_move(move_at(r.ts, r.x, false)), false)});
        r.h.step(milliseconds(16));
        r.absorb();
        const uint16_t seq = r.h.conn.notify().out_seq();
        if (seq < last_seq) { ++wraps; }
        last_seq = seq;
        if (i % 1000 == 100) { r.trigger(kDodge); }
        if (i % 1000 == 999) {
            ++cycles;
            if (!near(r.m.stamina_now(), r.rt().stamina(), 1.3)) {
                ++bad_cycles;
                std::printf("    cycle %zu at step %d: client Stamina %.2f, runtime "
                            "%.2f (out seq %u)\n",
                            cycles, i, r.m.stamina_now(), r.rt().stamina(), seq);
            }
        }
    }
}
static void test_e1_long_session_with_loss(unsigned loss_percent, unsigned burst) {
    std::printf("E1: a long session across the packet sequence wrap, %u%% loss, "
                "bursts of %u\n",
                loss_percent, burst);
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    std::string error;
    r.h.fx.world->load_area(spawns::AreaConfig{}, error);
    send_ready_to_phase_in(r);
    std::mt19937 rng(7);
    auto link = [&, left = std::make_shared<unsigned>(0)](const std::vector<uint8_t> &) {
        if (*left > 0) {
            --*left;
            return true;
        }
        if (rng() % 100 < loss_percent) {
            *left = burst - 1;
            return true;
        }
        return false;
    };
    if (loss_percent > 0) {
        r.h.lose_to_client = link;
        r.h.lose_to_server = link;
    }
    size_t wraps = 0;
    size_t bad_cycles = 0;
    size_t cycles = 0;
    run_loss_session_steps(r, wraps, cycles, bad_cycles);
    std::printf("    wraps %zu, cycles %zu, bad %zu, packets in flight %zu, "
                "unreliable dropped %zu, lost to client %zu, to server %zu\n",
                wraps, cycles, bad_cycles, r.h.conn.packets_in_flight(), r.h.conn.unreliable_dropped(), r.h.lost_to_client, r.h.lost_to_server);
    CHECK(wraps >= 1 && bad_cycles == 0, "the client's Stamina follows the runtime's across the packet sequence "
                                         "wrap");
}
static void test_e1_clock_reset() {
    std::printf("E1: the client's movement clock resets at 240 s\n");
    Run r;
    r.ts = 238.0f;
    r.play(1.0, true);
    const size_t acked_before = r.h.s->moves_acknowledged();
    CHECK(acked_before >= 9, "(the moves of the first 239 s are acknowledged)");
    r.ts = 239.9f;
    r.play(0.1, true);
    r.ts = 0.0f;
    const size_t before_reset = r.h.s->moves_acknowledged();
    r.play(1.0, true);
    CHECK(r.h.s->moves_acknowledged() >= before_reset + 9, "the moves after the reset (TimeStamp 0.1 ...) are acknowledged, one "
                                                           "by one");
    CHECK(r.h.s->moves_stale() == 0 && r.h.s->moves_refused() == 0, "none is dropped as a duplicate, none refused");
    CHECK(r.h.s->movement().good().time_stamp < 5.0f, "the last good TimeStamp follows the client's new clock");
    const int32_t id = r.trigger(kSprint);
    r.play(3.0, true);
    CHECK(r.m.stamina_now() < 96.0f && r.rt().sprinting(), "a Sprint run after the reset drains the client's Stamina (the server "
                                                           "hears the player move)");
    CHECK(r.m.stamina.size() >= 10, "...in steps the HUD sees");
    r.cancel(id);
    r.ts = 239.95f;
    r.play(0.2, true);
    r.ts = 0.0f;
    const size_t before_second = r.h.s->moves_acknowledged();
    r.play(1.0, true);
    CHECK(r.h.s->moves_acknowledged() >= before_second + 9, "and again at the next 240 s");
    const size_t acked = r.h.s->moves_acknowledged();
    r.ts -= 2.0f;
    r.play(0.1, true);
    CHECK(r.h.s->moves_acknowledged() == acked && r.h.s->moves_stale() >= 1, "a TimeStamp 2 s back is no reset: dropped without a reply");
}
static bool verify_maddening_dance_open(Run &r, double t_trigger, uint32_t &out_channel) {
    r.trigger(kMaddening);
    r.play(0.2, false);
    CHECK(r.m.other.empty(), "nothing at the trigger and 200 ms on: the "
                             "montage's first notify comes at 300 ms");
    r.play(0.4, false);
    CHECK(r.m.other.size() == 1 && r.m.other[0].open && !r.m.other[0].close, "Maddening Dance: one actor channel opens");
    if (r.m.other.empty()) { return false; }
    out_channel = r.m.other[0].channel;
    const double opened_at = r.m.other[0].at - t_trigger;
    CHECK(opened_at >= 0.3 && opened_at <= 0.55, "within 300 to 500 ms of the trigger, as retail's (+362 ms)");
    CHECK(r.m.other[0].exports >= 1 && r.m.other[0].bits > 1500 && r.m.other[0].bits < 2600, "it is the retail actor's size: an export bunch (the archetype, the "
                                                                                             "pawn's mesh, its three components) and about 2,000 bits");
    CHECK(out_channel != r.h.pawn_ch && out_channel != r.h.pc_ch && r.h.s->zones_opened() == 1 && r.h.s->zone_actors() == 1, "(on a channel of its own; the session holds it)");
    r.play(3.0, false);
    CHECK(r.m.other.size() == 1 && r.h.s->zone_actors() == 1 && !r.h.s->abilities_failed(), "3.6 s on: no close, no update");
    return true;
}
static void verify_maddening_dance_close(Run &r, double t_trigger, uint32_t channel) {
    r.h.s->teach_ability(kFlourish);
    r.trigger(kFlourish);
    r.play(0.8, false);
    CHECK(r.m.other.size() == 1, "(a Flourish meanwhile does not end it)");
    r.play(1.8, false);
    bool faded = false;
    bool closed = false;
    double closed_at = 0;
    for (size_t i = 1; i < r.m.other.size(); ++i) {
        const Mirror::ChannelEvent &e = r.m.other[i];
        if (e.channel != channel) { continue; }
        faded = faded || (!e.open && !e.close && e.bits > 0);
        if (e.close) {
            closed = true;
            closed_at = e.at - t_trigger;
        }
    }
    CHECK(faded && closed, "6.2 s after the trigger: the actor got its fade-out "
                           "update (bFadingOut) and its channel is closed");
    CHECK(closed && closed_at > 5.1 && closed_at < 5.8, "the close came about 5.4 s after the trigger [R: 5,383 ms]");
    CHECK(r.h.s->zone_actors() == 0 && r.h.s->zones_closed() == 1 && r.h.s->zone_ops_pending() == 0, "(the session holds none: one opened, one closed)");
}
static void verify_maddening_dance_actor(Run &r) {
    const double t_trigger = r.now();
    uint32_t channel = 0;
    if (verify_maddening_dance_open(r, t_trigger, channel)) { verify_maddening_dance_close(r, t_trigger, channel); }
}
static void verify_shielding_dance_actor(Run &r) {
    r.play(20.0, false);
    const double t_shield = r.now();
    r.trigger(kShielding);
    r.play(1.0, false);
    CHECK(r.m.has_effect(6064631046974416627ll) && r.m.other.size() > 2, "Shielding Dance: its Aura is on the Bard and a second actor is open");
    const size_t events = r.m.other.size();
    r.play(3.0, false);
    CHECK(r.m.has_effect(6064631046974416627ll), "4 s on: the Aura is still there");
    r.play(2.0, false);
    CHECK(!r.m.has_effect(6064631046974416627ll), "6 s on: the Aura is gone (it stayed for good before)");
    bool second_closed = false;
    double second_at = 0;
    for (size_t i = events; i < r.m.other.size(); ++i) {
        if (r.m.other[i].close) {
            second_closed = true;
            second_at = r.m.other[i].at - t_shield;
        }
    }
    CHECK(second_closed && second_at > 5.0 && second_at < 5.6 && r.h.s->zone_actors() == 0 && !r.h.s->abilities_failed(), "and the second actor is closed about 5.2 s after the trigger [R: "
                                                                                                                          "5,123 and 5,371 ms]");
}
static void test_d2_dance_actor() {
    std::printf("D2: a dance opens the zone's actor on a channel of its own and "
                "destroys it when the dance ends\n");
    const svc::Character rec = bard25();
    Run r(0, true, &rec);
    send_ready_to_phase_in(r);
    r.play(0.5, true);
    r.h.s->teach_ability(kShielding);
    r.h.s->teach_ability(kMaddening);
    CHECK(r.m.other.empty() && r.h.s->zone_actors() == 0, "(no actor before a dance)");
    verify_maddening_dance_actor(r);
    verify_shielding_dance_actor(r);
}
int main() {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log);
    auto logger = std::make_shared<spdlog::logger>("test", sink);
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    if (ability_rig::real_db_present()) {
        test_e1_sprint_with_world();
        test_e1_clock_resync();
        test_e1_sprint_under_load();
        test_e1_long_session_with_loss(0, 1);
        test_e1_long_session_with_loss(5, 4);
        test_e1_long_session_with_loss(10, 20);
        test_e1_clock_reset();
        test_d2_dance_actor();
    } else {
        std::printf("E1/D2 ability session part4 tests: skipped (no content.db)\n");
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
