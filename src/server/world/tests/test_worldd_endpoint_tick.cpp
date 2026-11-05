// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "test_worldd_endpoint_common.h"
#include "net/tick_loop.h"
#include "net/udp_socket.h"
#include "npc/npc_world.h"
#include "session/actor_view.h"
#include "session/view_phases.h"
#include <deque>

using namespace aoc::worldd;

namespace fixed_tick_checks {

using std::chrono::duration;
using std::chrono::microseconds;
using std::chrono::nanoseconds;

const Clock::time_point kStart{std::chrono::seconds(100)};

struct FakeNet {
    Clock::time_point now = kStart;
    std::deque<Clock::time_point> arrivals;
    std::vector<Clock::time_point> arrived;
    std::vector<Clock::time_point> handled;
    std::vector<Clock::time_point> ticks;
    std::vector<size_t> handled_before_tick;
    std::function<void(Clock::time_point)> work;
    Clock::time_point last_read{};

    TickLoop::Ports ports() {
        TickLoop::Ports p;
        p.now = [this] { return now; };
        p.receive = [this](microseconds timeout) -> std::optional<size_t> {
            if (!arrivals.empty() && arrivals.front() <= now + timeout) {
                if (arrivals.front() > now) now = arrivals.front();
                last_read = arrivals.front();
                arrivals.pop_front();
                return size_t{40};
            }
            now += timeout;
            return size_t{0};
        };
        p.datagram = [this](size_t, Clock::time_point t) {
            arrived.push_back(last_read);
            handled.push_back(t);
            now += microseconds(100);
        };
        p.tick = [this](Clock::time_point t) {
            ticks.push_back(t);
            handled_before_tick.push_back(handled.size());
            if (work) work(t);
        };
        return p;
    }
};

void run_until(TickLoop& loop, FakeNet& net, Clock::time_point end) {
    TickLoop::Ports p = net.ports();
    while (net.now < end) {
        if (!loop.run_once(p)) return;
    }
}

double seconds_of(Clock::time_point t) { return duration<double>(t - kStart).count(); }

template <class... A>
std::string text(const char* format, A... a) {
    char buf[400];
    std::snprintf(buf, sizeof(buf), format, a...);
    return buf;
}

}  // namespace fixed_tick_checks

static void test_fixed_tick_one_tick_for_many_datagrams() {
    using namespace fixed_tick_checks;
    std::printf("the fixed-rate tick: the datagrams between two due ticks run one tick\n");
    TickLoop loop(TickLoop::Config{30, 4, 256}, kStart);
    const Clock::duration period = loop.schedule().period();
    CHECK(period == nanoseconds(33333334), "30 a second is a period of 33,333,334 ns (1/30 s and a hair)");
    FakeNet net;
    for (int i = 0; i < 50; ++i) net.arrivals.push_back(kStart + std::chrono::milliseconds(5));
    run_until(loop, net, kStart + period + std::chrono::milliseconds(10));
    CHECK(net.handled.size() == 50 && net.ticks.size() == 1,
          "50 datagrams that arrive between two due ticks are handled and run ONE tick (the old loop ran 50)");
    CHECK(net.ticks.size() == 1 && net.ticks[0] == kStart + period && net.handled_before_tick[0] == 50,
          "the tick runs at its time on the grid, after the 50 datagrams");
    bool own_time = !net.handled.empty() && net.handled.front() == kStart + std::chrono::milliseconds(5);
    for (size_t i = 1; i < net.handled.size(); ++i)
        own_time = own_time && net.handled[i] == net.handled[i - 1] + microseconds(100);
    CHECK(own_time, "each datagram is handled the moment it is read, at its own time (not at a tick's)");
    const TickLoop::Stats s = loop.take_stats();
    CHECK(s.datagrams == 50 && s.ticks == 1 && s.max_datagrams_per_tick == 50,
          "the loop counts them: 50 datagrams, 1 tick, 50 between two ticks");

    TickLoop loop2(TickLoop::Config{30, 4, 256}, kStart);
    FakeNet net2;
    for (int i = 0; i < 5; ++i) net2.arrivals.push_back(kStart + period - microseconds(150));
    run_until(loop2, net2, kStart + period + std::chrono::milliseconds(5));
    bool ordered = net2.ticks.size() == 1 && net2.handled.size() == 5;
    std::vector<Clock::time_point> seen;
    for (size_t i = 0, d = 0; ordered && i <= net2.ticks.size(); ++i) {
        const size_t upto = i < net2.ticks.size() ? net2.handled_before_tick[i] : net2.handled.size();
        for (; d < upto; ++d) seen.push_back(net2.handled[d]);
        if (i < net2.ticks.size()) seen.push_back(net2.ticks[i]);
    }
    for (size_t i = 1; ordered && i < seen.size(); ++i) ordered = seen[i] >= seen[i - 1];
    CHECK(ordered && net2.handled_before_tick[0] == 2,
          "a tick due in the middle of a batch runs between its datagrams, and the times go forward only");
}

