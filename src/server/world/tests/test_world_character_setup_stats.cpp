// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_world_character_setup_common.h"

static svc::Character
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

static std::string cyb_json_of(const char *race, const char *gender) {
  std::string j = replaced(cyb::kAppearanceJsonb, "\"gender\": \"Male\"",
                           std::string("\"gender\": \"") + gender + "\"");
  return replaced(j, "\"race\": \"Kaelar\"",
                  std::string("\"race\": \"") + race + "\"");
}

static const char *race_name_of(int32_t race) {
  static const char *const names[] = {"Kaelar",   "Vaelune", "Vek",   "Renkai",
                                      "Empyrean", "Pyrai",   "Dunir", "Nikua"};
  return names[race];
}

static void check_capture_record(const bs::PlayerSetup &cap,
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

static bool same_stat_array_layout(const bs::StatsSetup &got_s,
                                   const bs::StatsSetup &want_s) {
  const std::pair<const bs::StatArray *, const bs::StatArray *> arrays[] = {
      {&got_s.owner, &want_s.owner},
      {&got_s.owner_proxy, &want_s.owner_proxy},
      {&got_s.everyone_proxy, &want_s.everyone_proxy}};
  bool layout_kept = true;
  for (const auto &[got, want] : arrays) {
    layout_kept = layout_kept && got->array_key == want->array_key &&
                  got->items.size() == want->items.size();
    for (size_t i = 0; layout_kept && i < got->items.size(); ++i) {
      layout_kept = got->items[i].id == want->items[i].id &&
                    got->items[i].record == want->items[i].record;
    }
  }
  return layout_kept;
}

static void check_setup_stats_bare(const bs::PlayerSetup &cap,
                                   const content::ContentDb &db,
                                   const bs::StatContext &ctx) {
  std::string error;
  svc::Character bare = captured_record(cap);
  const bs::CharacterSetupResult rb =
      bs::build_player_setup(bare, db, cap.login, session_of(cap), ctx);
  CHECK(rb.setup.has_value(), "the Bard with no items builds");
  if (!rb.setup)
    return;
  const auto attrs = db.primary_attributes(content_fixture::kBard, 1, error);
  bool six = attrs.has_value() && attrs->size() == 6;
  if (six) {
    for (const content::PrimaryAttribute &a : *attrs) {
      const auto it = std::find_if(
          rb.setup->stats.owner.items.begin(),
          rb.setup->stats.owner.items.end(),
          [&](const bs::StatItem &i) { return i.record == a.stat_guid; });
      six = six && it != rb.setup->stats.owner.items.end() &&
            std::bit_cast<float>(it->value) == static_cast<float>(a.value);
    }
  }
  CHECK(six, "its six primary attributes are class_stat_base's (Strength 24, "
             "Dexterity 24, Constitution 30, Intelligence 39, Wisdom 36, "
             "Mentality 30)");
  const std::vector<std::string> dd = diff_setups(*rb.setup, cap);
  bool some_values = false;
  for (const std::string &x : dd)
    some_values = some_values || x.find("].value") != std::string::npos;
  CHECK(same_stat_array_layout(rb.setup->stats, cap.stats) && some_values,
        "with no gear the values differ from the capture (its mitigation, "
        "Intelligence and what follows), the ids, keys and order do not");
}

static bool all_class_stat_vectors_distinct(
    const std::map<int, std::vector<uint32_t>> &owner_values) {
  int distinct = 0;
  for (const auto &[id, vals] : owner_values) {
    bool unique = true;
    for (const auto &[other, ov] : owner_values)
      unique = unique && (other == id || ov != vals);
    distinct += unique;
  }
  return distinct == 8;
}

static void check_bard_vs_tank_resource(const bs::PlayerSetup &cap,
                                        const content::ContentDb &db,
                                        const bs::StatContext &ctx) {
  const auto find_item = [](const bs::StatArray &a, int64_t guid) {
    return std::find_if(
        a.items.begin(), a.items.end(),
        [&](const bs::StatItem &i) { return i.record == guid; });
  };
  const bs::CharacterSetupResult rbard = bs::build_player_setup(
      captured_record_with_kit(cap), db, cap.login, session_of(cap), ctx);
  svc::Character tank = captured_record(cap);
  tank.class_id = 7;
  tank.class_name = "Tank";
  tank.archetype_id = 0;
  tank.items = kit_items(gear_fixture::kTankKit,
                         static_cast<int>(std::size(gear_fixture::kTankKit)));
  const bs::CharacterSetupResult rtank =
      bs::build_player_setup(tank, db, cap.login, session_of(cap), ctx);
  constexpr int64_t kBardResource = 6064632920367763592ll;
  CHECK(rbard.setup && rtank.setup &&
            find_item(rbard.setup->stats.owner, kBardResource) !=
                rbard.setup->stats.owner.items.end() &&
            find_item(rtank.setup->stats.owner, kBardResource) ==
                rtank.setup->stats.owner.items.end(),
        "the Bard's own resource stat is in the Bard's Owner array and not in "
        "the Tank's");
}

static void check_setup_stats_other_classes(const bs::PlayerSetup &cap,
                                            const content::ContentDb &db,
                                            const bs::StatContext &ctx) {
  std::map<int, std::vector<uint32_t>> owner_values;
  for (const gear_fixture::ClassKit &k : gear_fixture::kClassKits) {
    svc::Character c = captured_record(cap);
    c.class_id = k.class_id;
    c.class_name = k.name;
    c.archetype_id = 0;
    c.items = kit_items(k.items, k.count);
    const bs::CharacterSetupResult rc =
        bs::build_player_setup(c, db, cap.login, session_of(cap), ctx);
    CHECK(rc.setup.has_value() && rc.setup->class_record_guid == k.guid,
          (std::string(k.name) + ": builds with its class's record Guid")
              .c_str());
    if (!rc.setup) {
      std::printf("    %s\n", rc.error.c_str());
      continue;
    }
    const size_t owner_stats = rc.setup->stats.owner.items.size();
    CHECK(owner_stats >= 334 && owner_stats <= 336 &&
              rc.setup->stats.owner_proxy.items.size() == 96 &&
              rc.setup->stats.everyone_proxy.items.size() == 11,
          (std::string(k.name) + ": 334 to 336 Owner stats (its own resource "
                                 "stats), 96 OwnerProxy and 11 EveryoneProxy")
              .c_str());
    std::vector<uint32_t> vals;
    for (const bs::StatItem &it : rc.setup->stats.owner.items)
      vals.push_back(it.value);
    owner_values[k.class_id] = vals;
    CHECK(rc.setup->stats.owner.array_key == 417 &&
              rc.setup->stats.owner_proxy.array_key == 169 &&
              rc.setup->stats.everyone_proxy.array_key == 21,
          (std::string(k.name) +
           ": the array keys are the captured 417, 169 and 21")
              .c_str());
  }
  CHECK(all_class_stat_vectors_distinct(owner_values),
        "no two classes have the same Owner stats");
  check_bard_vs_tank_resource(cap, db, ctx);
}

static bool all_stats_match_arrays(const bs::CharacterSetupResult &r) {
  if (!r.character_stats || !r.setup)
    return false;
  for (const stats::StatValue &v : r.character_stats->values()) {
    if (v.float_array() || v.replication == 0 || v.replication == 6)
      continue;
    const bs::StatArray &a = v.replication == 1 ? r.setup->stats.owner
                             : v.replication == 5
                                 ? r.setup->stats.owner_proxy
                                 : r.setup->stats.everyone_proxy;
    const auto it =
        std::find_if(a.items.begin(), a.items.end(),
                     [&](const bs::StatItem &i) { return i.record == v.guid; });
    if (it == a.items.end() || it->value != std::bit_cast<uint32_t>(v.value))
      return false;
  }
  return true;
}

static void test_setup_stats(const bs::PlayerSetup &cap,
                             const content::ContentDb &db, const char *label) {
  std::printf("the stats of the setup (%s)\n", label);
  const bs::StatContext &ctx = world_stats::of(db);
  const bs::CharacterSetupResult r = bs::build_player_setup(
      captured_record_with_kit(cap), db, cap.login, session_of(cap), ctx);
  CHECK(r.setup.has_value() && r.error.empty(),
        "the captured Bard in its kit builds a setup");
  if (!r.setup) {
    std::printf("    %s\n", r.error.c_str());
    return;
  }
  const std::vector<std::string> d = diff_setups(*r.setup, cap);
  std::string list;
  for (const auto &x : d)
    list += (list.empty() ? "" : ", ") + x;
  CHECK(d == std::vector<std::string>{"orgs_at_war_toast"},
        "equal to captured_character_setup() in every field but the toast "
        "flag, the 442 stats (values, ids, keys, base and equipment) and "
        "HeldItems included");
  if (d.size() != 1)
    std::printf("    differs in: %s\n", list.c_str());
  CHECK(r.setup->stats.owner.items.size() == 335 &&
            r.setup->stats.owner_proxy.items.size() == 96 &&
            r.setup->stats.everyone_proxy.items.size() == 11 &&
            r.setup->stats.float_everyone_proxy.size() == 1,
        "335 Owner, 96 OwnerProxy, 11 EveryoneProxy items and the float "
        "array's one");
  CHECK(r.character_stats && r.character_stats->started() &&
            r.character_stats->values().size() == 466 &&
            r.character_stats->faults() == 0 &&
            r.character_stats->unknown_items() == 0,
        "the session starts with the 466 stats of the Bard's four lists (443 "
        "replicate: the 442 and TempHealth), no fault and no unknown item");
  CHECK(all_stats_match_arrays(r),
        "every stat of the character is the item of its array");
  check_setup_stats_bare(cap, db, ctx);
  check_setup_stats_other_classes(cap, db, ctx);
}

static void check_capture_default_spawn(const bs::PlayerSetup &cap,
                                        const content::ContentDb &content,
                                        const svc::Character &rec) {
  svc::Character none = rec;
  none.has_position = false;
  none.position = {1, 2, 3, 4};
  const bs::CharacterSetupResult d = bs::build_player_setup(
      none, content, cap.login, session_of(cap), world_stats::of(content));
  CHECK(
      d.setup &&
          std::tie(d.setup->x, d.setup->y, d.setup->z, d.setup->yaw_degrees) ==
              std::tie(bs::kDefaultSpawn.x, bs::kDefaultSpawn.y,
                       bs::kDefaultSpawn.z, bs::kDefaultSpawn.yaw_degrees),
      "no saved position: the default spawn, whatever the unset position "
      "holds");
  CHECK(d.setup && std::tie(d.setup->x, d.setup->y, d.setup->z,
                            d.setup->yaw_degrees) ==
                       std::tie(cap.x, cap.y, cap.z, cap.yaw_degrees),
        "...and the default spawn is the captured character's spawn point and "
        "yaw");
  CHECK(d.notes.size() == 1 && contains(d.notes[0], "default spawn"),
        "...with a note saying so");
}

static void test_capture_record(const bs::PlayerSetup &cap,
                                const content::ContentDb &content) {
  std::printf("the capture's record shape against the small content.db\n");
  check_capture_record(cap, content, "small content.db");

  const svc::Character rec = captured_record(cap);
  const bs::CharacterSetupResult r = bs::build_player_setup(
      rec, content, cap.login, session_of(cap), world_stats::of(content));
  if (!r.setup)
    return;
  CHECK(r.setup->character_guid[0] == 3635609113u &&
            r.setup->name == "RandomChar" && r.setup->race == 0 &&
            r.setup->gender == 0 && r.setup->level == 1 &&
            r.setup->class_record_guid == 108640158570105ll,
        "the Guid words, the name, Kaelar 0, Male 0, level 1 and the Bard's "
        "class Guid");
  CHECK(
      r.setup->x == -777664.5 && r.setup->y == 616578.6875 &&
          r.setup->z == 15952.0498046875 &&
          r.setup->yaw_degrees == 16550.0 * 360.0 / 65536.0,
      "the spawn is the record's position (the capture's spawn point and yaw)");
  check_capture_default_spawn(cap, content, rec);
}

static void check_cyb_setup_fields(const bs::PlayerSetup &s,
                                   const bs::PlayerSetup &cap, uint8_t race,
                                   uint8_t gender, const std::string &label) {
  CHECK(s.race == race && s.gender == gender && s.customization.race_gender &&
            s.customization.race_gender->race == race &&
            s.customization.race_gender->gender == gender,
        (label + ": race " + std::to_string(race) + " and gender " +
         std::to_string(gender) +
         " in the setup and in the customization's Race and Gender bytes")
            .c_str());
  const bool identity_ok = s.name == "cyb" && s.level == 1 &&
                           s.class_record_guid == content_fixture::kBard &&
                           s.login.name_option == "cyb" &&
                           s.player_id == cap.player_id &&
                           s.guids.pawn == cap.guids.pawn;
  const bool pos_ok =
      s.x == 100.5 && s.y == -200.25 && s.z == 300.0 && s.yaw_degrees == 45.0;
  CHECK(identity_ok && pos_ok,
        (label +
         ": name, level, class, the saved position, the login and the session")
            .c_str());
  CHECK(s.character_guid == std::array<uint32_t, 4>{0xbbec5c86u, 0xeb101fe5u,
                                                    0xf38c87f0u, 0xfa22e7d0u},
        (label + ": cyb's id bbec5c86eb101fe5f38c87f0fa22e7d0 as four words")
            .c_str());
}

static void check_cyb_bootstrap_join(const bs::PlayerSetup &s,
                                     const std::string &label) {
  CHECK(codec_round_trip(s.customization),
        (label + ": the customization encodes and decodes to the last bit")
            .c_str());
  bs::PlayerBootstrap pb(s, bs::GameStateMode::Tier2);
  const auto seq = pb.ok() ? pb.join_sequence() : std::nullopt;
  CHECK(pb.ok() && seq.has_value() && !seq->empty(),
        (label + ": a PlayerBootstrap accepts it and builds the join sequence")
            .c_str());
  if (!pb.ok())
    std::printf("    %s\n", pb.error().c_str());
  bool pawn_open = false;
  if (seq) {
    for (const bs::Send &send : *seq) {
      pawn_open =
          pawn_open || (send.role == bs::Role::Pawn && send.opens_channel &&
                        send.emission.payload.bit_pos() > 26000);
    }
  }
  CHECK(pawn_open,
        (label + ": the pawn's open is built, over 26,000 bits (the "
                 "customization alone is about 26,900 in the capture)")
            .c_str());
}

static void check_cyb_race_gender_case(const bs::PlayerSetup &cap,
                                       const content::ContentDb &content,
                                       int32_t race_id, int32_t gender_id) {
  const char *gender_names[] = {"Male", "Female"};
  const char *rname = race_name_of(race_id);
  const char *gname = gender_names[gender_id];
  const uint8_t race = static_cast<uint8_t>(race_id);
  const uint8_t gender = static_cast<uint8_t>(gender_id);
  const std::string label = std::string(rname) + " " + gname;
  svc::Character rec =
      cyb_record(race_id, gender_id, cyb_json_of(rname, gname));
  rec.has_position = true;
  rec.position = {100.5, -200.25, 300.0, 45.0f};
  bs::LoginIdentity login;
  login.name_option = "cyb";
  login.unique_id = "cyb";
  const bs::CharacterSetupResult r = bs::build_player_setup(
      rec, content, login, session_of(cap), world_stats::of(content));
  CHECK(r.setup.has_value(), (label + ": builds a setup").c_str());
  if (!r.setup) {
    std::printf("    %s\n", r.error.c_str());
    return;
  }
  check_cyb_setup_fields(*r.setup, cap, race, gender, label);
  check_cyb_bootstrap_join(*r.setup, label);
}

static void check_cyb_level2_and_tank(const bs::PlayerSetup &cap,
                                      const content::ContentDb &content) {
  svc::Character l2 = cyb_record(0, 0);
  l2.level = 2;
  const bs::CharacterSetupResult r2 = bs::build_player_setup(
      l2, content, {}, session_of(cap), world_stats::of(content));
  CHECK(r2.setup.has_value() && r2.setup->level == 2, "level 2 builds");
  CHECK(r2.notes.size() == 1 && contains(r2.notes[0], "default spawn"),
        "level 2: only the default spawn is noted (the stat program rounds, "
        "there is no ambiguity to note)");
  if (r2.setup) {
    const auto it = std::find_if(
        r2.setup->stats.owner.items.begin(), r2.setup->stats.owner.items.end(),
        [](const bs::StatItem &i) {
          return i.record == content_fixture::kStrength;
        });
    const float strength = it != r2.setup->stats.owner.items.end()
                               ? std::bit_cast<float>(it->value)
                               : 0;
    CHECK(strength == std::floor(strength) && strength >= 24,
          "...and the Strength item is a whole number of at least the level-1 "
          "Bard's 24");
  }
  svc::Character tank = cyb_record(0, 0);
  tank.class_id = 7;
  tank.archetype_id = 17754;
  tank.class_name = "Tank";
  const bs::CharacterSetupResult rt = bs::build_player_setup(
      tank, content, {}, session_of(cap), world_stats::of(content));
  CHECK(rt.setup && rt.setup->class_record_guid == content_fixture::kTank &&
            rt.setup->race == 0,
        "class 7 is the Tank's class Guid, and a Kaelar Tank is a Kaelar");
}

static void check_cyb_class_notes(const bs::PlayerSetup &cap,
                                  const content::ContentDb &content) {
  svc::Character bare = cyb_record(0, 0);
  bare.archetype_id = 0;
  bare.class_name = "";
  const bs::CharacterSetupResult rb = bs::build_player_setup(
      bare, content, {}, session_of(cap), world_stats::of(content));
  CHECK(rb.setup && rb.setup->class_record_guid == content_fixture::kBard &&
            rb.notes.size() == 1 && contains(rb.notes[0], "default spawn"),
        "the class is the class id: no archetype id and no class name needed, "
        "and nothing noted");
  svc::Character odd = cyb_record(0, 0);
  odd.class_name = "Mage";
  odd.archetype_id = 17750;
  const bs::CharacterSetupResult ro = bs::build_player_setup(
      odd, content, {}, session_of(cap), world_stats::of(content));
  CHECK(ro.setup && ro.setup->class_record_guid == content_fixture::kBard &&
            ro.notes.size() >= 2 && contains(ro.notes[0], "Mage") &&
            contains(ro.notes[1], "17750"),
        "a class name and an archetype id that disagree with the class: the "
        "class id decides, and both are noted");
  const bs::CharacterSetupResult rk = bs::build_player_setup(
      cyb_record(0, 0,
                 replaced(cyb::kAppearanceJson, "\"nailOpacity\":0",
                          "\"nailOpacity\":0,\"hairBand\":3")),
      content, {}, session_of(cap), world_stats::of(content));
  CHECK(rk.setup && !rk.notes.empty() && contains(rk.notes[0], "hairBand"),
        "a member of the JSON this server does not read is noted");
}

static void test_cyb_like(const bs::PlayerSetup &cap,
                          const content::ContentDb &content) {
  std::printf("cyb-like records of every race (0-7) and both genders\n");
  for (int32_t r = 0; r <= 7; ++r) {
    for (int32_t g = 0; g <= 1; ++g)
      check_cyb_race_gender_case(cap, content, r, g);
  }
  check_cyb_level2_and_tank(cap, content);
  check_cyb_class_notes(cap, content);
}

static void check_setup_refused(const bs::PlayerSetup &cap,
                                const content::ContentDb &content,
                                const svc::Character &c, const char *part,
                                const char *msg) {
  const bs::CharacterSetupResult r = bs::build_player_setup(
      c, content, {}, session_of(cap), world_stats::of(content));
  const bool ok = !r.setup && contains(r.error, part);
  CHECK(ok, msg);
  if (!ok)
    std::printf("    error: '%s'\n", r.error.c_str());
}

static void check_errors_identity(const bs::PlayerSetup &cap,
                                  const content::ContentDb &content,
                                  const svc::Character &base) {
  svc::Character c = base;
  c.id = "bbec5c86eb101fe5f38c87f0fa22e7d";
  check_setup_refused(cap, content, c, "32 hex digits", "a 31-digit id");
  c = base;
  c.id = "";
  check_setup_refused(cap, content, c, "32 hex digits", "an empty id");
  c = base;
  c.name = "";
  check_setup_refused(cap, content, c, "no name", "an empty name");
  c = base;
  c.name = std::string("a\0b", 3);
  check_setup_refused(cap, content, c, "name cannot be sent",
                      "a name with a NUL");
  c = base;
  c.name = "\xC3\x28";
  check_setup_refused(cap, content, c, "name cannot be sent",
                      "a name that is not UTF-8");
}

static void check_errors_race_and_gender(const bs::PlayerSetup &cap,
                                         const content::ContentDb &content,
                                         const svc::Character &base) {
  const auto build = [&](const svc::Character &c) {
    return bs::build_player_setup(c, content, {}, session_of(cap),
                                  world_stats::of(content));
  };
  svc::Character c = base;
  for (int32_t race : {8, 9, 10, -1, 100}) {
    c = base;
    c.race_id = race;
    const bs::CharacterSetupResult r = build(c);
    const bool ok =
        !r.setup &&
        contains(r.error, ("race id " + std::to_string(race)).c_str()) &&
        contains(r.error, "not a playable race") &&
        r.refusal == bs::SetupRefusal::UnsupportedRace;
    CHECK(ok,
          ("race id " + std::to_string(race) +
           " (Tulnar or past the enum) is an unsupported race naming the id")
              .c_str());
  }
  for (int32_t race = 0; race <= 7; ++race) {
    c = base;
    c.race_id = race;
    c.appearance_json = cyb_json_of(race_name_of(race), "Male");
    CHECK(build(c).setup.has_value(),
          ("race id " + std::to_string(race) + " is accepted").c_str());
  }
  c = base;
  c.gender_id = 2;
  check_setup_refused(cap, content, c, "gender id 2", "gender 2");
  c.gender_id = -1;
  check_setup_refused(cap, content, c, "gender id -1", "gender -1");
  c.gender_id = 3;
  check_setup_refused(cap, content, c, "gender id 3", "gender 3");
  c = base;
  c.gender_id = 0;
  CHECK(build(c).setup.has_value(), "gender 0 is Male");
}

static void check_errors_level_and_class(const bs::PlayerSetup &cap,
                                         const content::ContentDb &content,
                                         const svc::Character &base) {
  const auto build = [&](const svc::Character &c) {
    return bs::build_player_setup(c, content, {}, session_of(cap),
                                  world_stats::of(content));
  };
  svc::Character c = base;
  c.level = 0;
  check_setup_refused(cap, content, c, "level 0", "level 0");
  c.level = 51;
  check_setup_refused(cap, content, c, "level 51", "level 51");
  c.level = 3;
  CHECK(build(c).setup.has_value(),
        "level 3 builds: the stat engine computes every level of content.db's "
        "range, class_stat_base is not read");

  c = base;
  c.class_id = 0;
  check_setup_refused(
      cap, content, c, "class id 0",
      "a record with no class (the service could not derive it)");
  CHECK(contains(build(c).error, "could not derive"), "...which says why");
  c.class_id = 9;
  check_setup_refused(cap, content, c, "class id 9",
                      "a class id past the enum");
  c.class_id = -1;
  check_setup_refused(cap, content, c, "class id -1", "a negative class id");
  c.class_id = 8;
  CHECK(build(c).setup.has_value(),
        "the Mage builds: the small content.db has a row for every class");
  c.class_id = 3;
  CHECK(build(c).setup.has_value(),
        "the Cleric builds: its stats come from the stat program, not from "
        "class_stat_base");
  c = base;
  c.archetype_id = 0;
  CHECK(build(c).setup.has_value(),
        "a record with no archetype id is fine: the class id decides");
}

static void check_errors_appearance(const bs::PlayerSetup &cap,
                                    const content::ContentDb &content,
                                    const svc::Character &base) {
  svc::Character c = base;
  c.appearance_json = "";
  check_setup_refused(cap, content, c, "not valid JSON", "an empty appearance");
  c.appearance_json = "{\"presetGuid\":\"5\"}";
  check_setup_refused(cap, content, c, "presetGuid",
                      "an appearance naming a preset");
  c = cyb_record(0, 0, cyb_json_of("Dunir", "Male"));
  check_setup_refused(cap, content, c, "Dunir",
                      "a Dunir appearance on a Kaelar record");
  c = cyb_record(0, 1);
  check_setup_refused(cap, content, c, "Female",
                      "a Male appearance on a Female record");
}

static void check_errors_position_and_faults(const bs::PlayerSetup &cap,
                                             const content::ContentDb &content,
                                             const svc::Character &base) {
  const auto build = [&](const svc::Character &c) {
    return bs::build_player_setup(c, content, {}, session_of(cap),
                                  world_stats::of(content));
  };
  svc::Character c = base;
  c.has_position = true;
  c.position = {std::numeric_limits<double>::quiet_NaN(), 0, 0, 0};
  check_setup_refused(cap, content, c, "not finite",
                      "a saved position with NaN");
  c.position = {0, 0, 0, std::numeric_limits<float>::infinity()};
  check_setup_refused(cap, content, c, "not finite", "a saved yaw of infinity");
  c.position = {0, 0, 0, 0};
  CHECK(build(c).setup.has_value(),
        "a saved position at the origin is a position, not a missing one");

  c = base;
  svc::InventoryItem shirt;
  shirt.container = svc::ItemContainer::Equipment;
  shirt.slot = 19;
  shirt.item_record_id = content_fixture::item::kShirt;
  shirt.instance_guid = "0123456789abcdef0123456789abcdef";
  shirt.quantity = 1;
  shirt.quality = 1;
  shirt.durability = 11;
  shirt.created_at_ms = 1'771'299'474'000ll;
  c.items.push_back(shirt);
  const bs::CharacterSetupResult rf = build(c);
  CHECK(!rf.setup && contains(rf.error, "stat program faulted"),
        "a worn item that makes the stat program fault is refused at the join, "
        "naming the fault");
  c.items.clear();
  CHECK(build(c).setup.has_value(), "(the same character without it builds)");

  c = base;
  c.race_id = 8;
  const bs::CharacterSetupResult r = build(c);
  CHECK(!r.setup && !r.error.empty(), "a refusal has an error and no setup");
}

static void test_errors(const bs::PlayerSetup &cap,
                        const content::ContentDb &content) {
  std::printf("build_player_setup: what is refused\n");
  const svc::Character base = cyb_record(0, 0);
  CHECK(bs::build_player_setup(base, content, {}, session_of(cap),
                               world_stats::of(content))
            .setup.has_value(),
        "the base record builds");
  check_errors_identity(cap, content, base);
  check_errors_race_and_gender(cap, content, base);
  check_errors_level_and_class(cap, content, base);
  check_errors_appearance(cap, content, base);
  check_errors_position_and_faults(cap, content, base);
}

static std::string
canonical_slot_name(const std::map<int, std::string> &slot_names,
                    const std::string &s) {
  if (s.rfind("Slot", 0) != 0)
    return s;
  const int num = std::atoi(s.c_str() + 4);
  return slot_names.count(num) ? slot_names.at(num) : s;
}

static int resolve_corpus_slot(const std::map<std::string, int> &slot_of,
                               const std::string &s) {
  if (slot_of.count(s))
    return slot_of.at(s);
  if (s.rfind("Slot", 0) == 0)
    return std::atoi(s.c_str() + 4);
  return -1;
}

static bool
diff_creation_gear_visuals(const std::map<int, std::string> &slot_names,
                           const stats::GearInputs &g,
                           const stats_fixture::CorpusCase &c) {
  std::vector<std::pair<std::string, std::string>> a, b = c.tags;
  for (const auto &t : g.equipped)
    a.emplace_back(canonical_slot_name(slot_names, t.slot), t.tag);
  for (auto &t : b)
    t.first = canonical_slot_name(slot_names, t.first);
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return a != b;
}

static bool check_corpus_gear_case(const bs::StatContext &ctx,
                                   const std::map<std::string, int> &slot_of,
                                   const std::map<int, std::string> &slot_names,
                                   const stats_fixture::CorpusCase &c) {
  std::vector<stats::WornItem> worn;
  bool named = true;
  for (const stats_fixture::CorpusItem &it : c.items) {
    const int slot = resolve_corpus_slot(slot_of, it.slot);
    named = named && slot >= 0;
    worn.push_back({it.record, it.quality, slot});
  }
  const stats::GearInputs g = stats::gear_inputs(
      *ctx.gear, worn,
      c.half_up ? stats::Rounding::HalfUp : stats::Rounding::None);
  std::string why = named ? std::string() : "a slot with no number";
  if (why.empty())
    why = mods_difference(g.mods, c);
  if (why.empty() && diff_creation_gear_visuals(slot_names, g, c))
    why = "the tags differ";
  if (why.empty() && !matches_corpus_digest(ctx, c, g, why))
    why = "digest: " + why;
  if (!why.empty())
    std::printf("    %s: %s\n", c.name.c_str(), why.c_str());
  return why.empty();
}

static void test_corpus_cases(const content::ContentDb &db) {
  std::printf("the corpus's gear cases against the real tables (Go's mods, "
              "tags and digest of every stat)\n");
  const bs::StatContext &ctx = world_stats::of(db);
  std::string error;
  const auto slot_names = db.equip_slot_names(error);
  CHECK(slot_names.has_value() && slot_names->size() == 83,
        "the 83 EEquipSlot names are read");
  if (!slot_names)
    return;
  std::map<std::string, int> slot_of;
  for (const auto &[slot, name] : *slot_names)
    slot_of[name] = slot;
  size_t cases = 0, good = 0;
  for (const stats_fixture::CorpusCase &c : stats_fixture::corpus().cases) {
    if (!c.has_gear)
      continue;
    ++cases;
    good += check_corpus_gear_case(ctx, slot_of, *slot_names, c);
  }
  CHECK(cases == 18 && good == cases,
        "the captured Bard, the retail Ranger and the sixteen kit cases: mods, "
        "tags and every stat equal Go's (bit for bit)");
}

static bool check_real_content_race_gender(const content::ContentDb &db,
                                           const bs::PlayerSetup &cap,
                                           int32_t race, int32_t gender) {
  bool every = true;
  for (int32_t class_id = 1; class_id <= 8; ++class_id) {
    svc::Character c =
        cyb_record(race, gender,
                   cyb_json_of(race_name_of(race), gender ? "Female" : "Male"));
    c.class_id = class_id;
    c.class_name = bs::client_class_name(class_id);
    c.archetype_id = 0;
    const bs::CharacterSetupResult rr =
        bs::build_player_setup(c, db, {}, session_of(cap), world_stats::of(db));
    const bool ok =
        rr.setup && rr.setup->race == race && rr.setup->gender == gender;
    if (!ok)
      std::printf("    race %d gender %d class %d: %s\n", race, gender,
                  class_id, rr.error.c_str());
    every = every && ok;
  }
  return every;
}

static void check_real_content_matrix(const content::ContentDb &db,
                                      const bs::PlayerSetup &cap) {
  bool all = true;
  for (int32_t class_id = 1; class_id <= 8; ++class_id) {
    for (int level : {1, 20, 50}) {
      svc::Character c = cyb_record(0, 0);
      c.class_id = class_id;
      c.archetype_id = 0;
      c.class_name = "";
      c.level = level;
      all = all && bs::build_player_setup(c, db, {}, session_of(cap),
                                          world_stats::of(db))
                       .setup.has_value();
    }
  }
  CHECK(all,
        "all 8 classes (ECharacterClass 1-8) build at levels 1, 20 and 50");
  bool every = true;
  for (int32_t race = 0; race <= 7; ++race) {
    for (int32_t gender = 0; gender <= 1; ++gender) {
      every = every && check_real_content_race_gender(db, cap, race, gender);
    }
  }
  CHECK(every, "all 8 races x 2 genders x 8 classes build a setup with that "
               "race and gender");
  svc::Character l20 = cyb_record(0, 0);
  l20.level = 20;
  const bs::CharacterSetupResult r20 =
      bs::build_player_setup(l20, db, {}, session_of(cap), world_stats::of(db));
  CHECK(r20.setup.has_value(), "the Bard at level 20 builds");
}

static void test_real_content(const bs::PlayerSetup &cap) {
  std::printf("against the real content.db (AOC_CONTENT_DB)\n");
  const char *env = std::getenv("AOC_CONTENT_DB");
  if (!env || !*env) {
    std::printf("  [skip] AOC_CONTENT_DB is not set (cd services && go run "
                "./cmd/contentc; AOC_CONTENT_DB=dist\\content\\content.db)\n");
    return;
  }
  std::string error;
  auto db = content::ContentDb::open(env, error);
  CHECK(db.has_value(), "the real content.db opens");
  if (!db) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  check_capture_record(cap, *db, "real content.db");
  test_setup_stats(cap, *db, "the real content.db");
  run_kits_against_go(*db);
  test_corpus_cases(*db);

  const bs::CharacterSetupResult r = bs::build_player_setup(
      cyb_record(0, 0), *db, cap.login, session_of(cap), world_stats::of(*db));
  CHECK(r.setup.has_value(), "cyb builds against the real content.db");
  if (!r.setup) {
    std::printf("    %s\n", r.error.c_str());
    return;
  }
  std::map<int64_t, uint32_t> v;
  for (const auto &it : r.setup->stats.owner.items)
    v[it.record] = it.value;
  const auto f = [&](int64_t guid) { return std::bit_cast<float>(v[guid]); };
  CHECK(f(109183598614138ll) == 24.0f && f(109183598614139ll) == 24.0f &&
            f(109183598614140ll) == 30.0f && f(109183598614143ll) == 30.0f &&
            f(109183598614142ll) == 36.0f && f(109183598614141ll) == 39.0f,
        "cyb's Owner stats: Strength 24, Dexterity 24, Constitution 30, "
        "Mentality 30, Wisdom 36, Intelligence 39 (the capture has 60)");
  CHECK(r.setup->class_record_guid == 108640158570105ll,
        "cyb's class is the Bard's record Guid");
  check_real_content_matrix(*db, cap);
}

void run_character_setup_real_content(const bs::PlayerSetup &cap) {
  test_real_content(cap);
}

void run_character_setup_stats(const bs::PlayerSetup &cap,
                               const content::ContentDb &db) {
  test_capture_record(cap, db);
  test_setup_stats(cap, db, "the small content.db");
  test_cyb_like(cap, db);
  test_errors(cap, db);
}
