// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "content/content_db_internal.h"

#include <cctype>
#include <cstdlib>

namespace aoc::worldd::content {
namespace {

constexpr const char *kItemNameSql =
    "SELECT display_name FROM item WHERE guid = ?1";
constexpr const char *kVendorByGuidSql =
    "SELECT guid, name FROM vendor_def WHERE guid = ?1";
constexpr const char *kVendorByNameSql =
    "SELECT guid, name FROM vendor_def WHERE name = ?1";
constexpr const char *kVendorNamesWithPrefixSql =
    "SELECT name FROM vendor_def WHERE substr(name, 1, length(?1)) = ?1 ORDER "
    "BY name";
constexpr const char *kVendorInventoriesSql =
    "SELECT d.inventory_guid, i.name FROM vendor_def_inventory d LEFT JOIN "
    "vendor_inventory i ON i.guid = d.inventory_guid WHERE d.vendor_guid = ?1 "
    "ORDER BY d.ord";
constexpr const char *kVendorListingSql =
    "SELECT item_guid, offers, hidden FROM vendor_listing WHERE inventory_guid "
    "= ?1 ORDER BY ord";
constexpr const char *kGmItemSql =
    "SELECT guid, name, max_stack_size FROM item WHERE name LIKE 'GM%' OR name "
    "LIKE '%PTR%' ORDER BY guid";
constexpr const char *kMountSql =
    "SELECT item_guid, item_name, kind, blueprint_path, blueprint, activation, "
    "cast_ability_guid, level, ground_pct, flying_pct, water_pct, health_pct, "
    "mule, crate_capacity, tradeable, description FROM mount ORDER BY "
    "item_name";
constexpr const char *kMountStatSql =
    "SELECT level, stat_guid, stat_name, value FROM mount_stat ORDER BY level, "
    "stat_guid";
constexpr const char *kMountAbilitySql =
    "SELECT ability_guid, name, role, usable_while_mounted, auto_dismount FROM "
    "mount_ability ORDER BY name";
constexpr const char *kMountEffectSql =
    "SELECT effect_guid, name, kind, tier, stat_guid, value, relative, "
    "infinite, duration, force_dismount FROM mount_effect ORDER BY name";

void skip_spaces(const std::string &text, size_t &i) {
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
    ++i;
}

bool parse_int_token(const std::string &text, size_t &i, int64_t &value) {
  const size_t start = i;
  if (i < text.size() && (text[i] == '-' || text[i] == '+'))
    ++i;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9')
    ++i;
  if (i == start)
    return false;
  value = std::strtoll(text.substr(start, i - start).c_str(), nullptr, 10);
  return true;
}

bool parse_int_array_elements(const std::string &text, size_t i,
                              std::vector<int64_t> &out) {
  while (i < text.size()) {
    skip_spaces(text, i);
    int64_t value = 0;
    if (!parse_int_token(text, i, value))
      return false;
    out.push_back(value);
    skip_spaces(text, i);
    if (i < text.size() && text[i] == ',') {
      ++i;
      continue;
    }
    return i < text.size() && text[i] == ']' && i + 1 == text.size();
  }
  return false;
}

bool parse_int_array(const std::string &text, std::vector<int64_t> &out) {
  size_t i = 0;
  skip_spaces(text, i);
  if (i >= text.size() || text[i] != '[')
    return false;
  ++i;
  skip_spaces(text, i);
  if (i < text.size() && text[i] == ']')
    return i + 1 == text.size();
  return parse_int_array_elements(text, i, out);
}

struct VendorLookup {
  const char *sql;
  const std::string *name;
  int64_t guid;
};

bool vendor_failed(sqlite3 *db, const char *what, std::string &error) {
  return fail_msg(std::string(what) + " query failed: " + db_message(db) +
                      " (the vendor tables are contentc version 8's: rebuild "
                      "the file with this checkout's contentc)",
                  error);
}

int read_vendor_header(sqlite3 *db, VendorLookup lookup, VendorDefRow &out,
                       std::string &error) {
  Stmt q(db, lookup.sql);
  if (!q.ok()) {
    vendor_failed(db, "vendor_def", error);
    return -1;
  }
  if (lookup.name)
    sqlite3_bind_text(q.get(), 1, lookup.name->c_str(),
                      static_cast<int>(lookup.name->size()), SQLITE_TRANSIENT);
  else
    sqlite3_bind_int64(q.get(), 1, lookup.guid);
  const int step = q.step();
  if (step == SQLITE_DONE)
    return 0;
  if (step != SQLITE_ROW) {
    vendor_failed(db, "vendor_def", error);
    return -1;
  }
  if (!check_cols(q.get(), "vendor_def", "i", error))
    return -1;
  out.guid = col_i64(q.get(), 0);
  out.name = column_text(q.get(), 1);
  return 1;
}

bool read_vendor_inventories(sqlite3 *db, VendorDefRow &out,
                             std::string &error) {
  Stmt q(db, kVendorInventoriesSql, out.guid);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!check_cols(q.get(), "vendor_def_inventory", "i", error))
      return false;
    out.inventories.push_back({col_i64(q.get(), 0), column_text(q.get(), 1)});
  }
  return step == SQLITE_DONE ? true
                             : vendor_failed(db, "vendor_def_inventory", error);
}