static void test_fixed_tick_catch_up() {
    using namespace fixed_tick_checks;
    std::printf("the fixed-rate tick: a slow tick is caught up within the cap; a stall starts the grid again; datagrams are read under overload\n");
    TickLoop loop(TickLoop::Config{30, 4, 256}, kStart);
    const Clock::duration period = loop.schedule().period();
    FakeNet net;
    net.work = [&](Clock::time_point) {
        if (net.ticks.size() == 3) net.now += period * 7 / 2;
        if (net.ticks.size() == 10) net.now += period * 21 / 2;
    };
    run_until(loop, net, kStart + period * 30);
    bool grid = net.ticks.size() >= 10;
    for (size_t i = 0; grid && i < 10; ++i) grid = net.ticks[i] == kStart + period * static_cast<int64_t>(i + 1);
    CHECK(grid, "the three ticks behind the slow one run back to back at their own times on the grid (4, 5, 6), then the grid goes on");
    CHECK(loop.schedule().caught_up() == 2, "two of them ran a whole period or more late (caught up)");
    const Clock::time_point stall_end = kStart + period * 10 + period * 21 / 2;
    CHECK(net.ticks.size() > 11 && net.ticks[10] >= stall_end && net.ticks[10] - stall_end < microseconds(1) &&
              net.ticks[11] == net.ticks[10] + period,
          "after a stall of 10.5 periods (more than the cap of 4) the next tick runs at once and the grid starts again from it");
    CHECK(loop.schedule().skipped() == 9 && loop.schedule().restarts() == 1,
          "the 9 ticks the stall missed are counted as skipped, not run");

    TickLoop busy(TickLoop::Config{30, 4, 256}, kStart);
    FakeNet net2;
    net2.work = [&](Clock::time_point) { net2.now += period * 3 / 2; };
    for (int i = 0; i < 200; ++i) net2.arrivals.push_back(kStart + std::chrono::milliseconds(10 * i));
    run_until(busy, net2, kStart + std::chrono::seconds(2) + std::chrono::milliseconds(500));
    double worst = 0;
    for (size_t i = 0; i < net2.handled.size(); ++i)
        worst = std::max(worst, duration<double, std::milli>(net2.handled[i] - net2.arrived[i]).count());
    const double bound = duration<double, std::milli>(period * 15 / 2 + period * 3 / 2 + period).count();
    CHECK(net2.handled.size() == 200 && worst <= bound,
          text("every tick 1.5 periods long: all 200 datagrams are still read, the longest waits %.0f ms (at most 5 ticks in a row and one more: %.0f ms)",
               worst, bound)
              .c_str());
    CHECK(busy.schedule().restarts() > 0 && net2.ticks.size() > 30,
          text("it keeps ticking (%zu ticks in 2.5 s) and drops the ticks it cannot run (%llu skipped)",
               net2.ticks.size(), static_cast<unsigned long long>(busy.schedule().skipped()))
              .c_str());
}

