#pragma once
#include <chrono>
#include <cstdint>
#include <thread>
#ifdef __linux__
#include <cerrno>
#include <ctime>
#endif
#ifdef _MSC_VER
__pragma(fp_contract(off))
#else
#pragma STDC FP_CONTRACT OFF
#endif

namespace aoccore::pal {

[[nodiscard]] inline float FmaContractDisabledProbe(float a, float b, float c) noexcept {
    volatile float prod = a * b;
    return prod + c;
}

[[nodiscard]] inline uint64_t MonotonicNowNs() noexcept {
#ifdef __linux__
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
#else
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

inline void SleepUntilMonotonicNs(uint64_t deadline_ns) noexcept {
#ifdef __linux__
    timespec ts{static_cast<time_t>(deadline_ns / 1000000000ULL), static_cast<long>(deadline_ns % 1000000000ULL)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
#else
    const auto dur = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(deadline_ns));
    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(dur));
#endif
}
}  // namespace aoccore::pal
