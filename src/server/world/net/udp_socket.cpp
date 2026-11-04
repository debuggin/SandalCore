// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/udp_socket.cpp
// ============================================================================
#include "net/udp_socket.h"
#include <charconv>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aoc { namespace worldd {

namespace {

using SocketHandle = decltype(::socket(0, 0, 0));

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WinsockInit() { WSACleanup(); }
};
void ensure_winsock() { static WinsockInit init; }
constexpr intptr_t kInvalid = static_cast<intptr_t>(INVALID_SOCKET);
void close_handle(intptr_t h) { closesocket(static_cast<SOCKET>(h)); }
#else
void ensure_winsock() {}
constexpr intptr_t kInvalid = -1;
void close_handle(intptr_t h) { ::close(static_cast<int>(h)); }
#endif

constexpr int kDefaultSocketBufferBytes = 1048576;

void configure_socket(SocketHandle s) noexcept {
    static_assert(kDefaultSocketBufferBytes == UdpSocket::kDefaultSocketBufBytes);
    for (const int opt : {SO_RCVBUF, SO_SNDBUF}) {
        for (int bytes = kDefaultSocketBufferBytes; bytes >= 65536; bytes /= 2) {
            if (::setsockopt(s, SOL_SOCKET, opt, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0) break;
        }
    }
#ifdef _WIN32
    // Without this, an ICMP "port unreachable" from a client that went away
    // makes the next recvfrom fail with WSAECONNRESET.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#endif
}

int get_int_sockopt(intptr_t h, int opt) noexcept {
    if (h == kInvalid) return 0;
    int val = 0;
    socklen_t len = sizeof(val);
    return ::getsockopt(static_cast<SocketHandle>(h), SOL_SOCKET, opt, reinterpret_cast<char*>(&val), &len) == 0 ? val : 0;
}

sockaddr_in to_sockaddr(const Address& a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(a.ip);
    sa.sin_port = htons(a.port);
    return sa;
}

Address from_sockaddr(const sockaddr_in& sa) {
    return {ntohl(sa.sin_addr.s_addr), ntohs(sa.sin_port)};
}

}  // namespace

std::string Address::to_string() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                  (ip >> 8) & 0xFF, ip & 0xFF, port);
    return buf;
}

std::optional<Address> parse_address(const std::string& host_port) {
    unsigned parts[5]{};
    const char* next = host_port.data();
    const char* end = next + host_port.size();
    for (size_t i = 0; i < 5; ++i) {
        const auto parsed = std::from_chars(next, end, parts[i]);
        if (parsed.ec != std::errc{} || parts[i] > (i == 4 ? 65535u : 255u)) return std::nullopt;
        if (i == 4) {
            if (parsed.ptr != end) return std::nullopt;
        } else {
            if (parsed.ptr == end || *parsed.ptr != (i == 3 ? ':' : '.')) return std::nullopt;
            next = parsed.ptr + 1;
        }
    }
    return Address{(parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3], static_cast<uint16_t>(parts[4])};
}

bool PacketBatch::push(const Address& peer, const uint8_t* payload, std::size_t len) noexcept {
    if (count >= kBatchSize || len > kMaxPayloadBytes || (len > 0 && !payload)) return false;
    entries[count] = {peer, static_cast<uint16_t>(len), {}};
    if (len > 0) std::memcpy(entries[count].data, payload, len);
    ++count;
    return true;
}

std::unique_ptr<UdpSocket> UdpSocket::open(const Address& local, std::string* error) {
    ensure_winsock();
    const intptr_t h = static_cast<intptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (h == kInvalid) {
        if (error) *error = "socket() failed";
        return nullptr;
    }
    const auto s = static_cast<SocketHandle>(h);
    configure_socket(s);
    const sockaddr_in sa = to_sockaddr(local);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        if (error) *error = "cannot bind UDP " + local.to_string() + " (is another server using it?)";
        close_handle(h);
        return nullptr;
    }
    return std::unique_ptr<UdpSocket>(new UdpSocket(h));
}

std::unique_ptr<UdpSocket> UdpSocket::open_dual_stack(uint16_t port, std::string* error) {
    ensure_winsock();
    const intptr_t h = static_cast<intptr_t>(::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP));
    if (h == kInvalid) { if (error) *error = "socket(AF_INET6) failed"; return nullptr; }
    const auto s = static_cast<SocketHandle>(h);
    const int v6only = 0;
    ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    configure_socket(s);
    sockaddr_in6 sa6{};
    sa6.sin6_family = AF_INET6; sa6.sin6_addr = in6addr_any; sa6.sin6_port = htons(port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&sa6), sizeof(sa6)) != 0) {
        if (error) *error = "cannot bind dual-stack UDP port " + std::to_string(port);
        close_handle(h); return nullptr;
    }
    return std::unique_ptr<UdpSocket>(new UdpSocket(h));
}
// Cyb captures, C-O-A code

UdpSocket::~UdpSocket() { close(); }
void UdpSocket::close() noexcept { if (handle_ != kInvalid) { close_handle(handle_); handle_ = kInvalid; } }
bool UdpSocket::is_open() const noexcept { return handle_ != kInvalid; }
int UdpSocket::rcvbuf_size() const noexcept { return get_int_sockopt(handle_, SO_RCVBUF); }
int UdpSocket::sndbuf_size() const noexcept { return get_int_sockopt(handle_, SO_SNDBUF); }

