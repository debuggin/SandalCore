// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "net/cookie.h"
#include "net/spsc_ring.h"
#include "net/tick_profile.h"
#include "session/player_session.h"
#include "session/view_pool.h"
#include <cstdlib>
#include <mutex>

namespace aoc { namespace worldd {

inline unsigned view_threads_from_test_environment() {
    const char* text = std::getenv("AOC_TEST_VIEW_THREADS");
    if (!text || !*text) return 0;
    const long n = std::strtol(text, nullptr, 10);
    return n < 0 ? 0u : n > static_cast<long>(kViewWorkers) ? static_cast<unsigned>(kViewWorkers) : static_cast<unsigned>(n);
}

struct EndpointConfig {
    WorldInfo world;
    std::array<double, 3> spawn = {-777664.5, 616578.7, 15952.0};
    std::chrono::milliseconds close_linger{2000};
    std::chrono::seconds login_deadline{Connection::kLoginTimeout};
    size_t max_sessions = 1024;
    PlayerWorld* players = nullptr;
    uint32_t max_session_bytes_per_second = 262144;
    uint32_t session_burst_seconds = 2;
    unsigned view_threads = view_threads_from_test_environment();
};

class Endpoint {
public:
    using Send = std::function<void(const Address&, const std::vector<uint8_t>&)>;
    static constexpr std::chrono::seconds kLimitLogInterval{10};

    Endpoint(EndpointConfig cfg, CookieSigner& signer, LoginQueue& logins, Send send,
             CookieSigner::RandomFill random, Clock::time_point start);

    void on_datagram(const Address& from, const uint8_t* data, size_t len, Clock::time_point now);
    void post_login_result(uint64_t session_id, LoginResult result);
    void tick(Clock::time_point now);
    void kick_all(const std::string& text, Clock::time_point now);
    void drop(const Address& from, const char* why);
    void finish_players(const char* why);

    size_t session_count() const { return sessions_.size(); }
    size_t datagrams_dropped_for_rate() const { return rate_dropped_; }
    std::optional<Address> session_of(const std::string& character_id) const;
    const PlayerSession* player_at(const Address& a) const;
    std::vector<PlayerHandle> players();
    void take_costliest(TickProfile& profile);
    unsigned view_threads() const { return view_pool_ ? view_pool_->threads() : 0; }
    double   view_busy_seconds() const { return view_pool_ ? view_pool_->busy_seconds() : 0.0; }
    uint64_t view_jobs() const { return view_pool_ ? view_pool_->jobs_run() : 0; }

    bool enqueue_inbound(const Address& from, const uint8_t* data, size_t len, Clock::time_point now);
    size_t drain_inbound(Clock::time_point now);
    bool enqueue_outbound(const Address& to, const uint8_t* data, size_t len);
    size_t drain_outbound();
    InboundNetRing& inbound_ring() noexcept { return rings_->inbound; }
    const InboundNetRing& inbound_ring() const noexcept { return rings_->inbound; }
    OutboundNetRing& outbound_ring() noexcept { return rings_->outbound; }
    const OutboundNetRing& outbound_ring() const noexcept { return rings_->outbound; }
    AsyncTaskCompletionRing& completion_ring() noexcept { return rings_->completions; }
    const AsyncTaskCompletionRing& completion_ring() const noexcept { return rings_->completions; }
    TokenSnapshotStore& token_snapshots() noexcept { return token_snapshots_; }
    const TokenSnapshotStore& token_snapshots() const noexcept { return token_snapshots_; }
    void publish_token_snapshot(TokenSnapshot snapshot) { token_snapshots_.publish(std::move(snapshot)); }
    std::shared_ptr<const TokenSnapshot> token_snapshot() const noexcept { return token_snapshots_.load(); }

private:
    struct Session {
        // Carried-over logins retain the replaced session's submission id so pending gateway completions still route.
        uint64_t id = 0;
        Address address;
        Cookie cookie{};
        double cookie_timestamp = 0;
        Clock::time_point deadline{};
        std::vector<uint8_t> ack_packet;
        Connection connection;
        ControlChannel control;
        LoginRequest request;
        std::string character_name;
        std::unique_ptr<PlayerSession> player;
        bool carried_items_differ = false;
        bool logged_in = false;
        bool closing = false;
        Clock::time_point close_by{};
        double byte_tokens = 0;
        Clock::time_point byte_at{};
        bool rate_started = false;
        size_t rate_dropped = 0;
        double profile_us = 0;
        double profile_view_us = 0;

        Session(const ConnectionConfig& cc, Clock::time_point now, WorldInfo world, std::string challenge)
            : connection(cc, now), control(std::move(world), std::move(challenge)) {}
    };

    void on_handshake(const Address& from, const uint8_t* data, size_t len, Clock::time_point now);
    void apply(Session& s, ControlChannel::Reply reply, Clock::time_point now);
    void start_player(Session& s, Clock::time_point now);
    // Passes LoginResult by value so handover from an already-playing session can update position and items in place.
    void on_login_result(Session& s, LoginResult result, Clock::time_point now);
    void remove(const Address& a, const char* why);
    bool session_limit_reached(const Address& from, Clock::time_point now);
    void log_refused_for_limit(Clock::time_point now);
    void tick_in_phases(Clock::time_point now, std::vector<std::pair<Address, const char*>>& drop);
    double seconds(Clock::time_point t) const;
    std::string random_challenge();
    bool within_rate(Session& s, const Address& from, size_t len, Clock::time_point now);

    EndpointConfig cfg_;
    CookieSigner& signer_;
    LoginQueue& logins_;
    Send send_;
    CookieSigner::RandomFill random_;
    Clock::time_point start_;
    uint64_t next_session_id_ = 1;
    std::unordered_map<Address, std::unique_ptr<Session>, AddressHash> sessions_;
    std::unordered_map<std::string, Address> by_character_;
    std::unordered_map<uint64_t, Address> by_id_;
    size_t refused_for_limit_ = 0;
    size_t rate_dropped_ = 0;
    std::optional<Clock::time_point> limit_logged_;
    std::unique_ptr<ViewPool> view_pool_;

    struct Rings {
        InboundNetRing inbound{};
        OutboundNetRing outbound{};
        AsyncTaskCompletionRing completions{};
    };
    std::unique_ptr<Rings> rings_ = std::make_unique<Rings>();
    TokenSnapshotStore token_snapshots_{};
    std::mutex results_mu_;
    std::vector<std::pair<uint64_t, LoginResult>> results_;
};

}} // namespace aoc::worldd
