// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "net/tick_profile.h"
#include "session/view_worker.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aoc::worldd {

class ViewPool {
public:
    struct JobError {
        bool        failed = false;
        std::string what;
    };

    explicit ViewPool(unsigned threads);
    ~ViewPool();
    ViewPool(const ViewPool&) = delete;
    ViewPool& operator=(const ViewPool&) = delete;

    unsigned threads() const { return threads_; }

    /// Each call owns its Run state so a delayed worker never touches a subsequent tick's jobs.
    void run(size_t count, const std::function<void(size_t)>& fn, std::vector<JobError>* errors = nullptr);

    double   busy_seconds() const;
    uint64_t jobs_run() const { return jobs_.load(std::memory_order_relaxed); }
    uint64_t worker_jobs() const { return worker_jobs_.load(std::memory_order_relaxed); }

    void set_jitter(uint64_t seed) { jitter_ = seed; }
    void set_worker_gate(std::function<void(unsigned)> gate) { gate_ = std::move(gate); }

private:
    struct Run {
        const std::function<void(size_t)>* fn = nullptr;
        size_t                             count = 0;
        std::vector<JobError>*             errors = nullptr;
        std::atomic<size_t>                next{0};
        std::atomic<size_t>                done{0};
    };

    void worker_main(unsigned index);
    void work(unsigned index, Run& r);

    unsigned                      threads_;
    std::vector<std::thread>      workers_;
    std::mutex                    mu_;
    std::condition_variable       wake_, finished_;
    uint64_t                      generation_ = 0;
    bool                          stop_ = false;
    std::shared_ptr<Run>          current_;
    std::vector<TickProfile>      profiles_;
    std::vector<double>           busy_;
    std::atomic<uint64_t>         jobs_{0};
    std::atomic<uint64_t>         worker_jobs_{0};
    uint64_t                      jitter_ = 0;
    std::function<void(unsigned)> gate_;
};

}  // namespace aoc::worldd
