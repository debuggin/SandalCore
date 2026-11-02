// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

type syntheticErrorCase struct {
	name   string
	mutate func(s *source)
	want   string
}

func dropRowByName(rows []designdb.Row, name string) []designdb.Row {
	var keep []designdb.Row
	for _, r := range rows {
		if r.Name != name {
			keep = append(keep, r)
		}
	}
	return keep
}

func dropRowByGuid(rows []designdb.Row, guid int64) []designdb.Row {
	var keep []designdb.Row
	for _, r := range rows {
		if r.Guid != guid {
			keep = append(keep, r)
		}
	}
	return keep
}

func setRowFieldByGuid(rows []designdb.Row, guid int64, field string, val designdb.Value) {
	for i := range rows {
		if rows[i].Guid == guid {
			rows[i].Fields[field] = val
		}
	}
}

func setAbilityCostEnabled(rows []designdb.Row, guid int64, enabled string) {
	for i := range rows {
		if rows[i].Guid == guid {
			costs := rows[i].Fields["Costs"].(list)
			costs[0].(st)["Enabled"] = enabled
		}
	}
}

func syntheticSchemaAndStatErrorCases() []syntheticErrorCase {
	return []syntheticErrorCase{
		{"a formula outside the subset", func(s *source) {
			l := &s.rows["StatInitializerListRecordBase"][1]
			l.Fields["StatIDs"].(list)[0] = st{"StatId": statID(content.GuidStrength), "ValueExpression": "GetStatInitRunningTotal() * 2"}
		}, `unsupported function "GetStatInitRunningTotal"`},
		{"an attribute missing from a class's list", func(s *source) {
			l := &s.rows["StatInitializerListRecordBase"][1]
			ids := l.Fields["StatIDs"].(list)
			l.Fields["StatIDs"] = ids[1:]
		}, "Player_Base_Bard has 0 entries for stat"},
		{"a class row missing", func(s *source) {
			s.rows["CharacterClassesRecordBase"] = dropRowByName(s.rows["CharacterClassesRecordBase"], "Tank")
		}, "no CharacterClasses row named Tank"},
		{"a wrong class enum", func(s *source) {
			s.rows["CharacterClassesRecordBase"][0].Fields["Class"] = "ECharacterClass::Mage"
		}, "Class is ECharacterClass::Mage, want ECharacterClass::Bard"},
		{"a class list missing", func(s *source) {
			s.rows["StatInitializerListRecordBase"][8].Name = "Player_Base_Nobody"
		}, "no StatInitializerList Player_Base_Tank"},
		{"a stat of the wrong name", func(s *source) {
			s.rows["StatTypeDefRecordBase"][4].Name = "Power"
		}, "content names it Strength"},
		{"an unknown stat in a list", func(s *source) {
			l := &s.rows["StatInitializerListRecordBase"][0]
			l.Fields["StatIDs"] = append(l.Fields["StatIDs"].(list), st{"StatId": statID(42), "ValueExpression": ""})
		}, "names stat 42, which is not a StatTypeDef row"},
		{"a missing field", func(s *source) {
			delete(s.rows["StatTypeDefRecordBase"][0].Fields, "ValueMax")
		}, "StatTypeDef Stamina (6064629575339934476): no field ValueMax"},
		{"a field of the wrong kind", func(s *source) {
			s.rows["StatTypeDefRecordBase"][0].Fields["Replication"] = "seven"
		}, "field Replication is string, want int64"},
		{"a duplicate stat", func(s *source) {
			s.rows["StatTypeDefRecordBase"] = append(s.rows["StatTypeDefRecordBase"], s.rows["StatTypeDefRecordBase"][0])
		}, "two StatTypeDef rows have Guid"},
	}
}

