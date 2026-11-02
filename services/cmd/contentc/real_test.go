// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"hash"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
	"aocrop/services/internal/usmap"
)

// Inputs stay outside the repo (AOC_DESIGN_DB, AOC_USMAP, optional AOC_CLIENT_CONFIG) and skip when unset.
func realInputs(t *testing.T) (dbc, usmap, config string) {
	t.Helper()
	dbc, usmap, config = os.Getenv("AOC_DESIGN_DB"), os.Getenv("AOC_USMAP"), os.Getenv("AOC_CLIENT_CONFIG")
	if dbc == "" || usmap == "" {
		t.Skip("AOC_DESIGN_DB and AOC_USMAP not set")
	}
	for _, p := range []string{dbc, usmap} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("%v", err)
		}
	}
	return dbc, usmap, config
}

// [CoA ~ C:Y:B]

var built struct {
	once   sync.Once
	dir    string
	path   string
	stdout string
	stderr string
	code   int
}

func TestMain(m *testing.M) {
	code := m.Run()
	if built.dir != "" {
		os.RemoveAll(built.dir)
	}
	os.Exit(code)
}

func builtDB(t *testing.T) *content.DB {
	t.Helper()
	dbc, usmap, config := realInputs(t)
	built.once.Do(func() {
		var err error
		if built.dir, err = os.MkdirTemp("", "contentc-test-"); err != nil {
			t.Fatal(err)
		}
		built.path = filepath.Join(built.dir, "out", "content.db")
		var stdout, stderr bytes.Buffer
		args := []string{"--dbc", dbc, "--usmap", usmap, "--out", built.path}
		if config != "" {
			args = append(args, "--config", config)
		}
		built.code = run(args, &stdout, &stderr)
		built.stdout, built.stderr = stdout.String(), stderr.String()
	})
	if built.code != 0 {
		t.Fatalf("contentc exited %d: %s", built.code, built.stderr)
	}
	db, err := content.Open(built.path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

func scalarInt(t *testing.T, db *content.DB, query string, args ...any) int64 {
	t.Helper()
	var n int64
	if err := db.SQL().QueryRow(query, args...).Scan(&n); err != nil {
		t.Fatalf("%s: %v", query, err)
	}
	return n
}

func scalarGuid(t *testing.T, db *content.DB, table, name string) int64 {
	t.Helper()
	return scalarInt(t, db, `SELECT guid FROM `+table+` WHERE name = ?`, name)
}

// Client-generated usmap lists AbilityHitRecordBase.bMayCorrupt whereas the older usmap does not.
func generatedUsmap(t *testing.T, path string) bool {
	t.Helper()
	m, err := usmap.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range m.Schema("AbilityHitRecordBase").Props {
		if p.Name == "bMayCorrupt" {
			return true
		}
	}
	return false
}

func expectedPatches(generated bool) (status map[string]string, applied []string) {
	status = map[string]string{}
	applied = []string{}
	for _, p := range designdb.Patches() {
		s := designdb.PatchApplied
		if generated {
			s = designdb.PatchPresent
		}
		status[p.String()] = s.String()
		if s == designdb.PatchApplied {
			applied = append(applied, p.String())
		}
	}
	sort.Strings(applied)
	return status, applied
}

// CoA with C:Y:B

func fileSHA256AndSize(t *testing.T, path string) (string, int64) {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:]), int64(len(data))
}