static void test_fixed_tick_rate() {
    using namespace fixed_tick_checks;
    std::printf("the fixed-rate tick: the rate asked for is the rate run\n");
    CHECK(TickSchedule::kMinHz == 10 && TickSchedule::kMaxHz == 120,
          "the start option allows 10 to 120 ticks a second (0: the old loop)");
    for (const int hz : {10, 15, 20, 30, 60, 120}) {
        TickLoop loop(TickLoop::Config{hz, 4, 256}, kStart);
        FakeNet net;
        run_until(loop, net, kStart + std::chrono::seconds(10) + std::chrono::milliseconds(1));
        const Clock::duration period = TickSchedule::period_of(hz);
        bool exact = period == nanoseconds(1000000000LL / hz + 1);
        for (size_t i = 0; exact && i < net.ticks.size(); ++i)
            exact = net.ticks[i] == kStart + period * static_cast<int64_t>(i + 1);
        CHECK(net.ticks.size() == static_cast<size_t>(hz) * 10 && exact,
              text("%d ticks a second: %zu ticks in 10 s, each at its time on the grid", hz, net.ticks.size()).c_str());
    }
}

static void check_game_timers_at_hz(int hz) {
    using namespace fixed_tick_checks;
    TickLoop loop(TickLoop::Config{hz, 4, 256}, kStart);
    const Clock::duration period = loop.schedule().period();
    world::ActorRegistry registry;
    npc::NpcWorld npcs(registry, nullptr);
    ActorView::Config view_config;
    tick_view_timing(view_config.relevancy, period);
    ActorView view(registry, view_config, [](uint32_t) { return false; });
    ActorView::Config old_config;
    ActorView old_view(registry, old_config, [](uint32_t) { return false; });
    Connection conn(ConnectionConfig{}, kStart);
    Connection old_conn(ConnectionConfig{}, kStart);
    uint64_t keepalives = 0, think = 0, regen = 0, step_ticks_two = 0, last_steps = 0;
    double next_think = -1, next_regen = -1;
    FakeNet net;
    net.work = [&](Clock::time_point t) {
        const double s = seconds_of(t);
        npcs.tick(s);
        const uint64_t steps = npcs.stats().steps;
        if (steps - last_steps > 1) ++step_ticks_two;
        last_steps = steps;
        view.tick(conn, 0, 0, t);
        old_view.tick(old_conn, 0, 0, t);
        keepalives += conn.flush(t).size();
        if (s >= next_think) {
            ++think;
            next_think = s + 0.2;
        }
        if (s >= next_regen) {
            ++regen;
            next_regen = s + 1.0;
        }
    };
    run_until(loop, net, kStart + std::chrono::seconds(10) + std::chrono::milliseconds(1));
    const uint64_t steps = npcs.stats().steps;
    CHECK(steps >= 149 && steps <= 151 && step_ticks_two == 0,
          text("%d Hz: the NPC world steps %llu times in 10 s (15 a second), never twice in one tick", hz,
               static_cast<unsigned long long>(steps))
              .c_str());
    const size_t want_passes = hz == 20 ? 200 : 150;
    CHECK(view.stats().passes + 1 >= want_passes && view.stats().passes <= want_passes + 1,
          text("%d Hz: each player's view passes %zu times in 10 s (its gate %lld ms on the grid)", hz,
               view.stats().passes, static_cast<long long>(view_config.relevancy.tick.count()))
              .c_str());
    CHECK(keepalives >= 49 && keepalives <= 51,
          text("%d Hz: a quiet connection's keep-alive goes out %llu times in 10 s (every 200 ms)", hz,
               static_cast<unsigned long long>(keepalives))
              .c_str());
    CHECK(think >= 49 && think <= 51 && regen >= 10 && regen <= 11,
          text("%d Hz: a 0.2 s gate opens %llu times in 10 s and a 1 s gate %llu times (never a tick late)", hz,
               static_cast<unsigned long long>(think), static_cast<unsigned long long>(regen))
              .c_str());
    const Clock::duration close = std::chrono::duration_cast<Clock::duration>(view_config.relevancy.dormancy_delay);
    const int64_t close_ticks = (close + period - Clock::duration(1)) / period;
    const int64_t want_close = (134 * hz + 500) / 1000;
    CHECK(close_ticks == want_close,
          text("%d Hz: a Dormancy close goes out %lld ticks after its open, the tick nearest 134 ms", hz,
               static_cast<long long>(close_ticks))
              .c_str());
    if (hz == 30) {
        CHECK(old_view.stats().passes <= 101,
              text("(30 Hz with the struct's 67 ms gate the view would pass only %zu times in 10 s: why the server sets its own)",
                   old_view.stats().passes)
                  .c_str());
    }
}

