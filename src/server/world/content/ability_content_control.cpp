// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "content/ability_content.h"

#include <sqlite3.h>

#include <functional>
#include <utility>

namespace aoc::worldd::content {

namespace {

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) { rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr); }
    ~Stmt() { if (stmt_) sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    bool ok() const { return rc_ == SQLITE_OK && stmt_ != nullptr; }
    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
    int           rc_ = SQLITE_OK;
};

std::string col_text(sqlite3_stmt* s, int col) {
    const unsigned char* t = sqlite3_column_text(s, col);
    return t ? std::string(reinterpret_cast<const char*>(t), static_cast<size_t>(sqlite3_column_bytes(s, col))) : std::string();
}
int64_t col_i64(sqlite3_stmt* s, int col) { return sqlite3_column_int64(s, col); }
int col_int(sqlite3_stmt* s, int col) { return static_cast<int>(sqlite3_column_int64(s, col)); }
bool col_bool(sqlite3_stmt* s, int col) { return sqlite3_column_int64(s, col) != 0; }
double col_f64(sqlite3_stmt* s, int col) { return sqlite3_column_double(s, col); }

std::string db_message(sqlite3* db) {
    const char* m = sqlite3_errmsg(db);
    return m ? m : "unknown SQLite error";
}

bool run(sqlite3* db, const char* sql, const std::function<void(sqlite3_stmt*)>& bind, const std::function<bool(sqlite3_stmt*)>& row, std::string& error) {
    Stmt q(db, sql);
    if (!q.ok()) {
        error = std::string("content.db: ") + db_message(db);
        return false;
    }
    if (bind) bind(q.get());
    int step;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        if (!row(q.get())) return false;
    }
    if (step != SQLITE_DONE) {
        error = std::string("content.db: ") + db_message(db);
        return false;
    }
    return true;
}

constexpr const char* kCrowdControlSql =
    "SELECT guid, name, applied_status, snare_strength, animation_request, priority, disable_guid, face_source, ignore_non_combatant, restart_montage, override_client_move, "
    "push_parameterization, push_magnitude, push_duration, push_dir_x, push_dir_y, push_dir_z, push_arrival_distance, end_push_at_arrival FROM ab_crowd_control WHERE guid = ?1";
constexpr const char* kAbilityDisableSql = "SELECT guid, name, filter FROM ab_ability_disable WHERE guid = ?1";
constexpr const char* kFragilitySql = "SELECT guid, name, fragility_type, break_threshold, accumulation_rate, tick_frequency, break_immediately, evaluate_accumulation, bindings "
                                      "FROM ab_fragility WHERE guid = ?1";
constexpr const char* kEffectFragilitiesSql = "SELECT fragility_guid FROM ab_effect_fragility WHERE effect_guid = ?1 ORDER BY ord";

}  // namespace

std::optional<CrowdControlRow> AbilityContent::crowd_control(int64_t guid, std::string& error) const {
    std::optional<CrowdControlRow> out;
    if (!run(db_, kCrowdControlSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, guid); }, [&](sqlite3_stmt* s) {
            CrowdControlRow r;
            r.guid = col_i64(s, 0);
            r.name = col_text(s, 1);
            r.applied_status = col_i64(s, 2);
            r.snare_strength = col_f64(s, 3);
            r.animation_request = col_text(s, 4);
            r.priority = col_int(s, 5);
            r.disable_guid = col_i64(s, 6);
            r.face_source = col_bool(s, 7);
            r.ignore_non_combatant = col_bool(s, 8);
            r.restart_montage = col_bool(s, 9);
            r.override_client_move = col_bool(s, 10);
            r.push_parameterization = col_text(s, 11);
            r.push_magnitude = col_text(s, 12);
            r.push_duration = col_text(s, 13);
            r.push_dir_x = col_f64(s, 14);
            r.push_dir_y = col_f64(s, 15);
            r.push_dir_z = col_f64(s, 16);
            r.push_arrival_distance = col_f64(s, 17);
            r.end_push_at_arrival = col_bool(s, 18);
            out = std::move(r);
            return true;
        }, error))
        return std::nullopt;
    if (!out) error = "content.db has no crowd-control record " + std::to_string(guid);
    return out;
}

std::optional<AbilityDisableRow> AbilityContent::ability_disable(int64_t guid, std::string& error) const {
    std::optional<AbilityDisableRow> out;
    if (!run(db_, kAbilityDisableSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, guid); }, [&](sqlite3_stmt* s) {
            AbilityDisableRow r;
            r.guid = col_i64(s, 0);
            r.name = col_text(s, 1);
            r.filter = col_text(s, 2);
            out = std::move(r);
            return true;
        }, error))
        return std::nullopt;
    if (!out) error = "content.db has no ability-disable record " + std::to_string(guid);
    return out;
}

std::optional<FragilityRow> AbilityContent::fragility(int64_t guid, std::string& error) const {
    std::optional<FragilityRow> out;
    if (!run(db_, kFragilitySql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, guid); }, [&](sqlite3_stmt* s) {
            FragilityRow r;
            r.guid = col_i64(s, 0);
            r.name = col_text(s, 1);
            r.fragility_type = col_text(s, 2);
            r.break_threshold = col_text(s, 3);
            r.accumulation_rate = col_text(s, 4);
            r.tick_frequency = col_f64(s, 5);
            r.break_immediately = col_text(s, 6);
            r.evaluate_accumulation = col_text(s, 7);
            r.bindings = col_text(s, 8);
            out = std::move(r);
            return true;
        }, error))
        return std::nullopt;
    if (!out) error = "content.db has no fragility record " + std::to_string(guid);
    return out;
}

bool AbilityContent::effect_fragilities(int64_t effect, std::vector<int64_t>& out, std::string& error) const {
    out.clear();
    return run(db_, kEffectFragilitiesSql, [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, effect); }, [&](sqlite3_stmt* s) {
        out.push_back(col_i64(s, 0));
        return true;
    }, error);
}

void AbilityContent::modify_cooldowns(std::vector<ModifyCooldownRow>& out) const {
    out.clear();
    if (!reactions_) return;
    std::string error;
    constexpr const char* sql = "SELECT guid, name, delta_expr, abilities FROM ab_modify_cooldown";
    if (!run(db_, sql, nullptr, [&](sqlite3_stmt* s) {
            ModifyCooldownRow r;
            r.guid = col_i64(s, 0);
            r.name = col_text(s, 1);
            r.delta_expr = col_i64(s, 2);
            r.abilities = col_text(s, 3);
            out.push_back(std::move(r));
            return true;
        }, error)) {
        out.clear();
    }
}

}  // namespace aoc::worldd::content