func verifyRealBuildMeta(t *testing.T, meta map[string]string, dbcPath, usmapPath string) {
	t.Helper()
	patchStatus, patchesApplied := expectedPatches(generatedUsmap(t, usmapPath))
	statusJSON, _ := json.Marshal(patchStatus)
	appliedJSON, _ := json.Marshal(patchesApplied)
	dbcSum, dbcSize := fileSHA256AndSize(t, dbcPath)
	mapSum, mapSize := fileSHA256AndSize(t, usmapPath)
	for key, want := range map[string]string{
		"schema_version":          "1",
		"record.encoding":         content.EncodingGzip,
		"contentc.version":        Version,
		"source.dbc.sha256":       dbcSum,
		"source.dbc.size":         strconv.FormatInt(dbcSize, 10),
		"source.dbc.tables":       "369",
		"source.usmap.sha256":     mapSum,
		"source.usmap.size":       strconv.FormatInt(mapSize, 10),
		"decode.tables":           "369",
		"decode.rows":             "205882",
		"decode.skipped_tables":   "0",
		"decode.nonfinite_floats": "0",
		"decode.patches":          string(appliedJSON),
		"decode.patch_status":     string(statusJSON),
	} {
		if meta[key] != want {
			t.Errorf("meta %s = %q, want %q", key, meta[key], want)
		}
	}
	if name := filepath.Base(dbcPath); meta["source.dbc.name"] != name {
		t.Errorf("meta source.dbc.name = %q, want %q", meta["source.dbc.name"], name)
	}
	if !strings.HasPrefix(meta["source.dbc.crc"], "ok:") {
		t.Errorf("meta source.dbc.crc = %q", meta["source.dbc.crc"])
	}
	if meta["source.dbc.generated_at"] != "2026-01-20T01:53:09Z" {
		t.Errorf("meta source.dbc.generated_at = %q", meta["source.dbc.generated_at"])
	}
	if !strings.Contains(meta["note.0_AoCEvolve"], "0_AoCEvolve.dbc") || !strings.Contains(meta["note.0_AoCEvolve"], "6 tables") {
		t.Errorf("meta note.0_AoCEvolve = %q", meta["note.0_AoCEvolve"])
	}
	for _, k := range []string{"built_at", "build_seconds", "source.dbc.path", "source.usmap.path", "typed.stat_def", "typed.class_stat_base"} {
		if meta[k] == "" {
			t.Errorf("meta %s is empty", k)
		}
	}
}

func TestRealBuild(t *testing.T) {
	dbcPath, usmapPath, _ := realInputs(t)
	db := builtDB(t)
	if !strings.Contains(built.stdout, "369 tables decoded of 369, 205882 rows") {
		t.Errorf("summary line: %s", built.stdout)
	}
	t.Logf("summary: %s", strings.TrimSpace(built.stdout[strings.LastIndex(strings.TrimSpace(built.stdout), "\n")+1:]))
	st, err := os.Stat(built.path)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("content.db: %d MB", st.Size()>>20)
	if st.Size() > 330<<20 {
		t.Errorf("content.db is %d MB, over 330 MB", st.Size()>>20)
	}
	if _, err := os.Stat(built.path + ".tmp"); err == nil {
		t.Error("the temporary file was left behind")
	}
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	verifyRealBuildMeta(t, meta, dbcPath, usmapPath)
}

func verifyRealTableRowCounts(t *testing.T, db *content.DB, tables []content.RecordTable, perTable map[string]int) {
	t.Helper()
	decoded, total := 0, 0
	for _, tb := range tables {
		inRecord := int(scalarInt(t, db, `SELECT count(*) FROM record WHERE table_index = ?`, tb.Index))
		if !tb.Decoded {
			if inRecord != 0 {
				t.Errorf("table %d did not decode but has %d records", tb.Index, inRecord)
			}
			if _, ok := perTable[strconv.Itoa(tb.Index)]; ok {
				t.Errorf("meta counts the undecoded table %d", tb.Index)
			}
			continue
		}
		decoded++
		total += inRecord
		if inRecord != tb.Rows || perTable[strconv.Itoa(tb.Index)] != tb.Rows {
			t.Errorf("table %d (%s): directory %d rows, record %d, meta %d", tb.Index, tb.Schema, tb.Rows, inRecord, perTable[strconv.Itoa(tb.Index)])
		}
	}
	if decoded != 369 || total != 205882 || len(perTable) != 369 {
		t.Errorf("decoded %d tables, %d rows (meta lists %d tables); want 369, 205882", decoded, total, len(perTable))
	}
}

