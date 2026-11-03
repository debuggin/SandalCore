// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_udp_socket.cpp: net/udp_socket over loopback
// ============================================================================
#include "net/udp_socket.h"
#include <cstdio>
#include <string>

using namespace aoc::worldd;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static void test_batch_and_dual_stack(UdpSocket& s1, UdpSocket& s2, const Address& a1, const Address& a2) {
    std::printf("socket buffers & dual-stack\n");
    CHECK(s1.rcvbuf_size() >= UdpSocket::kDefaultSocketBufBytes &&
          s1.sndbuf_size() >= UdpSocket::kDefaultSocketBufBytes, "1 MB SO_RCVBUF and SO_SNDBUF applied");

    std::string err;
    auto ds = UdpSocket::open_dual_stack(0, &err);
    CHECK(ds && ds->is_open() && ds->local_port() != 0, "open_dual_stack(0) creates dual-stack socket and assigns port");
    if (ds) {
        CHECK(ds->rcvbuf_size() >= UdpSocket::kDefaultSocketBufBytes &&
              ds->sndbuf_size() >= UdpSocket::kDefaultSocketBufBytes, "dual-stack socket applies 1 MB buffers");
        ds->close();
        CHECK(!ds->is_open(), "close() marks dual-stack socket closed");
    }

    std::printf("packet batch (kBatchSize = 64)\n");
    static_assert(PacketBatch::kBatchSize == 64 && PacketBatch::kMaxPayloadBytes == 1400);
    PacketBatch tx_batch{};
    uint8_t dummy[8]{1, 2, 3, 4};
    CHECK(!tx_batch.push(a2, dummy, PacketBatch::kMaxPayloadBytes + 1), "push rejects oversized payload");
    bool push_ok = true;
    for (std::size_t i = 0; i < PacketBatch::kBatchSize; ++i) {
        uint8_t payload[40]{static_cast<uint8_t>(i)};
        const std::size_t plen = 8 + (i % 32);
        payload[plen - 1] = static_cast<uint8_t>(i ^ 0xA5u);
        push_ok = push_ok && tx_batch.push(a2, payload, plen);
    }
    CHECK(push_ok && tx_batch.count == PacketBatch::kBatchSize, "push 64 datagrams into tx_batch");
    CHECK(!tx_batch.push(a2, dummy, sizeof(dummy)), "push rejects 65th datagram");
    CHECK(s1.send_batch(tx_batch) == PacketBatch::kBatchSize, "send_batch transmits all 64 datagrams");

    std::size_t total_rx = 0;
    bool payload_ok = true;
    for (int attempt = 0; attempt < 20 && total_rx < PacketBatch::kBatchSize; ++attempt) {
        PacketBatch rx_batch{};
        const std::size_t got = s2.recv_batch(rx_batch);
        for (std::size_t j = 0; j < got; ++j) {
            const std::size_t idx = total_rx + j, expected_len = 8 + (idx % 32);
            const auto& e = rx_batch.entries[j];
            if (e.peer != a1 || e.len != expected_len || e.data[0] != static_cast<uint8_t>(idx) ||
                e.data[expected_len - 1] != static_cast<uint8_t>(idx ^ 0xA5u)) payload_ok = false;
        }
        total_rx += got;
    }
    CHECK(total_rx == PacketBatch::kBatchSize && payload_ok, "recv_batch round-trips all 64 datagrams over loopback");
}

int main() {
    std::printf("addresses\n");
    const auto a = parse_address("127.0.0.1:7777");
    CHECK(a && a->ip == 0x7F000001u && a->port == 7777, "parses 127.0.0.1:7777");
    CHECK(a && a->to_string() == "127.0.0.1:7777" && a->is_loopback(), "prints it back; loopback");
    CHECK(!parse_address("localhost:7777") && !parse_address("192.0.2.4") && !parse_address("192.0.2.256:1") &&
          !parse_address("192.0.2.4:70000") && !parse_address("192.0.2.4:5x"),
          "refuses names, missing or bad ports and octets");
    CHECK(!parse_address("4294967423.0.0.1:7777") && !parse_address("127.0.0.1:4294975073") &&
          !parse_address("-0.0.0.0:7777") && !parse_address("+127.0.0.1:7777") &&
          !parse_address(std::string("127.0.0.1:7777\0junk", 19)),
          "oversized numbers, signed fields and embedded NULs cannot become valid addresses");
    CHECK(!parse_address("192.0.2.1:1")->is_loopback(), "192.0.2.1 is not loopback");

    std::printf("sockets\n");
    std::string err;
    auto s1 = UdpSocket::open(*parse_address("127.0.0.1:0"), &err);
    auto s2 = UdpSocket::open(*parse_address("127.0.0.1:0"), &err);
    CHECK(s1 && s2, "two sockets bind to system-chosen ports");
    if (!s1 || !s2) return 1;
    const Address a1 = s1->local_address();
    const Address a2 = s2->local_address();
    CHECK(a1.port != 0 && a2.port != 0 && a1.port != a2.port, "local_address reports the chosen ports");

    auto busy = UdpSocket::open(a1, &err);
    CHECK(!busy && err.find("cannot bind") != std::string::npos, "binding a port in use fails with a reason");

    uint8_t buf[64];
    Address from;
    const auto none = s2->receive(buf, sizeof(buf), from, std::chrono::milliseconds(20));
    CHECK(none && *none == 0, "receive times out with 0");

    const uint8_t msg[] = {1, 2, 3, 4, 5};
    CHECK(s1->send(a2, msg, sizeof(msg)), "send");
    const auto n = s2->receive(buf, sizeof(buf), from, std::chrono::milliseconds(1000));
    CHECK(n && *n == 5 && buf[4] == 5 && from == a1, "the datagram arrives with its sender's address");

    test_batch_and_dual_stack(*s1, *s2, a1, a2);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
