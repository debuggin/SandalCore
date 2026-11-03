// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_character_setup_common.h"

#include "bootstrap/captured_setup.h"

#include <fstream>
#include <nlohmann/json.hpp>

static void diff_stat_array(std::vector<std::string> &d, const char *name,
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

static bool same_setup_guids(const bs::PlayerSetup &a,
                             const bs::PlayerSetup &b) {
  const auto &x = a.guids, &y = b.guids;
  return std::tie(x.player_state, x.player_controller, x.global_gm_commands,
                  x.game_state, x.pawn, x.base_character_info, x.combat_info,
                  x.owner_info, x.backpack, x.equipment, x.quest_storage,
                  x.reward_storage, x.appearance, x.resource_storage[0],
                  x.resource_storage[1], x.resource_storage[2],
                  x.collision_cylinder, x.alignment, x.ability, x.stats) ==
         std::tie(y.player_state, y.player_controller, y.global_gm_commands,
                  y.game_state, y.pawn, y.base_character_info, y.combat_info,
                  y.owner_info, y.backpack, y.equipment, y.quest_storage,
                  y.reward_storage, y.appearance, y.resource_storage[0],
                  y.resource_storage[1], y.resource_storage[2],
                  y.collision_cylinder, y.alignment, y.ability, y.stats);
}

static void diff_setup_arrays_and_items(std::vector<std::string> &d,
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

std::vector<std::string> diff_setups(const bs::PlayerSetup &a,
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

static bool same_but_order(repl::CharacterCustomization a,
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

static std::string lower(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static std::vector<stats::WornItem> kit_worn(const gear_fixture::ClassKit &k) {
  std::vector<stats::WornItem> w;
  for (int i = 0; i < k.count; ++i)
    w.push_back({k.items[i].record, 1, k.items[i].slot});
  return w;
}

static bool same_mods_without_durability(stats::GearInputs half,
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

static bool check_class_kit_against_go(const bs::StatContext &ctx,
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

static void test_kits_against_go(const content::ContentDb &db) {
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

void run_kits_against_go(const content::ContentDb &db) {
  test_kits_against_go(db);
}

static void check_race_and_gender_ids() {
  bool races = true;
  for (int32_t id = 0; id <= 7; ++id) {
    const auto r = bs::client_race_from_id(id);
    races = races && r && static_cast<int>(*r) == id;
  }
  CHECK(races, "races 0 Kaelar to 7 Nikua are the wire's races, unchanged");
  CHECK(bs::client_race_from_id(0) == bs::ClientRace::Kaelar && bs::client_race_from_id(6) == bs::ClientRace::Dunir &&
            bs::client_race_from_id(4) == bs::ClientRace::Empyrean && bs::client_race_from_id(2) == bs::ClientRace::Vek,
        "race 0 is Kaelar, 2 Vek (not Kaelar: the old 2), 4 Empyrean, 6 Dunir (not 7)");
  bool others = true;
  for (int32_t id : {-1, 8, 9, 10, 11, 100})
    others = others && !bs::client_race_from_id(id);
  CHECK(others, "Tulnar (8) and every id past the enum (-1, 9-11, 100) are nullopt");
  CHECK(bs::client_gender_from_id(0) == bs::ClientGender::Male && static_cast<int>(*bs::client_gender_from_id(0)) == 0,
        "gender 0 Male");
  CHECK(bs::client_gender_from_id(1) == bs::ClientGender::Female &&
            static_cast<int>(*bs::client_gender_from_id(1)) == 1,
        "gender 1 Female");
  CHECK(!bs::client_gender_from_id(2) && !bs::client_gender_from_id(3) && !bs::client_gender_from_id(-1),
        "gender 2, 3 and -1 are unknown");
}

static void check_race_and_gender_names() {
  const char *names[] = {"Kaelar", "Vaelune", "Vek", "Renkai", "Empyrean", "Pyrai", "Dunir", "Nikua", "Tulnar"};
  bool names_ok = true;
  for (int i = 0; i < 9; ++i) {
    const auto race = static_cast<bs::ClientRace>(i);
    names_ok = names_ok && std::string(bs::client_race_name(race)) == names[i] &&
               bs::client_race_from_name(names[i]) == race;
  }
  CHECK(names_ok && !bs::client_race_name(static_cast<bs::ClientRace>(9)) && !bs::client_race_from_name("Aela") &&
            !bs::client_race_from_name("kaelar"),
        "the enum's nine names (ECharacterRace 0-8, enum_values.txt) both ways; a tenth value and a stranger have none");
  CHECK(std::string(bs::client_gender_name(bs::ClientGender::Female)) == "Female" &&
            bs::client_gender_from_name("Male") == bs::ClientGender::Male && !bs::client_gender_from_name("male") &&
            !bs::client_gender_name(static_cast<bs::ClientGender>(2)),
        "gender names Male and Female both ways");
}

static void check_enum_names() {
  const char *classes[] = {nullptr, "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"};
  bool class_names = true;
  for (int32_t id = 1; id <= 8; ++id) {
    class_names = class_names && bs::client_class_name(id) && std::string(bs::client_class_name(id)) == classes[id];
  }
  CHECK(class_names && !bs::client_class_name(0) && !bs::client_class_name(9) && !bs::client_class_name(-1),
        "ECharacterClass: 1 Ranger, 2 Bard, 3 Cleric, 4 Fighter, 5 Rogue, 6 Summoner, 7 Tank, 8 Mage; 0 and past 8 have no name");
  check_race_and_gender_names();
}

static void check_guid_words_and_sections() {
  const auto w = bs::character_guid_words("d8b2fe1995fb4df99b1e39586c2b2bd4");
  CHECK(w && (*w)[0] == 3635609113u && (*w)[1] == 2516274681u && (*w)[2] == 2602449240u && (*w)[3] == 1814768596u,
        "d8b2fe1995fb4df99b1e39586c2b2bd4 -> 3635609113, 2516274681, 2602449240, 1814768596 (pkt#78 h3-h6)");
  CHECK(bs::character_guid_words("D8B2FE1995FB4DF99B1E39586C2B2BD4") == w, "upper case reads the same");
  CHECK(!bs::character_guid_words("") && !bs::character_guid_words("d8b2fe1995fb4df99b1e39586c2b2bd") &&
            !bs::character_guid_words("d8b2fe1995fb4df99b1e39586c2b2bd40") &&
            !bs::character_guid_words("g8b2fe1995fb4df99b1e39586c2b2bd4") &&
            !bs::character_guid_words("d8b2fe19-95fb-4df9-9b1e-39586c2b2bd4"),
        "31 or 33 digits, a non-hex digit and the dashed form are refused");
  CHECK(bs::body_section_name(0) == std::string("None") && bs::body_section_name(1) == std::string("LowerArms") &&
            bs::body_section_name(28) == std::string("RacialGroup") &&
            bs::body_section_name(29) == std::string("cCount") && !bs::body_section_name(30),
        "the body section enum: None 0, LowerArms 1 ... RacialGroup 28, cCount 29");
}

static void test_tables() {
  std::printf("race, gender and class: the client's enums, taken as they are\n");
  check_race_and_gender_ids();
  check_enum_names();
  check_guid_words_and_sections();
}

static bool has_expected_capture_counts(const repl::CharacterCustomization &c) {
  return c.deltas.size() == 20 && c.decals && c.decals->size() == 2 && c.blend_groups &&
         c.blend_groups->size() == 2 && !(*c.blend_groups)[0] && c.sections_values &&
         c.sections_values->size() == 30 && c.face_morph_weights && c.face_morph_weights->size() == 83 &&
         c.race_gender && c.random_seed == 1454u;
}

static void check_capture_extra_roundtrip(const repl::CharacterCustomization &c) {
  repl::CharacterCustomization full = c;
  full.preset_guid.reset();
  full.helmet_visible = true;
  full.cape_visible = false;
  full.deltas[repl::CustomizationField::NailOpacity] = std::bit_cast<uint32_t>(0.25f);
  full.deltas[repl::CustomizationField::SkinSet] = static_cast<uint32_t>(-3);
  full.deltas[repl::CustomizationField::RacialHorns] = 0x8000000000000001ull;
  const auto fj = bs::customization_to_json(full);
  const auto fb = fj ? bs::customization_from_json(*fj, bs::ClientRace::Kaelar, bs::ClientGender::Male)
                     : bs::CustomizationFromJson{};
  CHECK(fb.value && *fb.value == full, "booleans, a negative SkinSet and an id above 2^63 come back equal");
}

static void test_capture_roundtrip(const bs::PlayerSetup &cap) {
  std::printf("customization (a): the capture's own value -> the client's JSON -> back\n");
  const repl::CharacterCustomization &c = cap.customization;
  CHECK(has_expected_capture_counts(c),
        "the capture's value: 20 scalar deltas, 2 decals, 2 blend groups (the first not sent), 30 sections, 83 morphs, seed 1454");
  const auto js = bs::customization_to_json(c);
  CHECK(js.has_value(), "it has a JSON form");
  if (!js)
    return;
  std::printf("    the JSON form is %zu characters\n", js->size());
  const auto back = bs::customization_from_json(*js, bs::ClientRace::Kaelar, bs::ClientGender::Male);
  CHECK(back.value.has_value() && back.error.empty(), "the JSON form reads");
  if (!back.value) {
    std::printf("    %s\n", back.error.c_str());
    return;
  }
  CHECK(*back.value == c, "...to a value equal to the decoded capture's, member for member and bit for bit");
  CHECK(back.ignored_keys.empty(), "...with no member left unread");
  CHECK(std::tie(back.value->deltas, back.value->face_morph_weights, back.value->sections_values, back.value->decals,
                 back.value->blend_groups, back.value->race_gender) ==
            std::tie(c.deltas, c.face_morph_weights, c.sections_values, c.decals, c.blend_groups, c.race_gender),
        "(deltas, morphs in order, sections in order, decals, blend groups, Race and Gender each equal)");
  CHECK(codec_round_trip(*back.value), "...and it encodes and decodes through the replicator's codec to the last bit");
  const auto again = bs::customization_to_json(*back.value);
  CHECK(again && *again == *js, "writing the value read back gives the same JSON text");
  check_capture_extra_roundtrip(c);
}

static void check_cyb_jsonb(const repl::CharacterCustomization &c, const std::string &json) {
  const std::string jsonb = cyb::kAppearanceJsonb;
  CHECK(jsonb != json && jsonb.find("\"race\": \"Kaelar\", \"class\"") == 1,
        "kAppearanceJsonb is a different text (its first keys are race, class, gender)");
  const auto j = bs::customization_from_json(jsonb, bs::ClientRace::Kaelar, bs::ClientGender::Male);
  CHECK(j.value.has_value() && j.error.empty() && j.ignored_keys.empty(),
        "the jsonb form maps without error and with no key ignored");
  if (j.value) {
    CHECK(!(*j.value == c), "...but its morphs and sections come in another order than the client's");
    CHECK(same_but_order(*j.value, c),
          "...and are the same value but for that order (30 scalar members, 84 morphs, 30 sections)");
    CHECK(codec_round_trip(*j.value), "...which also encodes and decodes to the last bit");
  }
}

static void check_cyb_capture_and_jsonb(const repl::CharacterCustomization &c, const std::string &json) {
  const auto cap = bs::captured_character_setup();
  if (cap) {
    const repl::CharacterCustomization &k = cap->customization;
    CHECK(k.random_seed == c.random_seed &&
              k.deltas.at(repl::CustomizationField::EyeColor) == c.deltas.at(repl::CustomizationField::EyeColor),
          "randomSeed 1454 and eyeColor are the capture's (transcript section 0)");
    size_t found = 0;
    for (const auto &m : *k.face_morph_weights) {
      found += std::any_of(c.face_morph_weights->begin(), c.face_morph_weights->end(),
                           [&](const auto &n) { return n.name == m.name; });
    }
    CHECK(found == 83,
          "all 83 captured morph FNames (text and number) are among cyb's: the capitalisation and '_0' -> number 1 rules match the capture");
    CHECK(k.sections_values && k.sections_values->size() == c.sections_values->size() &&
              (*k.sections_values)[5].key == (*c.sections_values)[5].key,
          "the captured sections are the same 30 keys");
  }

  size_t bits = 0;
  CHECK(codec_round_trip(c, &bits), "the value encodes and decodes through the replicator's codec to the last bit");
  std::printf("    cyb's CharacterCustomization is %zu bits on the wire\n", bits);
  CHECK(repl::struct_accepts("FCharacterCustomizationSaveData", repl::Value(c)), "struct_accepts takes it");
  check_cyb_jsonb(c, json);
}

static void check_cyb_sections_and_morph_counts(const repl::CharacterCustomization &c) {
  CHECK(c.sections_values && c.sections_values->size() == 30 && c.face_morph_weights &&
            c.face_morph_weights->size() == 84,
        "30 body sections, 84 face morphs");
  bool section_keys = c.sections_values.has_value();
  for (size_t i = 0; section_keys && i < c.sections_values->size(); ++i) {
    section_keys = (*c.sections_values)[i].key == i;
  }
  CHECK(section_keys, "the section keys are 0..29 in order: the enum names in the JSON's order (none, lowerArms ... racialGroup, cCount)");
}

static void check_cyb_customization_fields(const repl::CharacterCustomization &c) {
  CHECK(c.deltas.at(repl::CustomizationField::EyeColor) == 108238840485496ull &&
            c.deltas.at(repl::CustomizationField::SkinSet) == 1u &&
            c.deltas.at(repl::CustomizationField::ScleraShape) == 6064630690389884928ull &&
            c.deltas.at(repl::CustomizationField::RacialHorns) == 0 &&
            c.deltas.at(repl::CustomizationField::SkinColorHue) == std::bit_cast<uint32_t>(0.85246866941452026f) &&
            c.deltas.at(repl::CustomizationField::NormalDetailStrength01) == 0,
        "ids from decimal strings (eyeColor 108238840485496), floats as float32 bits, skinSet 1");
  CHECK(c.decals && c.decals->empty() && c.blend_groups && c.blend_groups->empty() && c.helmet_visible == true &&
            c.cape_visible == true,
        "decalData and decalBlendGroups [] (sent as empty arrays), both visibility flags true");
  check_cyb_sections_and_morph_counts(c);
}

static void test_cyb_customization() {
  std::printf("customization (b): cyb's stored JSON\n");
  const std::string json = cyb::kAppearanceJson;
  const auto r = bs::customization_from_json(json, bs::ClientRace::Kaelar, bs::ClientGender::Male);
  CHECK(r.value.has_value() && r.error.empty(), "cyb's 41-key JSON maps without error");
  if (!r.value) {
    std::printf("    %s\n", r.error.c_str());
    return;
  }
  const repl::CharacterCustomization &c = *r.value;
  CHECK(r.ignored_keys.empty(), "every key is read: none ignored (class, gender and race are read as well)");
  CHECK(c.deltas.size() == 30 && !c.preset_guid && c.random_seed == 1454u && c.race_gender &&
            c.race_gender->race == 0 && c.race_gender->gender == 0,
        "all 30 scalar members, no preset, seed 1454, Race 0 Gender 0 (Kaelar, Male: the capture's bytes)");
  check_cyb_customization_fields(c);
  check_cyb_capture_and_jsonb(c, json);
}

static void check_customization_scalar_errors(const std::string &cyb_json) {
  const auto read = [](const std::string &json) {
    return bs::customization_from_json(json, bs::ClientRace::Kaelar, bs::ClientGender::Male);
  };
  auto r = read(replaced(cyb_json, "\"presetGuid\":\"0\"", "\"presetGuid\":\"108136976045690\""));
  CHECK(!r.value && contains(r.error, "presetGuid") && contains(r.error, "preset table"),
        "a non-zero presetGuid is refused: whether Race and Gender are on the wire is not known");
  r = read(replaced(cyb_json, "\"skinSet\":1", "\"skinSet\":\"one\""));
  CHECK(!r.value && contains(r.error, "skinSet"), "a member of the wrong type names the member");
  r = read(replaced(cyb_json, "\"skinSet\":1", "\"skinSet\":3000000000"));
  CHECK(!r.value && contains(r.error, "skinSet") && contains(r.error, "32 bits"), "skinSet beyond 32 bits is refused");
  r = read(replaced(cyb_json, "\"eyeColor\":\"108238840485496\"", "\"eyeColor\":\"12ab\""));
  CHECK(!r.value && contains(r.error, "eyeColor"), "an id that is not a decimal number is refused");
  r = read(replaced(cyb_json, "\"eyeColor\":\"108238840485496\"", "\"eyeColor\":\"99999999999999999999999\""));
  CHECK(!r.value && contains(r.error, "eyeColor"), "an id beyond 64 bits is refused");
  r = read(replaced(cyb_json, "\"headHairLength\":0.7976233959197998", "\"headHairLength\":1e300"));
  CHECK(!r.value && contains(r.error, "headHairLength") && contains(r.error, "finite"),
        "a float that overflows float32 is refused");
  r = read(replaced(cyb_json, "\"bIsHelmetVisible\":true", "\"bIsHelmetVisible\":1"));
  CHECK(!r.value && contains(r.error, "bIsHelmetVisible"), "a visibility flag that is not a boolean is refused");
}

static void check_customization_struct_errors(const std::string &cyb_json) {
  const auto read = [](const std::string &json) {
    return bs::customization_from_json(json, bs::ClientRace::Kaelar, bs::ClientGender::Male);
  };
  auto r = read(replaced(cyb_json, "\"lowerArms\":", "\"lowerLegs\":"));
  CHECK(!r.value && contains(r.error, "lowerLegs") && contains(r.error, "body section"),
        "an unknown body section is refused by name");
  r = read(replaced(cyb_json, "\"none\":{\"x\":", "\"none\":{\"q\":"));
  CHECK(!r.value && contains(r.error, "sectionsValues.none.x"), "a section without x is refused");
  r = read(replaced(cyb_json, "\"decalData\":[]", "\"decalData\":[{\"guid\":\"1\"}]"));
  CHECK(!r.value && contains(r.error, "decalData[0]"), "a decal missing members is refused");
  r = read(replaced(cyb_json, "\"decalBlendGroups\":[]", "\"decalBlendGroups\":{}"));
  CHECK(!r.value && contains(r.error, "decalBlendGroups"), "decalBlendGroups that is not an array is refused");
  r = read(replaced(cyb_json, "\"faceMorphWeightMaps\":{", "\"faceMorphWeightMaps\":{\"Nose_Bridge_Hooked_0\":0.5,"));
  CHECK(!r.value && contains(r.error, "cannot carry"),
        "a morph named twice (the JSON key and its capitalised twin) is refused before the wire");
  r = read(replaced(cyb_json, "\"nose_Bridge_Hooked_0\":0", "\"nose_Bridge_Hooked_0\":\"high\""));
  CHECK(!r.value && contains(r.error, "nose_Bridge_Hooked_0"), "a morph weight that is not a number names the morph");
}

static void check_customization_mismatch_and_nan(const std::string &cyb_json) {
  const auto read = [](const std::string &json, bs::ClientRace race = bs::ClientRace::Kaelar,
                       bs::ClientGender g = bs::ClientGender::Male) {
    return bs::customization_from_json(json, race, g);
  };
  auto r = read(cyb_json, bs::ClientRace::Dunir, bs::ClientGender::Male);
  CHECK(!r.value && contains(r.error, "Kaelar") && contains(r.error, "Dunir"),
        "the JSON's race against the record's: a Kaelar appearance for a Dunir record is refused");
  r = read(cyb_json, bs::ClientRace::Kaelar, bs::ClientGender::Female);
  CHECK(!r.value && contains(r.error, "Male") && contains(r.error, "Female"),
        "the JSON's gender against the record's likewise");
  r = read(replaced(cyb_json, "\"race\":\"Kaelar\"", "\"race\":\"Elf\""));
  CHECK(!r.value && contains(r.error, "'race'"), "a race name that is none of the nine is refused");
  r = read(replaced(cyb_json, "\"class\":\"Bard\"", "\"class\":7"));
  CHECK(!r.value && contains(r.error, "'class'"), "a class that is not text is refused");
  r = read(replaced(cyb_json, "\"nailOpacity\":0", "\"nailOpacity\":0,\"hairBand\":3"));
  CHECK(r.value && r.ignored_keys.size() == 1 && r.ignored_keys[0] == "hairBand",
        "a member this server does not know is ignored and listed");
  const std::string no_meta = replaced(
      replaced(replaced(cyb_json, ",\"gender\":\"Male\"", ""), ",\"race\":\"Kaelar\"", ""), ",\"class\":\"Bard\"", "");
  r = read(no_meta);
  CHECK(r.value && r.ignored_keys.empty() && r.value->race_gender && r.value->race_gender->race == 0,
        "without race, gender and class keys the arguments decide");

  repl::CharacterCustomization nan;
  nan.deltas[repl::CustomizationField::SkinColorHue] = std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN());
  CHECK(!bs::customization_to_json(nan).has_value(), "a NaN float has no JSON form");
  repl::CharacterCustomization hard;
  hard.face_morph_weights = std::vector<repl::CustomizationMorph>{{repl::NameValue::hardcoded_name(5), 0}};
  CHECK(!bs::customization_to_json(hard).has_value(), "a hardcoded morph name has no JSON form");
}

static void test_customization_errors() {
  std::printf("customization: what is refused\n");
  const auto read = [](const std::string &json) {
    return bs::customization_from_json(json, bs::ClientRace::Kaelar, bs::ClientGender::Male);
  };
  const std::string cyb_json = cyb::kAppearanceJson;
  auto r = read("");
  CHECK(!r.value && contains(r.error, "not valid JSON"), "an empty string is not JSON");
  r = read("{\"presetGuid\":");
  CHECK(!r.value && contains(r.error, "not valid JSON"), "cut-off JSON is refused");
  r = read("[1,2]");
  CHECK(!r.value && contains(r.error, "not a JSON object"), "an array is not a customization");
  r = read("{}");
  CHECK(r.value && r.value->deltas.empty() && r.value->race_gender && !r.value->decals && !r.value->sections_values,
        "{} is a customization with no member: all defaults, with Race and Gender");
  r = read(std::string(70000, ' ') + "{}");
  CHECK(!r.value && contains(r.error, "too large"),
        "JSON over kMaxAppearanceBytes (64 KB) is refused before it is parsed, valid or not");
  r = read(std::string(bs::kMaxAppearanceBytes - 2, ' ') + "{}");
  CHECK(r.value.has_value(), "(exactly kMaxAppearanceBytes is still read)");

  check_customization_scalar_errors(cyb_json);
  check_customization_struct_errors(cyb_json);
  check_customization_mismatch_and_nan(cyb_json);
}

static std::optional<std::string> read_corpus() {
  const fs::path candidates[] = {
      fs::path("fixtures") / "customization_cases.json",
      fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "fixtures" /
          "customization_cases.json",
  };
  for (const fs::path &p : candidates) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
      continue;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
  }
  return std::nullopt;
}

static std::string replace_index_token(std::string u, int idx) {
  for (size_t at; (at = u.find("{n}")) != std::string::npos;)
    u.replace(at, 3, std::to_string(idx));
  return u;
}

static std::string corpus_case_sent(const nlohmann::ordered_json &c) {
  if (c.contains("text"))
    return c["text"].get<std::string>();
  if (!c.contains("text_repeat"))
    return c["customization"].dump();
  const auto &r = c["text_repeat"];
  std::string sent = r["prefix"].get<std::string>();
  const std::string unit = r["unit"].get<std::string>();
  for (int i = 0; i < r["count"].get<int>(); ++i) {
    sent += replace_index_token(unit, i);
  }
  sent += r["suffix"].get<std::string>();
  return sent;
}

struct CorpusStats {
  size_t cases = 0;
  size_t accepted = 0;
  size_t built = 0;
  size_t refused = 0;
  size_t wrong = 0;
};

static void check_corpus_case(const nlohmann::ordered_json &c, CorpusStats &st) {
  ++st.cases;
  const std::string name = c.value("name", "?");
  const std::string sent = corpus_case_sent(c);
  const std::string stored = c.contains("stored") ? c["stored"].dump() : sent;
  const auto race = bs::client_race_from_id(c["race"].get<int32_t>());
  const auto gender = bs::client_gender_from_id(c["gender"].get<int32_t>());
  if (!race || !gender) {
    std::printf("  [FAIL] %s: race %d gender %d is not a race and gender\n", name.c_str(), c["race"].get<int>(),
                c["gender"].get<int>());
    ++st.wrong;
    return;
  }
  const bool want_built = c["worldd"].get<std::string>() == "builds";
  const bool is_accepted = c["accepted"].get<bool>();
  const auto r = bs::customization_from_json(stored, *race, *gender);
  if (r.value.has_value() != want_built) {
    std::printf("  [FAIL] %s: worldd %s it, the corpus says it %s (%s)\n", name.c_str(),
                r.value ? "builds" : "refuses", want_built ? "builds" : "refuses", r.error.c_str());
    ++st.wrong;
    return;
  }
  if (is_accepted && !r.value) {
    std::printf("  [FAIL] %s: the service accepts it and worldd refuses it\n", name.c_str());
    ++st.wrong;
    return;
  }
  if (c.contains("stored") && bs::customization_from_json(sent, *race, *gender).value.has_value()) {
    std::printf("  [FAIL] %s: worldd builds the sent form, which has a preset: Normalize would not be needed\n",
                name.c_str());
    ++st.wrong;
    return;
  }
  st.accepted += is_accepted;
  st.built += r.value.has_value();
  st.refused += !r.value.has_value();
}

static void test_customization_corpus() {
  std::printf("the customization corpus shared with the character service (fixtures/customization_cases.json)\n");
  const auto text = read_corpus();
  if (!text) {
    std::printf("  [FAIL] fixtures/customization_cases.json is not there (looked in the working directory and by the source's place)\n");
    g_fail++;
    return;
  }
  using ojson = nlohmann::ordered_json;
  const ojson file = ojson::parse(*text, nullptr, false);
  if (file.is_discarded() || !file.contains("cases") || !file["cases"].is_array()) {
    std::printf("  [FAIL] the corpus is not JSON with a 'cases' array\n");
    g_fail++;
    return;
  }
  CorpusStats st;
  for (const ojson &c : file["cases"])
    check_corpus_case(c, st);
  std::printf("  %zu cases: %zu accepted by the service, worldd builds %zu and refuses %zu\n", st.cases, st.accepted,
              st.built, st.refused);
  CHECK(st.cases > 300, "the corpus has the 125 retail customizations, cyb and the hostile cases");
  CHECK(st.wrong == 0, "worldd builds exactly the cases the corpus says, and every case the service accepts");
  CHECK(st.accepted > 130 && st.refused > 100,
        "the corpus has accepted cases (cyb, the retail ones) and cases worldd refuses");
}

static stats::StatValue make_stat_val(int64_t guid, uint8_t rep, float v, uint8_t flags = 0) {
  stats::StatValue s;
  s.guid = guid;
  s.replication = rep;
  s.flags = flags;
  s.value = v;
  s.base = v - 1;
  s.equip = 1;
  return s;
}

static void check_stats_setup_edge_cases(const bs::StatsSetup &layout) {
  std::string error;
  std::vector<stats::StatValue> twice = {make_stat_val(100, 1, 1), make_stat_val(100, 1, 2)};
  CHECK(!bs::stats_setup_for(twice, layout, error) && contains(error, "twice"), "a stat twice is refused");
  std::vector<stats::StatValue> nan = {make_stat_val(100, 1, std::numeric_limits<float>::quiet_NaN())};
  CHECK(!bs::stats_setup_for(nan, layout, error) && contains(error, "finite"), "a NaN is refused");

  std::vector<stats::StatValue> two_floats = {make_stat_val(900, 7, 0, stats::kStatFloatArray),
                                              make_stat_val(901, 7, 0, stats::kStatFloatArray)};
  const auto f = bs::stats_setup_for(two_floats, layout, error);
  CHECK(f && f->float_everyone_proxy.size() == 2 && f->float_everyone_proxy[1].id == 2 &&
            f->float_everyone_proxy[1].record == 901,
        "a second float array stat takes the next id");
}

static void check_stats_setup_owner_items(const bs::StatsSetup &out) {
  CHECK(out.owner.items.size() == 3 && out.owner.items[0].id == 2 && out.owner.items[0].record == 100 &&
            out.owner.items[1].id == 9 && out.owner.items[1].record == 102 && out.owner.items[2].id == 10 &&
            out.owner.items[2].record == 103,
        "Owner: the layout's items still there keep their ids and order (2, 9), the class's own resource takes the next id (10), 101 is gone");
  CHECK(out.owner.array_key == 12, "...and the key stays the layout's 12");
  CHECK(out.owner.items[0].value == std::bit_cast<uint32_t>(1.5f) && !out.owner.items[0].extended,
        "a stat's value is its float bits");
  CHECK(out.owner.items[1].extended && out.owner.items[1].base == std::bit_cast<uint32_t>(1.5f) &&
            out.owner.items[1].equipment == std::bit_cast<uint32_t>(1.0f),
        "a stat that replicates base and equipment carries both");
}

static void check_stats_setup_proxy_items(const bs::StatsSetup &out) {
  CHECK(out.owner_proxy.items.size() == 1 && out.owner_proxy.items[0].id == 3 && out.owner_proxy.array_key == 3,
        "OwnerProxy: 200 keeps id 3 and the key");
  CHECK(out.everyone_proxy.items.size() == 2 && out.everyone_proxy.items[1].id == 6 &&
            out.everyone_proxy.items[1].record == 301 && out.everyone_proxy.array_key == 7,
        "EveryoneProxy: 300 keeps 5, 301 takes 6, the key stays 7");
  CHECK(out.float_everyone_proxy.size() == 1 && out.float_everyone_proxy[0].id == 1 &&
            out.float_everyone_proxy[0].record == 900 && out.float_array_key == 4,
        "the float array stat keeps its item");
  bool placed = true;
  for (const bs::StatArray *a : {&out.owner, &out.owner_proxy, &out.everyone_proxy}) {
    for (const bs::StatItem &it : a->items) {
      placed = placed && it.record != 104 && it.record != 105 && it.record != 900;
    }
  }
  CHECK(placed, "a stat of audience 6 (other players) or 0, and the float array's stat, are in none of the Int32 arrays");
}

static void test_stats_setup_for() {
  std::printf("stats_setup_for: the arrays from the engine's stats\n");
  bs::StatsSetup layout;
  layout.owner = {12, {{2, 100, 0, false, 0, 0}, {4, 101, 0, false, 0, 0}, {9, 102, 0, true, 0, 0}}};
  layout.owner_proxy = {3, {{3, 200, 0, false, 0, 0}}};
  layout.everyone_proxy = {7, {{5, 300, 0, false, 0, 0}}};
  layout.float_array_key = 4;
  layout.float_everyone_proxy = {{1, 900}};
  std::string error;

  std::vector<stats::StatValue> v = {make_stat_val(100, 1, 1.5f),
                                     make_stat_val(102, 1, 2.5f, stats::kStatBuckets),
                                     make_stat_val(103, 1, 3.5f),
                                     make_stat_val(104, 6, 4.5f),
                                     make_stat_val(105, 0, 5.5f),
                                     make_stat_val(200, 5, 6.5f),
                                     make_stat_val(300, 7, 7.5f),
                                     make_stat_val(301, 7, 8.5f),
                                     make_stat_val(900, 7, 0, stats::kStatFloatArray)};
  const auto out = bs::stats_setup_for(v, layout, error);
  CHECK(out.has_value(), "the arrays are made");
  if (!out) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  check_stats_setup_owner_items(*out);
  check_stats_setup_proxy_items(*out);
  check_stats_setup_edge_cases(layout);
}

static svc::InventoryItem make_mount_test_item(svc::ItemContainer c, int slot, int64_t record, unsigned n) {
  svc::InventoryItem it;
  it.container = c;
  it.slot = slot;
  it.item_record_id = record;
  char guid[33];
  std::snprintf(guid, sizeof guid, "%08x%08x%08x%08x", 0xC0000000u + n, 0x1000u + n, 0x2000u + n, n + 1);
  it.instance_guid = guid;
  it.quality = 1;
  it.durability = 11;
  it.created_at_ms = 1'771'299'474'000ll;
  return it;
}

static void check_carried_item_join_lists(
    const bs::PlayerSetup &cap, const content::ContentDb &db, const content::MountData &data) {
  constexpr int64_t kSummon = 108436548376185ll, kMountA = 9990001, kMountB = 9990002;
  bs::StatContext ctx = world_stats::of(db);
  svc::Character without = captured_record_with_kit(cap);
  svc::Character with = without;
  with.items.push_back(make_mount_test_item(svc::ItemContainer::Backpack, 3, kMountA, 7));
  with.items.push_back(make_mount_test_item(svc::ItemContainer::Backpack, 4, kMountB, 8));
  ctx.mounts = &data;
  const bs::CharacterSetupResult r0 = bs::build_player_setup(without, db, cap.login, session_of(cap), ctx);
  const bs::CharacterSetupResult r1 = bs::build_player_setup(with, db, cap.login, session_of(cap), ctx);
  CHECK(r0.setup && r1.setup, "the characters build");
  if (!r0.setup || !r1.setup)
    return;
  const auto count_of = [](const std::vector<bs::AbilityRecord> &v, int64_t g) {
    return std::count_if(v.begin(), v.end(), [&](const bs::AbilityRecord &r) { return r.guid == g; });
  };
  CHECK(count_of(r0.setup->records_to_load, kSummon) == 0 &&
            r0.setup->records_to_load.size() == cap.records_to_load.size(),
        "no mount item: the captured Bard's records");
  CHECK(count_of(r1.setup->records_to_load, kSummon) == 1 &&
            r1.setup->records_to_load.size() == cap.records_to_load.size() + 1,
        "two mount items: one more record, Summon_Mount");
  CHECK(std::count(r1.setup->known_abilities.begin(), r1.setup->known_abilities.end(), kSummon) == 0 &&
            r1.setup->known_abilities == r0.setup->known_abilities,
        "and KnownAbilities are as they were");
  const std::span<const int64_t> tail = bs::static_known_tail();
  bool tail_last = r1.setup->records_to_load.size() >= tail.size();
  for (size_t i = 0; tail_last && i < tail.size(); ++i) {
    tail_last = r1.setup->records_to_load[r1.setup->records_to_load.size() - tail.size() + i].guid == tail[i];
  }
  CHECK(tail_last, "the static tail still ends the records: the item's ability lies before it");
  bs::StatContext no_mounts = world_stats::of(db);
  const bs::CharacterSetupResult r2 = bs::build_player_setup(with, db, cap.login, session_of(cap), no_mounts);
  CHECK(r2.setup && count_of(r2.setup->records_to_load, kSummon) == 0, "a world with no mount tables adds nothing");
}

static void test_carried_item_abilities(const bs::PlayerSetup &cap, const content::ContentDb &db) {
  std::printf("the abilities of the items the character carries (a mount item's Summon_Mount)\n");
  constexpr int64_t kSummon = 108436548376185ll, kMountA = 9990001, kMountB = 9990002, kNotAMount = 9990003;
  content::MountData data;
  for (const int64_t item : {kMountA, kMountB}) {
    content::MountRow row;
    row.item = item;
    row.cast_ability = kSummon;
    data.mounts.push_back(row);
  }
  content::MountRow no_ability;
  no_ability.item = 9990004;
  data.mounts.push_back(no_ability);
  std::string error;
  const auto inv = aoc::worldd::items::Inventory::from_items(
      {make_mount_test_item(svc::ItemContainer::Backpack, 3, kMountA, 1),
       make_mount_test_item(svc::ItemContainer::Backpack, 4, kNotAMount, 2),
       make_mount_test_item(svc::ItemContainer::Backpack, 5, kMountB, 3),
       make_mount_test_item(svc::ItemContainer::Backpack, 6, 9990004, 4)},
      error);
  CHECK(inv.has_value(), "the test bag builds");
  if (!inv)
    return;
  CHECK(bs::carried_item_abilities(&data, *inv) == std::vector<int64_t>{kSummon},
        "two mount items and others: Summon_Mount, once");
  CHECK(bs::carried_item_abilities(nullptr, *inv).empty(), "no mount tables: nothing");
  const auto none = aoc::worldd::items::Inventory::from_items(
      {make_mount_test_item(svc::ItemContainer::Backpack, 3, kNotAMount, 5)}, error);
  CHECK(none && bs::carried_item_abilities(&data, *none).empty(), "no mount item: nothing");
  const auto worn = aoc::worldd::items::Inventory::from_items(
      {make_mount_test_item(svc::ItemContainer::Equipment, 40, kMountA, 6)}, error);
  CHECK(worn && bs::carried_item_abilities(&data, *worn) == std::vector<int64_t>{kSummon},
        "a mount item worn counts too (retail's LoadEquipmentAbilities)");
  check_carried_item_join_lists(cap, db, data);
}

int main() {
  const auto cap = bs::captured_character_setup();
  if (!cap) {
    std::printf("  [FAIL] captured_character_setup() did not read\n");
    return 1;
  }
  test_tables();
  test_capture_roundtrip(*cap);
  test_cyb_customization();
  test_customization_errors();
  test_customization_corpus();
  test_stats_setup_for();

  const auto dir = content_fixture::make_temp_dir("aoc_worldd_character_setup_test_");
  if (content_fixture::make_db(dir / "content.db")) {
    std::string error;
    auto db = content::ContentDb::open((dir / "content.db").string(), error);
    if (db) {
      run_character_setup_stats(*cap, *db);
      test_carried_item_abilities(*cap, *db);
      test_kits_against_go(*db);
    } else {
      std::printf("  [FAIL] the small content.db did not open: %s\n", error.c_str());
      g_fail++;
    }
  } else {
    g_fail++;
  }
  std::error_code ec;
  fs::remove_all(dir, ec);

  run_character_setup_real_content(*cap);
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