func syntheticAbilityAndDrainErrorCases() []syntheticErrorCase {
	return []syntheticErrorCase{
		{"no Sprint ability", func(s *source) {
			s.rows["AoCAbilityRecordBase"] = s.rows["AoCAbilityRecordBase"][:1]
		}, "no Ability row 6064631641648332884"},
		{"a hit that does not exist", func(s *source) {
			s.rows["AbilityHitRecordBase"] = s.rows["AbilityHitRecordBase"][1:]
		}, "no AbilityHit row 8000000000000000001"},
		{"a stat mod that does not exist", func(s *source) {
			s.rows["StatModRecordBase"] = s.rows["StatModRecordBase"][1:]
		}, "names StatMod 9000000000000000001"},
		{"a regeneration formula that names no effect the data has", func(s *source) {
			s.rows["EffectRecordBase"] = dropRowByGuid(s.rows["EffectRecordBase"], effGuard)
		}, "no Effect row 6064631667969556480"},
		{"a record struct with another TypeId", func(s *source) {
			s.typeIDs["EffectRecordBase"]++
		}, "EffectRecordBase has TypeId"},
		{"a table that did not decode", func(s *source) {
			delete(s.rows, "StatModRecordBase")
		}, "no decoded StatModRecordBase table"},
		{"a drain effect that takes another amount than the Sprint cost", func(s *source) {
			setRowFieldByGuid(s.rows["StatModRecordBase"], modDrain, "Value", "-2.0")
		}, "the drain effect is not the Sprint's"},
		{"a drain effect that takes Stamina while standing still", func(s *source) {
			setRowFieldByGuid(s.rows["StatModRecordBase"], modDrain, "Value", "SelectFloat(IsInCombat(GetSource()), -2.5, -1.25)")
		}, "takes 1.25 Stamina per tick while the owner stands still"},
		{"a drain effect with no tick", func(s *source) {
			setRowFieldByGuid(s.rows["EffectRecordBase"], effDrain, "TickTimer", float32(0))
		}, "want an infinite effect with a tick"},
		{"no drain effect", func(s *source) {
			s.rows["EffectRecordBase"] = dropRowByGuid(s.rows["EffectRecordBase"], effDrain)
		}, "no Effect row 6064632350237720599"},
	}
}

func syntheticModAndCostErrorCases() []syntheticErrorCase {
	return []syntheticErrorCase{
		{"no Rest ability", func(s *source) {
			s.rows["AoCAbilityRecordBase"] = dropRowByGuid(s.rows["AoCAbilityRecordBase"], content.GuidRest)
		}, "no Ability row 109393746155128"},
		{"a stat mod that is not linear in its stat", func(s *source) {
			setRowFieldByGuid(s.rows["StatModRecordBase"], modRestHealth, "Value", "Min(GetStat(GetInstigator(), $#5365104778484440215:108986356618873$), 10)")
		}, "is not linear in stat 108986356618873"},
		{"a stat mod that reads two stats", func(s *source) {
			setRowFieldByGuid(s.rows["StatModRecordBase"], modRestHealth, "Value", "GetStat(GetInstigator(), $#5365104778484440215:108986356618873$) + GetStat(GetInstigator(), $#5365104778484440215:109183578756729$)")
		}, "reads 2 stats, the runtime handles one"},
		{"an activation requirement that reads another stat", func(s *source) {
			setRowFieldByGuid(s.rows["AoCAbilityRecordBase"], content.GuidSprint, "ActivationRequirements", "GetStat(GetSource(), $#5365104778484440215:108986356618873$) >= 1.0")
		}, "stat 108986356618873 has no value in this environment"},
		{"an activation requirement that no character can meet", func(s *source) {
			setRowFieldByGuid(s.rows["AoCAbilityRecordBase"], content.GuidRest, "ActivationRequirements", "false")
		}, "no character can use the ability"},
		{"a cost charged only while moving that is not the Sprint's", func(s *source) {
			setAbilityCostEnabled(s.rows["AoCAbilityRecordBase"], content.GuidDodge, "IsMoving(GetOwner())")
		}, "the runtime knows the cadence of the Sprint's alone"},
		{"a cost group enabled by something else", func(s *source) {
			setAbilityCostEnabled(s.rows["AoCAbilityRecordBase"], content.GuidDodge, "IsInCombat(GetOwner())")
		}, "is neither always true nor true exactly while moving"},
		{"an End hit that applies two regeneration pauses", func(s *source) {
			setRowFieldByGuid(s.rows["AbilityHitRecordBase"], hitEvasionEnd, "ApplyEffects", list{applyEffect(effExertion, "", "", ""), applyEffect(effStun, "", "", "")})
		}, "its End hits apply 6064631668308443138 and 6064631799761208354"},
		{"a Player_Base without Stamina's value", func(s *source) {
			l := &s.rows["StatInitializerListRecordBase"][0]
			l.Fields["StatIDs"].(list)[1] = st{"StatId": statID(content.GuidStamina), "ValueExpression": "GetStat(GetOwner(), $#5365104778484440215:1$)"}
		}, `unsupported function "GetStat"`},
	}
}

