// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#ifndef TALENT_FIXTURE_PATH
#define TALENT_FIXTURE_PATH "server/world/tests/talent_fixture.sql"
#endif
#ifndef STATS_CORPUS_PATH
#define STATS_CORPUS_PATH "libs/aocstats/testdata/corpus.txt"
#endif

#include "session/player_world.h"
#include "test_harness.h"
#include "tests/content_db_fixture.h"

#include <thread>

namespace {

namespace content = aoc::worldd::content;

constexpr std::string_view kShaV1 = "a1ef902348bc76de1092384756abcdef0192837465fedcba9081726354ef0123";
constexpr std::string_view kShaV2 = "b2ef902348bc76de1092384756abcdef0192837465fedcba9081726354ef0456";

bool exec_sql_batch(const std::filesystem::path& db_path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) return false;
    char* errmsg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (errmsg) sqlite3_free(errmsg);
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

bool seed_craft_and_mount_tables(const std::filesystem::path& db_path) {
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS mount (
            item_guid INTEGER PRIMARY KEY, item_name TEXT NOT NULL, kind TEXT NOT NULL, blueprint_path TEXT NOT NULL,
            blueprint TEXT NOT NULL, activation TEXT NOT NULL, cast_ability_guid INTEGER NOT NULL, level INTEGER NOT NULL,
            ground_pct INTEGER NOT NULL, flying_pct INTEGER NOT NULL, water_pct INTEGER NOT NULL, health_pct INTEGER NOT NULL,
            mule INTEGER NOT NULL, crate_capacity INTEGER NOT NULL, tradeable INTEGER NOT NULL, description TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS mount_stat (
            level INTEGER NOT NULL, stat_guid INTEGER NOT NULL, stat_name TEXT NOT NULL, value REAL NOT NULL,
            PRIMARY KEY (level, stat_guid)
        ) WITHOUT ROWID;
        CREATE TABLE IF NOT EXISTS mount_ability (
            ability_guid INTEGER PRIMARY KEY, name TEXT NOT NULL, role TEXT NOT NULL,
            usable_while_mounted INTEGER NOT NULL, auto_dismount INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS mount_effect (
            effect_guid INTEGER PRIMARY KEY, name TEXT NOT NULL, kind TEXT NOT NULL, tier INTEGER NOT NULL,
            stat_guid INTEGER NOT NULL, value REAL NOT NULL, relative INTEGER NOT NULL, infinite INTEGER NOT NULL,
            duration REAL NOT NULL, force_dismount INTEGER NOT NULL
        );
        INSERT OR REPLACE INTO mount VALUES (
            6064631121016127508, 'Mount_Grounded_TerrorBird_0', 'ground',
            '/Game/GameBlueprints/Mounts/Mount_Grounded_TerrorBird_0.Mount_Grounded_TerrorBird_0_C',
            'Mount_Grounded_TerrorBird_0', 'SummonMount', 108436548376185, 20, 165, 0, 0, 100, 0, 0, 1, 'Ground Mount'
        );
        INSERT OR REPLACE INTO mount_stat VALUES (20, 108986356618873, 'MaxHealth', 3572), (25, 109228544644729, 'MoveSpeedMult', 1);
        INSERT OR REPLACE INTO mount_ability VALUES (108436548376185, 'Summon_Mount', 'summon', 1, 0);
        INSERT OR REPLACE INTO mount_effect VALUES (6064632988601155680, 'Road_T3_Speed_Mount', 'road', 3, 109228544644729, 0.25, 0, 1, 0, 0);
        CREATE TABLE IF NOT EXISTS craft_station_def (
            guid INTEGER PRIMARY KEY, name TEXT, display_name TEXT, kind TEXT,
            max_tier INTEGER, queue_size INTEGER, simultaneous INTEGER, individual INTEGER,
            tax_tag TEXT, recipe_set_guid INTEGER
        );
        INSERT OR REPLACE INTO craft_station_def VALUES (88001, 'Station_Forge', 'Forge', 'Crafting', 4, 3, 1, 1, 'tax.forge', 88002);
        CREATE TABLE IF NOT EXISTS craft_station_asset (asset_guid INTEGER PRIMARY KEY, def_guid INTEGER);
        INSERT OR REPLACE INTO craft_station_asset VALUES (88003, 88001);
        CREATE TABLE IF NOT EXISTS craft_recipe (
            guid INTEGER PRIMARY KEY, name TEXT, profession_guid INTEGER, tier INTEGER,
            base_duration REAL, std_cost TEXT, cost_guid INTEGER, reward_guid INTEGER,
            learnable INTEGER, destination TEXT, fuel INTEGER, blacklisted INTEGER,
            category_tag TEXT, duration_formula TEXT, quantity_formula TEXT, quality_formula TEXT
        );
        INSERT OR REPLACE INTO craft_recipe VALUES (
            89001, 'Recipe_Iron_Ingot', 1, 1, 5.0, '10', 0, 0, 1, 'Inventory', 0, 0, 'cat.metal', '', '', ''
        );
        CREATE TABLE IF NOT EXISTS craft_recipe_input (
            recipe_guid INTEGER, kind TEXT, ord INTEGER, item_guid INTEGER,
            filter_guid INTEGER, tags TEXT, quantity INTEGER, min_rarity INTEGER, consume INTEGER
        );
        CREATE TABLE IF NOT EXISTS craft_pred (
            owner_kind TEXT, owner_guid INTEGER, ord INTEGER, type TEXT,
            node_level_n INTEGER, biomes TEXT, modifications TEXT, nodes TEXT, hidden INTEGER
        );
        CREATE TABLE IF NOT EXISTS craft_cost (guid INTEGER PRIMARY KEY, tier_guid INTEGER, value TEXT);
        CREATE TABLE IF NOT EXISTS craft_recipe_set_list (set_guid INTEGER, ord INTEGER, list_guid INTEGER);
        CREATE TABLE IF NOT EXISTS craft_recipe_list_member (list_guid INTEGER, ord INTEGER, recipe_guid INTEGER);
        CREATE TABLE IF NOT EXISTS craft_reward (guid INTEGER PRIMARY KEY, body TEXT);
    )SQL";
    return exec_sql_batch(db_path, sql);
}

bool seed_profession_currency_and_ability_tables(const std::filesystem::path& db_path) {
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS currency_tier (guid INTEGER PRIMARY KEY, name TEXT, max_base_quantity INTEGER);
        CREATE TABLE IF NOT EXISTS currency_def (
            guid INTEGER PRIMARY KEY, name TEXT, tier_guid INTEGER, ord INTEGER, item_guid INTEGER, conversion_rate INTEGER
        );
        CREATE TABLE IF NOT EXISTS profession (
            guid INTEGER PRIMARY KEY, name TEXT, category TEXT, leveling_type_guid INTEGER, skill_tree_guid INTEGER
        );
        CREATE TABLE IF NOT EXISTS certification_level (
            guid INTEGER PRIMARY KEY, name TEXT, tier INTEGER, min_level INTEGER, max_level INTEGER,
            has_max_allowable INTEGER, max_allowable INTEGER
        );
        INSERT OR REPLACE INTO certification_level VALUES
            (101, 'Novice', 0, 1, 10, 0, 0), (102, 'Apprentice', 1, 10, 20, 1, 5),
            (103, 'Journeyman', 2, 20, 30, 1, 4), (104, 'Master', 3, 30, 40, 1, 3),
            (105, 'GrandMaster', 4, 40, 50, 1, 2);
        CREATE TABLE IF NOT EXISTS action_certification (
            action_guid INTEGER PRIMARY KEY, name TEXT, profession_guid INTEGER, action TEXT
        );
        CREATE TABLE IF NOT EXISTS ab_expr (id INTEGER PRIMARY KEY, text TEXT, terms TEXT, program INTEGER, uses_target INTEGER, error TEXT);
        CREATE TABLE IF NOT EXISTS ab_ability (
            guid INTEGER PRIMARY KEY, name TEXT, display_name TEXT, tags TEXT, held_behavior TEXT, activation_input TEXT,
            target_requirement TEXT, fallback_target TEXT, applicable_targets TEXT, applicable_types TEXT,
            weapon_requirement TEXT, shared_cooldown_tag TEXT, disable_during_cooldown INTEGER, max_hold_time REAL,
            hold_animation TEXT, usable_in_air INTEGER, usable_while_swimming INTEGER, usable_while_walking INTEGER,
            usable_while_mounted INTEGER, usable_from_hotbar INTEGER, area_range REAL, area_radius REAL,
            networking_max_speed REAL, cooldown_expr INTEGER, charges_expr INTEGER, cast_expr INTEGER,
            requirement_expr INTEGER, can_break_expr INTEGER, has_proc_expr INTEGER, extra TEXT
        );
        CREATE TABLE IF NOT EXISTS ab_cost (
            ability_guid INTEGER, cost_ord INTEGER, stat_ord INTEGER, enabled_expr INTEGER,
            triggers_cooldown INTEGER, charge_cost_expr INTEGER, stat_guid INTEGER, stat_name TEXT, amount_expr INTEGER
        );
        CREATE TABLE IF NOT EXISTS ab_effect_cost (
            ability_guid INTEGER, cost_ord INTEGER, ord INTEGER, effect_guid INTEGER, filter TEXT, num_expr INTEGER
        );
        CREATE TABLE IF NOT EXISTS ab_montage (
            ability_guid INTEGER, anim_index INTEGER, soft_path TEXT, direction TEXT,
            select_expr INTEGER, start_section TEXT, targeting_key INTEGER, net_guid INTEGER
        );
        CREATE TABLE IF NOT EXISTS ab_targeting (ability_guid INTEGER, key INTEGER, targeting_guid INTEGER, targeting_name TEXT);
        CREATE TABLE IF NOT EXISTS ab_ability_hit (
            ability_guid INTEGER, source TEXT, key INTEGER, owner_guid INTEGER, role TEXT, ord INTEGER, hit_guid INTEGER
        );
        CREATE TABLE IF NOT EXISTS ab_ability_effect (ability_guid INTEGER, key INTEGER, effect_guid INTEGER);
        CREATE TABLE IF NOT EXISTS ab_hit (
            guid INTEGER PRIMARY KEY, name TEXT, net_type TEXT, applicable_targets TEXT, applicable_types TEXT,
            fallback_target TEXT, requirement_expr INTEGER, delay_expr INTEGER, threat_expr INTEGER, max_targets_expr INTEGER, extra TEXT
        );
        CREATE TABLE IF NOT EXISTS ab_hit_effect (
            hit_guid INTEGER, kind TEXT, ord INTEGER, effect_guid INTEGER, requirement_expr INTEGER,
            stack_count_expr INTEGER, duration_expr INTEGER, on_hit_behavior TEXT, max_applications INTEGER
        );
        CREATE TABLE IF NOT EXISTS ab_hit_projectile (hit_guid INTEGER, kind TEXT, ord INTEGER, projectile_guid INTEGER);
        CREATE TABLE IF NOT EXISTS ab_effect (
            guid INTEGER PRIMARY KEY, name TEXT, display_name TEXT, category TEXT, tags TEXT, effect_tags TEXT,
            infinite_duration INTEGER, duration_expr INTEGER, tick_timer REAL, ignore_initial_tick INTEGER,
            stack_type TEXT, global_stack_limit INTEGER, in_place_stack_limit_expr INTEGER, stack_duration_refresh INTEGER,
            persists_through_death INTEGER, persists_relog INTEGER, dispellable INTEGER, significant INTEGER,
            requirement_expr INTEGER, crowd_control_guid INTEGER, extra TEXT
        );
        CREATE TABLE IF NOT EXISTS ab_effect_hit (effect_guid INTEGER, phase TEXT, ord INTEGER, hit_guid INTEGER);
        CREATE TABLE IF NOT EXISTS ab_mod (
            owner_kind TEXT, owner_guid INTEGER, ord INTEGER, stat_mod_guid INTEGER,
            stat_guid INTEGER, stat_name TEXT, value_expr INTEGER, shield_absorb INTEGER
        );
        CREATE TABLE IF NOT EXISTS ab_projectile (
            guid INTEGER PRIMARY KEY, name TEXT, projectile_type TEXT, speed REAL, range REAL,
            life_time REAL, max_hits INTEGER, chain_hit_count INTEGER, chain_radius REAL, sphere_radius REAL
        );
        CREATE TABLE IF NOT EXISTS ab_projectile_hit (projectile_guid INTEGER, ord INTEGER, hit_guid INTEGER);
        CREATE TABLE IF NOT EXISTS ab_zone (
            guid INTEGER PRIMARY KEY, name TEXT, life_time REAL, tick_rate REAL, use_global_tick_rate INTEGER,
            charges INTEGER, delay_time REAL, delay_type TEXT, speed REAL,
            expire_after_hits_expr INTEGER, expire_if_expr INTEGER, targeting_guid INTEGER
        );
        CREATE TABLE IF NOT EXISTS ab_zone_hit (zone_guid INTEGER, role TEXT, ord INTEGER, hit_guid INTEGER);
        CREATE TABLE IF NOT EXISTS ab_reaction (
            guid INTEGER PRIMARY KEY, name TEXT, instigator TEXT, targets_mask INTEGER,
            bind_hit INTEGER, bind_effect INTEGER, bind_ability INTEGER, bind_projectile INTEGER,
            bind_zone INTEGER, bind_movement INTEGER, bind_fishing INTEGER, bind_stat INTEGER,
            global_limit INTEGER, instigator_limit INTEGER, eval_applying_hit INTEGER, proc_on_apply INTEGER, trigger_expr INTEGER
        );
        CREATE TABLE IF NOT EXISTS ab_reaction_hit (reaction_guid INTEGER, ord INTEGER, hit_guid INTEGER);
        CREATE TABLE IF NOT EXISTS ab_hit_recursive (hit_guid INTEGER, ord INTEGER, child_hit_guid INTEGER, targeting_guid INTEGER);
    )SQL";
    return exec_sql_batch(db_path, sql);
}

int verify_overlay_and_wal_fallback(const std::filesystem::path& db_path) {
    sqlite3* raw_db = nullptr;
    SANDAL_ASSERT(sqlite3_open(db_path.string().c_str(), &raw_db) == SQLITE_OK);
    std::string err;
    SANDAL_ASSERT(content::ContentSnapshotRegistry::enable_sqlite_wal_fallback(raw_db, err));
    SANDAL_ASSERT(err.empty());
    SANDAL_ASSERT(content::ContentSnapshotRegistry::apply_sql_overlay_file(
        raw_db, "data/hotfixes/0001_initial_overlay.sql", err));
    SANDAL_ASSERT(content::ContentSnapshotRegistry::apply_sql_overlay_file(
        raw_db, "data/hotfixes/0001_initial_overlay.sql", err));

    sqlite3_stmt* stmt = nullptr;
    SANDAL_ASSERT(sqlite3_prepare_v2(raw_db,
        "SELECT version, sha256_digest FROM content_hotfix_ledger WHERE hotfix_id = '0001_initial_overlay'",
        -1, &stmt, nullptr) == SQLITE_OK);
    SANDAL_ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    SANDAL_ASSERT(sqlite3_column_int64(stmt, 0) == 1);
    const char* digest = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    SANDAL_ASSERT(digest != nullptr && content::is_valid_sha256_hex(digest));
    sqlite3_finalize(stmt);
    sqlite3_close(raw_db);
    return 0;
}

int test_snapshot_15_catalogs_and_wal(const std::filesystem::path& db_path) {
    SANDAL_ASSERT(verify_overlay_and_wal_fallback(db_path) == 0);
    std::string err;
    auto db = content::ContentDb::open(db_path.string(), err);
    SANDAL_ASSERT(db.has_value());
    auto snap = content::ContentSnapshot::load_from_db(*db, 1U, std::string(kShaV1), err);
    SANDAL_ASSERT(snap != nullptr);
    SANDAL_ASSERT(snap->version == 1U);
    SANDAL_ASSERT(snap->sha256_digest == kShaV1);
    SANDAL_ASSERT(snap->wal_mode_enabled);
    SANDAL_ASSERT(snap->loaded_catalog_count() == 15U);
    SANDAL_ASSERT(snap->item_quality(content_fixture::item::kShirt) == 1);
    SANDAL_ASSERT(snap->item_name(content_fixture::item::kShirt) == "Test_Shirt");
    return 0;
}

bool apply_v2_hotfix_mutation(const std::filesystem::path& db_path) {
    const char* sql = R"SQL(
        UPDATE item SET name = 'Test_Shirt_Hotfixed_v2', default_quality = 7
        WHERE guid = 6064631187700000011;
        UPDATE ability_runtime SET cost_amount = 15.0
        WHERE ability_guid = 6064631641648332884;
    )SQL";
    return exec_sql_batch(db_path, sql);
}

