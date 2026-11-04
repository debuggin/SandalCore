// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package db opens the PostgreSQL pool and applies the embedded migrations.
package db

import (
	"cmp"
	"context"
	"fmt"
	"io/fs"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

var migrations = newSQLMigrationsFS()

type sqlMigrationsFS struct {
	root fs.FS
}

func newSQLMigrationsFS() sqlMigrationsFS {
	for _, candidate := range []string{"sql", "services/sql", "src/server/services/sql", "../sql", "../../sql", "../../../services/sql", "../../../sql"} {
		if info, err := os.Stat(filepath.Join(candidate, "migrations")); err == nil && info.IsDir() {
			return sqlMigrationsFS{root: os.DirFS(candidate)}
		}
	}
	if _, callerFile, _, ok := runtime.Caller(0); ok {
		for _, rel := range [][]string{{"..", "..", "sql"}, {"..", "..", "..", "services", "sql"}, {"..", "..", "..", "sql"}} {
			parts := append([]string{filepath.Dir(callerFile)}, rel...)
			dir := filepath.Clean(filepath.Join(parts...))
			if info, err := os.Stat(filepath.Join(dir, "migrations")); err == nil && info.IsDir() {
				return sqlMigrationsFS{root: os.DirFS(dir)}
			}
		}
		repoSQL := filepath.Clean(filepath.Join(filepath.Dir(callerFile), "..", "..", "sql"))
		return sqlMigrationsFS{root: os.DirFS(repoSQL)}
	}
	return sqlMigrationsFS{root: os.DirFS("sql")}
}

func (s sqlMigrationsFS) Open(name string) (fs.File, error) {
	return s.root.Open(name)
}

func (s sqlMigrationsFS) ReadDir(name string) ([]fs.DirEntry, error) {
	return fs.ReadDir(s.root, name)
}

func (s sqlMigrationsFS) ReadFile(name string) ([]byte, error) {
	return fs.ReadFile(s.root, name)
}

// migrationLock is the advisory-lock key that serialises Migrate across
// services starting together.
const migrationLock = 7_320_001

// cleanupTimeout bounds releasing the migration lock (and, failing that,
// closing the connection that holds it).
const cleanupTimeout = 5 * time.Second

// [C O A x C Y B]

// migrationName matches NNNN_name.sql: a four-digit version, an underscore and
// a non-empty description.
var migrationName = regexp.MustCompile(`^(\d{4})_.+\.sql$`)

// Open connects to the PostgreSQL server at url and checks the connection with
// a ping. Errors from pgx never include the password, so they are safe to log.
// The server's warnings, and the notices a migration raises (their text starts
// with "migration "), go to the log (logNotice).
func Open(ctx context.Context, url string) (*pgxpool.Pool, error) {
	cfg, err := pgxpool.ParseConfig(url)
	if err != nil {
		return nil, err
	}
	cfg.ConnConfig.OnNotice = logNotice
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		return nil, err
	}
	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("database ping: %w", err)
	}
	return pool, nil
}

// logNotice logs what PostgreSQL says outside a result: a warning at WARN, a
// migration's own notice ("migration 0003: ...") at INFO, and every other
// notice (such as "relation already exists, skipping") at DEBUG.
func logNotice(_ *pgconn.PgConn, n *pgconn.Notice) {
	switch {
	case n.SeverityUnlocalized == "WARNING":
		slog.Warn("postgres: " + n.Message)
	case strings.HasPrefix(n.Message, "migration "):
		slog.Info("postgres: " + n.Message)
	default:
		slog.Debug("postgres: "+n.Message, "severity", n.SeverityUnlocalized, "code", n.Code)
	}
}

// Migrate applies the embedded migrations/NNNN_*.sql files that are not yet
// recorded in schema_migrations, in version order, each in its own
// transaction. It holds a PostgreSQL advisory lock while it works, so services
// starting together can all call it safely.
func Migrate(ctx context.Context, pool *pgxpool.Pool) error {
	sub, err := fs.Sub(migrations, "migrations")
	if err != nil {
		return err
	}
	return migrate(ctx, pool, sub)
}