func TestSyntheticErrors(t *testing.T) {
	cases := append(syntheticSchemaAndStatErrorCases(), syntheticAbilityAndDrainErrorCases()...)
	cases = append(cases, syntheticModAndCostErrorCases()...)
	for _, tt := range cases {
		src := syntheticSource()
		tt.mutate(src)
		_, err := buildSynthetic(t, src)
		if err == nil || !strings.Contains(err.Error(), tt.want) {
			t.Errorf("%s: error %v, want one containing %q", tt.name, err, tt.want)
		}
	}
}

func TestStatRegenTime(t *testing.T) {
	dir := t.TempDir()
	ini := "\xef\xbb\xbf[/Script/GameSystemsPlugin.CombatConfig]\r\nGameSettings=(General=(InCombatTimer=6.000000),Stats=(StatRegenTime=2.500000,GlobalDamageMultiplier=1.000000))\r\n"
	if err := os.WriteFile(filepath.Join(dir, "DefaultCombat.ini"), []byte(ini), 0o644); err != nil {
		t.Fatal(err)
	}
	v, src, err := statRegenTime(dir)
	if err != nil || v != 2.5 || !strings.HasPrefix(src, "DefaultCombat.ini") {
		t.Errorf("statRegenTime(dir) = %v, %q, %v", v, src, err)
	}
	v, src, err = statRegenTime("")
	if err != nil || v != content.DefaultStatRegenTime || !strings.Contains(src, "no client Config directory") {
		t.Errorf("statRegenTime(\"\") = %v, %q, %v", v, src, err)
	}
	v, src, err = statRegenTime(filepath.Join(dir, "missing"))
	if err != nil || v != content.DefaultStatRegenTime || !strings.Contains(src, "does not exist") {
		t.Errorf("statRegenTime(missing) = %v, %q, %v", v, src, err)
	}
	other := t.TempDir()
	os.WriteFile(filepath.Join(other, "DefaultCombat.ini"), []byte("[x]\r\nStats=(Other=1.0)\r\n"), 0o644)
	if _, _, err := statRegenTime(other); err == nil || !strings.Contains(err.Error(), "has no StatRegenTime") {
		t.Errorf("an ini without StatRegenTime: %v", err)
	}
	_, src, _ = statRegenTime(dir)
	if want := "DefaultCombat.ini [/Script/GameSystemsPlugin.CombatConfig] GameSettings.Stats.StatRegenTime"; src != want {
		t.Errorf("source = %q, want %q", src, want)
	}
}

// (C:Y:B + C O A)

