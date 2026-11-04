// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "net/endpoint.h"
#include "net/spsc_ring.h"
#include "net/tick_profile.h"
#include "protocol/conn/handshake.h"
#include "protocol/wire/ue5_primitives.h"
#include <algorithm>
#include <cstdio>

namespace aoc { namespace worldd {

namespace conn = protocol::conn;

namespace {

std::vector<uint8_t> build_challenge_packet(const conn::HandshakePacket& req, CookieSigner& signer,
                                            const CookieSigner::RandomFill& random, const std::array<double, 3>& spawn,
                                            double t, const std::string& from_str) {
    conn::HandshakePacket c = req;
    c.type = conn::HandshakeType::Challenge;
    c.restart = false;
    c.secret_id = signer.secret_id();
    c.timestamp = t;
    c.cookie = signer.sign(c.secret_id, t, from_str);
    std::array<uint8_t, 38> server_bytes{};
    random(server_bytes.data(), server_bytes.size());
    c.ext = conn::IntrepidExt::for_server(*req.ext, server_bytes, spawn);
    c.padding.assign(conn::kChallengePaddingBytes, 0);
    random(c.padding.data(), c.padding.size());
    return conn::build_handshake(c);
}

std::vector<uint8_t> build_ack_packet(const conn::HandshakePacket& req, const CookieSigner::RandomFill& random) {
    conn::HandshakePacket a = req;
    a.type = conn::HandshakeType::Ack;
    a.timestamp = -1.0;
    a.padding.assign(conn::kAckPaddingBytes, 0);
    random(a.padding.data(), a.padding.size());
    return conn::build_handshake(a);
}

}  // namespace

Endpoint::Endpoint(EndpointConfig cfg, CookieSigner& signer, LoginQueue& logins, Send send,
                   CookieSigner::RandomFill random, Clock::time_point start)
    : cfg_(std::move(cfg)), signer_(signer), logins_(logins), send_(std::move(send)),
      random_(std::move(random)), start_(start) {
    if (cfg_.view_threads >= 1 && cfg_.players) view_pool_ = std::make_unique<ViewPool>(cfg_.view_threads);
}

double Endpoint::seconds(Clock::time_point t) const {
    return std::chrono::duration<double>(t - start_).count();
}

std::string Endpoint::random_challenge() {
    uint8_t b[4];
    random_(b, sizeof(b));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", b[0], b[1], b[2], b[3]);
    return buf;
}

void Endpoint::on_datagram(const Address& from, const uint8_t* data, size_t len, Clock::time_point now) {
    if (conn::is_handshake(data, len)) {
        on_handshake(from, data, len, now);
        return;
    }
    auto it = sessions_.find(from);
    if (it == sessions_.end()) return;
    Session& s = *it->second;
    if (!within_rate(s, from, len, now)) return;
    try {
        // Apply packet acknowledgements before bunches so join replies omit data acknowledged in the same datagram.
        const std::vector<InBunch> bunches = s.connection.receive(data, len, now);
        if (s.player) s.player->take_acks(s.connection);
        bool touched_player = false;
        for (const InBunch& b : bunches) {
            if (b.channel != 0) {
                if (s.player && !s.closing) {
                    s.player->on_bunch(s.connection, b, now);
                    touched_player = true;
                } else {
                    spdlog::debug("[endpoint] {}: ignoring a bunch on channel {}", from.to_string(), b.channel);
                }
                continue;
            }
            if (b.close || b.bits == 0) continue;
            const auto msg = conn::parse_control_message(b.data.data(), b.data.size(), 0, b.bits);
            if (!msg) {
                spdlog::debug("[endpoint] {}: unreadable control message", from.to_string());
                continue;
            }
            apply(s, s.control.on_message(*msg), now);
        }
        if (touched_player && s.player && !s.closing) s.player->end_of_datagram(s.connection);
    } catch (const std::exception& e) {
        spdlog::error("[endpoint] {} ({}): a datagram threw {}: dropping the session", from.to_string(), s.request.character_id, e.what());
        remove(from, "threw an exception");
    }
}

bool Endpoint::within_rate(Session& s, const Address& from, size_t len, Clock::time_point now) {
    const double rate = static_cast<double>(cfg_.max_session_bytes_per_second);
    if (rate <= 0) return true;
    const double capacity = rate * static_cast<double>(std::max<uint32_t>(cfg_.session_burst_seconds, 1));
    if (!s.rate_started) {
        s.rate_started = true;
        s.byte_tokens = capacity;
        s.byte_at = now;
    } else if (now > s.byte_at) {
        s.byte_tokens = std::min(capacity, s.byte_tokens + rate * std::chrono::duration<double>(now - s.byte_at).count());
        s.byte_at = now;
    }
    if (s.byte_tokens >= static_cast<double>(len)) {
        s.byte_tokens -= static_cast<double>(len);
        return true;
    }
    ++rate_dropped_;
    if (++s.rate_dropped == 1) {
        spdlog::warn("[endpoint] {} ({}): datagrams come faster than {} bytes a second: dropped (further ones are counted, not logged)", from.to_string(),
                     s.request.character_id, cfg_.max_session_bytes_per_second);
    }
    return false;
}

void Endpoint::drop(const Address& from, const char* why) { remove(from, why); }

void Endpoint::on_handshake(const Address& from, const uint8_t* data, size_t len, Clock::time_point now) {
    const auto p = conn::parse_handshake(data, len);
    if (!p || !p->ext) return;
    const double t = seconds(now);
    if (p->type == conn::HandshakeType::Initial) {
        if (!p->ext->has_initial_marker() || session_limit_reached(from, now)) return;
        send_(from, build_challenge_packet(*p, signer_, random_, cfg_.spawn, t, from.to_string()));
        return;
    }
    if (p->type != conn::HandshakeType::Response) return;
    auto existing = sessions_.find(from);
    if (existing != sessions_.end() && existing->second->cookie == p->cookie) {
        send_(from, existing->second->ack_packet);
        return;
    }
    if (existing != sessions_.end() && p->timestamp <= existing->second->cookie_timestamp) {
        spdlog::debug("[endpoint] {}: ignoring a Response older than its session", from.to_string());
        return;
    }
    if (!signer_.verify(p->secret_id, p->timestamp, from.to_string(), p->cookie, t)) {
        spdlog::debug("[endpoint] {}: Response with a wrong or expired cookie", from.to_string());
        return;
    }
    if (existing == sessions_.end() && session_limit_reached(from, now)) return;
    const std::string character_id = ::ue5::guid_to_hex(p->ext->character_id().data());
    const std::string ticket = ::ue5::guid_to_hex(p->ext->ticket().data());
    // Carry over an active login with the same character and ticket so the gateway is not asked to redeem a spent ticket twice.
    bool carried = false;
    uint64_t carried_id = 0;
    std::optional<LoginResult> carried_result;
    const bool no_content = cfg_.players && !cfg_.players->ready();
    if (existing != sessions_.end()) {
        const Session& old = *existing->second;
        const auto& result = old.control.login();
        if (!old.closing && old.request.character_id == character_id && old.request.ticket == ticket && (!result || result->accepted)) {
            carried = true;
            carried_id = old.id;
            carried_result = result;
        }
        remove(from, carried ? "reconnected, keeping its login" : "reconnected");
    }
    const auto seqs = conn::initial_sequences(p->cookie);
    ConnectionConfig cc{conn::outer_header_of(*p), p->ext->custom_field(), seqs.server, seqs.client};
    auto s = std::make_unique<Session>(cc, now, cfg_.world, random_challenge());
    s->id = carried ? carried_id : next_session_id_++;
    s->address = from;
    s->cookie = p->cookie;
    s->cookie_timestamp = p->timestamp;
    s->deadline = now + cfg_.login_deadline;
    s->request = LoginRequest{character_id, ticket};
    s->ack_packet = build_ack_packet(*p, random_);
    send_(from, s->ack_packet);
    Session& session = *s;
    by_id_[session.id] = from;
    if (carried) {
        spdlog::info("[endpoint] {} connected again for character {}: its login goes on", from.to_string(), character_id);
    } else if (no_content) {
        spdlog::warn("[endpoint] {} connected for character {}: refused, worldd has no content.db ({})", from.to_string(), character_id, cfg_.players->content_problem());
    } else {
        spdlog::info("[endpoint] {} connected for character {}", from.to_string(), character_id);
        logins_.submit(session.id, session.request);
    }
    sessions_[from] = std::move(s);
    if (carried_result) on_login_result(session, *carried_result, now);
    if (!carried && no_content) on_login_result(session, LoginResult{false, {}, login_text::kContentMissing}, now);
}

void Endpoint::post_login_result(uint64_t session_id, LoginResult result) {
    AsyncTaskCompletion item{session_id, std::move(result)};
    if (rings_->completions.try_push(std::move(item))) return;
    std::lock_guard<std::mutex> lock(results_mu_);
    results_.emplace_back(item.session_id, std::move(item.result));
}

bool Endpoint::enqueue_inbound(const Address& from, const uint8_t* data, size_t len, Clock::time_point now) {
    return rings_->inbound.try_push(InboundNetFrame::make(from, data, len, now));
}

size_t Endpoint::drain_inbound(Clock::time_point now) {
    size_t count = 0;
    for (InboundNetFrame f{}; rings_->inbound.try_pop(f); ++count) {
        const auto at = f.received_at == Clock::time_point{} ? now : f.received_at;
        on_datagram(f.from, f.payload.data(), f.len, at);
    }
    return count;
}

bool Endpoint::enqueue_outbound(const Address& to, const uint8_t* data, size_t len) {
    return rings_->outbound.try_push(OutboundNetFrame::make(to, data, len));
}

size_t Endpoint::drain_outbound() {
    size_t count = 0;
    for (OutboundNetFrame f{}; rings_->outbound.try_pop(f); ++count) {
        send_(f.to, std::vector<uint8_t>(f.payload.begin(), f.payload.begin() + f.len));
    }
    return count;
}

void Endpoint::on_login_result(Session& s, LoginResult result, Clock::time_point now) {
    if (s.closing) return;
    if (result.accepted) {
        auto other = by_character_.find(result.character.id);
        if (other != by_character_.end() && !(other->second == s.address)) {
            auto os = sessions_.find(other->second);
            if (os != sessions_.end()) {
                spdlog::info("[endpoint] character {} logged in again: dropping {}", result.character.id,
                             other->second.to_string());
                if (PlayerSession* old = os->second->player.get()) {
                    const GoodPosition g = old->last_position();
                    if (g.has_time_stamp) {
                        result.character.has_position = true;
                        result.character.position = {g.x, g.y, g.z, static_cast<float>(g.yaw_degrees)};
                    }
                    // Finish the old session first so mount health is persisted into its inventory before handover.
                    old->finish("logged in elsewhere");
                    std::string unused;
                    const auto record_items = items::Inventory::from_items(result.character.items, unused);
                    s.carried_items_differ = !record_items || !(*record_items == old->inventory());
                    result.character.items = old->inventory().items();
                }
                apply(*os->second, os->second->control.kick(login_text::kLoggedInElsewhere), now);
            }
        }
        by_character_[result.character.id] = s.address;
        s.logged_in = true;
        s.character_name = result.character.name;
    }
    apply(s, s.control.on_login(result), now);
    if (!result.accepted && !s.closing) {
        const auto soon = now + cfg_.close_linger;
        if (soon < s.deadline) s.deadline = soon;
    }
}

void Endpoint::apply(Session& s, ControlChannel::Reply reply, Clock::time_point now) {
    conn::BunchHeader control;
    control.reliable = true;
    control.channel = 0;
    for (const auto& m : reply.messages) {
        if (!s.connection.send(control, m)) reply.close = true;
    }
    if (reply.joined) {
        s.connection.set_playing();
        spdlog::info("[endpoint] character {} ({}) finished loading and joined", s.character_name,
                     s.request.character_id);
        start_player(s, now);
    }
    if (reply.close && !s.closing) {
        s.connection.send_close();
        s.closing = true;
        s.close_by = now + cfg_.close_linger;
    }
}

void Endpoint::start_player(Session& s, Clock::time_point now) {
    if (!cfg_.players || s.player || s.closing) return;
    const auto& login = s.control.login();
    if (!login || !login->accepted) return;
    std::vector<std::string> notes;
    const bootstrap::LoginIdentity identity = make_login_identity(s.control.login_message(), login->character, &notes);
    for (const std::string& note : notes) spdlog::info("[endpoint] character {} ({}): {}", s.character_name, s.request.character_id, note);
    std::string error;
    bootstrap::SetupRefusal refusal = bootstrap::SetupRefusal::None;
    s.player = PlayerSession::join(s.connection, *cfg_.players, login->character, identity, seconds(now), error, &refusal);
    if (s.player && s.carried_items_differ) s.player->inventory_may_be_unsaved();
    if (!s.player) {
        spdlog::error("[endpoint] character {} ({}) cannot join the world: {}", s.character_name, s.request.character_id, error);
        const char* text = refusal == bootstrap::SetupRefusal::UnsupportedRace ? login_text::kRaceUnsupported : login_text::kLoginFailed;
        apply(s, s.control.kick(text), now);
    }
}

void Endpoint::remove(const Address& a, const char* why) {
    auto it = sessions_.find(a);
    if (it == sessions_.end()) return;
    Session& s = *it->second;
    spdlog::info("[endpoint] {} ({}): {}", a.to_string(), s.request.character_id, why);
    if (s.player) s.player->finish(why);
    auto c = by_character_.find(s.request.character_id);
    if (c != by_character_.end() && c->second == a) by_character_.erase(c);
    auto i = by_id_.find(s.id);
    if (i != by_id_.end() && i->second == a) by_id_.erase(i);
    sessions_.erase(it);
}

bool Endpoint::session_limit_reached(const Address& from, Clock::time_point now) {
    if (cfg_.max_sessions == 0 || sessions_.size() < cfg_.max_sessions || sessions_.count(from)) return false;
    ++refused_for_limit_;
    log_refused_for_limit(now);
    return true;
}

void Endpoint::log_refused_for_limit(Clock::time_point now) {
    if (refused_for_limit_ == 0) return;
    if (limit_logged_ && now - *limit_logged_ < kLimitLogInterval) return;
    spdlog::warn("[endpoint] the limit of {} sessions is reached: refused {} handshake(s)", cfg_.max_sessions,
                 refused_for_limit_);
    refused_for_limit_ = 0;
    limit_logged_ = now;
}

void Endpoint::tick(Clock::time_point now) {
    drain_inbound(now);
    signer_.update(seconds(now));
    TickProfile& profile = tick_profile();
    const bool timing = profile.on;
    std::chrono::steady_clock::time_point t0{};
    if (timing) t0 = std::chrono::steady_clock::now();
    if (cfg_.players) cfg_.players->tick_world(seconds(now));
    if (timing) profile.world_us += tick_profile_us_since(t0);
    std::vector<std::pair<uint64_t, LoginResult>> done;
    for (AsyncTaskCompletion c{}; rings_->completions.try_pop(c);) done.emplace_back(c.session_id, std::move(c.result));
    {
        std::lock_guard<std::mutex> lock(results_mu_);
        for (auto& r : results_) done.push_back(std::move(r));
        results_.clear();
    }
    for (auto& [id, result] : done) {
        const auto at = by_id_.find(id);
        if (at == by_id_.end()) continue;
        const auto s = sessions_.find(at->second);
        if (s != sessions_.end() && s->second->id == id) on_login_result(*s->second, std::move(result), now);
    }
    log_refused_for_limit(now);
    std::vector<std::pair<Address, const char*>> drop;
    if (view_pool_) {
        tick_in_phases(now, drop);
    } else for (auto& [addr, s] : sessions_) {
        if (s->connection.closed_by_client()) { drop.emplace_back(addr, "the client quit"); continue; }
        if (s->connection.timed_out(now)) { drop.emplace_back(addr, "timed out"); continue; }
        if (s->connection.failed()) { drop.emplace_back(addr, "broke the protocol"); continue; }
        if (!s->closing && s->control.state() < ControlChannel::State::AwaitJoin && now >= s->deadline) {
            drop.emplace_back(addr, "did not log in in time");
            continue;
        }
        try {
            std::chrono::steady_clock::time_point a{}, b{};
            const double view_before = timing ? profile.view_us : 0.0;
            if (timing) a = std::chrono::steady_clock::now();
            if (s->player && !s->closing) {
                s->player->tick(s->connection, now);
                if (s->player->failed()) apply(*s, s->control.kick(login_text::kLoginFailed), now);
            }
            if (timing) b = std::chrono::steady_clock::now();
            for (auto& pkt : s->connection.flush(now)) send_(addr, pkt);
            if (timing) {
                const auto c = std::chrono::steady_clock::now();
                profile.sessions_us += std::chrono::duration<double, std::micro>(b - a).count();
                profile.flush_us += std::chrono::duration<double, std::micro>(c - b).count();
                s->profile_us += std::chrono::duration<double, std::micro>(c - a).count();
                s->profile_view_us += profile.view_us - view_before;
            }
            if (s->player) s->player->take_acks(s->connection);
            if (s->closing && (now >= s->close_by || s->connection.unacked_reliable(0) == 0)) drop.emplace_back(addr, "closed");
        } catch (const std::exception& e) {
            spdlog::error("[endpoint] {} ({}): the tick threw {}: dropping the session", addr.to_string(), s->request.character_id, e.what());
            drop.emplace_back(addr, "threw an exception");
        }
    }
    for (const auto& [addr, why] : drop) remove(addr, why);
    drain_outbound();
}

void Endpoint::take_costliest(TickProfile& profile) {
    const Session* top = nullptr;
    for (auto& [addr, s] : sessions_) {
        if (!top || s->profile_us > top->profile_us) top = s.get();
    }
    if (top && top->profile_us > 0) {
        profile.top_name = top->player ? top->player->name() : top->character_name;
        profile.top_us = top->profile_us;
        profile.top_view_us = top->profile_view_us;
        const ActorView* view = top->player ? top->player->actor_view() : nullptr;
        profile.top_awake = view ? view->count(world::ClientState::Awake) : 0;
        profile.top_dormant = view ? view->count(world::ClientState::Dormant) : 0;
    }
    for (auto& [addr, s] : sessions_) {
        s->profile_us = 0;
        s->profile_view_us = 0;
    }
}

void Endpoint::kick_all(const std::string& text, Clock::time_point now) {
    for (auto& [addr, s] : sessions_) apply(*s, s->control.kick(text), now);
}

void Endpoint::finish_players(const char* why) {
    for (auto& [addr, s] : sessions_) {
        if (!s->player) continue;
        try {
            s->player->finish(why);
        } catch (const std::exception& e) {
            spdlog::error("[endpoint] {} ({}): saving the position threw {}", addr.to_string(), s->request.character_id, e.what());
        }
    }
}

const PlayerSession* Endpoint::player_at(const Address& a) const {
    const auto it = sessions_.find(a);
    return it == sessions_.end() ? nullptr : it->second->player.get();
}

std::vector<PlayerHandle> Endpoint::players() {
    std::vector<std::pair<uint64_t, PlayerHandle>> found;
    for (auto& [address, s] : sessions_) {
        if (!s->player || s->closing || s->player->failed()) continue;
        found.push_back({s->id, PlayerHandle{s->player.get(), &s->connection}});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<PlayerHandle> out;
    out.reserve(found.size());
    for (const auto& f : found) out.push_back(f.second);
    return out;
}

std::optional<Address> Endpoint::session_of(const std::string& character_id) const {
    auto it = by_character_.find(character_id);
    if (it == by_character_.end()) return std::nullopt;
    return it->second;
}

}} // namespace aoc::worldd