std::optional<size_t> UdpSocket::receive(uint8_t* buf, size_t cap, Address& from,
                                         std::chrono::milliseconds timeout) {
    const auto s = static_cast<decltype(::socket(0, 0, 0))>(handle_);
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const int ready = ::select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &tv);
    if (ready < 0) return std::nullopt;
    if (ready == 0) return size_t{0};
    sockaddr_in sa{};
    socklen_t sa_len = sizeof(sa);
    const auto n = ::recvfrom(s, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0,
                              reinterpret_cast<sockaddr*>(&sa), &sa_len);
    if (n < 0) return size_t{0};  // e.g. a datagram larger than `cap`: dropped
    from = from_sockaddr(sa);
    return static_cast<size_t>(n);
}

bool UdpSocket::send(const Address& to, const uint8_t* data, size_t len) {
    const sockaddr_in sa = to_sockaddr(to);
    const auto s = static_cast<decltype(::socket(0, 0, 0))>(handle_);
    return ::sendto(s, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                    reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == static_cast<int>(len);
}

#ifdef __linux__
void init_mmsg(mmsghdr& m, iovec& v, void* buf, std::size_t len, sockaddr_in& a) noexcept {
    v = {buf, len};
    m.msg_hdr.msg_name = &a; m.msg_hdr.msg_namelen = sizeof(a);
    m.msg_hdr.msg_iov = &v; m.msg_hdr.msg_iovlen = 1;
}
#endif

std::size_t UdpSocket::recv_batch(PacketBatch& batch) {
    batch.clear();
    if (handle_ == kInvalid) return 0;
    const auto s = static_cast<SocketHandle>(handle_);
#ifdef __linux__
    mmsghdr msgs[PacketBatch::kBatchSize]{};
    iovec iovs[PacketBatch::kBatchSize]{};
    sockaddr_in addrs[PacketBatch::kBatchSize]{};
    for (std::size_t i = 0; i < PacketBatch::kBatchSize; ++i)
        init_mmsg(msgs[i], iovs[i], batch.entries[i].data, PacketBatch::kMaxPayloadBytes, addrs[i]);
    const int got = ::recvmmsg(s, msgs, static_cast<unsigned int>(PacketBatch::kBatchSize), MSG_DONTWAIT, nullptr);
    for (int i = 0; i < got; ++i, ++batch.count) {
        batch.entries[i].peer = from_sockaddr(addrs[i]);
        batch.entries[i].len = static_cast<uint16_t>(msgs[i].msg_len);
    }
#elif defined(_WIN32)
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);
    for (std::size_t i = 0; i < PacketBatch::kBatchSize; ++i, ++batch.count) {
        WSABUF wbuf{static_cast<ULONG>(PacketBatch::kMaxPayloadBytes), reinterpret_cast<CHAR*>(batch.entries[i].data)};
        DWORD got = 0, flags = 0;
        sockaddr_in sa{};
        int sa_len = sizeof(sa);
        if (::WSARecvFrom(s, &wbuf, 1, &got, &flags, reinterpret_cast<sockaddr*>(&sa), &sa_len, nullptr, nullptr) != 0) break;
        batch.entries[i].peer = from_sockaddr(sa);
        batch.entries[i].len = static_cast<uint16_t>(got);
    }
    nb = 0;
    ::ioctlsocket(s, FIONBIO, &nb);
#else
    for (std::size_t i = 0; i < PacketBatch::kBatchSize; ++i, ++batch.count) {
        sockaddr_in sa{};
        socklen_t sa_len = sizeof(sa);
        const auto n = ::recvfrom(s, reinterpret_cast<char*>(batch.entries[i].data), PacketBatch::kMaxPayloadBytes,
                                  MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&sa), &sa_len);
        if (n < 0) break;
        batch.entries[i].peer = from_sockaddr(sa);
        batch.entries[i].len = static_cast<uint16_t>(n);
    }
#endif
    return batch.count;
}

std::size_t UdpSocket::send_batch(const PacketBatch& batch) {
    if (handle_ == kInvalid) return 0;
    const auto s = static_cast<SocketHandle>(handle_);
    const std::size_t n = batch.count <= PacketBatch::kBatchSize ? batch.count : PacketBatch::kBatchSize;
#ifdef __linux__
    mmsghdr msgs[PacketBatch::kBatchSize]{};
    iovec iovs[PacketBatch::kBatchSize]{};
    sockaddr_in addrs[PacketBatch::kBatchSize]{};
    for (std::size_t i = 0; i < n; ++i) {
        addrs[i] = to_sockaddr(batch.entries[i].peer);
        init_mmsg(msgs[i], iovs[i], const_cast<uint8_t*>(batch.entries[i].data), batch.entries[i].len, addrs[i]);
    }
    const int sent = ::sendmmsg(s, msgs, static_cast<unsigned int>(n), MSG_DONTWAIT);
    return sent > 0 ? static_cast<std::size_t>(sent) : 0;
#elif defined(_WIN32)
    std::size_t sent = 0;
    for (std::size_t i = 0; i < n; ++i, ++sent) {
        WSABUF wbuf{static_cast<ULONG>(batch.entries[i].len), reinterpret_cast<CHAR*>(const_cast<uint8_t*>(batch.entries[i].data))};
        DWORD bytes_sent = 0;
        const sockaddr_in sa = to_sockaddr(batch.entries[i].peer);
        if (::WSASendTo(s, &wbuf, 1, &bytes_sent, 0, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa), nullptr, nullptr) != 0) break;
    }
    return sent;
#else
    std::size_t sent = 0;
    for (std::size_t i = 0; i < n && send(batch.entries[i].peer, batch.entries[i].data, batch.entries[i].len); ++i) ++sent;
    return sent;
#endif
}

Address UdpSocket::local_address() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    ::getsockname(static_cast<SocketHandle>(handle_), reinterpret_cast<sockaddr*>(&ss), &len);
    if (ss.ss_family == AF_INET6) return {0, ntohs(reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_port)};
    return from_sockaddr(*reinterpret_cast<const sockaddr_in*>(&ss));
}

}} // namespace aoc::worldd