static void test_fixed_tick_game_timers() {
    std::printf("the fixed-rate tick: the game's timers keep their real-time lengths at 15, 20 and 30 ticks a second\n");
    for (const int hz : {15, 20, 30}) check_game_timers_at_hz(hz);
}

namespace view_spread_checks {

const Clock::time_point kOrigin{std::chrono::seconds(500)};

template <class... A>
std::string text(const char* format, A... a) {
    char buf[400];
    std::snprintf(buf, sizeof(buf), format, a...);
    return buf;
}

struct Crowd {
    world::ActorRegistry                     registry;
    ActorView::Config                        config;
    std::shared_ptr<ViewPhases>              phases;
    std::vector<std::unique_ptr<Connection>> conns;
    std::vector<std::unique_ptr<ActorView>>  views;
    Clock::duration                          period;
    int64_t                                  k = 0;
    std::vector<size_t>                      per_tick;
    int64_t                                  per_pass = 1;

    explicit Crowd(bool spread, int hz = 30) : period(TickSchedule::period_of(hz)) {
        per_pass = tick_view_timing(config.relevancy, period);
        if (spread && per_pass > 1) {
            phases = std::make_shared<ViewPhases>(static_cast<unsigned>(per_pass));
            config.phases = phases;
        }
    }
    void join() {
        conns.push_back(std::make_unique<Connection>(ConnectionConfig{}, kOrigin));
        views.push_back(std::make_unique<ActorView>(registry, config, [](uint32_t) { return false; }));
    }
    size_t passes() const {
        size_t n = 0;
        for (const auto& v : views) n += v->stats().passes;
        return n;
    }
    void tick(int64_t skip = 0) {
        k += 1 + skip;
        if (phases) phases->begin_tick();
        const size_t before = passes();
        const Clock::time_point t = kOrigin + period * k;
        for (size_t i = 0; i < views.size(); ++i) views[i]->tick(*conns[i], 0, 0, t);
        per_tick.push_back(passes() - before);
    }
    void batch(int joins, int every, int after) {
        for (int j = 0; j < joins; ++j) {
            join();
            for (int e = 0; e < every; ++e) tick();
        }
        for (int a = 0; a < after; ++a) tick();
    }
    std::pair<size_t, size_t> spread_of_last(size_t n) const {
        const auto from = per_tick.end() - static_cast<std::ptrdiff_t>(std::min(n, per_tick.size()));
        return {*std::min_element(from, per_tick.end()), *std::max_element(from, per_tick.end())};
    }
};

}  // namespace view_spread_checks

