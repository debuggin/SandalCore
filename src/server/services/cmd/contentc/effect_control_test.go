// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

const (
	ecCC        = abBase + 70
	ecCCUnused  = abBase + 71
	ecDisable   = abBase + 72
	ecFragility = abBase + 73
	ecFragile2  = abBase + 74
	ecFragUnuse = abBase + 75
)

func setEffectFields(t *testing.T, src *source, guid int64, f st) {
	t.Helper()
	for i := range src.rows["EffectRecordBase"] {
		r := &src.rows["EffectRecordBase"][i]
		if r.Guid != guid {
			continue
		}
		for k, v := range f {
			r.Fields[k] = v
		}
		return
	}
	t.Fatalf("no effect row %d", guid)
}

func ccRecord(guid int64, name string, f st) designdb.Row {
	return mkRow(guid, name, merge(st{
		"AppliedStatus": int64(16), "Priority": int64(0), "AnimationRequest": "ECrowdControlAnimationRequest::None", "AbilitiesToDisableId": idv(0, 4582867823717639616, "None"),
		"bFaceSource": false, "bIgnoreNonCombatantImmunity": false, "bRestartMontageOnReapply": false, "bOverrideClientAuthMovement": true,
		"PushParameterization": "EPushParameterization::Force", "PushMagnitude": "", "PushDuration": "", "PushArrivalDistance": float32(50), "bEndPushAtArrival": true,
		"PushDirection": st{"X": float32(0), "Y": float32(0), "Z": float32(0)}, "SnareStrength": "",
	}, f))
}

func fragilityRecord(guid int64, name string, f st) designdb.Row {
	return mkRow(guid, name, merge(st{
		"EffectFragilityType": "EEffectFragilityType::FRAGILITY_Proc", "EffectBreakThreshold": "", "EffectBreakThresholdAccumulationRate": "", "EvaluationTickFrequency": int64(0),
		"ShouldBreakEffectImmediately": "false", "ShouldEvaluateThresholdAccumulation": "",
		"EventBindings": st{"AbilityEvents": int64(0), "HitEvents": int64(2), "EffectEvents": int64(0)},
	}, f))
}

func effectControlSource(t *testing.T) *source {
	src := abilitySource()
	src.rows["AbilityDisableRecordBase"] = []designdb.Row{mkRow(ecDisable, "Test_Silence", st{"Filter": "!AbilityHasTag(GetAbilityIterator(), \"petcommand\")"})}
	src.rows["CrowdControlRecordBase"] = []designdb.Row{
		ccRecord(ecCC, "Test_Snare", st{
			"AppliedStatus": int64(37), "Priority": int64(200), "AnimationRequest": "ECrowdControlAnimationRequest::Stunned", "AbilitiesToDisableId": idv(ecDisable, 4582867823717639616, "None"),
			"bFaceSource": true, "PushParameterization": "EPushParameterization::Time", "PushMagnitude": "1000", "PushDuration": "0.5", "PushArrivalDistance": float32(100),
			"bEndPushAtArrival": false, "PushDirection": st{"X": float32(1), "Y": float32(0), "Z": float32(-1)}, "SnareStrength": " 0.25 ",
		}),
		ccRecord(ecCCUnused, "Test_Unused", nil),
	}
	src.rows["FragileEffectDefRecordBase"] = []designdb.Row{
		fragilityRecord(ecFragility, "Test_Sleep", st{"ShouldBreakEffectImmediately": "GetHealthPercentage(GetOwner()) < 0.5", "EventBindings": st{"AbilityEvents": int64(0), "HitEvents": int64(4)}}),
		fragilityRecord(ecFragile2, "Test_Root", st{"EffectFragilityType": "EEffectFragilityType::FRAGILITY_Tick", "EffectBreakThreshold": "300", "EffectBreakThresholdAccumulationRate": "100", "EvaluationTickFrequency": int64(1)}),
		fragilityRecord(ecFragUnuse, "Test_Unused", nil),
	}
	setEffectFields(t, src, abEffPep, st{
		"bCompareSourceWhenStacking": true, "bRecalculateHitOnTick": true, "bNumStacksLostOnDurationElapse": true, "NumStacksLostOnDurationElapse": "7",
		"ExtendedMaximum": int64(15), "ExtensionAmount": "const dur = 2.0;\r\nMax(6.0 - dur, 3.0)",
	})
	setEffectFields(t, src, abEffTick, st{
		"CrowdControlId":          idv(ecCC, 2559080087696523252, "None"),
		"FragilityDefinitionsIds": list{idv(ecFragility, 3645681529784039325, "None"), idv(0, 3645681529784039325, "None"), idv(ecFragile2, 3645681529784039325, "None")},
	})
	return src
}

