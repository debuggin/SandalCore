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

inline aoc::protocol::repl::CodecOptions stat_options() {
  aoc::protocol::repl::CodecOptions o;
  o.stat_int32_extended = [](int64_t record) {
    const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
    return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
  };
  return o;
}

struct Block {
  bool is_actor = true;
  bool has_rep = false;
  bool dynamic = false;
  aoc::protocol::emit::FIntrepidNetworkGUID object;
  std::string name;
  std::optional<aoc::protocol::repl::RepState> rep;
  std::vector<aoc::protocol::repl::FastArrayField> fast;
  std::vector<aoc::protocol::repl::RpcCall> rpcs;
  size_t payload_bits = 0;
  bool read_to_end = false;
  std::vector<std::pair<uint32_t, size_t>> raw_fields;
};

struct Decoded {
  bool ok = false;
  aoc::protocol::emit::FIntrepidNetworkGUID actor;
  std::vector<Block> blocks;
  size_t new_actor_bits = 0;
  std::optional<aoc::protocol::repl::QuantVec> location;
  std::optional<aoc::protocol::repl::Rot> rotation;
};

inline const aoc::protocol::schema::ClassSchema *class_of_role(aoc::worldd::bootstrap::Role r) {
  namespace gen = aoc::protocol::schema::gen;
  using aoc::worldd::bootstrap::Role;
  switch (r) {
  case Role::PlayerController:
    return gen::find_class_schema("AoCPlayerControllerBP_C");
  case Role::GameState:
    return gen::find_class_schema("AoCGameStateBP_C");
  case Role::Pawn:
    return gen::find_class_schema("PlayerPawn_C");
  case Role::PlayerState:
    return gen::find_class_schema("AoCPlayerStateBP_C");
  }
  return nullptr;
}

inline bool read_new_actor(const aoc::protocol::emit::BunchWriter &w, bool player_controller, Decoded &d) {
  namespace repl = aoc::protocol::repl;
  aoc::protocol::wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
  auto read_guid = [&] {
    aoc::protocol::emit::FIntrepidNetworkGUID g;
    const uint64_t lo = r.read_uint32(), hi = r.read_uint32();
    g.ObjectId = lo | (hi << 32);
    g.ServerId = r.read_uint32();
    g.Randomizer = r.read_uint32();
    return g;
  };
  d.actor = read_guid();
  read_guid();
  read_guid();
  auto vector = [&](std::optional<repl::QuantVec> *out) {
    if (!r.read_bit())
      return true;
    if (!r.read_bit())
      return false;
    const auto v = repl::read_quantized_vector(r);
    if (!v)
      return false;
    if (out)
      *out = *v;
    return true;
  };
  if (!vector(&d.location))
    return false;
  if (r.read_bit()) {
    const auto rot = repl::read_struct("FRotator", r);
    const repl::Rot *p = rot ? rot->get_if<repl::Rot>() : nullptr;
    if (!p)
      return false;
    d.rotation = *p;
  }
  if (!vector(nullptr) || !vector(nullptr))
    return false;
  if (player_controller) {
    r.read_uint8();
    r.read_uint32();
  }
  if (r.overflowed())
    return false;
  d.new_actor_bits = r.pos();
  return true;
}

inline const aoc::protocol::schema::ClassSchema *subobject_class(const aoc::worldd::bootstrap::PlayerBootstrap &pb, const aoc::protocol::emit::FIntrepidNetworkGUID &g, std::string *name, bool *dynamic) {
  for (const aoc::protocol::repl::Actor *a : {&pb.pawn(), &pb.player_controller()}) {
    if (const aoc::protocol::repl::Subobject *s = a->find(g)) {
      *dynamic = s->dynamic_class.has_value();
      *name = s->dynamic_class ? s->dynamic_class->front().name : s->name;
      return s->obj.schema;
    }
  }
  return nullptr;
}

inline void decode_raw_rep_fields(const aoc::protocol::schema::ClassSchema &cls, const aoc::protocol::emit::BunchWriter &w, size_t end, size_t &p, Block &b) {
  namespace repl = aoc::protocol::repl;
  size_t r = p;
  while (r < end) {
    const auto h = repl::read_field_header(cls, w.data(), end, r);
    if (!h)
      break;
    b.raw_fields.emplace_back(h->field, h->payload_bits);
    r += h->payload_bits;
  }
  p = r;
}

