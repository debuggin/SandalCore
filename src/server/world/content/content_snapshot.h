// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "bootstrap/character_setup.h"
#include "items/currency.h"
#include "session/ability_catalog.h"
#include "session/riding.h"

#include <atomic>
#include <chrono>
#include <sqlite3.h>

namespace aoc::worldd::content {

inline constexpr double kMaxHotReloadSwapMs = 50.0;

inline bool is_valid_sha256_hex(std::string_view digest) noexcept {
    if (digest.size() != 64U) return false;
    for (const char c : digest) {
        const bool is_digit = (c >= '0' && c <= '9');
        const bool is_lower = (c >= 'a' && c <= 'f');
        const bool is_upper = (c >= 'A' && c <= 'F');
        if (!is_digit && !is_lower && !is_upper) return false;
    }
    return true;
}

struct ContentSnapshot {
    uint64_t    version = 1;
    std::string sha256_digest;
    int64_t     loaded_at_unix_ms = 0;
    bool        wal_mode_enabled = false;

    std::optional<AbilityRules>              rules;
    std::optional<items::ContentItemCatalog> items;
    std::optional<stats::StatBundle>         stat_bundle;
    std::optional<ContentGearCatalog>        gear;
    std::optional<LevelingType>              leveling_type;
    std::optional<leveling::LevelCurve>      curve;
    std::vector<AutoGrant>                   grants;
    std::optional<talents::Catalog>          talents;
    std::optional<professions::Book>         professions;
    std::optional<weaponxp::Lines>           weapon_lines;
    std::optional<items::CurrencyTable>      currencies;
    std::optional<abilities::Catalog>        ability_catalog;
    std::optional<spawns::MountBook>         mounts;
    std::optional<CraftStationData>          stations;
    std::optional<CraftRecipeData>           recipes;

    std::map<int64_t, int>                    quality_cache;
    std::map<int64_t, items::ItemRarityFacts> rarity_cache;
    std::map<int64_t, std::string>            name_cache;

    size_t loaded_catalog_count() const noexcept {
        return static_cast<size_t>(rules.has_value()) + static_cast<size_t>(items.has_value()) +
               static_cast<size_t>(stat_bundle.has_value()) + static_cast<size_t>(gear.has_value()) +
               static_cast<size_t>(leveling_type.has_value()) + static_cast<size_t>(curve.has_value()) +
               static_cast<size_t>(!grants.empty()) + static_cast<size_t>(talents.has_value()) +
               static_cast<size_t>(professions.has_value()) + static_cast<size_t>(weapon_lines.has_value()) +
               static_cast<size_t>(currencies.has_value()) + static_cast<size_t>(ability_catalog.has_value()) +
               static_cast<size_t>(mounts.has_value()) + static_cast<size_t>(stations.has_value()) +
               static_cast<size_t>(recipes.has_value());
    }

    int item_quality(int64_t item_guid) const noexcept {
        const auto it = quality_cache.find(item_guid);
        return it != quality_cache.end() ? it->second : 1;
    }

    std::string item_name(int64_t item_guid) const {
        const auto it = name_cache.find(item_guid);
        return it != name_cache.end() ? it->second : std::string{};
    }