// migration is one NNNN_name.sql file.
type migration struct {
	version int
	name    string
	sql     string
}

// loadMigrations reads every *.sql file at the root of fsys and returns them
// in version order. Every such file must be named NNNN_name.sql, and no two
// files may share a version. It touches no database, so a bad set of files is
// rejected before anything is applied.
func loadMigrations(fsys fs.FS) ([]migration, error) {
	entries, err := fs.ReadDir(fsys, ".")
	if err != nil {
		return nil, err
	}
	var list []migration
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, ".sql") {
			continue
		}
		m := migrationName.FindStringSubmatch(name)
		if m == nil {
			return nil, fmt.Errorf("migration %s: name must be NNNN_name.sql with a four-digit version", name)
		}
		version, err := strconv.Atoi(m[1])
		if err != nil {
			return nil, fmt.Errorf("migration %s: %w", name, err)
		}
		body, err := fs.ReadFile(fsys, name)
		if err != nil {
			return nil, err
		}
		list = append(list, migration{version: version, name: name, sql: string(body)})
	}
	slices.SortFunc(list, func(a, b migration) int {
		return cmp.Or(cmp.Compare(a.version, b.version), strings.Compare(a.name, b.name))
	})
	for i := 1; i < len(list); i++ {
		if list[i].version == list[i-1].version {
			return nil, fmt.Errorf("migrations %s and %s share version %d", list[i-1].name, list[i].name, list[i].version)
		}
	}
	return list, nil
}

// -- C | O | A x Cyb --

// migrate is Migrate over an arbitrary file system, so tests can feed it
// hand-made migrations.
func migrate(ctx context.Context, pool *pgxpool.Pool, fsys fs.FS) error {
	list, err := loadMigrations(fsys)
	if err != nil {
		return err
	}

	conn, err := pool.Acquire(ctx)
	if err != nil {
		return err
	}
	defer conn.Release()
	if _, err := conn.Exec(ctx, "SELECT pg_advisory_lock($1)", migrationLock); err != nil {
		return err
	}
	// Deferred after Release, so it runs first, while the connection is ours.
	defer unlock(conn)

	if _, err := conn.Exec(ctx, `CREATE TABLE IF NOT EXISTS schema_migrations (
		version    INT PRIMARY KEY,
		applied_at TIMESTAMPTZ NOT NULL DEFAULT now())`); err != nil {
		return err
	}
	for _, m := range list {
		var done bool
		if err := conn.QueryRow(ctx,
			"SELECT EXISTS (SELECT 1 FROM schema_migrations WHERE version = $1)", m.version).Scan(&done); err != nil {
			return err
		}
		if done {
			continue
		}
		if err := apply(ctx, conn, m); err != nil {
			return err
		}
	}
	return nil
}

// apply runs one migration and records its version in a single transaction.
func apply(ctx context.Context, conn *pgxpool.Conn, m migration) error {
	tx, err := conn.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx) // a no-op once committed
	if _, err := tx.Exec(ctx, m.sql); err != nil {
		return fmt.Errorf("migration %s: %w", m.name, err)
	}
	if _, err := tx.Exec(ctx, "INSERT INTO schema_migrations (version) VALUES ($1)", m.version); err != nil {
		return fmt.Errorf("migration %s: record version: %w", m.name, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("migration %s: commit: %w", m.name, err)
	}
	return nil
}

// unlock releases the migration lock taken on conn. If that cannot be
// confirmed, it destroys the connection instead of leaving it for the pool to
// reuse, so the session, and the lock with it, dies.
func unlock(conn *pgxpool.Conn) {
	ctx, cancel := context.WithTimeout(context.Background(), cleanupTimeout)
	defer cancel()
	var released bool
	if err := conn.QueryRow(ctx, "SELECT pg_advisory_unlock($1)", migrationLock).Scan(&released); err == nil && released {
		return
	}
	// Hijack takes the connection out of the pool, so the deferred Release is
	// then a no-op.
	_ = conn.Hijack().Close(ctx)
}