static void test_view_phases_hand_out_the_lightest() {
    using namespace view_spread_checks;
    std::printf("view phases: a new view takes the phase with the fewest views; a view of a heavier phase moves at its pass\n");
    ViewPhases p(2);
    CHECK(p.cycle() == 2 && p.views(0) == 0 && p.views(1) == 0, "two phases, none held");
    p.begin_tick();
    CHECK(p.join() == 1 && p.due(1) && !p.due(0),
          "the first view takes the phase of the tick it is on and passes on it at once");
    CHECK(p.join() == 0 && p.join() == 1 && p.join() == 0, "the next ones alternate");
    CHECK(p.views(0) == 2 && p.views(1) == 2, "two and two");
    p.leave(0);
    p.leave(0);
    CHECK(p.views(0) == 0 && p.views(1) == 2, "two left phase 0");
    CHECK(p.balance(1) == 0 && p.views(0) == 1 && p.views(1) == 1,
          "a view of phase 1 that passes moves to the empty phase 0 (two more views than it)");
    CHECK(p.balance(1) == 1 && p.balance(0) == 0, "and a difference of one or none moves nobody");
    p.leave(1);
    p.leave(1);
    p.leave(7);
    CHECK(p.views(0) == 1 && p.views(1) == 0, "a phase that does not exist is no phase to leave");
    ViewPhases one(1);
    CHECK(one.join() == 0 && one.due(0) && one.balance(0) == 0,
          "a cycle of one tick has nothing to spread: its only phase is always due");
    ViewPhases four(4);
    for (int i = 0; i < 40; ++i) four.join();
    CHECK(four.views(0) == 10 && four.views(1) == 10 && four.views(2) == 10 && four.views(3) == 10,
          "four phases: forty views split ten by ten");
}

static void test_view_passes_spread_over_both_ticks() {
    using namespace view_spread_checks;
    std::printf("view passes: forty players joining 200 ms apart pass half on each tick of the pair\n");
    Crowd old_gate(false);
    old_gate.batch(40, 6, 60);
    const auto bunched = old_gate.spread_of_last(60);
    CHECK(bunched.first == 0 && bunched.second == 40,
          text("without phases (the clock alone) the batch passes together: %zu passes on one tick of the pair, %zu on the other",
               bunched.second, bunched.first)
              .c_str());

    Crowd crowd(true);
    CHECK(crowd.per_pass == 2 && crowd.phases && crowd.phases->cycle() == 2,
          "30 ticks a second: a pass every second tick, two phases");
    crowd.batch(40, 6, 60);
    const auto even = crowd.spread_of_last(60);
    CHECK(even.first >= 19 && even.second <= 21,
          text("with phases the same batch splits: %zu to %zu passes on a tick (20 each)", even.first, even.second)
              .c_str());
    CHECK(crowd.phases->views(0) == 20 && crowd.phases->views(1) == 20, "twenty views on each phase");
    Crowd steady(true);
    steady.batch(40, 6, 4);
    const size_t before = steady.passes();
    for (int i = 0; i < 60; ++i) steady.tick();
    CHECK(steady.passes() - before == 40 * 30,
          text("40 views make %zu passes in 60 ticks (40 x 30): the rate is the same as without phases",
               steady.passes() - before)
              .c_str());
}

static void test_view_passes_stay_spread_after_a_stall_and_departures() {
    using namespace view_spread_checks;
    std::printf("view passes: a stall of the tick grid changes nobody's phase; players who leave do not leave one tick heavy\n");
    Crowd crowd(true);
    crowd.batch(40, 6, 20);
    crowd.tick(9);
    for (int i = 0; i < 40; ++i) crowd.tick();
    const auto after_stall = crowd.spread_of_last(30);
    CHECK(after_stall.first >= 19 && after_stall.second <= 21,
          text("after a stall of ten periods the passes are still split: %zu to %zu on a tick", after_stall.first,
               after_stall.second)
              .c_str());

    std::vector<size_t> keep;
    for (size_t i = 0; i < crowd.views.size(); ++i) {
        if (crowd.views[i]->phase() != 0) keep.push_back(i);
    }
    CHECK(keep.size() == 20, "twenty views are on phase 1");
    std::vector<std::unique_ptr<ActorView>> staying;
    std::vector<std::unique_ptr<Connection>> staying_conns;
    for (size_t i = 0; i < crowd.views.size(); ++i) {
        if (crowd.views[i]->phase() == 0) {
            crowd.views[i].reset();
        } else {
            staying.push_back(std::move(crowd.views[i]));
            staying_conns.push_back(std::move(crowd.conns[i]));
        }
    }
    crowd.views = std::move(staying);
    crowd.conns = std::move(staying_conns);
    CHECK(crowd.phases->views(0) == 0 && crowd.phases->views(1) == 20, "the phase of the departed holds none");
    for (int i = 0; i < 60; ++i) crowd.tick();
    const auto settled = crowd.spread_of_last(20);
    CHECK(settled.first >= 9 && settled.second <= 11 && crowd.phases->views(0) == 10 && crowd.phases->views(1) == 10,
          text("the twenty who stayed move to the empty tick at their passes and settle ten and ten: %zu to %zu passes on a tick",
               settled.first, settled.second)
              .c_str());
}

