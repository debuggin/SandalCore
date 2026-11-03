#include "libs/aoccore/pal/os_clock.h"
#include "libs/aoccore/pal/os_memory.h"
#include "libs/aoccore/pal/os_signals.h"
#include <bit>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

int main() {
    using namespace aoccore::pal;
    const auto check = [](bool ok) { assert(ok); if (!ok) { std::abort(); } };
    const float a = std::bit_cast<float>(0x3f800001u), b = std::bit_cast<float>(0x3f7ffffeu);
    check(std::bit_cast<uint32_t>(FmaContractDisabledProbe(a, b, -1.0f)) == 0x00000000u);

    const uint64_t t0 = MonotonicNowNs();
    SleepUntilMonotonicNs(t0 + 1000000ULL);
    const uint64_t t1 = MonotonicNowNs();
    check(t0 > 0 && t1 >= t0 + 1000000ULL);

    int drain_calls = 0, observed_sig = 0;
    SignalDrainWaiter waiter([&](int sig) { ++drain_calls; observed_sig = sig; });
    check(!waiter.stop_token().stop_requested() && !waiter.drain_invoked());
    check(waiter.TriggerSignal(SIGTERM));
    check(waiter.stop_token().stop_requested() && waiter.drain_invoked());
    check(waiter.last_signal() == SIGTERM && drain_calls == 1 && observed_sig == SIGTERM);
    check(!waiter.RequestStop(SIGINT) && drain_calls == 1 && waiter.last_signal() == SIGTERM);

    const auto s4k = ParseProcStatmLine("2048 1024 256 64 0 512 0\n", 4096ULL);
    check(s4k.vm_size_bytes == 8388608ULL && s4k.rss_bytes == 4194304ULL && s4k.shared_bytes == 1048576ULL);
    const auto s64k = ParseProcStatmLine("2048 1024 256 64 0 512 0", 65536ULL);
    check(s64k.vm_size_bytes == 134217728ULL && s64k.rss_bytes == 67108864ULL && s64k.shared_bytes == 16777216ULL);
    check(ParseProcStatmLine("", 4096ULL).rss_bytes == 0 && ParseProcStatmLine("100 50", 4096ULL).rss_bytes == 0);
    check(ParseProcStatmLine("100 bad 25", 4096ULL).rss_bytes == 0 && ParseProcStatmLine("100 50 25", 0ULL).rss_bytes == 0);
    check(ParseProcStatmLine("18446744073709551615 1 1", 4096ULL).vm_size_bytes == 0);

    const auto live = SampleProcessMemory();
    check(live.rss_bytes > 0 && live.peak_rss_bytes >= live.rss_bytes && live.vm_size_bytes > 0);

    std::ifstream presets_in("CMakePresets.json");
    const std::string presets((std::istreambuf_iterator<char>(presets_in)), std::istreambuf_iterator<char>());
    for (const char* req : {"windows-rel", "linux-release", "linux-asan-ubsan", "linux-tsan", "-ffp-contract=off"}) {
        check(presets.find(req) != std::string::npos);
    }
    return 0;
}