func TestStatRegenTimeChecksItsSection(t *testing.T) {
	const section = "[/Script/GameSystemsPlugin.CombatConfig]\r\n"
	for _, tt := range []struct {
		name, ini string
		want      float64
	}{
		{"the right section and property", section + "GameSettings=(Stats=(StatRegenTime=2.5))\r\n", 2.5},
		{"after other simple properties of Stats", section + "GameSettings=(General=(A=1.0),Stats=(Foo=1,Bar=x,StatRegenTime=4.0,Nested=(C=1)))\r\n", 4},
		{"after a nested property of Stats", section + "GameSettings=(Stats=(Nested=(C=1),StatRegenTime=4.0))\r\n", 0},
		{"in another section", "[/Script/Other.Config]\r\nGameSettings=(Stats=(StatRegenTime=3.0))\r\n", 0},
		{"in another property of the right section", section + "AccountSettings=(Stats=(StatRegenTime=9.0))\r\n", 0},
		{"in the right section after another section's", "[/Script/Other.Config]\r\nGameSettings=(Stats=(StatRegenTime=3.0))\r\n" + section + "GameSettings=(Stats=(StatRegenTime=6.0))\r\n", 6},
		{"before any section", "GameSettings=(Stats=(StatRegenTime=3.0))\r\n" + section, 0},
		{"in the value of another key", section + "Other=GameSettings=(Stats=(StatRegenTime=3.0))\r\n", 0},
	} {
		dir := t.TempDir()
		if err := os.WriteFile(filepath.Join(dir, "DefaultCombat.ini"), []byte("\xef\xbb\xbf"+tt.ini), 0o644); err != nil {
			t.Fatal(err)
		}
		v, src, err := statRegenTime(dir)
		if tt.want == 0 {
			if err == nil || !strings.Contains(err.Error(), "has no StatRegenTime in the GameSettings property of [/Script/GameSystemsPlugin.CombatConfig]") {
				t.Errorf("%s: statRegenTime = %v, %q, %v; want the no-StatRegenTime error", tt.name, v, src, err)
			}
			continue
		}
		if err != nil || v != tt.want || !strings.Contains(src, "[/Script/GameSystemsPlugin.CombatConfig] GameSettings.Stats.StatRegenTime") {
			t.Errorf("%s: statRegenTime = %v, %q, %v; want %v", tt.name, v, src, err, tt.want)
		}
	}
}

func TestRuleSetKeepsTheFirstProblem(t *testing.T) {
	rs := &ruleSet{rows: map[string][2]string{}}
	rs.put("a", "1", "first")
	rs.putFloat("b", 2.5, "src")
	if rs.err != nil {
		t.Fatalf("err = %v", rs.err)
	}
	rs.put("a", "2", "second")
	rs.putFloat("b", 9, "again")
	rs.put("c", "3", "ok")
	if rs.err == nil || !strings.Contains(rs.err.Error(), "stamina rule a is given twice") {
		t.Errorf("err = %v", rs.err)
	}
	if rs.rows["a"] != [2]string{"1", "first"} || rs.rows["b"][0] != "2.5" || rs.rows["c"][0] != "3" {
		t.Errorf("rows = %v", rs.rows)
	}
}

func verifyFieldReaderErrors(t *testing.T, f *fields) {
	t.Helper()
	f.str("missing")
	first := f.err
	f.int("s")
	if f.err != first || !strings.Contains(first.Error(), "Row X: no field missing") {
		t.Errorf("error = %v", f.err)
	}
	g := newFields("Row Y", st{"s": 5})
	if g.str("s"); g.err == nil || !strings.Contains(g.err.Error(), "field s is int, want string") {
		t.Errorf("wrong kind: %v", g.err)
	}
	h := newFields("Row Z", st{"id": st{"Guid": "x"}, "tag": list{3}})
	if h.id("id"); h.err == nil || !strings.Contains(h.err.Error(), "not an Id struct") {
		t.Errorf("bad id: %v", h.err)
	}
	h2 := newFields("Row Z", st{"tag": list{3}})
	if h2.tags("tag"); h2.err == nil || !strings.Contains(h2.err.Error(), "array of tags") {
		t.Errorf("bad tags: %v", h2.err)
	}
}

