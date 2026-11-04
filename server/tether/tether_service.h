// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using TetherSockLen = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
using TetherSockLen = socklen_t;
inline constexpr SOCKET INVALID_SOCKET = -1;
inline constexpr int SOCKET_ERROR = -1;
inline constexpr int WSAEINTR = EINTR;
inline constexpr int WSAEWOULDBLOCK = EWOULDBLOCK;
inline int closesocket(SOCKET s) noexcept { return ::close(s); }
inline int WSAGetLastError() noexcept { return errno; }
#endif

#include <spdlog/spdlog.h>
#include <atomic>

#include "auth/ics_token_file.h"
#include "tether_wire.h"

struct TetherSession {
    uint32_t    session_id  = 0;
    int64_t     send_seq    = 0;
    int64_t     recv_seq    = 0;
    uint32_t    window_size = 64;
    uint8_t     version     = 2;
    sockaddr_in peer        = {};
    bool        connected              = false;
    bool        connection_info_pushed = false;

    static constexpr size_t SENT_WINDOW = 16;
    std::vector<uint8_t> sent_payloads[SENT_WINDOW];

    void store_sent(int64_t seq, const std::vector<uint8_t>& payload) {
        sent_payloads[static_cast<size_t>(seq) % SENT_WINDOW] = payload;
    }
    const std::vector<uint8_t>* get_sent(int64_t seq) const {
        const auto& p = sent_payloads[static_cast<size_t>(seq) % SENT_WINDOW];
        return p.empty() ? nullptr : &p;
    }
};

class TetherServiceImpl {
public:
    TetherServiceImpl()  = default;
    ~TetherServiceImpl() { stop(); }

    TetherServiceImpl(const TetherServiceImpl&)            = delete;
    TetherServiceImpl& operator=(const TetherServiceImpl&) = delete;

    void set_ics_endpoint(std::string host, uint16_t port) {
        ics_host_ = std::move(host);
        ics_port_ = port;
    }

    void set_token_file(std::string path) {
        token_file_ = std::move(path);
    }

    bool start(uint16_t port) {
#ifdef _WIN32
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            spdlog::error("[Tether] socket() failed: {}", ::WSAGetLastError());
            return false;
        }

        // Allow quick restart on the same port across rapid launcher relaunches.
        const int reuse = 1;
        ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(socket_,
                   reinterpret_cast<const sockaddr*>(&addr),
                   sizeof(addr)) == SOCKET_ERROR) {
            spdlog::error("[Tether] bind() failed on UDP:{}: {}",
                          port, ::WSAGetLastError());
            ::closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }

        sockaddr_in bound{};
        TetherSockLen blen = sizeof(bound);
        ::getsockname(socket_,
                      reinterpret_cast<sockaddr*>(&bound), &blen);
        port_ = ntohs(bound.sin_port);

        running_ = true;
        thread_  = std::thread([this] { recv_loop(); });
        spdlog::info("[Tether] UDP ARQ server bound on 0.0.0.0:{}", port_);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        // Wake the blocking recvfrom with a zero-byte loopback datagram.
        if (socket_ != INVALID_SOCKET) {
            sockaddr_in self{};
            self.sin_family      = AF_INET;
            self.sin_port        = htons(port_);
            self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::sendto(socket_, nullptr, 0, 0,
                     reinterpret_cast<const sockaddr*>(&self), sizeof(self));
        }
        if (thread_.joinable()) thread_.join();
        if (socket_ != INVALID_SOCKET) {
            ::closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        spdlog::info("[Tether] stopped");
    }

    [[nodiscard]] bool     is_running() const noexcept { return running_.load(); }
    [[nodiscard]] uint16_t port()       const noexcept { return port_; }

