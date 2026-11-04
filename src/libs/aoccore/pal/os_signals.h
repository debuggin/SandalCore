#pragma once
#include <atomic>
#include <csignal>
#include <functional>
#include <stop_token>
#include <thread>

namespace aoccore::pal {

class SignalDrainWaiter {
public:
    explicit SignalDrainWaiter(std::function<void(int)> on_drain = {}) : on_drain_(std::move(on_drain)) {}
    ~SignalDrainWaiter() { stop_source_.request_stop(); }
    SignalDrainWaiter(const SignalDrainWaiter&) = delete;
    SignalDrainWaiter& operator=(const SignalDrainWaiter&) = delete;

    bool TriggerSignal(int signum) noexcept {
        bool expected = false;
        if (!drain_invoked_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) { return false; }
        last_signal_.store(signum, std::memory_order_release);
        if (on_drain_) { on_drain_(signum); }
        stop_source_.request_stop();
        return true;
    }

    bool RequestStop(int signum = SIGTERM) noexcept { return TriggerSignal(signum); }
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }
    [[nodiscard]] int last_signal() const noexcept { return last_signal_.load(std::memory_order_acquire); }
    [[nodiscard]] bool drain_invoked() const noexcept { return drain_invoked_.load(std::memory_order_acquire); }

#ifdef __linux__
    static void BlockProcessSignals(sigset_t& mask) noexcept {
        sigemptyset(&mask);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGHUP);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    }
    void StartPosixWaiter(sigset_t mask) {
        waiter_ = std::jthread([this, mask]() mutable noexcept {
            siginfo_t info{};
            while (sigwaitinfo(&mask, &info) < 0) {}
            TriggerSignal(info.si_signo);
        });
    }
#endif

private:
    std::function<void(int)> on_drain_{};
    std::stop_source stop_source_{};
    std::atomic<int> last_signal_{0};
    std::atomic<bool> drain_invoked_{false};
    std::jthread waiter_{};
};

}  // namespace aoccore::pal
