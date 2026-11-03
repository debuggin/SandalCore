// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "bootstrap/bootstrap.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "gear_fixture.h"
#include "stats_fixture.h"
#include "world_stats.h"

#include <sstream>

namespace bs = aoc::worldd::bootstrap;
namespace content = aoc::worldd::content;
namespace stats = aoc::worldd::stats;
namespace svc = aoc::worldd::svc;
namespace repl = aoc::protocol::repl;
namespace emit = aoc::protocol::emit;
namespace wire = aoc::protocol::wire;
namespace fs = std::filesystem;

inline int g_pass = 0;
inline int g_fail = 0;
inline void CHECK(bool cond, const char *msg) {
  if (cond) {
    std::printf("  [ok ] %s\n", msg);
    g_pass++;
  } else {
    std::printf("  [FAIL] %s\n", msg);
    g_fail++;
  }
}

inline bool contains(const std::string &s, const char *part) {
  return s.find(part) != std::string::npos;
}

std::vector<std::string> diff_setups(const bs::PlayerSetup &a,
                                     const bs::PlayerSetup &b);

inline svc::Character captured_record(const bs::PlayerSetup &cap) {
  svc::Character c;
  c.id = "d8b2fe1995fb4df99b1e39586c2b2bd4";
  c.account_id = 1;
  c.name = "RandomChar";
  c.race_id = 0;
  c.gender_id = 0;
  c.class_id = 2;
  c.archetype_id = 17747;
  c.class_name = "Bard";
  const auto js = bs::customization_to_json(cap.customization);
  c.appearance_json = js ? *js : std::string();
  c.level = 1;
  c.has_position = true;
  c.position = {cap.x, cap.y, cap.z, static_cast<float>(cap.yaw_degrees)};
  return c;
}

inline bs::SessionValues session_of(const bs::PlayerSetup &cap) {
  bs::SessionValues s;
  s.server_id = cap.server_id;
  s.world_time = cap.world_time;
  s.player_id = cap.player_id;
  s.guids = cap.guids;
  return s;
}

inline std::string replaced(std::string s, const std::string &from,
                            const std::string &to) {
  const size_t at = s.find(from);
  if (at != std::string::npos)
    s.replace(at, from.size(), to);
  return s;
}

inline bool codec_round_trip(const repl::CharacterCustomization &c,
                             size_t *bits_out = nullptr) {
  using repl::Value;
  emit::BunchWriter w;
  if (!repl::write_struct("FCharacterCustomizationSaveData", Value(c), w))
    return false;
  wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
  const auto back = repl::read_struct("FCharacterCustomizationSaveData", r);
  if (!back || r.pos() != w.bit_pos())
    return false;
  const auto *v = back->get_if<repl::CharacterCustomization>();
  if (!v || !(*v == c))
    return false;
  emit::BunchWriter w2;
  if (!repl::write_struct("FCharacterCustomizationSaveData", *back, w2) ||
      w2.bit_pos() != w.bit_pos())
    return false;
  for (size_t i = 0; i < w.byte_size(); ++i) {
    if (w.data()[i] != w2.data()[i])
      return false;
  }
  if (bits_out)
    *bits_out = w.bit_pos();
  return true;
}

inline std::vector<svc::InventoryItem>
kit_items(const gear_fixture::KitItem *kit, int count) {
  std::vector<svc::InventoryItem> out;
  for (int i = 0; i < count; ++i) {
    svc::InventoryItem it;
    it.container = svc::ItemContainer::Equipment;
    it.slot = kit[i].slot;
    it.item_record_id = kit[i].record;
    char guid[33];
    std::snprintf(guid, sizeof guid, "%08x%08x%08x%08x",
                  0xB0000000u + static_cast<unsigned>(i),
                  0x1000u + static_cast<unsigned>(i),
                  0x2000u + static_cast<unsigned>(i),
                  static_cast<unsigned>(i) + 1);
    it.instance_guid = guid;
    it.quality = 1;
    it.durability = 11;
    it.created_at_ms = 1'771'299'474'000ll;
    out.push_back(it);
  }
  return out;
}

inline svc::Character captured_record_with_kit(const bs::PlayerSetup &cap) {
  svc::Character c = captured_record(cap);
  c.items = kit_items(gear_fixture::kBardKit,
                      static_cast<int>(std::size(gear_fixture::kBardKit)));
  return c;
}

inline std::string mods_difference(const std::vector<stats::StatMod> &got,
                                   const stats_fixture::CorpusCase &want) {
  if (got.size() != want.mods.size()) {
    return std::to_string(got.size()) + " mods, the corpus has " +
           std::to_string(want.mods.size());
  }
  for (size_t i = 0; i < got.size(); ++i) {
    uint64_t b;
    std::memcpy(&b, &got[i].value, 8);
    if (got[i].stat != want.mods[i].first || b != want.mods[i].second) {
      return "mod " + std::to_string(i) + " (stat " +
             std::to_string(got[i].stat) + ")";
    }
  }
  return {};
}

inline std::string tags_difference(std::vector<stats::EquippedTag> got,
                                   const stats_fixture::CorpusCase &want) {
  std::vector<std::pair<std::string, std::string>> a, b = want.tags;
  for (const auto &t : got)
    a.emplace_back(t.slot, t.tag);
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  if (a != b)
    return std::to_string(a.size()) + " tags, the corpus has " +
           std::to_string(b.size());
  return {};
}

inline bool matches_corpus_digest(const bs::StatContext &ctx,
                                  const stats_fixture::CorpusCase &c,
                                  const stats::GearInputs &g,
                                  std::string &why) {
  stats::StatInputs in;
  in.class_enum = c.class_enum;
  in.level = c.level;
  in.mods = g.mods;
  in.equipped = g.equipped;
  stats::StatResult res;
  std::string err;
  if (!stats::run_stats(*ctx.bundle, in, stats::StatState{}, res, err)) {
    why = err;
    return false;
  }
  if (stats_fixture::digest(res) != c.digest || res.stats.size() != c.members ||
      res.faults != c.faults || res.ignored_mods != c.ignored) {
    why = "digest " + std::to_string(stats_fixture::digest(res)) + " vs " +
          std::to_string(c.digest) + ", " + std::to_string(res.stats.size()) +
          " stats vs " + std::to_string(c.members) + ", ignored " +
          std::to_string(res.ignored_mods) + " vs " + std::to_string(c.ignored);
    return false;
  }
  return true;
}

void run_kits_against_go(const content::ContentDb &db);
void run_character_setup_stats(const bs::PlayerSetup &cap,
                               const content::ContentDb &db);
void run_character_setup_real_content(const bs::PlayerSetup &cap);