func verifyRealTypedTableCounts(t *testing.T, db *content.DB, tables []content.RecordTable) {
	t.Helper()
	for _, w := range []struct {
		table  int
		schema string
		rows   int
	}{
		{0, "AoCAbilityRecordBase", 2130},
		{2, "AbilityHitRecordBase", 3608},
		{3, "StatTypeDefRecordBase", 732},
		{4, "EffectRecordBase", 2794},
		{44, "CharacterClassesRecordBase", 16},
		{296, "StatCurveRecordBase", 122},
		{303, "StatInitializerListRecordBase", 145},
		{307, "StatModRecordBase", 3133},
	} {
		tb := tables[w.table]
		if tb.Schema != w.schema || tb.Rows != w.rows || !tb.Decoded {
			t.Errorf("table %d = %+v, want %s with %d rows, decoded", w.table, tb, w.schema, w.rows)
		}
	}
	for table, count := range map[string]int64{
		"stat_def":        732,
		"stat_curve_def":  122,
		"stat_curve":      3067,
		"stat_init":       1087,
		"class":           8,
		"class_stat_base": 8 * 6 * 50,
		"ability":         3,
		"ability_cost":    2,
		"effect":          14,
		"hit":             14,
		"ability_runtime": 3,
		"effect_runtime":  4,
	} {
		if got := scalarInt(t, db, `SELECT count(*) FROM `+table); got != count {
			t.Errorf("%s has %d rows, want %d", table, got, count)
		}
	}
}

func TestRealRecordCounts(t *testing.T) {
	db := builtDB(t)
	meta, err := db.Meta()
	if err != nil {
		t.Fatal(err)
	}
	var perTable map[string]int
	if err := json.Unmarshal([]byte(meta["decode.table_rows"]), &perTable); err != nil {
		t.Fatal(err)
	}
	tables, err := db.RecordTables()
	if err != nil {
		t.Fatal(err)
	}
	if len(tables) != 369 {
		t.Errorf("record_table has %d tables, want 369", len(tables))
	}
	verifyRealTableRowCounts(t, db, tables, perTable)
	verifyRealTypedTableCounts(t, db, tables)
}

// built by C O A with Cyb

func verifyRealStaminaRecordAndStatDef(t *testing.T, db *content.DB) {
	t.Helper()
	var guid, tid int64
	if err := db.SQL().QueryRow(`SELECT type_id, guid FROM record WHERE table_index = 230 LIMIT 1`).Scan(&tid, &guid); err != nil {
		t.Fatal(err)
	}
	if r, err := db.Record(uint64(tid), guid); err != nil || r.Schema != "PopulationInstanceRecordBase" || !json.Valid(r.Fields) {
		t.Errorf("a PopulationInstance record = %+v, %v", r, err)
	}
	srec, err := db.Record(content.TypeStatDef, content.GuidStamina)
	if err != nil {
		t.Fatal(err)
	}
	var sf struct {
		ValueMax    string
		RegenRate   string
		Replication int
	}
	if err := json.Unmarshal(srec.Fields, &sf); err != nil {
		t.Fatal(err)
	}
	s, _ := db.StatDef(content.GuidStamina)
	if sf.ValueMax != s.MaxExpr || sf.RegenRate != s.RegenExpr || sf.Replication != s.Replication {
		t.Errorf("Stamina record %+v differs from stat_def %+v", sf, s)
	}
}

func TestRealRecord(t *testing.T) {
	db := builtDB(t)
	rec, err := db.Record(content.TypeAbility, content.GuidSprint)
	if err != nil {
		t.Fatal(err)
	}
	if rec.Name != "General_Sprint" || rec.TableIndex != 0 || rec.Schema != "AoCAbilityRecordBase" || rec.DevOnly {
		t.Errorf("Sprint record = %+v", rec)
	}
	var f struct {
		Cooldown string
		Costs    []struct {
			Enabled    string
			StatCosts  []struct{ Value string }
			ChargeCost string
		}
		AbilityName struct{ Source string }
		HitsIds     []struct{ Key int }
		EffectsIds  []struct {
			Key   int
			Value struct{ Guid int64 }
		}
	}
	if err := json.Unmarshal(rec.Fields, &f); err != nil {
		t.Fatal(err)
	}
	if f.Cooldown != "0" || len(f.Costs) != 1 || f.Costs[0].Enabled != "IsMoving(GetOwner())" || f.AbilityName.Source != "Sprint" ||
		len(f.Costs[0].StatCosts) != 1 || !strings.Contains(f.Costs[0].StatCosts[0].Value, "var Cost = 1.25;") ||
		len(f.EffectsIds) != 1 || f.EffectsIds[0].Value.Guid != 6064631641826394197 {
		t.Errorf("Sprint fields = %s", rec.Fields)
	}
	verifyRealStaminaRecordAndStatDef(t, db)
}

