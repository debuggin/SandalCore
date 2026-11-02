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

inline void diff_stat_array(std::vector<std::string> &d, const char *name,
                            const bs::StatArray &a, const bs::StatArray &b) {
  if (a.array_key != b.array_key)
    d.push_back(std::string(name) + ".array_key");
  if (a.items.size() != b.items.size()) {
    d.push_back(std::string(name) + ".size");
    return;
  }
  for (size_t i = 0; i < a.items.size(); ++i) {
    const bs::StatItem &x = a.items[i], &y = b.items[i];
    if (x.id != y.id || x.record != y.record || x.extended != y.extended ||
        x.base != y.base || x.equipment != y.equipment) {
      d.push_back(std::string(name) + "[" + std::to_string(i) +
                  "].id/record/extended/base/equipment");
    }
    if (x.value != y.value)
      d.push_back(std::string(name) + "[record " + std::to_string(x.record) +
                  "].value");
  }
}

inline bool same_setup_guids(const bs::PlayerSetup &a,
                             const bs::PlayerSetup &b) {
  const emit::FIntrepidNetworkGUID *ga[] = {&a.guids.player_state,
                                            &a.guids.player_controller,
                                            &a.guids.global_gm_commands,
                                            &a.guids.game_state,
                                            &a.guids.pawn,
                                            &a.guids.base_character_info,
                                            &a.guids.combat_info,
                                            &a.guids.owner_info,
                                            &a.guids.backpack,
                                            &a.guids.equipment,
                                            &a.guids.quest_storage,
                                            &a.guids.reward_storage,
                                            &a.guids.appearance,
                                            &a.guids.resource_storage[0],
                                            &a.guids.resource_storage[1],
                                            &a.guids.resource_storage[2],
                                            &a.guids.collision_cylinder,
                                            &a.guids.alignment,
                                            &a.guids.ability,
                                            &a.guids.stats};
  const emit::FIntrepidNetworkGUID *gb[] = {&b.guids.player_state,
                                            &b.guids.player_controller,
                                            &b.guids.global_gm_commands,
                                            &b.guids.game_state,
                                            &b.guids.pawn,
                                            &b.guids.base_character_info,
                                            &b.guids.combat_info,
                                            &b.guids.owner_info,
                                            &b.guids.backpack,
                                            &b.guids.equipment,
                                            &b.guids.quest_storage,
                                            &b.guids.reward_storage,
                                            &b.guids.appearance,
                                            &b.guids.resource_storage[0],
                                            &b.guids.resource_storage[1],
                                            &b.guids.resource_storage[2],
                                            &b.guids.collision_cylinder,
                                            &b.guids.alignment,
                                            &b.guids.ability,
                                            &b.guids.stats};
  bool guids = true;
  for (size_t i = 0; i < std::size(ga); ++i)
    guids = guids && *ga[i] == *gb[i];
  return guids;
}

inline void diff_setup_arrays_and_items(std::vector<std::string> &d,
                                        const bs::PlayerSetup &a,
                                        const bs::PlayerSetup &b) {
  const auto note = [&](bool same, const char *name) {
    if (!same)
      d.push_back(name);
  };
  diff_stat_array(d, "stats.owner", a.stats.owner, b.stats.owner);
  diff_stat_array(d, "stats.owner_proxy", a.stats.owner_proxy,
                  b.stats.owner_proxy);
  diff_stat_array(d, "stats.everyone_proxy", a.stats.everyone_proxy,
                  b.stats.everyone_proxy);
  note(a.stats.float_array_key == b.stats.float_array_key,
       "stats.float_array_key");
  bool floats = a.stats.float_everyone_proxy.size() ==
                b.stats.float_everyone_proxy.size();
  for (size_t i = 0; floats && i < a.stats.float_everyone_proxy.size(); ++i) {
    floats = a.stats.float_everyone_proxy[i].id ==
                 b.stats.float_everyone_proxy[i].id &&
             a.stats.float_everyone_proxy[i].record ==
                 b.stats.float_everyone_proxy[i].record;
  }
  note(floats, "stats.float_everyone_proxy");
  note(a.known_abilities == b.known_abilities, "known_abilities");
  bool records = a.records_to_load.size() == b.records_to_load.size();
  for (size_t i = 0; records && i < a.records_to_load.size(); ++i) {
    records = a.records_to_load[i].guid == b.records_to_load[i].guid &&
              a.records_to_load[i].type_id == b.records_to_load[i].type_id;
  }
  note(records, "records_to_load");
  note(a.weapon_skill_line.guid == b.weapon_skill_line.guid &&
           a.weapon_skill_line.name == b.weapon_skill_line.name,
       "weapon_skill_line");
  note(a.attuned_blocks == b.attuned_blocks, "attuned_blocks");
  bool held = a.held_items.array_key == b.held_items.array_key &&
              a.held_items.items.size() == b.held_items.items.size();
  for (size_t i = 0; held && i < a.held_items.items.size(); ++i) {
    const bs::HeldItem &x = a.held_items.items[i], &y = b.held_items.items[i];
    held = std::tie(x.id, x.slot, x.item_guid, x.enchantment) ==
           std::tie(y.id, y.slot, y.item_guid, y.enchantment);
  }
  note(held, "held_items");
}

