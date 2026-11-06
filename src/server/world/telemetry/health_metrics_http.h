#pragma once
#include "net/udp_socket.h"
#include "npc/npc_world.h"
#include <atomic>
#include <cstdlib>
#include <thread>
#ifndef _WIN32
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#endif

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
    std::uint64_t active_npcs{0};
    std::uint64_t active_monsters{0};
    std::uint64_t resource_nodes{0};
    bool simulate_all_npcs{true};
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
    const bool sim = aoc::worldd::npc::NpcWorld::global_simulate_all_npcs();
    std::string b = "aoc_world_ready " + std::string(r ? "1\n" : "0\n") + "aoc_world_draining " + (st.draining ? "1\n" : "0\n") +
                    "aoc_world_active_sessions " + std::to_string(st.active_sessions) + "\naoc_world_tick_hz " + std::to_string(st.tick_hz) +
                    "\naoc_world_packets_in_total " + std::to_string(st.packets_in_total) + "\naoc_world_packets_out_total " + std::to_string(st.packets_out_total) +
                    "\naoc_world_active_npcs " + std::to_string(st.active_npcs) + "\naoc_world_active_monsters " + std::to_string(st.active_monsters) +
                    "\naoc_world_resource_nodes " + std::to_string(st.resource_nodes) + "\naoc_world_simulate_all_npcs " + (sim ? "1\n" : "0\n") + "# EOF\n";
    return {200, "application/openmetrics-text; version=1.0.0; charset=utf-8", std::move(b)};
}
[[nodiscard]] inline HttpProbeResponse HandleProbePath(std::string_view path, const HealthMetricsState& st, std::string_view full_req = {}) {
    if (path == "/admin/simulate_all_npcs" || path == "/gm/simulate_all_npcs" || full_req.find("SetSimulateAllNpcs") != std::string_view::npos ||
        full_req.find("simulate_all_npcs=") != std::string_view::npos) {
        (void)aoc::worldd::npc::NpcWorld::dispatch_gm_work_item("gm.SetSimulateAllNpcs", full_req);
        const bool sim = aoc::worldd::npc::NpcWorld::global_simulate_all_npcs();
        if (path != "/metrics") {
            return {200, "application/json", std::string("{\"status\":\"ok\",\"simulate_all_npcs\":") + (sim ? "true}" : "false}")};
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