bool append_vendor_listing_row(sqlite3_stmt *s, VendorInventoryRow &inv,
                               std::string &error) {
  if (!check_cols(s, "vendor_listing", "iti", error))
    return false;
  VendorListing l{col_i64(s, 0), {}, col_i64(s, 2) != 0};
  if (!parse_int_array(column_text(s, 1), l.offers))
    return fail_msg(
        "vendor_listing has offers that are no JSON array of integers", error);
  inv.listings.push_back(std::move(l));
  return true;
}

bool read_vendor_listings(sqlite3 *db, VendorDefRow &out, std::string &error) {
  for (VendorInventoryRow &inv : out.inventories) {
    Stmt q(db, kVendorListingSql, inv.guid);
    int step = SQLITE_ERROR;
    while ((step = q.step()) == SQLITE_ROW) {
      if (!append_vendor_listing_row(q.get(), inv, error))
        return false;
    }
    if (step != SQLITE_DONE)
      return vendor_failed(db, "vendor_listing", error);
  }
  return true;
}

std::optional<VendorDefRow> read_vendor(sqlite3 *db, VendorLookup lookup,
                                        std::string &error) {
  VendorDefRow out;
  if (read_vendor_header(db, lookup, out, error) <= 0 ||
      !read_vendor_inventories(db, out, error) ||
      !read_vendor_listings(db, out, error))
    return std::nullopt;
  return out;
}
std::vector<int64_t> int_list(const nlohmann::json &j, const char *key) {
  std::vector<int64_t> out;
  if (const auto it = j.find(key); it != j.end() && it->is_array()) {
    for (const auto &v : *it)
      out.push_back(v.is_number() ? v.get<int64_t>() : 0);
  }
  return out;
}
std::vector<double> number_list(const nlohmann::json &j, const char *key) {
  std::vector<double> out;
  if (const auto it = j.find(key); it != j.end() && it->is_array()) {
    for (const auto &v : *it)
      out.push_back(v.is_number() ? v.get<double>() : 0.0);
  }
  return out;
}
int64_t int_of(const nlohmann::json &j, const char *key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_number() ? it->get<int64_t>() : 0;
}
std::string str_of(const nlohmann::json &j, const char *key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>()
                                          : std::string();
}
void append_loot_reward_items(const nlohmann::json &rj, LootReward &r) {
  if (const auto is = rj.find("items"); is != rj.end() && is->is_array()) {
    for (const auto &ij : *is)
      r.items.push_back(
          {int_of(ij, "item"), int_of(ij, "variation"), int_of(ij, "qty")});
  }
}