type ForeignKeySpec struct {
	Table         string
	Columns       []string
	RefTable      string
	Indexed       bool
	CoveringIndex string
}

type MigrationCatalogReport struct {
	Versions       []int
	Names          []string
	Schemas        []string
	Roles          []string
	Tables         []string
	Indexes        []string
	ForeignKeys    []ForeignKeySpec
	UnindexedFKs   []ForeignKeySpec
	HasHotfix0020  bool
	HasSchema0021  bool
	AllFKsIndexed  bool
	ThreeSchemasOK bool
	FourRolesOK    bool
}

type HotfixOverlay struct {
	OverlayID      int64
	TargetTable    string
	RecordGUID     int64
	FieldPath      string
	OverrideJSON   []byte
	MinServerBuild int32
	Enabled        bool
	Reason         string
	CreatedBy      string
	CreatedAt      time.Time
	ExpiresAt      *time.Time
}

type indexSpec struct {
	name    string
	table   string
	columns []string
}

var (
	sqlSchemaRe    = regexp.MustCompile(`(?im)^\s*CREATE\s+SCHEMA\s+(?:IF\s+NOT\s+EXISTS\s+)?(?:"?([a-zA-Z0-9_]+)"?)\s*;`)
	sqlRoleLiteral = regexp.MustCompile(`'(sandal_[a-z0-9_]+)'|\bCREATE\s+ROLE\s+([a-zA-Z0-9_]+)`)
	sqlCreateTable = regexp.MustCompile(`(?is)CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?([a-zA-Z0-9_."]+)\s*\((.*?)\)\s*;`)
	sqlCreateIndex = regexp.MustCompile(`(?is)CREATE\s+(?:UNIQUE\s+)?INDEX\s+(?:IF\s+NOT\s+EXISTS\s+)?([a-zA-Z0-9_]+)\s+ON\s+([a-zA-Z0-9_."]+)\s*\(([^)]+)\)`)
	sqlTablePKRe   = regexp.MustCompile(`(?is)\bPRIMARY\s+KEY\s*\(([^)]+)\)`)
	sqlInlinePKRe  = regexp.MustCompile(`(?im)^\s*([a-zA-Z0-9_]+)\s+[a-zA-Z0-9_]+(?:\s*\([^)]*\))?[^,\n]*?\bPRIMARY\s+KEY\b`)
	sqlTableFKRe   = regexp.MustCompile(`(?is)\bFOREIGN\s+KEY\s*\(([^)]+)\)\s*REFERENCES\s+([a-zA-Z0-9_."]+)`)
	sqlInlineFKRe  = regexp.MustCompile(`(?im)^\s*([a-zA-Z0-9_]+)\s+[a-zA-Z0-9_]+(?:\s*\([^)]*\))?[^,\n]*?\bREFERENCES\s+([a-zA-Z0-9_."]+)`)
)

func InspectMigrationCatalog() (*MigrationCatalogReport, error) {
	sub, err := fs.Sub(newSQLMigrationsFS(), "migrations")
	if err != nil {
		return nil, err
	}
	return AuditMigrationCatalog(sub)
}

func AuditMigrationCatalog(fsys fs.FS) (*MigrationCatalogReport, error) {
	list, err := loadMigrations(fsys)
	if err != nil {
		return nil, err
	}
	if len(list) == 0 {
		return nil, fmt.Errorf("migration catalog is empty")
	}
	report := &MigrationCatalogReport{}
	var idxSpecs []indexSpec
	for i, m := range list {
		if m.version != i+1 {
			return nil, fmt.Errorf("non-contiguous migration version: got %d at index %d (%s)", m.version, i, m.name)
		}
		report.Versions = append(report.Versions, m.version)
		report.Names = append(report.Names, m.name)
		if m.name == "0020_hotfix_overlays.sql" {
			report.HasHotfix0020 = true
		}
		if m.name == "0021_three_schema_separation.sql" {
			report.HasSchema0021 = true
		}
		collectMigrationAST(m.sql, report, &idxSpecs)
	}
	finalizeCatalogReport(report, idxSpecs)
	if err := validateCatalogReport(report); err != nil {
		return report, err
	}
	return report, nil
}