    static std::shared_ptr<const ContentSnapshot> load_from_db(const ContentDb& db, uint64_t version,
                                                               std::string sha256_digest, std::string& out_error);
};

namespace detail {

inline bool load_core_catalogs(const ContentDb& db, ContentSnapshot& snap, std::string& out_error) {
    snap.rules = db.ability_rules(out_error);
    if (!snap.rules) return false;
    snap.items.emplace(db);
    snap.gear.emplace(db);
    if (!snap.gear->ok()) {
        out_error = snap.gear->error();
        return false;
    }
    std::string why;
    if (auto row = db.stat_program(why)) {
        stats::StatBundle bundle;
        if (!stats::parse_bundle(row->data, bundle, out_error)) return false;
        snap.stat_bundle = std::move(bundle);
    } else if (!why.empty()) {
        out_error = why;
        return false;
    }
    return true;
}

inline bool load_progression_catalogs(const ContentDb& db, ContentSnapshot& snap, std::string& out_error) {
    std::string why;
    if (auto lt = db.leveling_type_named("Adventuring", why)) {
        auto crv = leveling::LevelCurve::from(*lt, out_error);
        if (!crv) return false;
        auto gr = db.auto_grants(lt->guid, out_error);
        if (!gr) return false;
        for (AutoGrant& g : *gr) {
            if (g.tree_guid == kGuidUniversalTree) snap.grants.push_back(std::move(g));
        }
        snap.leveling_type = std::move(*lt);
        snap.curve = std::move(*crv);
    }
    why.clear();
    if (auto td = db.talent_data(why)) {
        if (auto cat = talents::Catalog::from(std::move(*td), why)) snap.talents = std::move(*cat);
    }
    why.clear();
    if (auto pd = db.profession_data(why)) {
        if (auto book = professions::Book::from(*pd, why)) snap.professions = std::move(*book);
    }
    return true;
}

inline void load_optional_catalogs(const ContentDb& db, ContentSnapshot& snap) {
    std::string why;
    if (auto wl = weaponxp::Lines::from(db, why)) snap.weapon_lines = std::move(*wl);
    why.clear();
    if (auto cd = db.currency_data(why)) {
        if (auto tbl = items::CurrencyTable::from(*cd, why)) snap.currencies = std::move(*tbl);
    }
    if (snap.stat_bundle && !db.path().empty()) {
        why.clear();
        if (auto ac = abilities::Catalog::load(db.path(), *snap.stat_bundle, why)) snap.ability_catalog = std::move(*ac);
    }
    why.clear();
    if (auto mb = spawns::MountBook::load(db, why)) snap.mounts = std::move(*mb);
    why.clear();
    if (auto st = db.craft_station_data(why)) snap.stations = std::move(*st);
    why.clear();
    if (auto rc = db.craft_recipe_data(why)) snap.recipes = std::move(*rc);
}

inline void populate_item_caches(const ContentDb& db, ContentSnapshot& snap) {
    if (db.path().empty()) return;
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(db.path().c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        return;
    }
    sqlite3_stmt* pragma_stmt = nullptr;
    if (sqlite3_prepare_v2(raw, "PRAGMA journal_mode;", -1, &pragma_stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(pragma_stmt) == SQLITE_ROW) {
            const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(pragma_stmt, 0));
            if (txt) {
                std::string mode(txt);
                snap.wal_mode_enabled = (mode == "wal" || mode == "WAL");
            }
        }
        sqlite3_finalize(pragma_stmt);
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(raw, "SELECT guid FROM item ORDER BY guid;", -1, &stmt, nullptr) == SQLITE_OK) {
        std::string err;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int64_t guid = sqlite3_column_int64(stmt, 0);
            if (auto rec = db.item(guid, err)) {
                snap.quality_cache[guid] = std::max(1, rec->default_quality);
                snap.rarity_cache[guid] = *rec;
                snap.name_cache[guid] = rec->name;
            }
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(raw);
}

inline std::shared_ptr<const ContentSnapshot> atomic_load_ptr(const std::shared_ptr<const ContentSnapshot>* slot) noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    auto res = std::atomic_load_explicit(slot, std::memory_order_acquire);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return res;
}

inline std::shared_ptr<const ContentSnapshot> atomic_exchange_ptr(std::shared_ptr<const ContentSnapshot>* slot,
                                                                  std::shared_ptr<const ContentSnapshot> next) noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    auto prev = std::atomic_exchange_explicit(slot, std::move(next), std::memory_order_acq_rel);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return prev;
}

}  // namespace detail

inline std::shared_ptr<const ContentSnapshot> ContentSnapshot::load_from_db(const ContentDb& db, uint64_t version,
                                                                            std::string sha256_digest,
                                                                            std::string& out_error) {
    if (version == 0U) {
        out_error = "content snapshot version must be > 0";
        return nullptr;
    }
    if (!is_valid_sha256_hex(sha256_digest)) {
        out_error = "content snapshot sha256_digest must be 64 hex characters";
        return nullptr;
    }
    auto snap = std::make_shared<ContentSnapshot>();
    snap->version = version;
    snap->sha256_digest = std::move(sha256_digest);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    snap->loaded_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    if (!detail::load_core_catalogs(db, *snap, out_error)) return nullptr;
    if (!detail::load_progression_catalogs(db, *snap, out_error)) return nullptr;
    detail::load_optional_catalogs(db, *snap);
    detail::populate_item_caches(db, *snap);
    return snap;
}

class ContentSnapshotRegistry {
public:
    ContentSnapshotRegistry() = default;

    std::shared_ptr<const ContentSnapshot> current() const noexcept { return detail::atomic_load_ptr(&active_); }

