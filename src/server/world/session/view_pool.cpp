// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "session/view_pool.h"

#include <algorithm>
#include <chrono>
#include <exception>

namespace aoc::worldd {

namespace {

/// Spin-waits `us` microseconds because OS sleep granularity on Windows exceeds sub-millisecond jitter targets.
void spin_for(uint64_t us) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
}

uint64_t mix(uint64_t seed, uint64_t i) {
    uint64_t x = seed + 0x9e3779b97f4a7c15ull * (i + 1);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

}  // namespace

ViewPool::ViewPool(unsigned threads) : threads_(std::clamp<unsigned>(threads, 1, static_cast<unsigned>(kViewWorkers))), profiles_(threads_), busy_(threads_, 0.0) {
    workers_.reserve(threads_ - 1);
    for (unsigned i = 1; i < threads_; ++i) workers_.emplace_back([this, i] { worker_main(i); });
}

ViewPool::~ViewPool() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void ViewPool::worker_main(unsigned index) {
    view_worker() = index;
    uint64_t seen = 0;
    for (;;) {
        std::shared_ptr<Run> run;
        {
            std::unique_lock<std::mutex> lock(mu_);
            wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            run = current_;
        }
        if (gate_) gate_(index);
        work(index, *run);
    }
}

void ViewPool::work(unsigned index, Run& r) {
    TickProfile* before = tick_profile_redirect();
    tick_profile_redirect() = &profiles_[index];
    for (;;) {
        const size_t i = r.next.fetch_add(1, std::memory_order_relaxed);
        if (i >= r.count) break;
        if (jitter_ != 0) spin_for(mix(jitter_, i) % 300);
        const auto started = std::chrono::steady_clock::now();
        try {
            (*r.fn)(i);
        } catch (const std::exception& e) {
            if (r.errors) (*r.errors)[i] = JobError{true, e.what()};
        } catch (...) {
            if (r.errors) (*r.errors)[i] = JobError{true, "an exception that is not a std::exception"};
        }
        busy_[index] += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        jobs_.fetch_add(1, std::memory_order_relaxed);
        if (index != 0) worker_jobs_.fetch_add(1, std::memory_order_relaxed);
        if (r.done.fetch_add(1, std::memory_order_acq_rel) + 1 == r.count) {
            std::lock_guard<std::mutex> lock(mu_);
            finished_.notify_all();
        }
    }
    tick_profile_redirect() = before;
}

void ViewPool::run(size_t count, const std::function<void(size_t)>& fn, std::vector<JobError>* errors) {
    if (count == 0) return;
    const bool on = tick_profile().on;
    for (TickProfile& p : profiles_) {
        p = TickProfile{};
        p.on = on;
    }
    if (threads_ == 1) {
        Run r;
        r.fn = &fn;
        r.count = count;
        r.errors = errors;
        work(0, r);
    } else {
        const auto r = std::make_shared<Run>();
        r->fn = &fn;
        r->count = count;
        r->errors = errors;
        {
            std::lock_guard<std::mutex> lock(mu_);
            current_ = r;
            ++generation_;
        }
        wake_.notify_all();
        work(0, *r);
        std::unique_lock<std::mutex> lock(mu_);
        finished_.wait(lock, [&] { return r->done.load(std::memory_order_acquire) == count; });
    }
    if (on) {
        TickProfile& global = tick_profile();
        for (const TickProfile& p : profiles_) global.add_view(p);
    }
}

double ViewPool::busy_seconds() const {
    double sum = 0;
    for (const double b : busy_) sum += b;
    return sum;
}

}  // namespace aoc::worldd
