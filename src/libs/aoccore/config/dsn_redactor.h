#pragma once
#include <string>
#include <string_view>

namespace aoc::world::config {
[[nodiscard]] inline std::string RedactDsnCredentials(std::string_view dsn) {
    std::string out(dsn);
    if (const std::size_t s = out.find("://"); s != std::string::npos) {
        const std::size_t at = out.find('@', s + 3), col = out.find(':', s + 3);
        if (at != std::string::npos && col != std::string::npos && col < at) {
            out.replace(col + 1, at - col - 1, "***REDACTED***");
        }
    }
    if (const std::size_t pw = out.find("password="); pw != std::string::npos) {
        const std::size_t st = pw + 9, end = out.find_first_of(" &;\t\r\n", st);
        out.replace(st, (end == std::string::npos ? out.size() : end) - st, "***REDACTED***");
    }
    return out;
}
}  // namespace aoc::world::config
