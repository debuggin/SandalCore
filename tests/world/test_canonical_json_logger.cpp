#include "libs/aoccore/telemetry/json_logger.h"
#include "libs/aoccore/telemetry/trace_context.h"

#include <cassert>
#include <string>

int main() {
    using namespace aoc::core::telemetry;
    constexpr auto kValid = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    const auto ctx = ParseTraceparent(kValid);
    assert(ctx.valid);
    assert(ctx.trace_id == "4bf92f3577b34da6a3ce929d0e0e4736");
    assert(ctx.span_id == "00f067aa0ba902b7");
    assert(ctx.trace_flags == 1);
    assert(FormatTraceparent(ctx) == kValid);

    const auto child = ChildSpan(ctx, "1111222233334444");
    assert(child.valid);
    assert(child.trace_id == ctx.trace_id);
    assert(child.span_id == "1111222233334444");

    assert(!ParseTraceparent("01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01").valid);
    assert(!ParseTraceparent("00-00000000000000000000000000000000-00f067aa0ba902b7-01").valid);
    assert(!ParseTraceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01").valid);
    assert(!ParseTraceparent("00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01").valid);
    assert(!ParseTraceparent("00-4bf92f35-00f067aa-01").valid);

    CanonicalLogEvent ev{};
    ev.level = "ERROR";
    ev.trace_id = ctx.trace_id;
    ev.span_id = ctx.span_id;
    ev.trace_flags = ctx.trace_flags;
    ev.duration_ms = 12.5;
    ev.message = "db error postgres://admin:s3cr3t@10.0.0.1:5432/aoc";
    const std::string line = FormatCanonicalJsonLine(ev);
    assert(line.ends_with("\n"));
    assert(line.find("\"trace_id\":\"4bf92f3577b34da6a3ce929d0e0e4736\"") != std::string::npos);
    assert(line.find("[REDACTED]") != std::string::npos);
    assert(line.find("s3cr3t") == std::string::npos);
    assert(ShouldSampleEvent(ev));
    ev.level = "INFO";
    ev.trace_flags = 0;
    ev.duration_ms = 2.0;
    assert(!ShouldSampleEvent(ev));
    ev.duration_ms = 50.0;
    assert(ShouldSampleEvent(ev));
    return 0;
}