inline std::vector<std::string> diff_setups(const bs::PlayerSetup &a,
                                            const bs::PlayerSetup &b) {
  std::vector<std::string> d;
  const auto note = [&](bool same, const char *name) {
    if (!same)
      d.push_back(name);
  };
  note(a.character_guid == b.character_guid, "character_guid");
  note(a.name == b.name, "name");
  note(a.race == b.race, "race");
  note(a.gender == b.gender, "gender");
  note(a.level == b.level, "level");
  note(a.class_record_guid == b.class_record_guid, "class_record_guid");
  note(a.customization == b.customization, "customization");
  note(a.shared_appearance_info_id == b.shared_appearance_info_id,
       "shared_appearance_info_id");
  note(std::bit_cast<uint64_t>(a.x) == std::bit_cast<uint64_t>(b.x), "x");
  note(std::bit_cast<uint64_t>(a.y) == std::bit_cast<uint64_t>(b.y), "y");
  note(std::bit_cast<uint64_t>(a.z) == std::bit_cast<uint64_t>(b.z), "z");
  note(std::bit_cast<uint64_t>(a.yaw_degrees) ==
           std::bit_cast<uint64_t>(b.yaw_degrees),
       "yaw_degrees");
  diff_setup_arrays_and_items(d, a, b);
  note(a.appearance_ids == b.appearance_ids, "appearance_ids");
  note(a.orgs_at_war_toast == b.orgs_at_war_toast, "orgs_at_war_toast");
  note(a.login.name_option == b.login.name_option &&
           a.login.unique_id == b.login.unique_id,
       "login");
  note(a.server_id == b.server_id, "server_id");
  note(std::bit_cast<uint64_t>(a.world_time) ==
           std::bit_cast<uint64_t>(b.world_time),
       "world_time");
  note(a.player_id == b.player_id, "player_id");
  note(same_setup_guids(a, b), "guids");
  return d;
}

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

inline svc::Character
cyb_record(int32_t race_id, int32_t gender_id,
           const std::string &json = cyb::kAppearanceJsonb) {
  svc::Character c;
  c.id = "bbec5c86eb101fe5f38c87f0fa22e7d0";
  c.account_id = 1;
  c.name = "cyb";
  c.race_id = race_id;
  c.gender_id = gender_id;
  c.class_id = 2;
  c.archetype_id = 17747;
  c.class_name = "Bard";
  c.appearance_json = json;
  c.level = 1;
  return c;
}

inline std::string replaced(std::string s, const std::string &from,
                            const std::string &to) {
  const size_t at = s.find(from);
  if (at != std::string::npos)
    s.replace(at, from.size(), to);
  return s;
}

inline std::string cyb_json_of(const char *race, const char *gender) {
  std::string j = replaced(cyb::kAppearanceJsonb, "\"gender\": \"Male\"",
                           std::string("\"gender\": \"") + gender + "\"");
  return replaced(j, "\"race\": \"Kaelar\"",
                  std::string("\"race\": \"") + race + "\"");
}

