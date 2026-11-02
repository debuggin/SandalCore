#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace aoc::world::telemetry {
struct HealthMetricsState {
    std::string server_id{"world-01"};
    bool content_loaded{false};
    bool mesh_registered{false};
    bool draining{false};
    std::uint32_t active_sessions{0};
    std::uint32_t tick_hz{30};
    std::uint64_t uptime_ms{0};
    std::uint64_t packets_in_total{0};
    std::uint64_t packets_out_total{0};
};
struct HttpProbeResponse {
    int status_code{200};
    std::string content_type{"application/json"};
    std::string body{};
};
struct DrainHooks {
    std::function<void()> stop_hello{};
    std::function<void()> release_lease{};
    std::function<void()> kick_sessions{};
    std::function<void()> flush_persistence{};
    std::function<void()> close_transports{};
};
struct DrainResult {
    bool completed_within_budget{false};
    std::uint64_t elapsed_ms{0};
    std::uint8_t phases_completed{0};
};
[[nodiscard]] inline HttpProbeResponse RenderLivez() {
    return {200, "application/json", "{\"status\":\"alive\"}"};
}
[[nodiscard]] inline HttpProbeResponse RenderReadyz(const HealthMetricsState& st) {
    if (st.content_loaded && st.mesh_registered && !st.draining)
        return {200, "application/json", "{\"status\":\"ready\"}"};
    return {503, "application/json", std::string("{\"status\":\"not_ready\",\"draining\":") + (st.draining ? "true}" : "false}")};
}
[[nodiscard]] inline HttpProbeResponse RenderHealthz(const HealthMetricsState& st) {
    const bool r = st.content_loaded && st.mesh_registered && !st.draining;
    std::string b = "{\"status\":\"" + std::string(r ? "ok" : "degraded") + "\",\"server_id\":\"" + st.server_id + "\",\"ready\":" +
                    (r ? "true" : "false") + ",\"draining\":" + (st.draining ? "true" : "false") + ",\"active_sessions\":" +
                    std::to_string(st.active_sessions) + ",\"tick_hz\":" + std::to_string(st.tick_hz) + ",\"uptime_ms\":" + std::to_string(st.uptime_ms) + "}";
    return {r ? 200 : 503, "application/json", std::move(b)};
}
[[nodiscard]] inline HttpProbeResponse RenderOpenMetrics(const HealthMetricsState& st) {
    const bool r = st.content_loaded && st.mesh_registered && !st.draining;
    std::string b = "aoc_world_ready " + std::string(r ? "1\n" : "0\n") + "aoc_world_draining " + (st.draining ? "1\n" : "0\n") +
                    "aoc_world_active_sessions " + std::to_string(st.active_sessions) + "\naoc_world_tick_hz " + std::to_string(st.tick_hz) +
                    "\naoc_world_packets_in_total " + std::to_string(st.packets_in_total) + "\naoc_world_packets_out_total " + std::to_string(st.packets_out_total) + "\n# EOF\n";
    return {200, "application/openmetrics-text; version=1.0.0; charset=utf-8", std::move(b)};
}
[[nodiscard]] inline HttpProbeResponse HandleProbePath(std::string_view path, const HealthMetricsState& st) {
    if (path == "/livez") return RenderLivez();
    if (path == "/readyz") return RenderReadyz(st);
    if (path == "/healthz") return RenderHealthz(st);
    if (path == "/metrics") return RenderOpenMetrics(st);
    return {404, "application/json", "{\"status\":\"not_found\"}"};
}
[[nodiscard]] inline DrainResult ExecuteGracefulDrain(HealthMetricsState& st, const DrainHooks& hooks, std::uint64_t budget_ms = 5000) {
    const auto t0 = std::chrono::steady_clock::now();
    st.draining = true;
    DrainResult res{};
    for (const auto* fn : {&hooks.stop_hello, &hooks.release_lease, &hooks.kick_sessions, &hooks.flush_persistence, &hooks.close_transports}) {
        if (*fn) (*fn)();
        ++res.phases_completed;
    }
    res.elapsed_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    res.completed_within_budget = (res.phases_completed == 5 && budget_ms > 0 && res.elapsed_ms < budget_ms);
    return res;
}
}  // namespace aoc::world::telemetry
