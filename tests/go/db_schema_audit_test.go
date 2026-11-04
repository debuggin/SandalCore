// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db_schema_audit_test

import (
	"cmp"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"testing"
)

type migrationFile struct {
	version int
	name    string
	sql     string
}

type fkSpec struct {
	table    string
	columns  []string
	refTable string
	indexed  bool
	index    string
}

type idxSpec struct {
	name    string
	table   string
	columns []string
}

var (
	migNameRe     = regexp.MustCompile(`^(\d{4})_.+\.sql$`)
	schemaRe      = regexp.MustCompile(`(?im)^\s*CREATE\s+SCHEMA\s+(?:IF\s+NOT\s+EXISTS\s+)?(?:"?([a-zA-Z0-9_]+)"?)\s*;`)
	roleRe        = regexp.MustCompile(`'(sandal_[a-z0-9_]+)'|\bCREATE\s+ROLE\s+([a-zA-Z0-9_]+)`)
	createTableRe = regexp.MustCompile(`(?is)CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?([a-zA-Z0-9_."]+)\s*\((.*?)\)\s*;`)
	createIndexRe = regexp.MustCompile(`(?is)CREATE\s+(?:UNIQUE\s+)?INDEX\s+(?:IF\s+NOT\s+EXISTS\s+)?([a-zA-Z0-9_]+)\s+ON\s+([a-zA-Z0-9_."]+)\s*\(([^)]+)\)`)
	tablePKRe     = regexp.MustCompile(`(?is)\bPRIMARY\s+KEY\s*\(([^)]+)\)`)
	inlinePKRe    = regexp.MustCompile(`(?im)^\s*([a-zA-Z0-9_]+)\s+[a-zA-Z0-9_]+(?:\s*\([^)]*\))?[^,\n]*?\bPRIMARY\s+KEY\b`)
	tableFKRe     = regexp.MustCompile(`(?is)\bFOREIGN\s+KEY\s*\(([^)]+)\)\s*REFERENCES\s+([a-zA-Z0-9_."]+)`)
	inlineFKRe    = regexp.MustCompile(`(?im)^\s*([a-zA-Z0-9_]+)\s+[a-zA-Z0-9_]+(?:\s*\([^)]*\))?[^,\n]*?\bREFERENCES\s+([a-zA-Z0-9_."]+)`)
)

func locateMigrationsDir(t *testing.T) string {
	t.Helper()
	for _, cand := range []string{
		filepath.Join("services", "sql", "migrations"),
		filepath.Join("..", "services", "sql", "migrations"),
		filepath.Join("..", "..", "services", "sql", "migrations"),
		filepath.Join("..", "..", "server", "services", "sql", "migrations"),
	} {
		if info, err := os.Stat(cand); err == nil && info.IsDir() {
			return cand
		}
	}
	if _, file, _, ok := runtime.Caller(0); ok {
		for _, rel := range [][]string{
			{filepath.Dir(file), "..", "..", "services", "sql", "migrations"},
			{filepath.Dir(file), "..", "..", "server", "services", "sql", "migrations"},
		} {
			dir := filepath.Clean(filepath.Join(rel...))
			if info, err := os.Stat(dir); err == nil && info.IsDir() {
				return dir
			}
		}
	}
	t.Fatal("unable to locate services/sql/migrations directory")
	return ""
}

func readAllMigrations(t *testing.T) []migrationFile {
	t.Helper()
	dir := locateMigrationsDir(t)
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("ReadDir(%s): %v", dir, err)
	}
	var out []migrationFile
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".sql") {
			continue
		}
		m := migNameRe.FindStringSubmatch(e.Name())
		if m == nil {
			t.Fatalf("invalid migration filename %q", e.Name())
		}
		v, err := strconv.Atoi(m[1])
		if err != nil {
			t.Fatalf("invalid version in %q: %v", e.Name(), err)
		}
		raw, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			t.Fatalf("ReadFile(%s): %v", e.Name(), err)
		}
		out = append(out, migrationFile{version: v, name: e.Name(), sql: string(raw)})
	}
	slices.SortFunc(out, func(a, b migrationFile) int {
		return cmp.Or(cmp.Compare(a.version, b.version), strings.Compare(a.name, b.name))
	})
	return out
}

func TestMigrationCatalogContiguityAndHotfixSchema(t *testing.T) {
	migs := readAllMigrations(t)
	if len(migs) < 21 {
		t.Fatalf("migration count = %d, want >= 21", len(migs))
	}
	for i, m := range migs {
		if m.version != i+1 {
			t.Fatalf("non-contiguous migration at index %d: got %d (%s), want %d", i, m.version, m.name, i+1)
		}
	}
	m20 := migs[19]
	if m20.name != "0020_hotfix_overlays.sql" {
		t.Fatalf("migration 20 = %q, want 0020_hotfix_overlays.sql", m20.name)
	}
	for _, token := range []string{
		"world_hotfix_overlays",
		"overlay_id",
		"target_table",
		"record_guid",
		"field_path",
		"override_json",
		"min_server_build",
		"enabled",
		"reason",
		"created_by",
		"created_at",
		"expires_at",
		"UNIQUE (target_table, record_guid, field_path)",
		"idx_hotfix_overlays_active",
		"hotfix_audit_log",
	} {
		if !strings.Contains(m20.sql, token) {
			t.Errorf("0020_hotfix_overlays.sql missing required token %q", token)
		}
	}
}