    bool publish(std::shared_ptr<const ContentSnapshot> candidate, std::string& out_error,
                 double* out_swap_ms = nullptr) {
        if (!candidate) {
            out_error = "candidate snapshot is null";
            return false;
        }
        if (candidate->version == 0U || !is_valid_sha256_hex(candidate->sha256_digest)) {
            out_error = "candidate snapshot has invalid version or sha256_digest";
            return false;
        }
        std::lock_guard<std::mutex> guard(write_mutex_);
        const auto active = detail::atomic_load_ptr(&active_);
        if (active && candidate->version <= active->version) {
            out_error = "version regression: candidate version " + std::to_string(candidate->version) +
                        " must exceed active version " + std::to_string(active->version);
            return false;
        }
        const auto t0 = std::chrono::steady_clock::now();
        detail::atomic_exchange_ptr(&active_, std::move(candidate));
        const auto t1 = std::chrono::steady_clock::now();
        const double swap_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (out_swap_ms) *out_swap_ms = swap_ms;
        if (swap_ms >= kMaxHotReloadSwapMs) {
            out_error = "snapshot RCU swap exceeded 50ms budget";
            return false;
        }
        return true;
    }

    bool reload_from_db(const ContentDb& db, uint64_t version, std::string sha256_digest, std::string& out_error,
                        double* out_swap_ms = nullptr) {
        const auto active = current();
        if (active && version <= active->version) {
            out_error = "version regression: candidate version " + std::to_string(version) +
                        " must exceed active version " + std::to_string(active->version);
            return false;
        }
        auto next = ContentSnapshot::load_from_db(db, version, std::move(sha256_digest), out_error);
        if (!next) return false;
        return publish(std::move(next), out_error, out_swap_ms);
    }

    static bool enable_sqlite_wal_fallback(sqlite3* db, std::string& out_error) {
        if (!db) {
            out_error = "sqlite3 handle is null";
            return false;
        }
        char* errmsg = nullptr;
        const int rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            out_error = errmsg ? std::string(errmsg) : "failed to enable SQLite WAL mode";
            if (errmsg) sqlite3_free(errmsg);
            return false;
        }
        return true;
    }

    static bool apply_sql_overlay_file(sqlite3* db, const std::string& sql_path, std::string& out_error) {
        if (!db) {
            out_error = "sqlite3 handle is null";
            return false;
        }
        std::FILE* fp = std::fopen(sql_path.c_str(), "rb");
        if (!fp && sql_path.rfind("config/", 0) == 0) fp = std::fopen(("src/" + sql_path).c_str(), "rb");
        if (!fp && sql_path.rfind("src/config/", 0) == 0) fp = std::fopen(sql_path.substr(4).c_str(), "rb");
        if (!fp) {
            out_error = "cannot open SQL overlay file: " + sql_path;
            return false;
        }
        std::fseek(fp, 0, SEEK_END);
        const long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (sz <= 0) {
            std::fclose(fp);
            out_error = "SQL overlay file is empty: " + sql_path;
            return false;
        }
        std::string sql(static_cast<size_t>(sz), '\0');
        const size_t nread = std::fread(sql.data(), 1U, static_cast<size_t>(sz), fp);
        std::fclose(fp);
        sql.resize(nread);
        if (sql.empty()) {
            out_error = "SQL overlay file is empty: " + sql_path;
            return false;
        }
        char* errmsg = nullptr;
        const std::string wrapped = "BEGIN IMMEDIATE;\n" + sql + "\nCOMMIT;";
        if (sqlite3_exec(db, wrapped.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK) {
            out_error = errmsg ? std::string(errmsg) : "failed to apply SQL overlay";
            if (errmsg) sqlite3_free(errmsg);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        return true;
    }

    static std::string build_duckdb_parquet_export_sql(std::string_view table_name, std::string_view parquet_out_path) {
        if (table_name.empty() || parquet_out_path.empty()) return {};
        for (const char c : table_name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
            if (!ok) return {};
        }
        std::string escaped_path;
        escaped_path.reserve(parquet_out_path.size());
        for (const char c : parquet_out_path) {
            if (c == '\'') escaped_path.push_back('\'');
            escaped_path.push_back(c);
        }
        return "COPY (SELECT * FROM " + std::string(table_name) + ") TO '" + escaped_path +
               "' (FORMAT PARQUET, COMPRESSION ZSTD);";
    }

    static std::string build_listen_notify_sql(std::string_view hotfix_id, uint64_t version,
                                               std::string_view sha256_digest) {
        if (hotfix_id.empty() || version == 0U || !is_valid_sha256_hex(sha256_digest)) return {};
        return "NOTIFY content_hotfix_applied, '{\"hotfix_id\":\"" + std::string(hotfix_id) +
               "\",\"version\":" + std::to_string(version) + ",\"sha256_digest\":\"" + std::string(sha256_digest) + "\"}';";
    }

private:
    mutable std::shared_ptr<const ContentSnapshot> active_;
    mutable std::mutex                             write_mutex_;
};

}  // namespace aoc::worldd::content