int test_concurrent_rcu_swap_and_pinning(const std::filesystem::path& db_path) {
    std::string err;
    auto db = content::ContentDb::open(db_path.string(), err);
    SANDAL_ASSERT(db.has_value());
    content::ContentSnapshotRegistry registry;
    double swap_ms_v1 = 0.0;
    SANDAL_ASSERT(registry.reload_from_db(*db, 1U, std::string(kShaV1), err, &swap_ms_v1));
    SANDAL_ASSERT(swap_ms_v1 < content::kMaxHotReloadSwapMs);

    const auto pinned_v1 = registry.current();
    SANDAL_ASSERT(pinned_v1 != nullptr && pinned_v1->version == 1U);

    std::atomic<int> ready_readers{0};
    std::atomic<bool> stop_readers{false};
    std::atomic<bool> reader_fault{false};
    std::vector<std::thread> readers;
    readers.reserve(8U);
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&]() {
            ready_readers.fetch_add(1, std::memory_order_release);
            for (int iter = 0; iter < 50000 && !stop_readers.load(std::memory_order_acquire); ++iter) {
                const auto cur = registry.current();
                if (!cur || (cur->version != 1U && cur->version != 2U)) {
                    reader_fault.store(true, std::memory_order_release);
                    break;
                }
                const int q = cur->item_quality(content_fixture::item::kShirt);
                if ((cur->version == 1U && q != 1) || (cur->version == 2U && q != 7)) {
                    reader_fault.store(true, std::memory_order_release);
                    break;
                }
            }
        });
    }
    for (int spin = 0; spin < 10000 && ready_readers.load(std::memory_order_acquire) < 8; ++spin) {
        std::this_thread::yield();
    }

    SANDAL_ASSERT(apply_v2_hotfix_mutation(db_path));
    double swap_ms_v2 = 0.0;
    SANDAL_ASSERT(registry.reload_from_db(*db, 2U, std::string(kShaV2), err, &swap_ms_v2));
    SANDAL_ASSERT(swap_ms_v2 < content::kMaxHotReloadSwapMs);

    stop_readers.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();
    SANDAL_ASSERT(!reader_fault.load(std::memory_order_acquire));

    SANDAL_ASSERT(pinned_v1->version == 1U);
    SANDAL_ASSERT(pinned_v1->item_quality(content_fixture::item::kShirt) == 1);
    SANDAL_ASSERT(pinned_v1->item_name(content_fixture::item::kShirt) == "Test_Shirt");

    const auto active_v2 = registry.current();
    SANDAL_ASSERT(active_v2 != nullptr && active_v2->version == 2U);
    SANDAL_ASSERT(active_v2->item_quality(content_fixture::item::kShirt) == 7);
    SANDAL_ASSERT(active_v2->item_name(content_fixture::item::kShirt) == "Test_Shirt_Hotfixed_v2");
    SANDAL_ASSERT(active_v2->rules.has_value());
    SANDAL_ASSERT(active_v2->rules->sprint.cost_amount == 15.0);
    return 0;
}