func TestThreeSchemaSeparationAndLeastPrivilegeRoles(t *testing.T) {
	migs := readAllMigrations(t)
	m21 := migs[20]
	if m21.name != "0021_three_schema_separation.sql" {
		t.Fatalf("migration 21 = %q, want 0021_three_schema_separation.sql", m21.name)
	}
	schemas, roles, tables, _, _ := parseCatalogAST(migs)
	for _, s := range []string{"auth", "character", "characters", "world", "realm_state"} {
		if !slices.Contains(schemas, s) {
			t.Errorf("missing schema %q in %v", s, schemas)
		}
	}
	for _, r := range []string{"sandal_auth", "sandal_char", "sandal_world", "sandal_admin"} {
		if !slices.Contains(roles, r) {
			t.Errorf("missing least-privilege role %q in %v", r, roles)
		}
	}
	for _, tbl := range []string{"world_hotfix_overlays", "hotfix_audit_log", "buy_orders", "spawner_state"} {
		if !slices.Contains(tables, tbl) {
			t.Errorf("missing table %q in %v", tbl, tables)
		}
	}
}

func TestForeignKeyCoveringIndexAudit(t *testing.T) {
	migs := readAllMigrations(t)
	_, _, _, fks, unindexed := parseCatalogAST(migs)
	if len(fks) < 25 {
		t.Fatalf("foreign key count = %d, want >= 25", len(fks))
	}
	if len(unindexed) != 0 {
		t.Fatalf("found %d unindexed foreign keys across 0001..0021: %+v", len(unindexed), unindexed)
	}
	_, _, _, _, pre20Unindexed := parseCatalogAST(migs[:19])
	if len(pre20Unindexed) == 0 {
		t.Fatal("expected unindexed FKs in 0001..0019 before 0020/0021 index remediation")
	}
}

func parseCatalogAST(migs []migrationFile) ([]string, []string, []string, []fkSpec, []fkSpec) {
	var schemas, roles, tables []string
	var fks []fkSpec
	var idxs []idxSpec
	for _, m := range migs {
		cleaned := stripComments(m.sql)
		for _, sm := range schemaRe.FindAllStringSubmatch(cleaned, -1) {
			schemas = addUnique(schemas, strings.ToLower(sm[1]))
		}
		for _, rm := range roleRe.FindAllStringSubmatch(cleaned, -1) {
			roles = addUnique(roles, strings.ToLower(cmp.Or(rm[1], rm[2])))
		}
		for _, tm := range createTableRe.FindAllStringSubmatch(cleaned, -1) {
			tbl := normIdent(tm[1])
			tables = addUnique(tables, tbl)
			extractTableConstraints(tbl, tm[2], &fks, &idxs)
		}
		for _, im := range createIndexRe.FindAllStringSubmatch(cleaned, -1) {
			idxs = append(idxs, idxSpec{
				name:    normIdent(im[1]),
				table:   normIdent(im[2]),
				columns: splitCols(im[3]),
			})
		}
	}
	var unindexed []fkSpec
	for i := range fks {
		for _, idx := range idxs {
			if idx.table == fks[i].table && hasPrefix(idx.columns, fks[i].columns) {
				fks[i].indexed = true
				fks[i].index = idx.name
				break
			}
		}
		if !fks[i].indexed {
			unindexed = append(unindexed, fks[i])
		}
	}
	return schemas, roles, tables, fks, unindexed
}

func extractTableConstraints(tbl, body string, fks *[]fkSpec, idxs *[]idxSpec) {
	for _, pm := range tablePKRe.FindAllStringSubmatch(body, -1) {
		*idxs = append(*idxs, idxSpec{name: tbl + "_pkey", table: tbl, columns: splitCols(pm[1])})
	}
	for _, pm := range inlinePKRe.FindAllStringSubmatch(body, -1) {
		col := strings.ToLower(strings.TrimSpace(pm[1]))
		if !isKeyword(col) {
			*idxs = append(*idxs, idxSpec{name: tbl + "_pkey", table: tbl, columns: []string{col}})
		}
	}
	for _, fm := range tableFKRe.FindAllStringSubmatch(body, -1) {
		*fks = append(*fks, fkSpec{table: tbl, columns: splitCols(fm[1]), refTable: normIdent(fm[2])})
	}
	for _, fm := range inlineFKRe.FindAllStringSubmatch(body, -1) {
		col := strings.ToLower(strings.TrimSpace(fm[1]))
		if !isKeyword(col) {
			*fks = append(*fks, fkSpec{table: tbl, columns: []string{col}, refTable: normIdent(fm[2])})
		}
	}
}

func stripComments(sqlText string) string {
	lines := strings.Split(sqlText, "\n")
	for i, line := range lines {
		if idx := strings.Index(line, "--"); idx >= 0 {
			lines[i] = line[:idx]
		}
	}
	return strings.Join(lines, "\n")
}

func normIdent(raw string) string {
	trimmed := strings.TrimSpace(strings.ReplaceAll(raw, `"`, ""))
	if dot := strings.LastIndexByte(trimmed, '.'); dot >= 0 {
		trimmed = trimmed[dot+1:]
	}
	return strings.ToLower(trimmed)
}

func splitCols(raw string) []string {
	parts := strings.Split(raw, ",")
	cols := make([]string, 0, len(parts))
	for _, p := range parts {
		fields := strings.Fields(strings.ReplaceAll(p, `"`, ""))
		if len(fields) > 0 {
			cols = append(cols, strings.ToLower(fields[0]))
		}
	}
	return cols
}

func hasPrefix(indexCols, fkCols []string) bool {
	if len(fkCols) == 0 || len(indexCols) < len(fkCols) {
		return false
	}
	for i := range fkCols {
		if indexCols[i] != fkCols[i] {
			return false
		}
	}
	return true
}

func isKeyword(w string) bool {
	return w == "constraint" || w == "primary" || w == "foreign" || w == "unique" || w == "check"
}

func addUnique(slice []string, item string) []string {
	if !slices.Contains(slice, item) {
		return append(slice, item)
	}
	return slice
}
