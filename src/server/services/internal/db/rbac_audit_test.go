// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db_test

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"testing"

	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"

	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

func readMigrationSQL(t *testing.T, filename string) string {
	t.Helper()
	candidates := []string{
		filepath.Join("sql", "migrations", filename),
		filepath.Join("services", "sql", "migrations", filename),
		filepath.Join("..", "..", "sql", "migrations", filename),
		filepath.Join("..", "..", "..", "services", "sql", "migrations", filename),
		filepath.Join("..", "..", "..", "sql", "migrations", filename),
	}
	if _, caller, _, ok := runtime.Caller(0); ok {
		candidates = append(candidates,
			filepath.Clean(filepath.Join(filepath.Dir(caller), "..", "..", "sql", "migrations", filename)),
			filepath.Clean(filepath.Join(filepath.Dir(caller), "..", "..", "..", "sql", "migrations", filename)))
	}
	for _, p := range candidates {
		if raw, err := os.ReadFile(p); err == nil {
			return string(raw)
		}
	}
	t.Fatalf("migration file %q not found", filename)
	return ""
}

func TestMigrationCatalog0022And0023(t *testing.T) {
	report, err := db.InspectMigrationCatalog()
	if err != nil {
		t.Fatalf("InspectMigrationCatalog: %v", err)
	}
	if len(report.Versions) < 23 {
		t.Fatalf("migration count = %d, want >= 23", len(report.Versions))
	}
	for i, v := range report.Versions {
		if v != i+1 {
			t.Fatalf("version[%d] = %d, want %d", i, v, i+1)
		}
	}
	for _, wantName := range []string{
		"0022_diff_save_procedures.sql",
		"0023_rbac_audit_and_idempotency.sql",
	} {
		if !slices.Contains(report.Names, wantName) {
			t.Errorf("missing migration %q in %v", wantName, report.Names)
		}
	}
	assertCatalogTablesAndIndexes(t, report)
}

func assertCatalogTablesAndIndexes(t *testing.T, report *db.MigrationCatalogReport) {
	t.Helper()
	for _, tbl := range []string{
		"character_save_fencing",
		"rbac_roles",
		"rbac_permissions",
		"rbac_role_permissions",
		"rbac_account_permissions",
		"gm_audit_log",
		"command_idempotency_ledger",
	} {
		if !slices.Contains(report.Tables, tbl) {
			t.Errorf("missing table %q in catalog report %v", tbl, report.Tables)
		}
	}
	for _, idx := range []string{
		"idx_character_items_covering_equip",
		"idx_character_resource_items_covering",
		"idx_character_quest_commands_covering",
		"idx_rbac_roles_parent",
		"idx_rbac_permissions_min_tier",
		"idx_rbac_role_permissions_perm",
		"idx_rbac_account_permissions_role",
		"idx_rbac_account_permissions_perm",
		"idx_rbac_account_permissions_granted_by",
		"idx_gm_audit_log_parent",
		"idx_gm_audit_log_actor_account",
		"idx_gm_audit_log_actor_character",
		"idx_gm_audit_log_actor_role",
		"idx_command_idempotency_actor_account",
		"idx_command_idempotency_audit_id",
		"idx_command_idempotency_expires_at",
	} {
		if !slices.Contains(report.Indexes, idx) {
			t.Errorf("missing index %q in catalog report %v", idx, report.Indexes)
		}
	}
	if !report.AllFKsIndexed || len(report.UnindexedFKs) != 0 {
		t.Fatalf("unindexed FKs detected: %+v", report.UnindexedFKs)
	}
}

func TestMigration0022DiffSaveStructuralInvariants(t *testing.T) {
	sql0022 := readMigrationSQL(t, "0022_diff_save_procedures.sql")
	for _, token := range []string{
		"CREATE INDEX IF NOT EXISTS idx_character_items_covering_equip ON character_items (character_id, equip_slot) INCLUDE (item_guid, charges, durability, condition_state);",
		"CREATE INDEX IF NOT EXISTS idx_character_resource_items_covering ON character_resource_items (character_id, bag_slot_type) INCLUDE (item_guid, charges, inventory_x, inventory_y);",
		"CREATE INDEX IF NOT EXISTS idx_character_quest_commands_covering ON character_quest_commands (character_id, quest_guid) INCLUDE (command_guid, current_progress, \"UpdateCounter\");",
		"characters.character_save_fencing",
		"\"character\".character_save_fencing",
		"fencing_token",
		"characters.fn_check_and_advance_fencing_token",
		"SQLSTATE '55000'",
		"characters.sp_diff_save_character_items",
		"characters.sp_diff_save_character_quests",
		"characters.sp_diff_save_character_wallet_and_progress",
		"\"character\".sp_diff_save_character_items",
		"\"character\".sp_diff_save_character_quests",
		"\"character\".sp_diff_save_character_wallet_and_progress",
		"FROM unnest(",
		"ON CONFLICT (id) DO UPDATE",
		"ANY(p_deleted_ids)",
		"sandal_char, sandal_admin",
	} {
		if !strings.Contains(sql0022, token) {
			t.Errorf("0022_diff_save_procedures.sql missing required token %q", token)
		}
	}
}