inline void decode_rep_block(const aoc::protocol::schema::ClassSchema &cls, const aoc::protocol::emit::BunchWriter &w, size_t end, size_t &p, Block &b) {
  namespace repl = aoc::protocol::repl;
  const repl::CodecOptions opts = stat_options();
  b.rep = repl::read_rep_block(cls, w.data(), end, p, opts);
  if (!b.rep || p >= end) {
    b.read_to_end = b.rep.has_value() && p == end;
    return;
  }
  size_t q = p;
  if (const auto f = repl::read_fast_array_fields(cls, w.data(), end, q, opts)) {
    b.fast = *f;
    p = q;
  } else {
    decode_raw_rep_fields(cls, w, end, p, b);
  }
  b.read_to_end = p == end;
}

inline void decode_non_rep_block(const aoc::protocol::schema::ClassSchema &cls, const aoc::protocol::emit::BunchWriter &w, size_t end, size_t &p, Block &b) {
  namespace repl = aoc::protocol::repl;
  const repl::CodecOptions opts = stat_options();
  size_t q = p;
  if (const auto f = repl::read_fast_array_fields(cls, w.data(), end, q, opts)) {
    b.fast = *f;
    p = q;
  } else {
    q = p;
    if (const auto r = repl::read_rpc_fields(cls, w.data(), end, q, opts)) {
      b.rpcs = *r;
      p = q;
    }
  }
  b.read_to_end = p == end;
}

inline Block decode_single_block(const aoc::worldd::bootstrap::PlayerBootstrap &pb, const aoc::worldd::bootstrap::Send &s, const aoc::protocol::repl::BlockView &v) {
  const aoc::protocol::emit::BunchWriter &w = s.emission.payload;
  Block b;
  b.is_actor = v.header.is_actor;
  b.has_rep = v.header.has_rep_layout;
  b.object = v.header.object;
  b.payload_bits = v.payload_bits;
  const aoc::protocol::schema::ClassSchema *cls = v.header.is_actor ? class_of_role(s.role) : subobject_class(pb, v.header.object, &b.name, &b.dynamic);
  size_t p = v.payload_pos;
  const size_t end = v.payload_pos + v.payload_bits;
  if (!cls || v.payload_bits == 0) {
    b.read_to_end = v.payload_bits == 0;
  } else if (v.header.has_rep_layout) {
    decode_rep_block(*cls, w, end, p, b);
  } else {
    decode_non_rep_block(*cls, w, end, p, b);
  }
  if (!v.header.is_actor && b.name.empty())
    b.name = "?";
  return b;
}

inline Decoded decode(const aoc::worldd::bootstrap::PlayerBootstrap &pb, const aoc::worldd::bootstrap::Send &s) {
  namespace repl = aoc::protocol::repl;
  Decoded d;
  const aoc::protocol::emit::BunchWriter &w = s.emission.payload;
  size_t pos = 0;
  if (s.opens_channel) {
    if (!read_new_actor(w, s.role == aoc::worldd::bootstrap::Role::PlayerController, d))
      return d;
    pos = d.new_actor_bits;
  }
  const auto views = repl::read_content_blocks(w.data(), w.bit_pos(), pos);
  if (!views)
    return d;
  d.ok = true;
  for (const repl::BlockView &v : *views)
    d.blocks.push_back(decode_single_block(pb, s, v));
  return d;
}

inline std::string block_summary(const Block &b) {
  std::string out = b.is_actor ? "actor" : "sub:" + b.name;
  if (!b.rpcs.empty()) {
    for (const aoc::protocol::repl::RpcCall &c : b.rpcs)
      out += std::string(".") + c.rpc->name;
    return out;
  }
  if (b.payload_bits == 0)
    return out + "(empty)";
  if (!b.has_rep)
    return out + "(fast)";
  return out + ((b.fast.empty() && b.raw_fields.empty()) ? "(rep)" : "(rep+fast)");
}

inline std::string summary(const Decoded &d) {
  std::string out;
  for (const Block &b : d.blocks) {
    if (!out.empty())
      out += ' ';
    out += block_summary(b);
  }
  return out;
}