LootReward parse_loot_reward(const nlohmann::json &rj) {
  LootReward r;
  if (const auto xs = rj.find("xp"); xs != rj.end() && xs->is_array()) {
    for (const auto &xj : *xs)
      r.xp.push_back({str_of(xj, "type"), int_of(xj, "expr")});
  }
  append_loot_reward_items(rj, r);
  if (const auto cu = rj.find("currency"); cu != rj.end() && cu->is_array()) {
    for (const auto &uj : *cu)
      r.currency.push_back({int_of(uj, "currency"), int_of(uj, "amount")});
  }
  return r;
}
LootContainer parse_loot_container(const nlohmann::json &cj) {
  LootContainer c{str_of(cj, "grant"),  str_of(cj, "algo"),
                  int_of(cj, "pred"),   int_of(cj, "n"),
                  number_list(cj, "w"), int_list(cj, "pct"),
                  int_list(cj, "xw")};
  if (const auto rs = cj.find("r"); rs != cj.end() && rs->is_array()) {
    for (const auto &rj : *rs)
      c.rewards.push_back(parse_loot_reward(rj));
  }
  return c;
}
bool parse_loot_table_row(int64_t guid, sqlite3_stmt *s, LootTable &t,
                          std::string &error) {
  const std::string name = column_text(s, 0);
  const nlohmann::json j =
      nlohmann::json::parse(column_text(s, 1), nullptr, false);
  if (!j.is_object())
    return fail_msg("loot_table " + name + " (" + std::to_string(guid) +
                        ") holds no JSON object",
                    error);
  t = {guid,
       name,
       int_of(j, "pred"),
       str_of(j, "algo"),
       int_of(j, "n"),
       int_list(j, "subs"),
       number_list(j, "w"),
       int_list(j, "pct"),
       int_list(j, "xw")};
  if (const auto cs = j.find("c"); cs != j.end() && cs->is_array()) {
    for (const auto &cj : *cs)
      t.containers.push_back(parse_loot_container(cj));
  }
  return true;
}
std::string shown_mount_name(const std::string &name) {
  static const std::string kRetired = "RETIRED_";
  if (name.size() <= kRetired.size() ||
      name.compare(0, kRetired.size(), kRetired) != 0)
    return name;
  std::string shown;
  for (const char c : name.substr(kRetired.size())) {
    if (!shown.empty() && std::isupper(static_cast<unsigned char>(c)) &&
        std::islower(static_cast<unsigned char>(shown.back())))
      shown += ' ';
    shown += c;
  }
  return shown;
}
bool mount_failed(sqlite3 *db, const char *what, bool with_rebuild,
                  std::string &error) {
  const std::string rebuild =
      with_rebuild ? " (the mount tables are contentc version 11's: rebuild "
                     "the file with this checkout's contentc)"
                   : "";
  return fail_msg(
      std::string(what) + " query failed: " + db_message(db) + rebuild, error);
}
bool read_mount_rows(sqlite3 *db, MountData &out, std::string &error) {
  Stmt q(db, kMountSql);
  if (!q.ok())
    return mount_failed(db, "mount", true, error);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "mount", "i.....iiiiiiiii", error))
      return false;
    out.mounts.push_back({col_i64(s, 0), column_text(s, 1), column_text(s, 2),
                          column_text(s, 3), column_text(s, 4),
                          column_text(s, 5), col_i64(s, 6), col_int(s, 7),
                          col_int(s, 8), col_int(s, 9), col_int(s, 10),
                          col_int(s, 11), col_i64(s, 12) != 0, col_int(s, 13),
                          col_i64(s, 14) != 0, column_text(s, 15)});
  }
  return step == SQLITE_DONE ? true : mount_failed(db, "mount", false, error);
}
void read_mount_display_names(sqlite3 *db, MountData &out) {
  Stmt q(db, kItemNameSql);
  if (!q.ok())
    return;
  for (MountRow &r : out.mounts) {
    sqlite3_reset(q.get());
    sqlite3_bind_int64(q.get(), 1, r.item);
    if (q.step() == SQLITE_ROW)
      r.display_name = shown_mount_name(column_text(q.get(), 0));
  }
}
bool read_mount_stats(sqlite3 *db, MountData &out, std::string &error) {
  Stmt q(db, kMountStatSql);
  if (!q.ok())
    return mount_failed(db, "mount_stat", true, error);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "mount_stat", "ii", error))
      return false;
    out.stats.push_back(
        {col_int(s, 0), col_i64(s, 1), column_text(s, 2), col_real(s, 3)});
  }
  return step == SQLITE_DONE || mount_failed(db, "mount_stat", false, error);
}
bool read_mount_abilities(sqlite3 *db, MountData &out, std::string &error) {
  Stmt q(db, kMountAbilitySql);
  if (!q.ok())
    return mount_failed(db, "mount_ability", true, error);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "mount_ability", "i..ii", error))
      return false;
    out.abilities.push_back({col_i64(s, 0), column_text(s, 1),
                             column_text(s, 2), col_i64(s, 3) != 0,
                             col_i64(s, 4) != 0});
  }
  return step == SQLITE_DONE || mount_failed(db, "mount_ability", false, error);
}
bool read_mount_effects(sqlite3 *db, MountData &out, std::string &error) {
  Stmt q(db, kMountEffectSql);
  if (!q.ok())
    return mount_failed(db, "mount_effect", true, error);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "mount_effect", "i..ii.ii.i", error))
      return false;
    out.effects.push_back({col_i64(s, 0), column_text(s, 1), column_text(s, 2),
                           col_int(s, 3), col_i64(s, 4), col_real(s, 5),
                           col_i64(s, 6) != 0, col_i64(s, 7) != 0,
                           col_real(s, 8), col_i64(s, 9) != 0});
  }
  return step == SQLITE_DONE || mount_failed(db, "mount_effect", false, error);
}