func collectMigrationAST(sqlText string, report *MigrationCatalogReport, idxSpecs *[]indexSpec) {
	cleaned := stripSQLLineComments(sqlText)
	for _, sm := range sqlSchemaRe.FindAllStringSubmatch(cleaned, -1) {
		report.Schemas = appendUnique(report.Schemas, strings.ToLower(sm[1]))
	}
	for _, rm := range sqlRoleLiteral.FindAllStringSubmatch(cleaned, -1) {
		role := cmp.Or(rm[1], rm[2])
		report.Roles = appendUnique(report.Roles, strings.ToLower(role))
	}
	for _, tm := range sqlCreateTable.FindAllStringSubmatch(cleaned, -1) {
		tbl := normalizeSQLIdent(tm[1])
		body := tm[2]
		report.Tables = appendUnique(report.Tables, tbl)
		collectTableKeys(tbl, body, report, idxSpecs)
	}
	for _, im := range sqlCreateIndex.FindAllStringSubmatch(cleaned, -1) {
		idxName := normalizeSQLIdent(im[1])
		tbl := normalizeSQLIdent(im[2])
		cols := parseSQLColumnList(im[3])
		report.Indexes = appendUnique(report.Indexes, idxName)
		*idxSpecs = append(*idxSpecs, indexSpec{name: idxName, table: tbl, columns: cols})
	}
}

func collectTableKeys(tbl, body string, report *MigrationCatalogReport, idxSpecs *[]indexSpec) {
	for _, pm := range sqlTablePKRe.FindAllStringSubmatch(body, -1) {
		*idxSpecs = append(*idxSpecs, indexSpec{
			name:    tbl + "_pkey",
			table:   tbl,
			columns: parseSQLColumnList(pm[1]),
		})
	}
	for _, pm := range sqlInlinePKRe.FindAllStringSubmatch(body, -1) {
		col := strings.ToLower(strings.TrimSpace(pm[1]))
		if !isSQLConstraintKeyword(col) {
			*idxSpecs = append(*idxSpecs, indexSpec{name: tbl + "_pkey", table: tbl, columns: []string{col}})
		}
	}
	for _, fm := range sqlTableFKRe.FindAllStringSubmatch(body, -1) {
		report.ForeignKeys = append(report.ForeignKeys, ForeignKeySpec{
			Table:    tbl,
			Columns:  parseSQLColumnList(fm[1]),
			RefTable: normalizeSQLIdent(fm[2]),
		})
	}
	for _, fm := range sqlInlineFKRe.FindAllStringSubmatch(body, -1) {
		col := strings.ToLower(strings.TrimSpace(fm[1]))
		if !isSQLConstraintKeyword(col) {
			report.ForeignKeys = append(report.ForeignKeys, ForeignKeySpec{
				Table:    tbl,
				Columns:  []string{col},
				RefTable: normalizeSQLIdent(fm[2]),
			})
		}
	}
}

func finalizeCatalogReport(report *MigrationCatalogReport, idxSpecs []indexSpec) {
	for i := range report.ForeignKeys {
		fk := &report.ForeignKeys[i]
		for _, idx := range idxSpecs {
			if idx.table == fk.Table && hasSQLColumnPrefix(idx.columns, fk.Columns) {
				fk.Indexed = true
				fk.CoveringIndex = idx.name
				break
			}
		}
		if !fk.Indexed {
			report.UnindexedFKs = append(report.UnindexedFKs, *fk)
		}
	}
	report.AllFKsIndexed = len(report.ForeignKeys) > 0 && len(report.UnindexedFKs) == 0
	hasAuth := slices.Contains(report.Schemas, "auth")
	hasChar := slices.Contains(report.Schemas, "character") || slices.Contains(report.Schemas, "characters")
	hasWorld := slices.Contains(report.Schemas, "world") || slices.Contains(report.Schemas, "realm_state")
	report.ThreeSchemasOK = hasAuth && hasChar && hasWorld
	report.FourRolesOK = slices.Contains(report.Roles, "sandal_auth") &&
		slices.Contains(report.Roles, "sandal_char") &&
		slices.Contains(report.Roles, "sandal_world") &&
		slices.Contains(report.Roles, "sandal_admin")
}

