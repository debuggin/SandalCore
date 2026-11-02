// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
//
// test_worldd_channels_common.h: shared fixtures, capture tables, and helpers for test_worldd_channels.cpp
#pragma once

#include "fake_client.h"
#include "net/connection.h"
#include "protocol/wire/ue5_primitives.h"

#include <cstdio>
#include <string>
#include <vector>

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg)                 \
  do {                                   \
    if (cond) {                          \
      std::printf("  [ok ] %s\n", msg);  \
      g_pass++;                          \
    } else {                             \
      std::printf("  [FAIL] %s\n", msg); \
      g_fail++;                          \
    }                                    \
  } while (0)

#define CHECK_QUIET(cond)                  \
  do {                                     \
    if (!(cond)) {                         \
      std::printf("  [FAIL] %s\n", #cond); \
      g_fail++;                            \
    }                                      \
  } while (0)

inline const aoc::protocol::conn::OuterHeader kOuter{0x500c7696u, 0, 5};
inline const aoc::protocol::conn::CustomField kCf{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d};
inline const aoc::worldd::Clock::time_point t0{std::chrono::seconds(1000)};

inline aoc::worldd::ConnectionConfig server_config() {
  aoc::worldd::ConnectionConfig c;
  c.outer = kOuter;
  c.custom_field = kCf;
  c.server_initial_seq = 14265;
  c.client_initial_seq = 15870;
  return c;
}

inline aoc::worldd::ConnectionConfig client_config() {
  aoc::worldd::ConnectionConfig c = server_config();
  std::swap(c.server_initial_seq, c.client_initial_seq);
  return c;
}

inline aoc::protocol::emit::BunchWriter random_bits(size_t bits, uint32_t seed) {
  aoc::protocol::emit::BunchWriter w(bits / 8 + 8);
  uint32_t x = seed * 2654435761u + 12345u;
  for (size_t i = 0; i < bits; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    w.write_bit(static_cast<int>(x & 1));
  }
  return w;
}

inline std::vector<uint8_t> unhex(const char *hex) {
  std::vector<uint8_t> out;
  const std::string s(hex);
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

inline std::vector<uint8_t> hex_bits(const char *hex, size_t bits) {
  const auto b = unhex(hex);
  std::vector<uint8_t> out;
  for (size_t i = 0; i < bits; ++i) {
    out.push_back((b[i / 8] >> (i % 8)) & 1);
  }
  return out;
}

inline aoc::protocol::emit::BunchWriter writer_of(const char *hex, size_t bits) {
  const auto b = unhex(hex);
  aoc::protocol::emit::BunchWriter w(b.size() + 8);
  w.write_bit_range(b.data(), 0, bits);
  return w;
}

inline std::vector<uint8_t> bits_of(const aoc::protocol::emit::BunchWriter &w) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < w.bit_pos(); ++i) {
    out.push_back((w.data()[i / 8] >> (i % 8)) & 1);
  }
  return out;
}

inline std::vector<uint8_t> bits_of(const aoc::worldd::InBunch &b) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < b.bits; ++i) {
    out.push_back((b.data[i / 8] >> (i % 8)) & 1);
  }
  return out;
}

