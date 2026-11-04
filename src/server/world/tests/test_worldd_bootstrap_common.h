// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bootstrap/bootstrap.h"
#include "bootstrap/captured_setup.h"
#include "capture_aoc_codecs.h"
#include "capture_bootstrap.h"
#include "net/connection.h"
#include "protocol/schema/generated/index.h"

#include <cstdio>

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

struct Ref {
  const char *name;
  size_t bits;
  const char *hex;
};

template <class B>
inline Ref ref(const B &b) {
  return {b.name, b.bits, b.hex};
}

inline bool same_as(const aoc::protocol::emit::BunchWriter &w, const Ref &b) {
  const auto want = capture_blocks::bytes_of(b.hex);
  size_t diff = 0;
  if (w.bit_pos() != b.bits) {
    std::printf("    wrote %zu bits, expected %zu (%s)", w.bit_pos(), b.bits, b.name);
    const size_t n = w.bit_pos() < b.bits ? w.bit_pos() : b.bits;
    if (!capture_blocks::same_bit_range(w.data(), want.data(), n, &diff))
      std::printf("; first differing bit %zu", diff);
    std::printf("\n");
    return false;
  }
  if (!capture_blocks::same_bit_range(w.data(), want.data(), b.bits, &diff)) {
    std::printf("    first differing bit %zu (%s)\n", diff, b.name);
    return false;
  }
  return true;
}

inline bool same_prefix(const aoc::protocol::emit::BunchWriter &w, const Ref &b, size_t n) {
  if (w.bit_pos() < n || b.bits < n) {
    std::printf("    %zu / %zu bits, wanted a prefix of %zu (%s)\n", w.bit_pos(), b.bits, n, b.name);
    return false;
  }
  const auto want = capture_blocks::bytes_of(b.hex);
  size_t diff = 0;
  if (!capture_blocks::same_bit_range(w.data(), want.data(), n, &diff)) {
    std::printf("    first differing bit %zu (%s)\n", diff, b.name);
    return false;
  }
  return true;
}

inline bool emitted_as(const std::optional<aoc::worldd::bootstrap::Send> &s, std::initializer_list<Ref> exports, const Ref &content) {
  if (!s || s->emission.exports.size() != exports.size()) {
    std::printf("    %zu export bunches, expected %zu\n", s ? s->emission.exports.size() : 0, exports.size());
    return false;
  }
  size_t i = 0;
  for (const Ref &x : exports) {
    if (!same_as(s->emission.exports[i++], x))
      return false;
  }
  return same_as(s->emission.payload, content);
}

inline aoc::protocol::emit::BunchWriter joined(std::initializer_list<Ref> parts) {
  aoc::protocol::emit::BunchWriter w;
  for (const Ref &b : parts) {
    const auto bytes = capture_blocks::bytes_of(b.hex);
    w.write_bit_range(bytes.data(), 0, b.bits);
  }
  return w;
}

inline aoc::protocol::emit::FIntrepidNetworkGUID guid(uint64_t id, uint32_t server = 0, uint32_t rnd = 0) {
  aoc::protocol::emit::FIntrepidNetworkGUID g;
  g.ObjectId = id;
  g.ServerId = server;
  g.Randomizer = rnd;
  return g;
}

inline aoc::worldd::bootstrap::PlayerSetup captured_setup() {
  auto s = aoc::worldd::bootstrap::captured_character_setup();
  if (!s) {
    std::printf("  [FAIL] the captured character does not build\n");
    std::exit(2);
  }
  return std::move(*s);
}

inline aoc::worldd::bootstrap::PlayerBootstrap captured_bootstrap(aoc::worldd::bootstrap::GameStateMode mode = aoc::worldd::bootstrap::GameStateMode::Tier2) {
  aoc::worldd::bootstrap::PlayerBootstrap pb(captured_setup(), mode);
  if (!pb.ok()) {
    std::printf("  [FAIL] the captured character's bootstrap does not build: %s\n", pb.error().c_str());
    std::exit(2);
  }
  return pb;
}


void run_bootstrap_sequence_tests();