private:
    static int64_t now_msec() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static std::string peer_str(const sockaddr_in& p) {
        char ip[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &p.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(p.sin_port));
    }

    static std::string hex_dump(const uint8_t* data, size_t len,
                                 size_t max_bytes = 64) {
        return tether::wire::hex_dump(data, len, max_bytes);
    }

    static uint32_t build_magic(uint8_t ver, tether::wire::MsgId id) noexcept {
        return (tether::wire::APP_NUMBER << 16) |
               (static_cast<uint32_t>(ver) << 8) |
                static_cast<uint32_t>(id);
    }

    std::vector<uint8_t> pkt_connect_response(const TetherSession& s) {
        tether::wire::Header h{};
        h.magic_number           = build_magic(s.version, tether::wire::MsgId::connect);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.sequence_number        = 0;
        h.next_expected_recv_seq = s.recv_seq;
        h.timestamp_msec         = now_msec();
        auto pkt = tether::wire::encode(h);

        if (s.version >= 2) {
            static constexpr char     PID[]    = "launcher-id-101";
            static constexpr uint32_t PID_LEN  = sizeof(PID) - 1;
            uint8_t lb[4];
            tether::wire::w32(lb, PID_LEN);
            pkt.insert(pkt.end(), lb, lb + 4);
            pkt.insert(pkt.end(),
                       reinterpret_cast<const uint8_t*>(PID),
                       reinterpret_cast<const uint8_t*>(PID) + PID_LEN);
        }
        return pkt;
    }

    std::vector<uint8_t> pkt_ack(const TetherSession& s) {
        tether::wire::Header h{};
        h.magic_number           = build_magic(s.version, tether::wire::MsgId::ack);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.sequence_number        = s.recv_seq;
        h.next_expected_recv_seq = s.recv_seq + 1;
        h.timestamp_msec         = now_msec();
        return tether::wire::encode(h);
    }

    std::vector<uint8_t> pkt_tell_window(const TetherSession& s) {
        tether::wire::Header h{};
        h.magic_number           = build_magic(s.version, tether::wire::MsgId::tell_window);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.next_expected_recv_seq = s.recv_seq + 1;
        h.timestamp_msec         = now_msec();
        return tether::wire::encode(h);
    }

    std::vector<uint8_t> pkt_data(TetherSession& s,
                                   const std::vector<uint8_t>& payload) {
        tether::wire::Header h{};
        h.magic_number           = build_magic(s.version, tether::wire::MsgId::data);
        h.session_id             = s.session_id;
        h.window_size            = 64;
        h.data_length            = static_cast<uint16_t>(payload.size());
        h.sequence_number        = s.send_seq++;
        h.next_expected_recv_seq = s.recv_seq + 1;
        h.timestamp_msec         = now_msec();
        s.store_sent(h.sequence_number, payload);
        auto pkt = tether::wire::encode(h);
        pkt.insert(pkt.end(), payload.begin(), payload.end());
        return pkt;
    }

    void send_raw(const std::vector<uint8_t>& pkt, const sockaddr_in& peer) {
        const int n = ::sendto(
            socket_,
            reinterpret_cast<const char*>(pkt.data()),
            static_cast<int>(pkt.size()),
            0,
            reinterpret_cast<const sockaddr*>(&peer),
            sizeof(peer));
        if (n == SOCKET_ERROR)
            spdlog::error("[Tether] sendto error: {}", ::WSAGetLastError());
    }

    void send_reply(TetherSession& s, const tether::wire::Message& msg) {
        send_raw(pkt_data(s, msg.encode()), s.peer);
    }

    void recv_loop() {
        std::vector<uint8_t> buf(65535);
        while (running_) {
            sockaddr_in peer{};
            TetherSockLen plen = sizeof(peer);
            const int n = ::recvfrom(
                socket_,
                reinterpret_cast<char*>(buf.data()),
                static_cast<int>(buf.size()),
                0,
                reinterpret_cast<sockaddr*>(&peer),
                &plen);

            if (!running_) break;
            if (n == SOCKET_ERROR) {
                const int err = ::WSAGetLastError();
                if (err == WSAEINTR || err == WSAEWOULDBLOCK) continue;
                spdlog::error("[Tether] recvfrom error: {}", err);
                continue;
            }
            if (n > 0) dispatch(buf.data(), static_cast<size_t>(n), peer);
        }
    }

    void dispatch(const uint8_t* buf, size_t len, const sockaddr_in& peer) {
        tether::wire::Header hdr{};
        if (!tether::wire::decode(buf, len, hdr) || !hdr.is_valid()) return;

        spdlog::info("[Tether] RX {} {} session={} seq={} nextRecv={} window={}",
                     peer_str(peer),
                     tether::wire::msg_name(hdr.msg_id()),
                     hdr.session_id,
                     hdr.sequence_number,
                     hdr.next_expected_recv_seq,
                     hdr.window_size);

        const uint8_t* payload     = buf + tether::wire::HEADER_SIZE;
        const size_t   payload_len =
            len > tether::wire::HEADER_SIZE ? len - tether::wire::HEADER_SIZE : 0;

        switch (hdr.msg_id()) {
        case tether::wire::MsgId::connect:
            on_connect(hdr, payload, payload_len, peer);
            break;

        case tether::wire::MsgId::data:
            if (session_.connected && session_.session_id == hdr.session_id)
                on_data(hdr, payload, payload_len);
            else
                spdlog::warn("[Tether] Data for unknown session {}", hdr.session_id);
            break;

        case tether::wire::MsgId::ack:
            break;

        case tether::wire::MsgId::ask_window:
            on_ask_window(hdr);
            break;

        case tether::wire::MsgId::tell_window:
            if (session_.connected && session_.session_id == hdr.session_id)
                session_.window_size = hdr.window_size;
            break;

        default:
            spdlog::warn("[Tether] Unknown MsgId 0x{:02x}  hex={}",
                         static_cast<int>(hdr.msg_id()),
                         hex_dump(buf, std::min(len, size_t(16))));
            break;
        }
    }

    void on_ask_window(const tether::wire::Header& hdr) {
        if (!session_.connected || session_.session_id != hdr.session_id) return;
        const int64_t wanted = hdr.next_expected_recv_seq;
        if (wanted >= session_.send_seq) {
            // Keep unsolicited window probes at the ARQ layer to avoid unexpected client replies.
            send_raw(pkt_tell_window(session_), session_.peer);
            return;
        }
        const auto* payload = session_.get_sent(wanted);
        if (!payload) {
            send_raw(pkt_tell_window(session_), session_.peer);
            return;
        }
        spdlog::debug("[Tether] AskWindow: retransmitting seq={}", wanted);
        tether::wire::Header rh{};
        rh.magic_number           = build_magic(session_.version, tether::wire::MsgId::data);
        rh.session_id             = session_.session_id;
        rh.window_size            = 64;
        rh.data_length            = static_cast<uint16_t>(payload->size());
        rh.sequence_number        = wanted;
        rh.next_expected_recv_seq = session_.recv_seq + 1;
        rh.timestamp_msec         = now_msec();
        auto pkt = tether::wire::encode(rh);
        pkt.insert(pkt.end(), payload->begin(), payload->end());
        send_raw(pkt, session_.peer);
    }

    void on_connect(const tether::wire::Header& hdr,
                    const uint8_t* payload, size_t plen,
                    const sockaddr_in& peer) {
        size_t off = 0;
        std::string remote_addr, processor_id;

        if (off + 4 <= plen) {
            const uint32_t alen = tether::wire::r32(payload + off); off += 4;
            if (off + alen <= plen) {
                remote_addr.assign(reinterpret_cast<const char*>(payload + off), alen);
                off += alen;
            }
        }
        if (hdr.version() >= 2 && off + 4 <= plen) {
            const uint32_t pil = tether::wire::r32(payload + off); off += 4;
            if (off + pil <= plen) {
                processor_id.assign(reinterpret_cast<const char*>(payload + off), pil);
            }
        }

        spdlog::info("[Tether] Connect: remote='{}' processorId='{}' ver={}",
                     remote_addr, processor_id, hdr.version());

        session_.session_id              = hdr.session_id;
        session_.recv_seq                = hdr.sequence_number;
        session_.send_seq                = 0;
        session_.window_size             = hdr.window_size > 0 ? hdr.window_size : 64;
        session_.version                 = hdr.version();
        session_.connection_info_pushed  = false;
        session_.peer        = peer;
        session_.connected   = true;

        send_raw(pkt_connect_response(session_), peer);
        spdlog::info("[Tether] Session {} established (ver={} window={})",
                     session_.session_id, session_.version, session_.window_size);
    }

    void on_data(const tether::wire::Header& hdr,
                 const uint8_t* payload, size_t plen) {
        session_.recv_seq = hdr.sequence_number;
        send_raw(pkt_ack(session_), session_.peer);

        if (plen == 0) return;

        auto msg = tether::wire::Message::decode(payload, plen);

        spdlog::debug("[Tether] TetherMessage tags={} data_string={}B data_bytes={}B",
                      msg.tags.size(), msg.data_string.size(), msg.data_bytes.size());
        for (const auto& [k, v] : msg.tags)
            spdlog::debug("[Tether]   [{}] = '{}'", k, v);

        auto it = msg.tags.find("message_type_name");
        if (it == msg.tags.end()) {
            spdlog::warn("[Tether] TetherMessage missing 'message_type_name' — ignoring");
            return;
        }

        const std::string& type = it->second;
        spdlog::info("[Tether] Dispatch '{}'", type);

        if      (type == "request_open_session")
            handle_open_session(msg);
        else if (type == "request_close_session")
            handle_close_session(msg);
        else if (type == "request_echo")
            handle_echo(msg);
        else if (type == "request_game_client_connection_info")
            handle_connection_info(msg);
        else
            spdlog::warn("[Tether] Unknown message type '{}'", type);
    }

    void handle_open_session(const tether::wire::Message& req) {
        spdlog::info("[Tether] open_session: sending echoed request_open_session with local ICS endpoint");

        const std::string port_s          = std::to_string(ics_port_);
        const std::string ics_endpoint    = ics_host_ + ":" + port_s;
        const std::string auth_endpoint   = "https://" + ics_endpoint;
        // Only hand the launcher's saved login token to a loopback peer on the same host.
        const bool peer_is_loopback =
            aoc::ics_token::is_loopback_ipv4(ntohl(session_.peer.sin_addr.s_addr));
        const auto tokens = aoc::ics_token::client_tokens(token_file_, peer_is_loopback);
        if (!token_file_.empty()) log_token_choice(tokens.saved, peer_is_loopback);
        const std::string& auth_token    = tokens.auth;
        const std::string& refresh_token = tokens.refresh;
        const std::string auth0_token     = "basin-timber-topaz-crane-5247bc426ad3b74b";

        tether::wire::Message echo;
        echo.tags              = req.tags;
        echo.data_string       = req.data_string;
        echo.data_bytes        = req.data_bytes;
        echo.tags["auth_endpoint"]           = auth_endpoint;
        echo.tags["auth_token"]              = auth_token;
        echo.tags["refresh_token"]           = refresh_token;
        echo.tags["auth0_accesstoken"]       = auth0_token;
        echo.tags["user_agent_prefix"]       = "AOC/1.0";
        echo.tags["grpc_metadata_magic_key"] = "x-ics-auth";
        echo.tags["grpc_metadata_magic_val"] = "emulator-magic";
        echo.tags["eac_deployment_id"]       = "aoc-emulator";
        echo.tags["eac_sandbox_id"]          = "emulator-sandbox";
        echo.tags["ics_host"]                = ics_host_;
        echo.tags["ics_port"]                = port_s;
        echo.tags["ics_endpoint"]            = ics_endpoint;
        echo.tags["ics_address"]             = ics_endpoint;
        echo.tags["connection_address"]      = ics_endpoint;
        echo.tags["connection_host"]         = ics_host_;
        echo.tags["connection_port"]         = port_s;
        send_reply(session_, echo);
    }

    void log_token_choice(bool saved, bool peer_is_loopback) const {
        if (!peer_is_loopback) {
            spdlog::warn("[Tether] open_session: the client at {} is not on this machine; "
                         "sending the emulator token, not the saved login token",
                         peer_str(session_.peer));
            return;
        }
        if (!saved) {
            spdlog::info("[Tether] open_session: no saved login token in {}, sending the emulator token "
                         "(the gateway refuses it: sign in through the launcher)", token_file_);
            return;
        }
        const auto age = aoc::ics_token::age(token_file_);
        spdlog::info("[Tether] open_session: sending the launcher's saved login token (saved {} ago)",
                     age ? aoc::ics_token::format_age(*age) : std::string("an unknown time"));
        if (age && *age > aoc::ics_token::kDefaultTokenLifetime)
            spdlog::warn("[Tether] open_session: the saved login token is older than 24 h, auth's default "
                         "token lifetime; if the gateway refuses it, sign in again through the launcher");
    }

    void handle_close_session(const tether::wire::Message& /*req*/) {
        spdlog::info("[Tether] close_session: disconnecting");

        tether::wire::Message reply;
        reply.tags["message_type_name"] = "reply_close_session";
        reply.tags["type"]              = "reply_close_session";
        send_reply(session_, reply);

        session_.connected = false;
    }

    void handle_echo(const tether::wire::Message& req) {
        spdlog::debug("[Tether] echo request");

        tether::wire::Message reply;
        reply.tags              = req.tags;
        reply.data_string       = req.data_string;
        reply.data_bytes        = req.data_bytes;
        reply.tags["message_type_name"] = "reply_echo";
        reply.tags["type"]              = "reply_echo";

        send_reply(session_, reply);
    }

    void handle_connection_info(const tether::wire::Message& req) {
        const std::string port_s = std::to_string(ics_port_);
        spdlog::info("[Tether] connection_info: advertising ICS {}:{}", ics_host_, ics_port_);

        tether::wire::Message reply;
        reply.tags              = req.tags;
        reply.data_string       = req.data_string;
        reply.data_bytes        = req.data_bytes;

        reply.tags["message_type_name"]  = "reply_game_client_connection_info";
        reply.tags["type"]               = "reply_game_client_connection_info";
        reply.tags["result"]             = "ok";

        // Populate every known endpoint tag key across varying game client builds.
        reply.tags["ics_host"]           = ics_host_;
        reply.tags["ics_port"]           = port_s;
        reply.tags["ics_endpoint"]       = ics_host_ + ":" + port_s;
        reply.tags["ics_address"]        = ics_host_ + ":" + port_s;
        reply.tags["connection_address"] = ics_host_ + ":" + port_s;
        reply.tags["connection_host"]    = ics_host_;
        reply.tags["connection_port"]    = port_s;

        send_reply(session_, reply);
    }

    SOCKET             socket_  = INVALID_SOCKET;
    uint16_t           port_    = 0;
    std::atomic<bool>  running_ = false;
    std::thread        thread_;

    TetherSession      session_;

    std::string        ics_host_ = "127.0.0.1";
    uint16_t           ics_port_ = 443;
    std::string        token_file_;
};