static void test_view_passes_spread_at_other_rates() {
    using namespace view_spread_checks;
    std::printf("view passes: the cycle follows the rate (60 ticks a second: a pass every fourth tick, four phases; 20 ticks a second: every tick)\n");
    Crowd fast(true, 60);
    CHECK(fast.per_pass == 4 && fast.phases && fast.phases->cycle() == 4, "60 ticks a second: four ticks to a pass");
    fast.batch(40, 12, 80);
    const auto split = fast.spread_of_last(80);
    CHECK(split.first >= 9 && split.second <= 11,
          text("forty players over four ticks: %zu to %zu passes on a tick (10 each)", split.first, split.second)
              .c_str());
    Crowd slow(true, 20);
    CHECK(slow.per_pass == 1 && !slow.phases,
          "20 ticks a second: every tick is a pass, there is nothing to spread (no phases)");
    slow.batch(10, 4, 20);
    const auto all = slow.spread_of_last(20);
    CHECK(all.first == 10 && all.second == 10, "every view passes on every tick");
}

namespace loop_stats_checks {

const Clock::time_point kOrigin{std::chrono::seconds(300)};

template <class... A>
std::string text(const char* format, A... a) {
    char buf[400];
    std::snprintf(buf, sizeof(buf), format, a...);
    return buf;
}

struct Run {
    Clock::time_point             now = kOrigin;
    uint64_t                      ticks = 0;
    std::function<void(uint64_t)> work;

    void run(TickLoop& loop, Clock::time_point end) {
        TickLoop::Ports p;
        p.now = [this] { return now; };
        p.receive = [this](std::chrono::microseconds timeout) -> std::optional<size_t> {
            now += timeout;
            return size_t{0};
        };
        p.datagram = [](size_t, Clock::time_point) {};
        p.tick = [this](Clock::time_point) {
            ++ticks;
            if (work) work(ticks);
        };
        while (now < end) {
            if (!loop.run_once(p)) return;
        }
    }
};

}  // namespace loop_stats_checks

static void test_late_max_counts_the_tick_that_restarts_the_grid() {
    using namespace loop_stats_checks;
    std::printf("the tick loop: the worst stall, the one that starts the grid again, is in the latest-tick figure\n");
    TickSchedule s(30, kOrigin);
    const Clock::duration period = s.period();
    const Clock::time_point t = s.take(kOrigin + period * 13);
    CHECK(t == kOrigin + period * 13 && s.restarts() == 1 && s.skipped() == 12,
          "a tick twelve periods late starts the grid again at the time it is (12 ticks dropped)");
    CHECK(s.late_max() == period * 12,
          text("... and its lateness, %.0f ms, is what the schedule reports (it recorded 0 before)",
               std::chrono::duration<double, std::milli>(s.late_max()).count())
              .c_str());
    s.reset_late_max();
    CHECK(s.late_max() == Clock::duration::zero(), "reset");
    TickSchedule r(30, kOrigin);
    r.restart(kOrigin + period * 6 + std::chrono::milliseconds(5));
    CHECK(r.restarts() == 1 && r.late_max() == period * 5 + std::chrono::milliseconds(5),
          "a restart by the loop records the lateness of the tick it dropped");

    TickLoop loop(TickLoop::Config{30, 4, 256}, kOrigin);
    Run run;
    run.work = [&](uint64_t n) {
        if (n == 10) run.now += period * 21 / 2;
    };
    run.run(loop, kOrigin + period * 30);
    CHECK(loop.schedule().late_max() >= period * 9,
          text("a stall of 10.5 periods in the tenth tick: the loop reports %.0f ms late",
               std::chrono::duration<double, std::milli>(loop.schedule().late_max()).count())
              .c_str());
}