func writeScannedValue(h hash.Hash, v any) {
	switch x := v.(type) {
	case []byte:
		h.Write(x)
	case string:
		h.Write([]byte(x))
	default:
		fmt.Fprintf(h, "%v", x)
	}
	h.Write([]byte{0})
}

func hashTableRows(t *testing.T, db *content.DB, query string) string {
	t.Helper()
	r, err := db.SQL().Query(query)
	if err != nil {
		t.Fatalf("%s: %v", query, err)
	}
	defer r.Close()
	cols, _ := r.Columns()
	h := sha256.New()
	n := 0
	for r.Next() {
		vals := make([]any, len(cols))
		ptrs := make([]any, len(cols))
		for i := range vals {
			ptrs[i] = &vals[i]
		}
		if err := r.Scan(ptrs...); err != nil {
			t.Fatal(err)
		}
		for _, v := range vals {
			writeScannedValue(h, v)
		}
		n++
	}
	return fmt.Sprintf("%d rows, %x", n, h.Sum(nil)[:8])
}

func dump(t *testing.T, db *content.DB) map[string]string {
	t.Helper()
	rows, err := db.SQL().Query(`SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name`)
	if err != nil {
		t.Fatal(err)
	}
	var tables []string
	for rows.Next() {
		var n string
		if err := rows.Scan(&n); err != nil {
			t.Fatal(err)
		}
		tables = append(tables, n)
	}
	rows.Close()
	sort.Strings(tables)
	out := map[string]string{}
	for _, table := range tables {
		query := `SELECT * FROM ` + table + ` ORDER BY rowid`
		if table == "meta" {
			query = `SELECT * FROM meta WHERE key NOT IN ('built_at', 'build_seconds', 'source.dbc.path', 'source.usmap.path') ORDER BY key`
		} else if _, err := db.SQL().Exec(`SELECT rowid FROM ` + table + ` LIMIT 0`); err != nil {
			query = `SELECT * FROM ` + table
		}
		out[table] = hashTableRows(t, db, query)
	}
	return out
}

// ~ Cyb :: C O A ~

// Gzip output in record.data is deterministic for a fixed Go toolchain version.
func TestRealDeterministic(t *testing.T) {
	dbc, usmap, config := realInputs(t)
	first := builtDB(t)

	dir := t.TempDir()
	path := filepath.Join(dir, "content.db")
	var stdout, stderr bytes.Buffer
	args := []string{"--dbc", dbc, "--usmap", usmap, "--out", path}
	if config != "" {
		args = append(args, "--config", config)
	}
	if code := run(args, &stdout, &stderr); code != 0 {
		t.Fatalf("second build exited %d: %s", code, stderr.String())
	}
	second, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()

	a, b := dump(t, first), dump(t, second)
	for table := range a {
		if a[table] != b[table] {
			t.Errorf("table %s differs between two builds: %s, %s", table, a[table], b[table])
		}
	}
	if len(a) != len(b) || len(a) < 20 {
		t.Errorf("%d and %d tables compared", len(a), len(b))
	}
	t.Logf("compared %d tables, e.g. record: %s", len(a), a["record"])

	before, _ := os.ReadFile(path)
	badArgs := []string{"--dbc", dbc, "--usmap", filepath.Join(dir, "no-such.usmap"), "--out", path}
	if code := run(badArgs, &stdout, &stderr); code == 0 {
		t.Error("a build with a missing usmap succeeded")
	}
	after, _ := os.ReadFile(path)
	if !bytes.Equal(before, after) {
		t.Error("a failed build changed the existing output")
	}
	if _, err := os.Stat(path + ".tmp"); err == nil {
		t.Error("a failed build left its temporary file")
	}
}