int test_fail_closed_rollback_and_helpers(const std::filesystem::path& db_path) {
    std::string err;
    auto db = content::ContentDb::open(db_path.string(), err);
    SANDAL_ASSERT(db.has_value());
    content::ContentSnapshotRegistry registry;
    SANDAL_ASSERT(registry.reload_from_db(*db, 2U, std::string(kShaV2), err));

    SANDAL_ASSERT(!registry.reload_from_db(*db, 1U, std::string(kShaV1), err));
    SANDAL_ASSERT(!registry.reload_from_db(*db, 2U, std::string(kShaV1), err));
    SANDAL_ASSERT(!registry.reload_from_db(*db, 3U, "bad_non_hex_sha256", err));
    SANDAL_ASSERT(registry.current() != nullptr && registry.current()->version == 2U);
    SANDAL_ASSERT(registry.current()->sha256_digest == kShaV2);

    aoc::protocol::ServerGuidAllocator guids(1U, 1U);
    aoc::worldd::PlayerWorld world(&*db, "", aoc::worldd::bootstrap::GameStateMode::Tier2, 1U, guids);
    SANDAL_ASSERT(world.reload_content_snapshot(*db, 1U, std::string(kShaV1), err));
    SANDAL_ASSERT(world.content_snapshot() != nullptr && world.content_snapshot()->version == 1U);
    SANDAL_ASSERT(!world.reload_content_snapshot(*db, 1U, std::string(kShaV2), err));
    SANDAL_ASSERT(world.content_snapshot()->version == 1U);
    SANDAL_ASSERT(world.reload_content_snapshot(*db, 2U, std::string(kShaV2), err));
    SANDAL_ASSERT(world.content_snapshot()->version == 2U);

    const std::string parquet_sql =
        content::ContentSnapshotRegistry::build_duckdb_parquet_export_sql("item", "out/item.parquet");
    SANDAL_ASSERT(parquet_sql == "COPY (SELECT * FROM item) TO 'out/item.parquet' (FORMAT PARQUET, COMPRESSION ZSTD);");
    SANDAL_ASSERT(content::ContentSnapshotRegistry::build_duckdb_parquet_export_sql("item; DROP TABLE item;", "x").empty());

    const std::string notify_sql =
        content::ContentSnapshotRegistry::build_listen_notify_sql("0001_initial_overlay", 2U, kShaV2);
    SANDAL_ASSERT(notify_sql.find("NOTIFY content_hotfix_applied") != std::string::npos);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    const auto temp_dir = content_fixture::make_temp_dir("content_hotfix_reload");
    const auto db_path = temp_dir / "content_hotfix.db";
    content_fixture::DbOptions opts;
    opts.talent_tables = true;
    SANDAL_ASSERT(content_fixture::make_db(db_path, opts));
    SANDAL_ASSERT(seed_craft_and_mount_tables(db_path));
    SANDAL_ASSERT(seed_profession_currency_and_ability_tables(db_path));

    int rc = 0;
    if (sandal::test::should_run_case(cfg, "snapshot_15_catalogs_and_wal")) {
        rc = test_snapshot_15_catalogs_and_wal(db_path);
        if (rc != 0) return rc;
    }
    if (sandal::test::should_run_case(cfg, "concurrent_rcu_swap_and_pinning")) {
        rc = test_concurrent_rcu_swap_and_pinning(db_path);
        if (rc != 0) return rc;
    }
    if (sandal::test::should_run_case(cfg, "fail_closed_rollback_and_helpers")) {
        rc = test_fail_closed_rollback_and_helpers(db_path);
        if (rc != 0) return rc;
    }
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    return 0;
}
