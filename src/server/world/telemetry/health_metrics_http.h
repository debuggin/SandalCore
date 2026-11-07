#pragma once
#include "bootstrap/constants.h"
#include "net/tick_loop.h"
#include "npc/npc_world.h"
#include <thread>
#ifndef _WIN32
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace aoc::world::telemetry {

[[nodiscard]] inline std::string_view ResolveDataMode() noexcept {
    for (const char* k : {"AOC_DATA_MODE_ZOIS", "AOC_DATA_MODE_DEFAULT"}) {
        if (const char* v = std::getenv(k); v && *v) {
            return (v[0] == 'd' || v[0] == 'D') ? "disk" : "memory";
        }
    }
    return "memory";
}

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
    std::uint64_t active_npcs{0};
    std::uint64_t active_monsters{0};
    std::uint64_t resource_nodes{0};
    bool simulate_all_npcs{true};
    std::uint32_t worker_index{0};
    std::string worker_name{"aoc-world"};
    std::string scope_kind{"region"};
    std::string data_mode{"memory"};
    bool dynamic_scaling_stub{false};
    double time_of_day_hours{12.0};
    double cpu_saturation_pct{37.5};
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
[[nodiscard]] inline bool HasTickMutationParam(std::string_view req) noexcept {
    return req.find("tick_hz=") != std::string_view::npos || req.find("\"tick_hz\"") != std::string_view::npos ||
           req.find("tick_hz:") != std::string_view::npos || req.find("tick_mode=") != std::string_view::npos ||
           req.find("\"tick_mode\"") != std::string_view::npos || req.find("SetTickRate") != std::string_view::npos;
}

[[nodiscard]] inline int ParseRequestedTickHz(std::string_view req, int fallback) noexcept {
    const bool explicit_manual = req.find("tick_mode=manual") != std::string_view::npos ||
                                 req.find("tick_mode=Manual") != std::string_view::npos ||
                                 req.find("\"tick_mode\":\"manual\"") != std::string_view::npos ||
                                 req.find("\"tick_mode\": \"manual\"") != std::string_view::npos;
    if (!explicit_manual && (req.find("dynamic") != std::string_view::npos || req.find("Dynamic") != std::string_view::npos)) return -1;
    for (const std::string_view key : {std::string_view("tick_hz="), std::string_view("\"tick_hz\":"), std::string_view("tick_hz:")}) {
        const size_t pos = req.find(key);
        if (pos == std::string_view::npos) continue;
        size_t i = pos + key.size();
        while (i < req.size() && (req[i] == ' ' || req[i] == '"' || req[i] == '\'')) ++i;
        if (i < req.size() && (req[i] == 'd' || req[i] == 'D' || req[i] == '-')) return -1;
        int val = 0;
        bool any = false;
        while (i < req.size() && req[i] >= '0' && req[i] <= '9') {
            val = val * 10 + (req[i] - '0');
            any = true;
            ++i;
        }
        if (any) return val == 0 ? (explicit_manual ? 0 : -1) : aoc::worldd::TickLoop::snap_manual_tick_hz(val);
    }
    if (fallback <= 0) return (fallback == 0 && explicit_manual) ? 0 : -1;
    return aoc::worldd::TickLoop::snap_manual_tick_hz(fallback);
}

[[nodiscard]] inline bool ParseKeyedDouble(std::string_view req, std::initializer_list<std::string_view> keys, double& out) noexcept {
    for (const std::string_view key : keys) {
        const size_t pos = req.find(key);
        if (pos == std::string_view::npos) continue;
        size_t i = pos + key.size();
        while (i < req.size() && (req[i] == ' ' || req[i] == '"' || req[i] == '\'')) ++i;
        std::string num;
        while (i < req.size() && ((req[i] >= '0' && req[i] <= '9') || req[i] == '.' || req[i] == '-')) num.push_back(req[i++]);
        if (!num.empty()) {
            out = std::strtod(num.c_str(), nullptr);
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline double ParseRequestedTimeOfDayHours(std::string_view req, double fallback = 12.0) noexcept {
    double out = fallback;
    (void)ParseKeyedDouble(req, {"time_of_day=", "hours=", "\"time_of_day\":", "\"hours\":", "\"time_of_day_hours\":"}, out);
    return out;
}

[[nodiscard]] inline HttpProbeResponse RenderHealthz(const HealthMetricsState& st) {
    const bool r = st.content_loaded && st.mesh_registered && !st.draining;
    const std::string_view wname = st.worker_name.empty() ? std::string_view(st.server_id) : std::string_view(st.worker_name);
    const std::string_view scope = st.scope_kind.empty() ? std::string_view("region") : std::string_view(st.scope_kind);
    const std::string_view dmode = st.data_mode.empty() ? ResolveDataMode() : std::string_view(st.data_mode);
    const bool is_dyn = aoc::worldd::TickLoop::runtime_tick_mode() == aoc::worldd::TickMode::Dynamic;
    const int raw_hz = aoc::worldd::TickLoop::effective_tick_hz(static_cast<int>(st.tick_hz));
    const int eff_hz = is_dyn ? std::max(aoc::worldd::TickLoop::kMinHz, raw_hz) : raw_hz;
    const int target_hz = is_dyn ? -1 : aoc::worldd::TickLoop::runtime_target_tick_hz();
    const char* tmode = is_dyn ? "dynamic" : "manual";
    const double tod = aoc::worldd::bootstrap::has_global_time_of_day_override() ? aoc::worldd::bootstrap::global_time_of_day_hours() : st.time_of_day_hours;
    const int32_t cal_year = aoc::worldd::bootstrap::global_calendar_year();
    const int32_t cal_day = aoc::worldd::bootstrap::global_calendar_day_of_year();
    std::string b = "{\"status\":\"" + std::string(r ? "ok" : "degraded") + "\",\"server_id\":\"" + st.server_id +
                    "\",\"worker_name\":\"" + std::string(wname) + "\",\"worker_index\":" + std::to_string(st.worker_index) +
                    ",\"scope_kind\":\"" + std::string(scope) + "\",\"data_mode\":\"" + std::string(dmode) +
                    "\",\"dynamic_scaling_stub\":" + (st.dynamic_scaling_stub ? "true" : "false") + ",\"ready\":" +
                    (r ? "true" : "false") + ",\"draining\":" + (st.draining ? "true" : "false") + ",\"active_sessions\":" +
                    std::to_string(st.active_sessions) + ",\"tick_hz\":" + std::to_string(eff_hz) +
                    ",\"current_tick_hz\":" + std::to_string(eff_hz) + ",\"target_tick_hz\":" + std::to_string(target_hz) +
                    ",\"tick_mode\":\"" + tmode + "\",\"time_of_day_hours\":" + std::to_string(tod) +
                    ",\"day_of_year\":" + std::to_string(cal_day) + ",\"year\":" + std::to_string(cal_year) +
                    ",\"cpu_saturation_pct\":" + std::to_string(st.cpu_saturation_pct) + ",\"uptime_ms\":" + std::to_string(st.uptime_ms) + "}";
    return {r ? 200 : 503, "application/json", std::move(b)};
}
[[nodiscard]] inline HttpProbeResponse RenderOpenMetrics(const HealthMetricsState& st) {
    const bool r = st.content_loaded && st.mesh_registered && !st.draining;
    const bool sim = aoc::worldd::npc::NpcWorld::global_simulate_all_npcs();
    const bool is_dyn = aoc::worldd::TickLoop::runtime_tick_mode() == aoc::worldd::TickMode::Dynamic;
    const int raw_hz = aoc::worldd::TickLoop::effective_tick_hz(static_cast<int>(st.tick_hz));
    const int eff_hz = is_dyn ? std::max(aoc::worldd::TickLoop::kMinHz, raw_hz) : raw_hz;
    const int target_hz = is_dyn ? -1 : aoc::worldd::TickLoop::runtime_target_tick_hz();
    const double tod = aoc::worldd::bootstrap::has_global_time_of_day_override() ? aoc::worldd::bootstrap::global_time_of_day_hours() : st.time_of_day_hours;
    const int32_t cal_year = aoc::worldd::bootstrap::global_calendar_year();
    const int32_t cal_day = aoc::worldd::bootstrap::global_calendar_day_of_year();
    std::string b = "aoc_world_ready " + std::string(r ? "1\n" : "0\n") + "aoc_world_draining " + (st.draining ? "1\n" : "0\n") +
                    "aoc_world_worker_index " + std::to_string(st.worker_index) + "\naoc_world_active_sessions " + std::to_string(st.active_sessions) +
                    "\naoc_world_tick_hz " + std::to_string(eff_hz) + "\naoc_world_current_tick_hz " + std::to_string(eff_hz) +
                    "\naoc_world_target_tick_hz " + std::to_string(target_hz) + "\naoc_world_tick_mode_dynamic " + (is_dyn ? "1\n" : "0\n") +
                    "aoc_world_packets_in_total " + std::to_string(st.packets_in_total) +
                    "\naoc_world_packets_out_total " + std::to_string(st.packets_out_total) + "\naoc_world_active_npcs " + std::to_string(st.active_npcs) +
                    "\naoc_world_active_monsters " + std::to_string(st.active_monsters) + "\naoc_world_resource_nodes " + std::to_string(st.resource_nodes) +
                    "\naoc_world_simulate_all_npcs " + (sim ? "1\n" : "0\n") + "aoc_world_time_of_day_hours " + std::to_string(tod) +
                    "\naoc_world_calendar_day_of_year " + std::to_string(cal_day) + "\naoc_world_calendar_year " + std::to_string(cal_year) +
                    "\naoc_world_cpu_saturation_pct " + std::to_string(st.cpu_saturation_pct) + "\n# EOF\n";
    return {200, "application/openmetrics-text; version=1.0.0; charset=utf-8", std::move(b)};
}
[[nodiscard]] inline HttpProbeResponse HandleProbePath(std::string_view path, const HealthMetricsState& st, std::string_view full_req = {}) {
    if (path == "/admin/tick_hz" || path == "/gm/tick_hz" || full_req.find("SetTickRate") != std::string_view::npos ||
        full_req.find("tick_hz=") != std::string_view::npos) {
        if (HasTickMutationParam(full_req) || full_req.rfind("POST ", 0) == 0 || full_req.rfind("PUT ", 0) == 0) {
            const int parsed_hz = ParseRequestedTickHz(full_req, -1);
            aoc::worldd::TickLoop::set_global_target_hz(parsed_hz);
            aoc::worldd::npc::NpcWorld::set_global_simulate_all_npcs(parsed_hz != 0);
        }
        const bool is_dyn = aoc::worldd::TickLoop::runtime_tick_mode() == aoc::worldd::TickMode::Dynamic;
        const int raw_hz = aoc::worldd::TickLoop::effective_tick_hz(static_cast<int>(st.tick_hz));
        const int eff_hz = is_dyn ? std::max(aoc::worldd::TickLoop::kMinHz, raw_hz) : raw_hz;
        const int target_hz = is_dyn ? -1 : aoc::worldd::TickLoop::runtime_target_tick_hz();
        const char* tmode = is_dyn ? "dynamic" : "manual";
        const double ewma = aoc::worldd::TickLoop::runtime_ewma_work_ms();
        const bool sim = aoc::worldd::npc::NpcWorld::global_simulate_all_npcs();
        if (path != "/metrics") {
            return {200, "application/json",
                    "{\"status\":\"ok\",\"tick_hz\":" + std::to_string(eff_hz) +
                        ",\"current_tick_hz\":" + std::to_string(eff_hz) + ",\"target_tick_hz\":" + std::to_string(target_hz) +
                        ",\"tick_mode\":\"" + tmode + "\",\"simulate_all_npcs\":" + (sim ? "true" : "false") +
                        ",\"ewma_work_ms\":" + std::to_string(ewma) + "}"};
        }
    }
    if (path == "/admin/simulate_all_npcs" || path == "/gm/simulate_all_npcs" || full_req.find("SetSimulateAllNpcs") != std::string_view::npos ||
        full_req.find("simulate_all_npcs=") != std::string_view::npos) {
        (void)aoc::worldd::npc::NpcWorld::dispatch_gm_work_item("gm.SetSimulateAllNpcs", full_req);
        const bool sim = aoc::worldd::npc::NpcWorld::global_simulate_all_npcs();
        if (path != "/metrics") {
            return {200, "application/json", std::string("{\"status\":\"ok\",\"simulate_all_npcs\":") + (sim ? "true}" : "false}")};
        }
    }
    if (path == "/admin/time_of_day" || path == "/gm/time_of_day" || full_req.find("SetTimeOfDay") != std::string_view::npos ||
        full_req.find("time_of_day=") != std::string_view::npos) {
        double parsed_tod = st.time_of_day_hours;
        double parsed_day = 0.0;
        double parsed_year = 0.0;
        const bool has_tod = ParseKeyedDouble(full_req, {"time_of_day=", "hours=", "\"time_of_day\":", "\"hours\":", "\"time_of_day_hours\":"}, parsed_tod);
        const bool has_day = ParseKeyedDouble(full_req, {"day_of_year=", "\"day_of_year\":"}, parsed_day);
        const bool has_year = ParseKeyedDouble(full_req, {"year=", "\"year\":"}, parsed_year);
        if (has_day || has_year) {
            aoc::worldd::bootstrap::set_global_calendar_date(
                has_year ? static_cast<int32_t>(parsed_year) : aoc::worldd::bootstrap::global_calendar_year(),
                has_day ? static_cast<int32_t>(parsed_day) : aoc::worldd::bootstrap::global_calendar_day_of_year());
        }
        if (has_tod) {
            aoc::worldd::bootstrap::set_global_time_of_day_hours(parsed_tod);
        }
        const double eff_tod = aoc::worldd::bootstrap::global_time_of_day_hours();
        const int32_t eff_day = aoc::worldd::bootstrap::global_calendar_day_of_year();
        const int32_t eff_year = aoc::worldd::bootstrap::global_calendar_year();
        if (path != "/metrics") {
            return {200, "application/json",
                    "{\"status\":\"ok\",\"time_of_day_hours\":" + std::to_string(eff_tod) +
                        ",\"day_of_year\":" + std::to_string(eff_day) + ",\"year\":" + std::to_string(eff_year) + "}"};
        }
    }
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

[[nodiscard]] inline std::string_view ExtractHttpRequestPath(std::string_view raw) noexcept {
    const size_t sp1 = raw.find(' ');
    if (sp1 == std::string_view::npos) return "/metrics";
    const size_t sp2 = raw.find(' ', sp1 + 1);
    std::string_view target = sp2 == std::string_view::npos ? raw.substr(sp1 + 1) : raw.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    return q == std::string_view::npos ? target : target.substr(0, q);
}

class HealthMetricsHttpServer {
public:
    using SnapshotFn = std::function<HealthMetricsState()>;

    explicit HealthMetricsHttpServer(SnapshotFn snapshot_fn, std::uint16_t default_port = 8080)
        : snapshot_fn_(std::move(snapshot_fn)) {
        std::uint16_t port = default_port;
        if (const char* env_p = std::getenv("AOC_WORLD_METRICS_PORT"); env_p && *env_p) {
            const long v = std::strtol(env_p, nullptr, 10);
            if (v >= 0 && v <= 65535) port = static_cast<std::uint16_t>(v);
        }
        if (port > 0) start(port);
    }

    ~HealthMetricsHttpServer() { stop(); }
    HealthMetricsHttpServer(const HealthMetricsHttpServer&) = delete;
    HealthMetricsHttpServer& operator=(const HealthMetricsHttpServer&) = delete;

    void stop() noexcept {
        stopping_.store(true);
        if (worker_.joinable()) worker_.join();
#ifndef _WIN32
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
#endif
    }

private:
    void start(std::uint16_t port) {
#ifndef _WIN32
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        const int one = 1;
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        worker_ = std::thread([this] { serve_loop(); });
#else
        (void)port;
#endif
    }

#ifndef _WIN32
    void serve_client(int client_fd) const {
        char buf[1024]{};
        const ssize_t n = ::recv(client_fd, buf, sizeof(buf) - 1, 0);
        const std::string_view req(buf, n > 0 ? static_cast<size_t>(n) : 0);
        const HttpProbeResponse resp = HandleProbePath(ExtractHttpRequestPath(req), snapshot_fn_(), req);
        const char* reason = resp.status_code == 200 ? "OK" : (resp.status_code == 503 ? "Service Unavailable" : "Not Found");
        const std::string wire = "HTTP/1.1 " + std::to_string(resp.status_code) + " " + reason + "\r\nContent-Type: " + resp.content_type +
                                 "\r\nContent-Length: " + std::to_string(resp.body.size()) + "\r\nConnection: close\r\n\r\n" + resp.body;
        (void)::send(client_fd, wire.data(), wire.size(), 0);
        ::close(client_fd);
    }

    void serve_loop() const {
        while (!stopping_.load()) {
            pollfd pfd{listen_fd_, POLLIN, 0};
            if (::poll(&pfd, 1, 200) <= 0 || (pfd.revents & POLLIN) == 0) continue;
            const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd >= 0) serve_client(client_fd);
        }
    }
#endif

    SnapshotFn snapshot_fn_;
    std::atomic<bool> stopping_{false};
    int listen_fd_{-1};
    std::thread worker_;
};
}  // namespace aoc::world::telemetry
