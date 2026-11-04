// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "net/endpoint.h"
#include "net/tick_profile.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <stdexcept>

namespace aoc { namespace worldd {

namespace {

using SteadyClock = std::chrono::steady_clock;

double micros(SteadyClock::time_point from, SteadyClock::time_point to) {
    return std::chrono::duration<double, std::micro>(to - from).count();
}

// Freezes the actor registry across parallel view passes and thaws it on scope exit.
class Frozen {
public:
    explicit Frozen(world::ActorRegistry& r) : r_(r) { r_.freeze(true); }
    ~Frozen() { r_.freeze(false); }
    Frozen(const Frozen&) = delete;
    Frozen& operator=(const Frozen&) = delete;

private:
    world::ActorRegistry& r_;
};

template <typename Item>
void run_view_passes(std::vector<Item>& items, ViewPool& view_pool, world::ActorRegistry& registry,
                     Clock::time_point now, TickProfile& profile, bool timing) {
    std::vector<Item*> jobs;
    for (Item& item : items) {
        if (!item.dropped && item.s->player && !item.s->closing && item.plan.view && !item.plan.stopped) jobs.push_back(&item);
    }
    if (jobs.empty()) return;
    std::vector<ViewPool::JobError> errors(jobs.size());
    SteadyClock::time_point a{};
    if (timing) a = SteadyClock::now();
    {
        Frozen frozen(registry);
        view_pool.run(
            jobs.size(),
            [&](size_t k) {
                Item& item = *jobs[k];
                const auto started = SteadyClock::now();
                item.s->player->run_view(item.s->connection, item.plan, now);
                item.job_us = micros(started, SteadyClock::now());
            },
            &errors);
    }
    if (timing) profile.sessions_us += micros(a, SteadyClock::now());
    for (size_t k = 0; k < jobs.size(); ++k) {
        jobs[k]->failed = errors[k].failed;
        jobs[k]->error = std::move(errors[k].what);
        if (timing) {
            jobs[k]->s->profile_us += jobs[k]->job_us;
            jobs[k]->s->profile_view_us += jobs[k]->job_us;
        }
    }
}

}  // namespace

void Endpoint::tick_in_phases(Clock::time_point now, std::vector<std::pair<Address, const char*>>& drop) {
    TickProfile& profile = tick_profile();
    const bool timing = profile.on;
    struct Item {
        const Address*          addr = nullptr;
        Session*                s = nullptr;
        PlayerSession::TickPlan plan;
        bool                    dropped = false;
        double                  job_us = 0;
        bool                    failed = false;
        std::string             error;
    };
    std::vector<Item> items;
    items.reserve(sessions_.size());

    for (auto& [addr, s] : sessions_) {
        if (s->connection.closed_by_client()) { drop.emplace_back(addr, "the client quit"); continue; }
        if (s->connection.timed_out(now)) { drop.emplace_back(addr, "timed out"); continue; }
        if (s->connection.failed()) { drop.emplace_back(addr, "broke the protocol"); continue; }
        if (!s->closing && s->control.state() < ControlChannel::State::AwaitJoin && now >= s->deadline) {
            drop.emplace_back(addr, "did not log in in time");
            continue;
        }
        Item item{&addr, s.get(), {}, false, 0.0, false, {}};
        try {
            SteadyClock::time_point a{};
            if (timing) a = SteadyClock::now();
            if (s->player && !s->closing) item.plan = s->player->tick_logic(s->connection, now, /*plan_view=*/true);
            if (timing) {
                const double us = micros(a, SteadyClock::now());
                profile.sessions_us += us;
                s->profile_us += us;
            }
        } catch (const std::exception& e) {
            spdlog::error("[endpoint] {} ({}): the tick threw {}: dropping the session", addr.to_string(), s->request.character_id, e.what());
            drop.emplace_back(addr, "threw an exception");
            item.dropped = true;
        }
        items.push_back(std::move(item));
    }

    run_view_passes(items, *view_pool_, cfg_.players->registry(), now, profile, timing);

    for (Item& item : items) {
        if (item.dropped) continue;
        Session* s = item.s;
        const Address& addr = *item.addr;
        try {
            if (item.failed) throw std::runtime_error(item.error.empty() ? "the view pass threw an exception without a message" : item.error);
            SteadyClock::time_point b{}, c{};
            if (timing) b = SteadyClock::now();
            if (s->player && !s->closing) {
                if (!item.plan.stopped) s->player->tick_after(now, item.plan);
                if (s->player->failed()) apply(*s, s->control.kick(login_text::kLoginFailed), now);
            }
            if (timing) c = SteadyClock::now();
            for (auto& pkt : s->connection.flush(now)) send_(addr, pkt);
            if (timing) {
                const auto d = SteadyClock::now();
                profile.sessions_us += micros(b, c);
                profile.flush_us += micros(c, d);
                s->profile_us += micros(b, d);
            }
            if (s->player) s->player->take_acks(s->connection);
            if (s->closing && (now >= s->close_by || s->connection.unacked_reliable(0) == 0)) drop.emplace_back(addr, "closed");
        } catch (const std::exception& e) {
            spdlog::error("[endpoint] {} ({}): the tick threw {}: dropping the session", addr.to_string(), s->request.character_id, e.what());
            drop.emplace_back(addr, "threw an exception");
        }
    }
}

}}  // namespace aoc::worldd