inline std::vector<uint8_t> concat(std::vector<uint8_t> a, const std::vector<uint8_t> &b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

struct Seen {
  size_t packet = 0;
  aoc::protocol::wire::ParsedBunch b;
  std::vector<uint8_t> header;
  std::vector<uint8_t> payload;
};
using Datagrams = std::vector<std::vector<uint8_t>>;

inline Seen make_seen_bunch(size_t pkt_idx, const std::vector<uint8_t> &p,
                            const aoc::protocol::wire::ParsedBunch &b) {
  Seen s;
  s.packet = pkt_idx;
  s.b = b;
  size_t off = b.data_start_bit - b.header_bits;
  for (size_t k = 0; k < b.header_bits; ++k) {
    s.header.push_back(static_cast<uint8_t>(::ue5::read_bits(p.data(), p.size(), off, 1)));
  }
  off = b.data_start_bit;
  for (size_t k = 0; k < b.bunch_data_bits; ++k) {
    s.payload.push_back(static_cast<uint8_t>(::ue5::read_bits(p.data(), p.size(), off, 1)));
  }
  return s;
}

inline std::vector<Seen> parse(const Datagrams &pkts) {
  std::vector<Seen> out;
  for (size_t i = 0; i < pkts.size(); ++i) {
    const auto &p = pkts[i];
    const auto dp = aoc::protocol::conn::read_data_packet(p.data(), p.size(), aoc::protocol::wire::Direction::ServerToClient);
    if (!dp) {
      continue;
    }
    for (const auto &b : aoc::protocol::conn::packet_bunches(p.data(), p.size(), *dp)) {
      out.push_back(make_seen_bunch(i, p, b));
    }
  }
  return out;
}

inline std::vector<Seen> on_channel(const std::vector<Seen> &all, uint32_t channel) {
  std::vector<Seen> out;
  for (const auto &s : all) {
    if (s.b.channel == channel) {
      out.push_back(s);
    }
  }
  return out;
}

struct Captured {
  unsigned chseq;
  size_t hdr_bits;
  const char *hdr_hex;
  size_t bits;
};

inline size_t mismatches(const std::vector<Seen> &seen, size_t first, const Captured *cap, size_t n) {
  size_t bad = 0;
  for (size_t i = 0; i < n; ++i) {
    const bool ok = first + i < seen.size() && seen[first + i].b.ch_sequence == cap[i].chseq &&
                    seen[first + i].header == hex_bits(cap[i].hdr_hex, cap[i].hdr_bits) &&
                    seen[first + i].payload.size() == cap[i].bits;
    if (!ok) {
      std::printf("    part %zu differs from the capture (chSeq %u)\n", i, cap[i].chseq);
      ++bad;
    }
  }
  return bad;
}

#define CHECK_CAPTURE(seen, first, table, msg) \
  CHECK(mismatches(seen, first, table, sizeof(table) / sizeof(table[0])) == 0, msg)

inline aoc::worldd::Chain chain(uint32_t channel, size_t export_bits, size_t payload_bits, bool open = false, uint32_t seed = 1) {
  aoc::worldd::Chain c;
  c.channel = channel;
  c.open = open;
  if (export_bits) {
    c.exports.push_back(random_bits(export_bits, seed));
  }
  c.payload = random_bits(payload_bits, seed + 100);
  return c;
}

inline std::vector<aoc::worldd::InBunch> deliver(aoc::worldd::Connection &client, const Datagrams &pkts, aoc::worldd::Clock::time_point t, size_t drop = SIZE_MAX) {
  std::vector<aoc::worldd::InBunch> got;
  for (size_t i = 0; i < pkts.size(); ++i) {
    if (i == drop) {
      continue;
    }
    for (auto &b : client.receive(pkts[i], t)) {
      got.push_back(std::move(b));
    }
  }
  return got;
}

inline void return_acks(aoc::worldd::Connection &server, aoc::worldd::Connection &client, aoc::worldd::Clock::time_point t) {
  for (const auto &p : client.flush(t)) {
    server.receive(p, t);
  }
}

inline const Captured kPkt22[] = {
    {954, 51, "d3a0ba27737603", 3545},
    {955, 49, "34e0eecc2c5200", 1314},
    {956, 49, "3428efc99c3600", 873},
    {957, 49, "3460efccdc0a00", 173},
};
inline const char *kHudExport =
    "020000001e2214eb02c1e9ea00000000000000000ace7b7ed7e3afa2d800000000000000000a00000000000000000000000000000000"
    "360000005e8ec2daca5eaa925eaed2c8cecae8e65e84a0be829e8690aa8800000000001800000084a0be829e8690aa88be8600a224de5801";
inline const char *kHudRpc = "de08d43020f02142b12e109cae0e0000000000000000";
inline const Captured kPkt23[] = {
    {958, 46, "3480ef99bd1b", 3550},
    {959, 46, "34c0ef990f02", 263},
};
inline const Captured kPkt72[] = {
    {960, 49, "3428f0c91c3f00", 1009},
    {961, 49, "3460f0c80cdc01", 7616},
    {962, 49, "34a0f0c80cdc01", 7616},
    {963, 49, "34e0f0c80cdc01", 7616},
    {964, 49, "3420f1cc9cba00", 2985},
};
inline const Captured kPkt77[] = {{965, 46, "3440f199d313", 2537}};
inline const Captured kGameState[] = {
    {954, 51, "93a1ba27736403", 3473},
    {955, 49, "64e0eec80cdc01", 7616},
    {956, 49, "6420efc80cdc01", 7616},
    {957, 49, "6460efc80cdc01", 7616},
    {958, 49, "64a0efc80cdc01", 7616},
    {959, 49, "64e0efc80cdc01", 7616},
    {960, 49, "6420f0c80cdc01", 7616},
    {961, 49, "6460f0c80cdc01", 7616},
    {962, 49, "64a0f0c80cdc01", 7616},
    {963, 49, "64e0f0c80cdc01", 7616},
    {964, 49, "6420f1c80cdc01", 7616},
    {965, 49, "6460f1c80cdc01", 7616},
    {966, 49, "64a0f1c80cdc01", 7616},
    {967, 49, "64e0f1c80cdc01", 7616},
    {968, 49, "6420f2c80cdc01", 7616},
    {969, 49, "6460f2c80cdc01", 7616},
    {970, 49, "64a0f2c80cdc01", 7616},
    {971, 49, "64e0f2cc1c7c00", 1985},
};
inline const Captured kCh14[] = {
    {954, 51, "93a3ba27732807", 7329},
    {955, 49, "e4e8eec81c3c00", 961},
    {956, 49, "e420efcc4ca901", 6804},
};
inline const Captured kCh21Open[] = {
    {954, 51, "53a5ba2773a005", 5761},
    {955, 49, "54e1eecc5cd701", 7541},
};
inline const Captured kCh21Close3[] = {{956, 53, "1d55c07b660000", 0}};
inline const Captured kCh21Whole[] = {{957, 48, "5305bd67b64b", 2422}};
inline const Captured kCh21Close1a[] = {{958, 53, "0d55e07b660000", 0}};
inline const Captured kCh21Split[] = {
    {959, 51, "5385bf27337007", 7616},
    {960, 49, "5421f0ccac4e01", 5354},
};
inline const Captured kCh21Close1b[] = {{961, 53, "0d55107c660000", 0}};
inline const Captured kUnreliable[] = {
    {0, 30, "30685204", 553},
    {0, 30, "3020b90a", 1372},
};

struct Pair {
  aoc::worldd::Connection s{server_config(), t0};
  aoc::worldd::Connection cl{client_config(), t0};
  aoc::worldd::Clock::time_point t = t0;
  Datagrams sent;
  std::vector<aoc::worldd::InBunch> at_client;

  Datagrams step(size_t drop = SIZE_MAX) {
    t += std::chrono::milliseconds(100);
    Datagrams pkts = s.flush(t);
    for (const auto &p : pkts) {
      sent.push_back(p);
    }
    for (auto &b : deliver(cl, pkts, t, drop)) {
      at_client.push_back(std::move(b));
    }
    t += std::chrono::milliseconds(100);
    return_acks(s, cl, t);
    return pkts;
  }
};

inline void agree(aoc::worldd::Connection &c, const aoc::worldd::Chain &chain_, bool expect, const char *msg) {
  const size_t unacked = c.unacked_reliable(chain_.channel);
  const bool was_open = c.channel_open(chain_.channel);
  const bool was_closing = c.channel_closing(chain_.channel);
  const bool can = c.can_send_chain(chain_);
  const bool can_again = c.can_send_chain(chain_);
  const bool untouched = c.unacked_reliable(chain_.channel) == unacked && c.channel_open(chain_.channel) == was_open &&
                         c.channel_closing(chain_.channel) == was_closing;
  const bool sent = c.send_chain(chain_) != 0;
  const bool ok = can == expect && can_again == can && untouched && sent == can;
  if (!ok) {
    std::printf("    can=%d again=%d untouched=%d sent=%d expected=%d\n", can, can_again, untouched, sent, expect);
  }
  CHECK(ok, msg);
}