func TestRealRejectsACorruptDesignDatabase(t *testing.T) {
	dbc, usmap, _ := realInputs(t)
	data, err := os.ReadFile(dbc)
	if err != nil {
		t.Fatal(err)
	}
	data[len(data)/2] ^= 0x40
	dir := t.TempDir()
	corrupt := filepath.Join(dir, "CacheDB.dbc")
	if err := os.WriteFile(corrupt, data, 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(dir, "out", "content.db")
	var stdout, stderr bytes.Buffer
	code := run([]string{"--dbc", corrupt, "--usmap", usmap, "--out", out}, &stdout, &stderr)
	if code != 1 || !strings.Contains(stderr.String(), "fails its own checksums") {
		t.Errorf("exit %d, stderr %q; want exit 1 and the checksum failure", code, stderr.String())
	}
	for _, p := range []string{out, out + ".tmp"} {
		if _, err := os.Stat(p); err == nil {
			t.Errorf("%s was written", p)
		}
	}
}

func TestRealRejectsAPatchThatFitsNothing(t *testing.T) {
	dbc, usmap, _ := realInputs(t)
	boolProp := designdb.Patches()[0].Prop
	noSuchProp := boolProp
	noSuchProp.Name = "NoSuchProp"
	for _, tt := range []struct {
		patch designdb.Patch
		want  string
	}{
		{designdb.Patch{Struct: "AbilityHitRecordBase", After: "NoSuchProperty", Prop: noSuchProp, Why: "test"},
			"AbilityHitRecordBase.NoSuchProp after NoSuchProperty: anchor missing"},
		{designdb.Patch{Struct: "NoSuchRecordBase", After: "X", Prop: boolProp, Why: "test"},
			"NoSuchRecordBase.bMayCorrupt after X: struct missing"},
	} {
		out := filepath.Join(t.TempDir(), "content.db")
		_, err := Build(context.Background(), Options{DBC: dbc, USMap: usmap, Out: out, Encoding: content.EncodingJSON,
			Patches: []designdb.Patch{tt.patch}})
		if err == nil || !strings.Contains(err.Error(), tt.want) {
			t.Errorf("patch %v: Build = %v, want an error containing %q", tt.patch, err, tt.want)
		}
		if _, err := os.Stat(out); err == nil {
			t.Errorf("patch %v: content.db was written", tt.patch)
		}
	}
}

// (CoA ~ C:Y:B)

func verifyRealSprintRuntime(t *testing.T, sprint content.AbilityRuntime) {
	t.Helper()
	if sprint.Name != "General_Sprint" || sprint.HeldBehavior != "Smart" || sprint.RequiresOutOfCombat || sprint.MinStamina != 1 ||
		sprint.CostStatGuid != content.GuidStamina || sprint.CostAmount != 1.25 || !sprint.CostRequiresMoving || sprint.ChargeInterval != 0.25 ||
		sprint.Cooldown != 0 || sprint.TriggersCooldown || sprint.DisableDuringCooldown || sprint.TargetsSelf ||
		sprint.EffectGuid != 6064631641826394197 || sprint.CancelSection != "End" ||
		sprint.MontagePath != "/Game/Characters/Animations/00_ABLT/CM_M_ABLT_Sprint_Montage.CM_M_ABLT_Sprint_Montage" {
		t.Errorf("Sprint = %+v", sprint)
	}
}

func verifyRealDodgeRuntime(t *testing.T, dodge content.AbilityRuntime) {
	t.Helper()
	if dodge.Name != "General_Dodge" || dodge.HeldBehavior != "Naive" || dodge.RequiresOutOfCombat || dodge.MinStamina != 0 ||
		dodge.CostStatGuid != content.GuidStamina || dodge.CostAmount != 50 || dodge.CostRequiresMoving || dodge.ChargeInterval != 0 ||
		dodge.Cooldown != 0.65 || !dodge.TriggersCooldown || !dodge.DisableDuringCooldown || dodge.TargetsSelf ||
		dodge.EffectGuid != 106223481149050 {
		t.Errorf("Dodge = %+v", dodge)
	}
}

func verifyRealRestRuntime(t *testing.T, rest content.AbilityRuntime) {
	t.Helper()
	if rest.Name != "General_Rest" || rest.HeldBehavior != "Toggle" || !rest.RequiresOutOfCombat || rest.CostStatGuid != 0 ||
		rest.CostAmount != 0 || rest.Cooldown != 0 || !rest.TargetsSelf || rest.EffectGuid != 109394092054139 ||
		rest.CancelSection != "SitEnd" ||
		rest.MontagePath != "/Game/Characters/Animations/00_ABLT/CM_M_ABLT_General_Rest_Montage.CM_M_ABLT_General_Rest_Montage" {
		t.Errorf("Rest = %+v", rest)
	}
}

func verifyRealEffectRuntimes(t *testing.T, db *content.DB) {
	t.Helper()
	effects, err := db.EffectRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	const exertion = 6064631668308443138
	want := map[int64]content.EffectRuntime{
		6064631641826394197: {Name: "General_Sprint", Infinite: true, TickTimer: 1, StopsRegenMoving: true, EndEffectGuid: exertion, EndEffectDuration: 1},
		106223481149050:     {Name: "General_Dodge_Evasion", Duration: 0.5, StopsRegenStanding: true, StopsRegenMoving: true, EndEffectGuid: exertion, EndEffectDuration: 1},
		109394092054139:     {Name: "General_Rest", Infinite: true},
		exertion:            {Name: "General_Stamina_DisableRegen", Duration: 1, StopsRegenStanding: true, StopsRegenMoving: true},
	}
	if len(effects) != len(want) {
		t.Errorf("%d effect_runtime rows, want %d: %+v", len(effects), len(want), effects)
	}
	for g, w := range want {
		got := effects[g]
		got.Source = ""
		w.EffectGuid = g
		if got != w {
			t.Errorf("effect %d = %+v, want %+v", g, got, w)
		}
	}
}

func verifyRealEffectStatRuntimes(t *testing.T, db *content.DB) {
	t.Helper()
	mods, err := db.EffectStatRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	rm := mods[109394092054139]
	if len(rm) != 2 || rm[0].StatName != "HealthRegeneration" || rm[0].SourceStatName != "MaxHealth" || rm[0].Factor != 0.01 || rm[0].Addend != 1 ||
		rm[1].StatName != "ManaRegeneration" || rm[1].SourceStatName != "MaxMana" || rm[1].Factor != 0.01 || rm[1].Addend != 1 {
		t.Errorf("Rest stat mods = %+v", rm)
	}
	if sm := mods[6064631641826394197]; len(sm) != 1 || sm[0].StatName != "MoveSpeedMult" || sm[0].SourceStatGuid != 0 || sm[0].Addend != 0.2 {
		t.Errorf("Sprint stat mods = %+v", sm)
	}
	if em := mods[106223481149050]; len(em) != 2 || em[0].Addend != 1 || em[1].Addend != 1 {
		t.Errorf("Evasion stat mods = %+v", em)
	}
}

func TestRealAbilityRuntime(t *testing.T) {
	db := builtDB(t)
	abilities, err := db.AbilityRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	if len(abilities) != 3 {
		t.Fatalf("%d ability_runtime rows, want 3: %+v", len(abilities), abilities)
	}
	verifyRealSprintRuntime(t, abilities[content.GuidSprint])
	verifyRealDodgeRuntime(t, abilities[content.GuidDodge])
	verifyRealRestRuntime(t, abilities[content.GuidRest])
	for _, a := range abilities {
		if a.Source == "" {
			t.Errorf("%s has no source", a.Name)
		}
	}
	verifyRealEffectRuntimes(t, db)
	verifyRealEffectStatRuntimes(t, db)
}