static void test_loop_counts_are_the_intervals() {
    using namespace loop_stats_checks;
    std::printf("the tick loop: ticks caught up, ticks dropped and restarts of the grid are counted per interval\n");
    TickLoop loop(TickLoop::Config{30, 4, 256}, kOrigin);
    const Clock::duration period = loop.schedule().period();
    Run run;
    run.work = [&](uint64_t n) {
        if (n == 3) run.now += period * 7 / 2;
        if (n == 12) run.now += period * 21 / 2;
    };
    run.run(loop, kOrigin + period * 40);
    const TickLoop::Stats first = loop.take_stats();
    CHECK(first.caught_up == 2 && first.skipped == 9 && first.restarts == 1,
          text("the first interval: %llu caught up, %llu dropped, %llu restart(s)",
               static_cast<unsigned long long>(first.caught_up), static_cast<unsigned long long>(first.skipped),
               static_cast<unsigned long long>(first.restarts))
              .c_str());
    run.run(loop, run.now + period * 30);
    const TickLoop::Stats second = loop.take_stats();
    CHECK(second.caught_up == 0 && second.skipped == 0 && second.restarts == 0 && second.ticks >= 29,
          "the next interval had no stall: all three counts are 0 (they were the totals since the start before)");
    run.work = [&](uint64_t n) {
        if (n % 7 == 0) run.now += period * 6;
    };
    run.run(loop, run.now + period * 60);
    const TickLoop::Stats third = loop.take_stats();
    CHECK(third.restarts > 0 && third.skipped >= third.restarts * 5 &&
              third.restarts == loop.schedule().restarts() - first.restarts,
          text("a third interval with stalls counts its own: %llu restarts, %llu dropped",
               static_cast<unsigned long long>(third.restarts), static_cast<unsigned long long>(third.skipped))
              .c_str());
}

static void test_socket_asks_for_a_big_receive_buffer() {
    using namespace loop_stats_checks;
    std::printf("the UDP socket: it asks the system for a receive buffer of 1 MB and reports what it got\n");
    std::string err;
    auto s = UdpSocket::open(*parse_address("127.0.0.1:0"), &err);
    CHECK(s != nullptr, "a socket opens on a port the system chooses");
    if (!s) return;
    CHECK(UdpSocket::kReceiveBufferBytes == 1024 * 1024, "the buffer asked for is 1 MB");
    CHECK(s->receive_buffer_bytes() >= 128 * 1024,
          text("the system granted %d KB (the default of about 64 KB is not enough for a stalled tick)",
               s->receive_buffer_bytes() / 1024)
              .c_str());
}

void run_endpoint_tick_tests() {
    test_fixed_tick_one_tick_for_many_datagrams();
    test_fixed_tick_catch_up();
    test_fixed_tick_rate();
    test_fixed_tick_game_timers();
    test_view_phases_hand_out_the_lightest();
    test_view_passes_spread_over_both_ticks();
    test_view_passes_stay_spread_after_a_stall_and_departures();
    test_view_passes_spread_at_other_rates();
    test_late_max_counts_the_tick_that_restarts_the_grid();
    test_loop_counts_are_the_intervals();
    test_socket_asks_for_a_big_receive_buffer();
}