func TestFieldReader(t *testing.T) {
	f := newFields("Row X", st{
		"s": "text", "b": true, "i": int64(7), "f32": float32(0.001), "f64": 2.5, "l": list{"a", "b"},
		"id": idv(5, 6, "Name"), "txt": designdb.Text{Key: "K", Source: ""}, "txt2": designdb.Text{Key: "K", Source: "S"},
		"ids": list{idv(1, 2, "A"), idv(3, 4, "B")}, "tags": list{st{"TagName": "t.a"}, st{"TagName": "t.b"}},
		"tags2": list{"u.a"}, "map": designdb.Pairs{{Key: int64(1), Value: idv(9, 8, "M")}},
	})
	if f.str("s") != "text" || !f.boolean("b") || f.int("i") != 7 || f.float("f32") != 0.001 || f.float("f64") != 2.5 || len(f.list("l")) != 2 {
		t.Error("scalar reads")
	}
	if g, n := f.id("id"); g != 5 || n != "Name" {
		t.Errorf("id = %d, %q", g, n)
	}
	if f.text("txt") != "K" || f.text("txt2") != "S" {
		t.Error("text reads")
	}
	if ids := f.ids("ids"); len(ids) != 2 || ids[1] != 3 {
		t.Errorf("ids = %v", ids)
	}
	if k, g := f.keyedIds("map"); len(k) != 1 || k[0] != 1 || g[0] != 9 {
		t.Errorf("keyedIds = %v %v", k, g)
	}
	if fmt.Sprint(f.tags("tags")) != "[t.a t.b]" || fmt.Sprint(f.tags("tags2")) != "[u.a]" {
		t.Error("tags")
	}
	if f.err != nil {
		t.Fatalf("unexpected error %v", f.err)
	}
	verifyFieldReaderErrors(t, f)
}

func TestDecimal32(t *testing.T) {
	for in, want := range map[float32]float64{0.001: 0.001, 1028.9219: 1028.9219, 0.05: 0.05, 3.4028235e+38: 3.4028235e+38, 30: 30, 0: 0} {
		if got := decimal32(in); got != want {
			t.Errorf("decimal32(%v) = %v, want %v", in, got, want)
		}
	}
}

// (C | Y | B ~ C-O-A)

func TestRunFlags(t *testing.T) {
	var out, errb strings.Builder
	for _, tt := range []struct {
		name string
		args []string
		env  map[string]string
		code int
		want string
	}{
		{"no inputs", nil, map[string]string{"AOC_DESIGN_DB": "", "AOC_USMAP": ""}, 2, "--dbc and --usmap are required"},
		{"unknown flag", []string{"--bogus"}, nil, 2, "flag provided but not defined"},
		{"stray argument", []string{"--dbc", "a", "--usmap", "b", "extra"}, nil, 2, `unexpected argument "extra"`},
		{"help", []string{"-h"}, nil, 0, "-record-encoding"},
		{"a missing design database", []string{"--dbc", filepath.Join(t.TempDir(), "x.dbc"), "--usmap", "y", "--out", filepath.Join(t.TempDir(), "o.db")}, nil, 1, "design database"},
		{"a bad encoding", []string{"--dbc", "a", "--usmap", "b", "--record-encoding", "zip"}, nil, 1, `unknown record encoding "zip"`},
	} {
		for k, v := range tt.env {
			t.Setenv(k, v)
		}
		out.Reset()
		errb.Reset()
		code := run(tt.args, &out, &errb)
		if code != tt.code || !strings.Contains(errb.String(), tt.want) {
			t.Errorf("%s: exit %d, stderr %q; want exit %d containing %q", tt.name, code, errb.String(), tt.code, tt.want)
		}
	}
}