inline bool same_but_order(repl::CharacterCustomization a,
                           repl::CharacterCustomization b) {
  for (repl::CharacterCustomization *c : {&a, &b}) {
    if (c->face_morph_weights) {
      std::sort(c->face_morph_weights->begin(), c->face_morph_weights->end(),
                [](const auto &x, const auto &y) {
                  return std::tie(x.name.hardcoded, x.name.index, x.name.text,
                                  x.name.number) <
                         std::tie(y.name.hardcoded, y.name.index, y.name.text,
                                  y.name.number);
                });
    }
    if (c->sections_values) {
      std::sort(c->sections_values->begin(), c->sections_values->end(),
                [](const auto &x, const auto &y) { return x.key < y.key; });
    }
  }
  return a == b;
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

inline std::string lower(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline std::vector<stats::WornItem> kit_worn(const gear_fixture::ClassKit &k) {
  std::vector<stats::WornItem> w;
  for (int i = 0; i < k.count; ++i)
    w.push_back({k.items[i].record, 1, k.items[i].slot});
  return w;
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

inline const char *race_name_of(int32_t race) {
  static const char *const names[] = {"Kaelar",   "Vaelune", "Vek",   "Renkai",
                                      "Empyrean", "Pyrai",   "Dunir", "Nikua"};
  return names[race];
}

inline bool same_mods_without_durability(stats::GearInputs half,
                                         stats::GearInputs none) {
  for (stats::GearInputs *g : {&half, &none}) {
    g->mods.erase(std::remove_if(g->mods.begin(), g->mods.end(),
                                 [](const stats::StatMod &m) {
                                   return m.stat == 6064629444242636800ll;
                                 }),
                  g->mods.end());
  }
  bool whole = half.mods.size() == none.mods.size();
  for (size_t i = 0; whole && i < half.mods.size(); ++i) {
    whole = half.mods[i].stat == none.mods[i].stat &&
            half.mods[i].value == none.mods[i].value;
  }
  return whole;
}

inline bool check_class_kit_against_go(const bs::StatContext &ctx,
                                       const stats_fixture::Corpus &corpus,
                                       const gear_fixture::ClassKit &k) {
  const std::string name = "kit_" + lower(k.name) + "_l1";
  const stats_fixture::CorpusCase *c = corpus.find(name);
  if (!c) {
    CHECK(false, (name + " is in the corpus").c_str());
    return false;
  }
  bool same_items = c->items.size() == static_cast<size_t>(k.count);
  for (int i = 0; same_items && i < k.count; ++i) {
    same_items = c->items[static_cast<size_t>(i)].record == k.items[i].record &&
                 c->items[static_cast<size_t>(i)].quality == 1;
  }
  CHECK(same_items && !c->half_up,
        (std::string(k.name) +
         ": the corpus case wears this kit at quality 1, unrounded")
            .c_str());
  const stats::GearInputs none =
      stats::gear_inputs(*ctx.gear, kit_worn(k), stats::Rounding::None);
  const stats::GearInputs half =
      stats::gear_inputs(*ctx.gear, kit_worn(k), stats::Rounding::HalfUp);
  std::string why = mods_difference(none.mods, *c);
  if (why.empty())
    why = tags_difference(none.equipped, *c);
  if (why.empty() && !matches_corpus_digest(ctx, *c, none, why))
    why = "digest: " + why;
  CHECK(why.empty(),
        (std::string(k.name) +
         ": the mods, the tags and the digest of every stat equal Go's")
            .c_str());
  if (!why.empty())
    std::printf("    %s\n", why.c_str());
  const bool whole = same_mods_without_durability(half, none);
  CHECK(whole && none.unknown_items == 0 && none.unknown_sets == 0,
        (std::string(k.name) +
         ": the kit's stat values are whole, so half-up rounding gives the "
         "same mods (but for MaxDurability's 37.5); every item is known")
            .c_str());
  return why.empty();
}

inline void test_kits_against_go(const content::ContentDb &db) {
  std::printf("each class's starting kit at level 1: the stats are the Go "
              "engine's, from the real stat blocks\n");
  const bs::StatContext &ctx = world_stats::of(db);
  const stats_fixture::Corpus &corpus = stats_fixture::corpus();
  int ok = 0;
  for (const gear_fixture::ClassKit &k : gear_fixture::kClassKits) {
    ok += check_class_kit_against_go(ctx, corpus, k);
  }
  CHECK(ok == 8, "all eight classes' kits equal Go's");
}

inline void check_capture_record(const bs::PlayerSetup &cap,
                                 const content::ContentDb &content,
                                 const char *label) {
  const svc::Character rec = captured_record_with_kit(cap);
  CHECK(!rec.appearance_json.empty(),
        (std::string(label) + ": the capture's customization has a JSON form")
            .c_str());
  const bs::CharacterSetupResult r = bs::build_player_setup(
      rec, content, cap.login, session_of(cap), world_stats::of(content));
  CHECK(r.setup.has_value() && r.error.empty(),
        (std::string(label) + ": the capture's record builds a setup").c_str());
  if (!r.setup) {
    std::printf("    %s\n", r.error.c_str());
    return;
  }
  const std::vector<std::string> d = diff_setups(*r.setup, cap);
  std::string list;
  for (const auto &x : d)
    list += (list.empty() ? "" : ", ") + x;
  std::printf("    differs from captured_character_setup() in: %s\n",
              list.c_str());
  CHECK(
      d == std::vector<std::string>{"orgs_at_war_toast"},
      (std::string(label) +
       ": the capture's record wearing the Bard's kit is equal to "
       "captured_character_setup() field by field, the 442 stats and HeldItems "
       "and AppearanceIDs included, but for the toast flag (the record does "
       "not carry it)")
          .c_str());
  CHECK(r.notes.size() == 1 && contains(r.notes[0], "13 worn"),
        (std::string(label) + ": the one note says what the character wears")
            .c_str());
  const bs::CharacterSetupResult none =
      bs::build_player_setup(captured_record(cap), content, cap.login,
                             session_of(cap), world_stats::of(content));
  CHECK(none.setup && none.setup->held_items.items.empty() &&
            none.setup->appearance_ids.empty() && none.notes.empty(),
        (std::string(label) + ": an unequipped character: no held items, no "
                              "appearance ids, nothing to note")
            .c_str());
}

void run_character_setup_part2(const bs::PlayerSetup &cap,
                               const content::ContentDb &db);
void test_real_content(const bs::PlayerSetup &cap);