func TestMigration0023RBACAuditAndIdempotencyStructuralInvariants(t *testing.T) {
	sql0023 := readMigrationSQL(t, "0023_rbac_audit_and_idempotency.sql")
	for _, token := range []string{
		"auth.rbac_roles",
		"CHECK (role_id BETWEEN 0 AND 4)",
		"(0, 'player', NULL)",
		"(1, 'guide', 0)",
		"(2, 'gm', 1)",
		"(3, 'lead_gm', 2)",
		"(4, 'admin', 3)",
		"auth.rbac_permissions",
		"auth.rbac_role_permissions",
		"auth.rbac_account_permissions",
		"realm_state.gm_audit_log",
		"world.gm_audit_log",
		"trg_gm_audit_log_immutable",
		"BEFORE UPDATE OR DELETE ON realm_state.gm_audit_log",
		"realm_state.fn_gm_audit_log_immutable()",
		"gm_audit_log is append-only; UPDATE and DELETE are prohibited",
		"ERRCODE = '55000'",
		"REVOKE UPDATE, DELETE, TRUNCATE ON realm_state.gm_audit_log, world.gm_audit_log FROM PUBLIC, sandal_auth, sandal_char, sandal_world;",
		"realm_state.command_idempotency_ledger",
		"world.command_idempotency_ledger",
		"CHECK (substring(command_id::text from 15 for 1) = '7')",
		"CHECK (octet_length(payload_sha256) = 32)",
	} {
		if !strings.Contains(sql0023, token) {
			t.Errorf("0023_rbac_audit_and_idempotency.sql missing required token %q", token)
		}
	}
}

func TestLiveDiffSaveRBACAuditAndIdempotency(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatalf("Migrate: %v", err)
	}
	accountID := seedLiveTestAccount(t, ctx, pool)
	verifyLiveFencingTokenGuard(t, ctx, pool)
	verifyLiveRBACHierarchy(t, ctx, pool, accountID)
	auditID := verifyLiveGMAuditLogImmutability(t, ctx, pool, accountID)
	verifyLiveIdempotencyLedger(t, ctx, pool, accountID, auditID)
}

func seedLiveTestAccount(t *testing.T, ctx context.Context, pool *pgxpool.Pool) int64 {
	t.Helper()
	var accountID int64
	err := pool.QueryRow(ctx,
		"INSERT INTO accounts (username, password_hash) VALUES ('rbac_admin', 'hash') RETURNING id",
	).Scan(&accountID)
	if err != nil {
		t.Fatalf("insert account: %v", err)
	}
	return accountID
}

func verifyLiveFencingTokenGuard(t *testing.T, ctx context.Context, pool *pgxpool.Pool) {
	t.Helper()
	if _, err := pool.Exec(ctx, "SELECT characters.fn_check_and_advance_fencing_token(101, 10)"); err != nil {
		t.Fatalf("advance fencing token to 10: %v", err)
	}
	if _, err := pool.Exec(ctx, "SELECT characters.fn_check_and_advance_fencing_token(101, 12)"); err != nil {
		t.Fatalf("advance fencing token to 12: %v", err)
	}
	_, err := pool.Exec(ctx, "SELECT characters.fn_check_and_advance_fencing_token(101, 11)")
	assertPGErrorCode(t, "stale fencing token", err, "55000")

	for _, callSQL := range []string{
		"SELECT characters.sp_diff_save_character_items(101, 13, NULL, NULL, NULL, NULL, NULL, NULL, NULL)",
		"SELECT characters.sp_diff_save_character_quests(101, 14, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)",
		"SELECT characters.sp_diff_save_character_wallet_and_progress(101, 15, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)",
	} {
		if _, callErr := pool.Exec(ctx, callSQL); callErr != nil {
			t.Fatalf("diff-save call %q: %v", callSQL, callErr)
		}
	}
	_, staleItemErr := pool.Exec(ctx,
		"SELECT characters.sp_diff_save_character_items(101, 14, NULL, NULL, NULL, NULL, NULL, NULL, NULL)")
	assertPGErrorCode(t, "stale diff-save items fencing token", staleItemErr, "55000")
}