inline bool all_read(const Decoded &d) {
  if (!d.ok)
    return false;
  for (const Block &b : d.blocks) {
    if (!b.read_to_end)
      return false;
  }
  return !d.blocks.empty();
}

inline const aoc::protocol::repl::Value *value_of(const Block &b, const aoc::protocol::schema::ClassSchema &cls, std::string_view path) {
  return b.rep ? b.rep->get(cls, path) : nullptr;
}

inline size_t parts_of(const aoc::worldd::bootstrap::Send &s) {
  const size_t bits = s.emission.payload.bit_pos();
  const size_t content = bits <= aoc::worldd::Connection::kMaxSingleBunchBits ? 1 : (bits + 7615) / 7616;
  return s.emission.exports.size() + content;
}

inline std::optional<std::vector<aoc::worldd::bootstrap::Send>> sequence_of(aoc::worldd::bootstrap::PlayerBootstrap &pb) {
  auto seq = pb.join_sequence();
  if (!seq)
    std::printf("    join_sequence failed (%s)\n", pb.error().c_str());
  return seq;
}

inline void log_sequence_bits(const std::vector<aoc::worldd::bootstrap::Send> &seq) {
  using aoc::worldd::bootstrap::Send;
  static const char *const kRole[] = {"PC", "GameState", "pawn", "PlayerState"};
  size_t total_bits = 0;
  for (const Send &s : seq) {
    size_t export_bits = 0;
    for (const aoc::protocol::emit::BunchWriter &e : s.emission.exports)
      export_bits += e.bit_pos();
    total_bits += export_bits + s.emission.payload.bit_pos();
    std::printf("    %-11s %-40s exports %5zu bits, payload %6zu bits, %2zu parts\n", kRole[static_cast<size_t>(s.role)], s.what, export_bits, s.emission.payload.bit_pos(), parts_of(s));
  }
  std::printf("    in all %zu bits (%.1f KB)\n", total_bits, total_bits / 8192.0);
}

inline bool exports_within_limits(const aoc::worldd::bootstrap::Send &s) {
  for (const aoc::protocol::emit::BunchWriter &e : s.emission.exports) {
    if (e.bit_pos() > aoc::worldd::Connection::kMaxSingleBunchBits || e.bit_pos() == 0)
      return false;
  }
  return true;
}

inline bool send_shape_within_limits(const aoc::worldd::bootstrap::Send &s) {
  if (s.emission.exported_by_bunch.size() != s.emission.exports.size())
    return false;
  return s.emission.payload.bit_pos() > 0 && s.emission.payload.bit_pos() <= aoc::worldd::Connection::kMaxPartialBits;
}

inline size_t max_channel_parts(const std::map<aoc::worldd::bootstrap::Role, size_t> &per_channel) {
  size_t worst = 0;
  for (const auto &[role, parts] : per_channel)
    worst = std::max(worst, parts);
  return worst;
}

inline void check_limits(const std::vector<aoc::worldd::bootstrap::Send> &seq, const char *what) {
  using aoc::worldd::bootstrap::Role;
  using aoc::worldd::bootstrap::Send;
  bool parts_ok = true, export_ok = true, shape_ok = true;
  std::map<Role, size_t> per_channel;
  log_sequence_bits(seq);
  for (const Send &s : seq) {
    const size_t parts = parts_of(s);
    parts_ok = parts_ok && parts <= aoc::worldd::Connection::kMaxUnackedReliable;
    export_ok = export_ok && exports_within_limits(s);
    shape_ok = shape_ok && send_shape_within_limits(s);
    if (s.reliable())
      per_channel[s.role] += parts;
  }
  const size_t worst = max_channel_parts(per_channel);
  const bool window_ok = worst <= aoc::worldd::Connection::kMaxUnackedReliable;
  char msg[200];
  std::snprintf(msg, sizeof msg, "%s: every chain has at most 256 parts and every export bunch at most 7621 bits", what);
  CHECK(parts_ok && export_ok && shape_ok, msg);
  std::snprintf(msg, sizeof msg, "%s: no channel has more than 256 reliable parts in flight if nothing is acknowledged (the most: %zu)", what, worst);
  CHECK(window_ok, msg);
}

void run_bootstrap_part2();
