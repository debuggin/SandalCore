#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aoc::core::telemetry {
struct TraceContext {
    std::string trace_id;
    std::string span_id;
    std::uint8_t trace_flags{1};
    bool valid{false};
};

[[nodiscard]] inline bool IsValidHexField(std::string_view s, std::size_t len, bool non_zero) noexcept {
    if (s.size() != len || s.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
        return false;
    }
    return !non_zero || s.find_first_not_of('0') != std::string_view::npos;
}

[[nodiscard]] inline TraceContext ParseTraceparent(std::string_view header) {
    if (header.size() != 55 || header.substr(0, 3) != "00-" || header[35] != '-' || header[52] != '-') {
        return {};
    }
    const std::string_view tid = header.substr(3, 32), sid = header.substr(36, 16), flg = header.substr(53, 2);
    if (!IsValidHexField(tid, 32, true) || !IsValidHexField(sid, 16, true) || !IsValidHexField(flg, 2, false)) {
        return {};
    }
    const auto hi = static_cast<std::uint8_t>(flg[0] <= '9' ? flg[0] - '0' : flg[0] - 'a' + 10);
    const auto lo = static_cast<std::uint8_t>(flg[1] <= '9' ? flg[1] - '0' : flg[1] - 'a' + 10);
    return TraceContext{std::string(tid), std::string(sid), static_cast<std::uint8_t>((hi << 4U) | lo), true};
}

[[nodiscard]] inline std::string FormatTraceparent(const TraceContext& ctx) {
    if (!ctx.valid || !IsValidHexField(ctx.trace_id, 32, true) || !IsValidHexField(ctx.span_id, 16, true)) {
        return "00-00000000000000000000000000000001-0000000000000001-01";
    }
    constexpr char kHex[] = "0123456789abcdef";
    const char flags[3] = {kHex[(ctx.trace_flags >> 4U) & 0x0FU], kHex[ctx.trace_flags & 0x0FU], '\0'};
    return "00-" + ctx.trace_id + "-" + ctx.span_id + "-" + flags;
}

[[nodiscard]] inline TraceContext ChildSpan(const TraceContext& parent, std::string_view new_span_id) {
    const bool ok = parent.valid && IsValidHexField(new_span_id, 16, true);
    return TraceContext{parent.trace_id, std::string(new_span_id), parent.trace_flags, ok};
}
}  // namespace aoc::core::telemetry