func validateCatalogReport(report *MigrationCatalogReport) error {
	if !report.HasHotfix0020 || !report.HasSchema0021 {
		return fmt.Errorf("missing required migrations 0020 (%v) or 0021 (%v)", report.HasHotfix0020, report.HasSchema0021)
	}
	if !report.ThreeSchemasOK {
		return fmt.Errorf("missing required 3-schema separation namespaces: got %v", report.Schemas)
	}
	if !report.FourRolesOK {
		return fmt.Errorf("missing required least-privilege roles: got %v", report.Roles)
	}
	for _, reqTable := range []string{"world_hotfix_overlays", "hotfix_audit_log", "buy_orders", "spawner_state"} {
		if !slices.Contains(report.Tables, reqTable) {
			return fmt.Errorf("missing required table %q in migration catalog", reqTable)
		}
	}
	if !report.AllFKsIndexed {
		return fmt.Errorf("unindexed foreign keys detected: %+v", report.UnindexedFKs)
	}
	return nil
}

func stripSQLLineComments(sqlText string) string {
	lines := strings.Split(sqlText, "\n")
	for i, line := range lines {
		if idx := strings.Index(line, "--"); idx >= 0 {
			lines[i] = line[:idx]
		}
	}
	return strings.Join(lines, "\n")
}

func normalizeSQLIdent(raw string) string {
	trimmed := strings.TrimSpace(strings.ReplaceAll(raw, `"`, ""))
	if dot := strings.LastIndexByte(trimmed, '.'); dot >= 0 {
		trimmed = trimmed[dot+1:]
	}
	return strings.ToLower(trimmed)
}

func parseSQLColumnList(raw string) []string {
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

func hasSQLColumnPrefix(indexCols, fkCols []string) bool {
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

func isSQLConstraintKeyword(word string) bool {
	switch word {
	case "constraint", "primary", "foreign", "unique", "check":
		return true
	default:
		return false
	}
}

func appendUnique(slice []string, item string) []string {
	if !slices.Contains(slice, item) {
		return append(slice, item)
	}
	return slice
}

func ActiveHotfixOverlaysSQL() string {
	return `SELECT overlay_id, target_table, record_guid, field_path, override_json,
       min_server_build, enabled, reason, created_by, created_at, expires_at
  FROM world.world_hotfix_overlays
 WHERE target_table = $1
   AND enabled = TRUE
   AND min_server_build <= $2
   AND (expires_at IS NULL OR expires_at > NOW())
 ORDER BY record_guid ASC, field_path ASC, overlay_id ASC`
}

func QueryActiveHotfixOverlays(
	ctx context.Context,
	pool *pgxpool.Pool,
	targetTable string,
	serverBuild int32,
) ([]HotfixOverlay, error) {
	if strings.TrimSpace(targetTable) == "" {
		return nil, fmt.Errorf("targetTable must not be empty")
	}
	if serverBuild < 0 {
		return nil, fmt.Errorf("serverBuild must be non-negative: %d", serverBuild)
	}
	if pool == nil {
		return nil, fmt.Errorf("database pool is nil")
	}
	rows, err := pool.Query(ctx, ActiveHotfixOverlaysSQL(), targetTable, serverBuild)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []HotfixOverlay
	for rows.Next() {
		var h HotfixOverlay
		if err := rows.Scan(
			&h.OverlayID,
			&h.TargetTable,
			&h.RecordGUID,
			&h.FieldPath,
			&h.OverrideJSON,
			&h.MinServerBuild,
			&h.Enabled,
			&h.Reason,
			&h.CreatedBy,
			&h.CreatedAt,
			&h.ExpiresAt,
		); err != nil {
			return nil, err
		}
		out = append(out, h)
	}
	return out, rows.Err()
}