bool load_loot_table_node(sqlite3 *db, sqlite3_stmt *s, int64_t guid,
                          std::optional<LootTable> &table_out,
                          std::string &error) {
  sqlite3_reset(s);
  sqlite3_bind_int64(s, 1, guid);
  const int step = sqlite3_step(s);
  if (step == SQLITE_DONE)
    return true;
  if (step != SQLITE_ROW)
    return fail_db(db, "loot_table query failed", error);
  LootTable t;
  if (!parse_loot_table_row(guid, s, t, error))
    return false;
  table_out = std::move(t);
  return true;
}
} // namespace

std::optional<VendorDefRow> ContentDb::vendor_def(int64_t guid,
                                                  std::string &error) const {
  return read_vendor(db_, {kVendorByGuidSql, nullptr, guid}, error);
}
std::optional<std::map<int64_t, LootTable>>
ContentDb::loot_tables(const std::vector<int64_t> &roots,
                       std::string &error) const {
  Stmt q(db_, "SELECT name, data FROM loot_table WHERE guid = ?1");
  if (!q.ok()) {
    fail_msg("loot_table query failed: " + db_message(db_) +
                 " (the reward tables are contentc version 10's: rebuild the "
                 "file with this checkout's contentc)",
             error);
    return std::nullopt;
  }
  std::map<int64_t, LootTable> out;
  std::vector<int64_t> todo(roots.begin(), roots.end());
  while (!todo.empty()) {
    const int64_t guid = todo.back();
    todo.pop_back();
    if (guid == 0 || out.count(guid))
      continue;
    std::optional<LootTable> t;
    if (!load_loot_table_node(db_, q.get(), guid, t, error))
      return std::nullopt;
    if (!t)
      continue;
    todo.insert(todo.end(), t->subs.begin(), t->subs.end());
    out.emplace(guid, std::move(*t));
  }
  return out;
}
std::optional<int64_t> ContentDb::xp_rule(const std::string &key,
                                          std::string &error) const {
  Stmt q(db_, "SELECT expr FROM xp_rule WHERE key = ?1");
  if (!q.ok()) {
    fail_msg("xp_rule query failed: " + db_message(db_) +
                 " (the reward tables are contentc version 10's: rebuild the "
                 "file with this checkout's contentc)",
             error);
    return std::nullopt;
  }
  sqlite3_bind_text(q.get(), 1, key.c_str(), static_cast<int>(key.size()),
                    SQLITE_TRANSIENT);
  const int step = q.step();
  if (step == SQLITE_ROW)
    return col_i64(q.get(), 0);
  if (step == SQLITE_DONE)
    return int64_t{0};
  fail_db(db_, "xp_rule query failed", error);
  return std::nullopt;
}
std::optional<VendorDefRow>
ContentDb::vendor_def_named(const std::string &name, std::string &error) const {
  return read_vendor(db_, {kVendorByNameSql, &name, 0}, error);
}
std::optional<std::vector<VendorDefRow>>
ContentDb::vendor_defs_with_prefix(const std::string &prefix,
                                   std::string &error) const {
  std::vector<std::string> names;
  {
    Stmt q(db_, kVendorNamesWithPrefixSql);
    if (!q.ok()) {
      vendor_failed(db_, "vendor_def", error);
      return std::nullopt;
    }
    sqlite3_bind_text(q.get(), 1, prefix.c_str(),
                      static_cast<int>(prefix.size()), SQLITE_TRANSIENT);
    int step = SQLITE_ERROR;
    while ((step = q.step()) == SQLITE_ROW)
      names.push_back(column_text(q.get(), 0));
    if (!check_done(db_, step, "vendor_def query failed", error))
      return std::nullopt;
  }
  std::vector<VendorDefRow> out;
  for (const std::string &n : names) {
    auto row = read_vendor(db_, {kVendorByNameSql, &n, 0}, error);
    if (!row && !error.empty())
      return std::nullopt;
    if (row)
      out.push_back(std::move(*row));
  }
  return out;
}
std::optional<std::vector<GmItemRow>>
ContentDb::gm_items(std::string &error) const {
  Stmt q(db_, kGmItemSql);
  std::vector<GmItemRow> out;
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!check_cols(q.get(), "item", "i.i", error))
      return std::nullopt;
    out.push_back(
        {col_i64(q.get(), 0), column_text(q.get(), 1), col_int(q.get(), 2)});
  }
  if (!check_done(db_, step, "item query failed", error))
    return std::nullopt;
  return out;
}
const MountRow *MountData::find(int64_t item) const {
  for (const MountRow &m : mounts)
    if (m.item == item)
      return &m;
  return nullptr;
}
double MountData::stat_at(int level, int64_t stat, double fallback) const {
  for (const MountStatRow &r : stats)
    if (r.level == level && r.stat == stat)
      return r.value;
  return fallback;
}
const MountAbilityRow *MountData::ability(int64_t guid) const {
  for (const MountAbilityRow &a : abilities)
    if (a.ability == guid)
      return &a;
  return nullptr;
}
const MountAbilityRow *
MountData::ability_with_role(const std::string &role) const {
  for (const MountAbilityRow &a : abilities)
    if (a.role == role)
      return &a;
  return nullptr;
}
const MountEffectRow *MountData::road(int tier) const {
  for (const MountEffectRow &e : effects)
    if (e.kind == "road" && e.tier == tier)
      return &e;
  return nullptr;
}
const MountEffectRow *MountData::dash() const {
  for (const MountEffectRow &e : effects)
    if (e.kind == "dash")
      return &e;
  return nullptr;
}
std::optional<MountData> ContentDb::mount_data(std::string &error) const {
  MountData out;
  if (!read_mount_rows(db_, out, error))
    return std::nullopt;
  read_mount_display_names(db_, out);
  if (!read_mount_stats(db_, out, error) ||
      !read_mount_abilities(db_, out, error) ||
      !read_mount_effects(db_, out, error))
    return std::nullopt;
  return out;
}
} // namespace aoc::worldd::content