func assertSyntheticEffectControlColumns(t *testing.T, db *content.DB) {
	t.Helper()
	pep, err := db.EffectControl(abEffPep)
	if err != nil {
		t.Fatal(err)
	}
	if !pep.CompareSource || !pep.RecalcOnTick || !pep.StacksLostOnElapse || pep.ExtendedMax != 15 || pep.ExtensionExpr == 0 || pep.StacksLostExpr == 0 {
		t.Errorf("Bard_Pep control = %+v", pep)
	}
	for _, c := range []struct {
		name string
		id   int64
		want string
	}{{"extension", pep.ExtensionExpr, "const dur = 2.0;\r\nMax(6.0 - dur, 3.0)"}, {"stack loss", pep.StacksLostExpr, "7"}} {
		e, err := db.Expr(c.id)
		if err != nil || e.Text != c.want {
			t.Errorf("the %s expression %d = %q, %v; want %q", c.name, c.id, e.Text, err, c.want)
		}
	}
	lo, hi := min(pep.StacksLostExpr, pep.ExtensionExpr), max(pep.StacksLostExpr, pep.ExtensionExpr)
	if n := scalarInt(t, db, `SELECT count(*) FROM ab_expr WHERE id > ?`, hi); n != 0 {
		t.Errorf("%d expressions come after the effect-control ones, want none", n)
	}
	if hi != lo+1 {
		t.Errorf("the two expressions are %d and %d, want two in a row", lo, hi)
	}
	for _, g := range []int64{abEffPassive, abEffTick} {
		c, err := db.EffectControl(g)
		if err != nil || c != (content.EffectControlRow{}) {
			t.Errorf("effect %d control = %+v, %v; want the defaults", g, c, err)
		}
	}
}

func assertSyntheticCrowdControlAndFragilities(t *testing.T, db *content.DB, meta map[string]string) {
	t.Helper()
	cc, err := db.CrowdControl(ecCC)
	if err != nil {
		t.Fatal(err)
	}
	want := content.CrowdControlRow{Guid: ecCC, Name: "Test_Snare", AppliedStatus: 37, SnareStrength: 0.25, AnimationRequest: "ECrowdControlAnimationRequest::Stunned", Priority: 200,
		DisableGuid: ecDisable, FaceSource: true, OverrideClientMove: true, PushParameterization: "EPushParameterization::Time", PushMagnitude: "1000", PushDuration: "0.5",
		PushDirX: 1, PushDirY: 0, PushDirZ: -1, PushArrivalDistance: 100}
	if cc != want {
		t.Errorf("crowd control = %+v\nwant           %+v", cc, want)
	}
	if _, err := db.CrowdControl(ecCCUnused); err == nil {
		t.Error("a crowd-control record no written effect names was written")
	}
	dis, err := db.AbilityDisable(ecDisable)
	if err != nil || dis.Name != "Test_Silence" || !strings.Contains(dis.Filter, "petcommand") {
		t.Errorf("ability disable = %+v, %v", dis, err)
	}
	fr, err := db.Fragility(ecFragility)
	if err != nil || fr.FragilityType != "EEffectFragilityType::FRAGILITY_Proc" || fr.BreakImmediately != "GetHealthPercentage(GetOwner()) < 0.5" || fr.Bindings != `{"AbilityEvents":0,"HitEvents":4}` {
		t.Errorf("fragility = %+v, %v", fr, err)
	}
	fr2, err := db.Fragility(ecFragile2)
	if err != nil || fr2.FragilityType != "EEffectFragilityType::FRAGILITY_Tick" || fr2.BreakThreshold != "300" || fr2.AccumulationRate != "100" || fr2.TickFrequency != 1 {
		t.Errorf("fragility 2 = %+v, %v", fr2, err)
	}
	if got, err := db.EffectFragilities(abEffTick); err != nil || len(got) != 2 || got[0] != ecFragility || got[1] != ecFragile2 {
		t.Errorf("the fragilities of Bard_Tick = %v, %v; want the two in order (the null reference left out)", got, err)
	}
	for table, n := range map[string]int64{"ab_effect_control": scalarInt(t, db, `SELECT count(*) FROM ab_effect`), "ab_crowd_control": 1, "ab_ability_disable": 1, "ab_fragility": 2, "ab_effect_fragility": 2} {
		if got := scalarInt(t, db, `SELECT count(*) FROM `+table); got != n {
			t.Errorf("%s has %d rows, want %d", table, got, n)
		}
	}
	for k, v := range map[string]string{"ab.crowd_controls": "1", "ab.ability_disables": "1", "ab.fragilities": "2"} {
		if meta[k] != v {
			t.Errorf("meta %s = %q, want %q", k, meta[k], v)
		}
	}
}

func TestEffectControlColumnsAndRecords(t *testing.T) {
	db, meta, err := buildAbilitySynthetic(t, effectControlSource(t))
	if err != nil {
		t.Fatal(err)
	}
	assertSyntheticEffectControlColumns(t, db)
	assertSyntheticCrowdControlAndFragilities(t, db, meta)
}

func TestEffectControlRefusesWhatItDoesNotUnderstand(t *testing.T) {
	for _, c := range []struct {
		name string
		edit func(*source)
		want string
	}{
		{"a snare strength that is an expression", func(s *source) {
			s.rows["CrowdControlRecordBase"][0].Fields["SnareStrength"] = "GetStat(GetTarget(), 1) * 0.5"
		}, "SnareStrength"},
		{"a crowd-control record that is not there", func(s *source) { s.rows["CrowdControlRecordBase"] = nil }, "no CrowdControl row"},
		{"an ability-disable record that is not there", func(s *source) { s.rows["AbilityDisableRecordBase"] = nil }, "no AbilityDisable row"},
		{"a fragility record that is not there", func(s *source) { s.rows["FragileEffectDefRecordBase"] = s.rows["FragileEffectDefRecordBase"][:1] }, "no FragileEffectDef row"},
	} {
		src := effectControlSource(t)
		c.edit(src)
		if _, _, err := buildAbilitySynthetic(t, src); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: error = %v, want one that names %q", c.name, err, c.want)
		}
	}
}