func verifyLiveRBACHierarchy(t *testing.T, ctx context.Context, pool *pgxpool.Pool, accountID int64) {
	t.Helper()
	var count int
	if err := pool.QueryRow(ctx, "SELECT count(*) FROM auth.rbac_roles").Scan(&count); err != nil || count != 5 {
		t.Fatalf("rbac_roles count = %d, err = %v; want 5", count, err)
	}
	_, err := pool.Exec(ctx, `
		INSERT INTO auth.rbac_permissions (permission_id, min_tier, description)
		VALUES ('gm.teleport', 2, 'Teleport character');
		INSERT INTO auth.rbac_role_permissions (role_id, permission_id)
		VALUES (2, 'gm.teleport');
		INSERT INTO auth.rbac_account_permissions (account_id, role_id, permission_id, granted, granted_by)
		VALUES ($1, 2, 'gm.teleport', TRUE, $1)`, accountID)
	if err != nil {
		t.Fatalf("seed RBAC permission graph: %v", err)
	}
}

func verifyLiveGMAuditLogImmutability(t *testing.T, ctx context.Context, pool *pgxpool.Pool, accountID int64) string {
	t.Helper()
	const auditUUID = "018f4a7c-8d10-7000-8000-000000000001"
	_, err := pool.Exec(ctx, `
		INSERT INTO realm_state.gm_audit_log
			(audit_id, actor_account_id, actor_role, command_name, before_state, after_state)
		VALUES ($1::uuid, $2, 2, 'gm.teleport', '{"x":0}'::jsonb, '{"x":100}'::jsonb)`,
		auditUUID, accountID)
	if err != nil {
		t.Fatalf("insert gm_audit_log: %v", err)
	}
	_, updErr := pool.Exec(ctx,
		"UPDATE realm_state.gm_audit_log SET command_name = 'tampered' WHERE audit_id = $1::uuid", auditUUID)
	assertPGErrorCode(t, "gm_audit_log UPDATE rejection", updErr, "55000")

	_, delErr := pool.Exec(ctx,
		"DELETE FROM realm_state.gm_audit_log WHERE audit_id = $1::uuid", auditUUID)
	assertPGErrorCode(t, "gm_audit_log DELETE rejection", delErr, "55000")
	return auditUUID
}

func verifyLiveIdempotencyLedger(
	t *testing.T,
	ctx context.Context,
	pool *pgxpool.Pool,
	accountID int64,
	auditUUID string,
) {
	t.Helper()
	const validUUIDv7 = "018f4a7c-8d10-7123-9abc-def012345678"
	const invalidUUIDv4 = "018f4a7c-8d10-4123-9abc-def012345678"
	digest := make([]byte, 32)

	_, err := pool.Exec(ctx, `
		INSERT INTO realm_state.command_idempotency_ledger
			(command_id, actor_account_id, command_name, payload_sha256, status, audit_id)
		VALUES ($1::uuid, $2, 'gm.teleport', $3, 1, $4::uuid)`,
		validUUIDv7, accountID, digest, auditUUID)
	if err != nil {
		t.Fatalf("insert valid UUIDv7 idempotency record: %v", err)
	}

	_, badErr := pool.Exec(ctx, `
		INSERT INTO realm_state.command_idempotency_ledger
			(command_id, actor_account_id, command_name, payload_sha256, status)
		VALUES ($1::uuid, $2, 'gm.teleport', $3, 1)`,
		invalidUUIDv4, accountID, digest)
	assertPGErrorCode(t, "UUIDv4 rejection on command_idempotency_ledger", badErr, "23514")
}

func assertPGErrorCode(t *testing.T, label string, err error, wantCode string) {
	t.Helper()
	var pgErr *pgconn.PgError
	if !errors.As(err, &pgErr) || pgErr.Code != wantCode {
		t.Fatalf("%s: got err=%v, want SQLSTATE %s", label, err, wantCode)
	}
}
