// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/udp_socket.h
//
//  An IPv4 UDP socket for the world server (Windows and POSIX), and the
//  client address type the endpoint keys connections by.
// ============================================================================
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace aoc { namespace worldd {

struct Address {
    uint32_t ip = 0;    // host byte order
    uint16_t port = 0;  // host byte order

    std::string to_string() const;  // "127.0.0.1:7777"
    bool is_loopback() const { return (ip >> 24) == 127; }
    bool operator==(const Address& o) const { return ip == o.ip && port == o.port; }
};

struct AddressHash {
    size_t operator()(const Address& a) const {
        return std::hash<uint64_t>()((static_cast<uint64_t>(a.ip) << 16) | a.port);
    }
};
// C Y B : CoA

/// Parses "a.b.c.d:port" (numeric IPv4 only).
std::optional<Address> parse_address(const std::string& host_port);

inline constexpr int kDefaultSocketBufBytes = 1024 * 1024;

struct PacketBatch {
    static constexpr std::size_t kBatchSize = 64;
    static constexpr std::size_t kMaxPayloadBytes = 1400;
    struct Entry { Address peer{}; uint16_t len = 0; uint8_t data[kMaxPayloadBytes]{}; };
    Entry entries[kBatchSize]{};
    std::size_t count = 0;
    void clear() noexcept { count = 0; }
    bool push(const Address& peer, const uint8_t* payload, std::size_t len) noexcept;
};

class UdpSocket {
public:
    static constexpr int kDefaultSocketBufBytes = 1024 * 1024;
    static constexpr std::size_t kBatchSize = PacketBatch::kBatchSize;
    static constexpr std::size_t kMaxPayloadBytes = PacketBatch::kMaxPayloadBytes;

    /// Binds to `local`; nullptr with a reason in `error` on failure.
    static std::unique_ptr<UdpSocket> open(const Address& local, std::string* error = nullptr);
    static std::unique_ptr<UdpSocket> open_dual_stack(uint16_t port, std::string* error = nullptr);
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] uint16_t local_port() const { return local_address().port; }
    [[nodiscard]] int rcvbuf_size() const noexcept;
    [[nodiscard]] int sndbuf_size() const noexcept;

    /// Waits up to `timeout` for a datagram (zero: does not wait).  Returns its
    /// size (0 when none came), or nullopt if the socket failed.
    std::optional<size_t> receive(uint8_t* buf, size_t cap, Address& from,
                                  std::chrono::microseconds timeout);
    std::optional<size_t> recv_from(uint8_t* buf, size_t cap, Address& from,
                                    std::chrono::microseconds timeout) { return receive(buf, cap, from, timeout); }
    bool send(const Address& to, const uint8_t* data, size_t len);
    bool send_to(const Address& to, const uint8_t* data, size_t len) { return send(to, data, len); }
    std::size_t recv_batch(PacketBatch& batch);
    std::size_t send_batch(const PacketBatch& batch);
    /// The bound address (the port the system chose for port 0).
    Address local_address() const;
    /// The size of the system's receive buffer in bytes, as the system granted it when the socket opened (open asks for kReceiveBufferBytes); 0 when it could not be read.
    int receive_buffer_bytes() const { return receive_buffer_; }
    /// What open asks the system for: datagrams that arrive while a tick runs long wait in it (the default on Windows is about 64 KB, a few hundred packets of a full server).
    static constexpr int kReceiveBufferBytes = 1 << 20;

private:
    explicit UdpSocket(intptr_t handle) : handle_(handle) {}
    intptr_t handle_;
    int receive_buffer_ = 0;
};

}} // namespace aoc::worldd
