#pragma once
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include "libs/aoccore/config/dsn_redactor.h"
#include "libs/aoccore/telemetry/trace_context.h"

namespace aoc::config {
[[nodiscard]] inline std::string redact_dsn_credentials(std::string_view dsn) {
    std::string out = aoc::world::config::RedactDsnCredentials(dsn);
    for (std::size_t p = out.find("***REDACTED***"); p != std::string::npos; p = out.find("***REDACTED***", p)) {
        out.replace(p, 14U, "[REDACTED]");
    }
    return out;
}
}  // namespace aoc::config

namespace aoc::core::telemetry {
struct CanonicalLogEvent {
    std::string timestamp{"2026-01-01T00:00:00Z"};
    std::string level{"INFO"};
    std::string service{"world"};
    std::string event{"zone.tick"};
    TraceContext trace{};
    std::string trace_id{};
    std::string span_id{};
    std::uint8_t trace_flags{1};
    double duration_ms{0.0};
    std::string outcome{"ok"};
    std::string message{};
};

[[nodiscard]] inline TraceContext ResolveTrace(const CanonicalLogEvent& ev) {
    if (!ev.trace_id.empty() || !ev.span_id.empty()) {
        const bool ok = IsValidHexField(ev.trace_id, 32, true) && IsValidHexField(ev.span_id, 16, true);
        return TraceContext{ev.trace_id, ev.span_id, ev.trace_flags, ok};
    }
    return ev.trace;
}

[[nodiscard]] inline std::string EscapeJsonString(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8U);
    for (const char c : input) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

[[nodiscard]] inline std::string FormatCanonicalJsonLine(const CanonicalLogEvent& ev) {
    const std::string redacted_msg = aoc::config::redact_dsn_credentials(ev.message);
    const TraceContext tc = ResolveTrace(ev);
    std::ostringstream out;
    out << "{\"timestamp\":\"" << EscapeJsonString(ev.timestamp) << "\",\"level\":\"" << EscapeJsonString(ev.level)
        << "\",\"service\":\"" << EscapeJsonString(ev.service) << "\",\"event\":\"" << EscapeJsonString(ev.event)
        << "\",\"trace_id\":\"" << EscapeJsonString(tc.trace_id) << "\",\"span_id\":\""
        << EscapeJsonString(tc.span_id) << "\",\"traceparent\":\"" << EscapeJsonString(FormatTraceparent(tc))
        << "\",\"duration_ms\":" << ev.duration_ms << ",\"outcome\":\"" << EscapeJsonString(ev.outcome)
        << "\",\"message\":\"" << EscapeJsonString(redacted_msg) << "\"}\n";
    return out.str();
}

[[nodiscard]] inline bool ShouldSampleEvent(const CanonicalLogEvent& ev, std::uint32_t slow_threshold_ms = 50) noexcept {
    const TraceContext tc = ResolveTrace(ev);
    return ev.level == "ERROR" || ev.outcome == "error" || ev.duration_ms >= static_cast<double>(slow_threshold_ms) ||
           (tc.valid && (tc.trace_flags & 0x01U) != 0U);
}
}  // namespace aoc::core::telemetry
