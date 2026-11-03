#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#ifdef __linux__
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

namespace aoccore::pal {

struct OsMemorySample {
    uint64_t vm_size_bytes = 0, rss_bytes = 0, shared_bytes = 0, peak_rss_bytes = 0;
};

[[nodiscard]] inline OsMemorySample ParseProcStatmLine(std::string_view statm_line, uint64_t page_size_bytes) noexcept {
    if (page_size_bytes == 0 || statm_line.empty()) { return {}; }
    uint64_t pages[7]{};
    std::size_t count = 0, pos = 0;
    const uint64_t max_pages = std::numeric_limits<uint64_t>::max() / page_size_bytes;
    while (pos < statm_line.size() && count < 7) {
        pos = statm_line.find_first_not_of(" \t\r\n", pos);
        if (pos == std::string_view::npos) { break; }
        const std::size_t end = std::min(statm_line.find_first_of(" \t\r\n", pos), statm_line.size());
        uint64_t val = 0;
        const auto [ptr, ec] = std::from_chars(statm_line.data() + pos, statm_line.data() + end, val);
        if (ec != std::errc{} || ptr != statm_line.data() + end || val > max_pages) { return {}; }
        pages[count++] = val;
        pos = end;
    }
    return count < 3 ? OsMemorySample{}
                     : OsMemorySample{pages[0] * page_size_bytes, pages[1] * page_size_bytes,
                                      pages[2] * page_size_bytes, pages[1] * page_size_bytes};
}

[[nodiscard]] inline OsMemorySample SampleProcessMemory() noexcept {
#ifdef __linux__
    std::ifstream in("/proc/self/statm");
    std::string line;
    std::getline(in, line);
    const long ps = sysconf(_SC_PAGESIZE);
    OsMemorySample s = ParseProcStatmLine(line, ps > 0 ? static_cast<uint64_t>(ps) : 4096ULL);
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0 && ru.ru_maxrss > 0) {
        s.peak_rss_bytes = std::max(s.rss_bytes, static_cast<uint64_t>(ru.ru_maxrss) * 1024ULL);
    }
    return s;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof(c);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c))) {
        const uint64_t rss = static_cast<uint64_t>(c.WorkingSetSize);
        return {static_cast<uint64_t>(c.PrivateUsage), rss, 0, std::max(static_cast<uint64_t>(c.PeakWorkingSetSize), rss)};
    }
    return {};
#else
    return {};
#endif
}

}  // namespace aoccore::pal