func TestRepoRootDefault(t *testing.T) {
	wd, _ := os.Getwd()
	root := repoRoot()
	if _, err := os.Stat(filepath.Join(root, "services", "go.mod")); err != nil {
		t.Errorf("repoRoot() = %s from %s: no services/go.mod there", root, wd)
	}
}

func TestProbeActivation(t *testing.T) {
	const stamina = "GetStat(GetSource(), $#5365104778484440215:6064629575339934476$)"
	for _, tt := range []struct {
		expr        string
		min         float64
		outOfCombat bool
		err         string
	}{
		{"", 0, false, ""},
		{stamina + " >= 1.0", 1, false, ""},
		{stamina + " > 2.5", 2.5, false, ""},
		{"!IsInCombat(GetInstigator())", 0, true, ""},
		{"!IsInCombat(GetInstigator()) && " + stamina + " >= 10", 10, true, ""},
		{"true", 0, false, ""},
		{"false", 0, false, "no character can use the ability"},
		{"1 + 2", 0, false, "not a condition"},
		{stamina + " < 3.0", 0, false, "no character can use the ability"},
	} {
		min, out, err := probeActivation(tt.expr)
		if tt.err != "" {
			if err == nil || !strings.Contains(err.Error(), tt.err) {
				t.Errorf("%q: error %v, want one containing %q", tt.expr, err, tt.err)
			}
			continue
		}
		if err != nil || min != tt.min || out != tt.outOfCombat {
			t.Errorf("%q = %v, %v, %v; want %v, %v", tt.expr, min, out, err, tt.min, tt.outOfCombat)
		}
	}
}

// C:Y:B captures, C | O | A code

func TestLinearMod(t *testing.T) {
	const x = "GetStat(GetInstigator(), $#5365104778484440215:7$)"
	const y = "GetStat(GetInstigator(), $#5365104778484440215:8$)"
	for _, tt := range []struct {
		expr           string
		source         int64
		factor, addend float64
		err            string
	}{
		{"0.2", 0, 0, 0.2, ""},
		{"1.0", 0, 0, 1, ""},
		{x + " * 0.01 + 1", 7, 0.01, 1, ""},
		{"const m = " + x + ";\r\nm * 0.5", 7, 0.5, 0, ""},
		{"-" + x + " * 2 + 3", 7, -2, 3, ""},
		{"Min(" + x + ", 10)", 0, 0, 0, "is not linear in stat 7"},
		{x + " * " + x, 0, 0, 0, "is not linear in stat 7"},
		{x + " + " + y, 0, 0, 0, "reads 2 stats"},
		{"GetDamage()", 0, 0, 0, "unsupported function"},
	} {
		m, err := linearMod(tt.expr)
		if tt.err != "" {
			if err == nil || !strings.Contains(err.Error(), tt.err) {
				t.Errorf("%q: error %v, want one containing %q", tt.expr, err, tt.err)
			}
			continue
		}
		if err != nil || m.source != tt.source || m.factor != tt.factor || m.addend != tt.addend {
			t.Errorf("%q = %+v, %v; want source %d factor %v addend %v", tt.expr, m, err, tt.source, tt.factor, tt.addend)
		}
	}
}

func TestCostNeedsMoving(t *testing.T) {
	for expr, want := range map[string]bool{"": false, "true": false, "IsMoving(GetOwner())": true, "IsMoving(GetSource()) == true": true} {
		got, err := costNeedsMoving(expr)
		if err != nil || got != want {
			t.Errorf("costNeedsMoving(%q) = %v, %v; want %v", expr, got, err, want)
		}
	}
	for _, expr := range []string{"false", "IsInCombat(GetOwner())", "!IsMoving(GetOwner())", "IsMoving(GetOwner()) && !IsInCombat(GetOwner())"} {
		if _, err := costNeedsMoving(expr); err == nil {
			t.Errorf("costNeedsMoving(%q) is not an error", expr)
		}
	}
}
