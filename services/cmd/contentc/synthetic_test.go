// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"database/sql"
	"fmt"
	"io"
	"path/filepath"
	"strings"
	"testing"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

type (
	st   = designdb.Struct
	list = []designdb.Value
)

func idv(guid, typeID int64, name string) st {
	return st{"Guid": guid, "TypeId": typeID, "Name": name}
}

func effectID(guid int64) st { return idv(guid, content.TypeEffect, "None") }
func hitID(guid int64) st    { return idv(guid, content.TypeAbilityHit, "None") }
func modID(guid int64) st    { return idv(guid, content.TypeStatMod, "None") }
func statID(guid int64) st   { return idv(guid, content.TypeStatDef, "None") }

func txt(source string) designdb.Text { return designdb.Text{Source: source} }

func crlf(s string) string { return strings.ReplaceAll(s, "\n", "\r\n") }

func mkRow(guid int64, name string, f st) designdb.Row {
	return designdb.Row{Guid: guid, Name: name, Fields: f}
}

func merge(base, over st) st {
	out := st{}
	for k, v := range base {
		out[k] = v
	}
	for k, v := range over {
		out[k] = v
	}
	return out
}

func statRow(guid int64, name string, rep int64, f st) designdb.Row {
	return mkRow(guid, name, merge(st{
		"GameplayType": "EStatGameplayType::Type_Combat", "StatType": "EStatRecordType::Type_Float", "Replication": rep,
		"base": "", "ValueMin": "0", "ValueMax": "", "RegenRate": "",
		"OnSpawn": "EStatResetBehavior::Reset_None", "OnDeath": "EStatResetBehavior::Reset_None", "OnRevive": "EStatResetBehavior::Reset_None",
		"bPersistValue": false, "bHidden": false, "bClampStatFinalValue": false, "bMultiplyStatMods": false,
		"bRegenWhileDead": false, "bRecalculateOnCombatState": false, "bDisplayAsPercentage": false,
	}, f))
}

func effectRow(guid int64, name string, f st) designdb.Row {
	return mkRow(guid, name, merge(st{
		"EffectName": txt(name), "EffectCategory": "EEffectCategory::EFFECT_Hidden", "Tags": list{}, "EffectTags": list{},
		"ApplicationRequirements": "", "bInfiniteDuration": false, "EffectDuration": "", "TickTimer": float32(0),
		"bIgnoreInitialTick": false, "EffectStackType": "EEffectStackingType::STACK_OverrideAlways", "GlobalStackLimit": int64(50),
		"StackDurationRefresh": "EEffectStackDurationType::STACK_AlwaysRefresh", "bPersistsThroughDeath": false,
		"bPersistsRelog": false, "bDispellable": false, "bSignificant": false,
		"PreApplyHitsIds": list{}, "ApplyHitsIds": list{}, "TickHitsIds": list{}, "ConsumeHitsIds": list{},
		"RemoveHitsIds": list{}, "ExpireHitsIds": list{}, "EndHitsIds": list{}, "StatModsIds": list{},
	}, f))
}

// == CoA | C:Y:B ==

func hitRow(guid int64, name string, f st) designdb.Row {
	return mkRow(guid, name, merge(st{
		"ApplicationRequirements": "", "ApplyEffects": list{}, "EffectsAppliedToSelf": list{}, "StatModsIds": list{},
		"OnHitFXId": st{"Guid": int64(0), "Name": "None"}, "OnActivateFXId": st{"Guid": int64(0), "Name": "None"}, "MaxNumEffectsToRemove": int64(0),
	}, f))
}

func applyEffect(guid int64, stack, requirement, duration string) st {
	return st{"StackCount": stack, "EffectId": effectID(guid), "EffectApplicationRequirement": requirement, "EffectDuration": duration}
}

func abilityRow(guid int64, name string, f st) designdb.Row {
	return mkRow(guid, name, merge(st{
		"AbilityTags": list{}, "AbilityName": txt(name), "ActivationRequirements": "", "Cooldown": "0",
		"bDisableDuringCooldown": false, "SharedCooldownTag": "None", "HeldAbilityBehavior": "EHeldAbilityPressBehavior::Naive",
		"bOverrideHeldBehavior": false, "bHoldAnimation": false, "MaximumHoldTime": float32(0),
		"ComboInputs": list{}, "AbilityActivationInput": "EAbilityActivationInput::UserConfigurable",
		"bUsableInAir": true, "bUsableWhileSwimming": false, "bUsableWhileWalking": true, "bUsableWhileMounted": false,
		"NetworkingMaxSpeed": float32(0), "Costs": list{}, "EffectsIds": designdb.Pairs{}, "HitsIds": designdb.Pairs{},
		"ApplicableTargets": int64(0), "CancelSectionName": "None", "AnimMontages": list{},
	}, f))
}

var (
	regenExpr = crlf(`const disableRegen = (GetEffectCount(GetSource(), $#3377240744094973029:6064631641826394197$) > 0 && IsMoving(GetSource())) ||
GetEffectCount(GetSource(), $#3377240744094973029:6064631667969556480$) > 0 ||
GetEffectCount(GetSource(), $#3377240744094973029:106223481149050$) > 0 ||
GetEffectCount(GetSource(), $#3377240744094973029:6064631799761208354$) > 0 ||
GetEffectCount(GetSource(), $#3377240744094973029:6064631668308443138$) > 0;

SelectFloat(disableRegen, 0.0, 5.0)`)
	sprintCostExpr = crlf(`var inCombat = IsInCombat(GetSource());
var Cost = 1.25;

if (inCombat)
{
set Cost = 2.5;
}

Cost`)
	dodgeCostExpr = crlf(`var snareStacks = GetEffectCount(GetInstigator(), $#3377240744094973029:6064629784692426711$);
var chilledStacks = GetEffectCount(GetInstigator(), $#3377240744094973029:6064628906043703296$);
var chilled = SelectInt(chilledStacks > 0, 2, 0);
var mult = 1 + (0.25 * Min((snareStacks + chilled),3));

SelectInt(GetEffectCount(GetInstigator(), $#3377240744094973029:6064632660379712165$) > 0 &&
GetSkillPurchased(GetOwner(),$#297887639323616934:6064630422842835238$,$#4146438674247114715:6064632892734570925$),
30,
50) * mult`)
	drainExpr = crlf(`var inCombat = IsInCombat(GetSource());
var isMoving = IsMoving(GetSource());
var Cost = 0.0;

if(isMoving)
{
set Cost = -1.25;

if (inCombat)
{
set Cost = -2.5;
}

}

Cost`)
	restHealthExpr = crlf(`const maxHp = GetStat(GetInstigator(), $#5365104778484440215:108986356618873$);
maxHp * 0.01 + 1`)
	restManaExpr = crlf(`const maxMp = GetStat(GetInstigator(), $#5365104778484440215:109183578756729$);
maxMp * 0.01 + 1`)
	dodgeCooldownExpr = crlf(`SelectFloat(GetEffectCount(GetInstigator(), $#3377240744094973029:6064632660379712165$) > 0 &&
GetSkillPurchased(GetOwner(),$#297887639323616934:6064630422842835238$,$#4146438674247114715:6064632892734570925$),
0.4,
0.65)`)
)

const (
	effSprint   = 6064631641826394197
	effGuard    = 6064631667969556480
	effEvasion  = 106223481149050
	effStun     = 6064631799761208354
	effExertion = 6064631668308443138
	effSkill    = 6064632660379712165
	effTracker  = 6064634618775274433
	effTick     = 7000000000000000001
	effBeyond   = 7000000000000000002
	effDodgeHit = 7000000000000000003
	effRest     = 109394092054139
	effDrain    = content.GuidSprintStaminaDrain

	hitTick       = 8000000000000000001
	hitEnd        = 8000000000000000002
	hitDodge      = 8000000000000000003
	hitExert      = 8000000000000000004
	hitDrain      = 8000000000000000005
	hitSprintEnd  = 8000000000000000006
	hitEvasionEnd = 8000000000000000007

	modSprint     = 9000000000000000001
	modHit        = 9000000000000000002
	modDrain      = 9000000000000000003
	modRestHealth = 109394190095997
	modRestMana   = 109394393781881
	statMoveSp    = 109228544644729
	statMaxHealth = 108986356618873
	statMaxMana   = 109183578756729
	statHealthReg = 109343399171806
	statManaReg   = 109343399171796
)

func attributeCurve(i int) int64 { return int64(5000 + i) }

func formula(curve int64, mult string) string {
	return fmt.Sprintf("const v = EvalCurve($#7037248826340794574:%d$, Float(GetCharacterLevel(GetOwner())));\r\nRound(v * %s)", curve, mult)
}

// ~ C | O | A :: Cyb ~

func syntheticStatRows() []designdb.Row {
	stats := []designdb.Row{
		statRow(content.GuidStamina, "Stamina", 1, st{"ValueMax": "GetStat(GetInstigator(), $#5365104778484440215:6064631747701440948$)",
			"RegenRate": "GetStat(GetSource(), $#5365104778484440215:6064631663405498419$)", "OnSpawn": "EStatResetBehavior::Reset_SetMax"}),
		statRow(content.GuidMaxStamina, "MaxStamina", 1, st{"ValueMin": "1"}),
		statRow(content.GuidStaminaRegeneration, "StaminaRegeneration", 1, st{"base": regenExpr}),
		statRow(statMoveSp, "MoveSpeedMult", 7, nil),
	}
	for _, a := range content.PrimaryAttributes {
		stats = append(stats, statRow(a.Guid, a.Name, 1, nil))
	}
	return append(stats, statRow(statMaxHealth, "MaxHealth", 7, nil), statRow(statMaxMana, "MaxMana", 7, nil),
		statRow(statHealthReg, "HealthRegeneration", 1, nil), statRow(statManaReg, "ManaRegeneration", 1, nil))
}

func syntheticCurveKeys() list {
	var keys list
	for level := 1; level <= 50; level++ {
		v := float32(100)
		if level == 20 {
			v = 335
		}
		keys = append(keys, st{"Time": float32(level), "Value": v, "InterpMode": "ERichCurveInterpMode::RCIM_Linear"})
	}
	return keys
}

func syntheticCurveRows() []designdb.Row {
	var curves []designdb.Row
	for i := range content.PrimaryAttributes {
		curves = append(curves, mkRow(attributeCurve(i), fmt.Sprintf("Attrib_%d", i),
			st{"Curve": st{"ExternalCurve": "", "EditorCurveData": st{"DefaultValue": float32(0), "Keys": syntheticCurveKeys()}}}))
	}
	return append(curves, mkRow(5100, "Empty", st{"Curve": st{"ExternalCurve": "/Game/Curves/X.X",
		"EditorCurveData": st{"DefaultValue": float32(3.4028235e+38), "Keys": list{}}}}))
}

func syntheticClassStatEntries() list {
	var entries list
	for i, a := range content.PrimaryAttributes {
		entries = append(entries, st{"StatId": statID(a.Guid), "ValueExpression": formula(attributeCurve(i), "1.3")})
	}
	return entries
}

func syntheticInitAndClassRows() ([]designdb.Row, []designdb.Row) {
	entry := func(stat int64, expr string) st { return st{"StatId": statID(stat), "ValueExpression": expr} }
	inits := []designdb.Row{mkRow(content.GuidPlayerBase, "Player_Base", st{"StatIDs": list{
		entry(content.GuidMaxStamina, "100"), entry(content.GuidStamina, "100"), entry(content.GuidStaminaRegeneration, ""),
	}})}
	var classes []designdb.Row
	for ci, name := range content.PlayerClasses {
		inits = append(inits, mkRow(int64(6000+ci), "Player_Base_"+name, st{"StatIDs": syntheticClassStatEntries()}))
		for _, n := range []string{name, name + "Stage"} {
			classes = append(classes, mkRow(int64(100+len(classes)), n, st{
				"Class": "ECharacterClass::" + n, "LevelTypeId": idv(1, 2, "Adventuring"), "ActiveSkillTreeId": idv(3, 4, n+"_SkillTable"),
				"bPrefersMelee": ci%2 == 0,
			}))
		}
	}
	return inits, classes
}

func syntheticStatModAndHitRows() ([]designdb.Row, []designdb.Row) {
	mods := []designdb.Row{
		mkRow(modSprint, "General_Sprint", st{"StatRefId": statID(statMoveSp), "Value": "0.2"}),
		mkRow(modHit, "DisableRegen_ApplyHit", st{"StatRefId": statID(content.GuidStamina), "Value": "0.001"}),
		mkRow(modDrain, "General_Sprint_StaminaDrain", st{"StatRefId": statID(content.GuidStamina), "Value": drainExpr}),
		mkRow(modRestHealth, "General_Rest_HealthRegen", st{"StatRefId": statID(statHealthReg), "Value": restHealthExpr}),
		mkRow(modRestMana, "General_Rest_ManaRegen", st{"StatRefId": statID(statManaReg), "Value": restManaExpr}),
	}
	hits := []designdb.Row{
		hitRow(hitTick, "Sprint_Tick_Hit", st{"ApplicationRequirements": "IsMoving(GetInstigator())",
			"ApplyEffects": list{applyEffect(effTick, "", "", "")}}),
		hitRow(hitEnd, "Tick_End_Hit", st{"ApplyEffects": list{applyEffect(effBeyond, "1", "x > 1", "2")},
			"EffectsAppliedToSelf": list{st{"Effect": effectID(effExertion), "MaxNumApplications": int64(1), "OnHitBehavior": "EApplyOnHitBehavior::ApplyIfHit", "StackCount": "3"}},
			"StatModsIds":          list{modID(modHit)}}),
		hitRow(hitDodge, "Dodge_Hit", st{"ApplyEffects": list{applyEffect(effDodgeHit, "", "", "")}}),
		hitRow(hitExert, "Exertion_Hit", nil),
		hitRow(hitDrain, "General_Sprint_StaminaDrain", st{"StatModsIds": list{modID(modDrain)}}),
		hitRow(hitSprintEnd, "General_Stamina_Sprint_EndHit", st{"ApplyEffects": list{applyEffect(effExertion, "", "", "")}}),
		hitRow(hitEvasionEnd, "General_Stamina_Dodge_EndHit", st{"ApplyEffects": list{applyEffect(effExertion, "", "", "")}}),
	}
	return mods, hits
}

func syntheticEffectRows() []designdb.Row {
	return []designdb.Row{
		effectRow(effSprint, "General_Sprint", st{"EffectName": txt("Sprint"), "EffectCategory": "EEffectCategory::EFFECT_Buff",
			"Tags": list{"Effect.DisableStamRegen"}, "EffectTags": list{st{"TagName": "Effect.DisableStamRegen"}},
			"bInfiniteDuration": true, "TickTimer": float32(1), "bIgnoreInitialTick": true,
			"TickHitsIds": list{hitID(hitTick)}, "EndHitsIds": list{hitID(hitSprintEnd)}, "StatModsIds": list{modID(modSprint)}}),
		effectRow(effGuard, "General_Guard", nil),
		effectRow(effEvasion, "General_Dodge_Evasion", st{"EffectDuration": "0.5", "EndHitsIds": list{hitID(hitEvasionEnd)}}),
		effectRow(effStun, "General_Guard_GuardBreak_Stun", nil),
		effectRow(effExertion, "General_Stamina_DisableRegen", st{"EffectDuration": "1", "ApplyHitsIds": list{hitID(hitExert)}}),
		effectRow(effSkill, "Rogue_Advantage", nil),
		effectRow(effTracker, "General_Stamina_Dodge_EvasiveManeuver_Tracker", st{"EffectDuration": "0.5"}),
		effectRow(effTick, "Sprint_Tick_Effect", st{"EndHitsIds": list{hitID(hitEnd)}}),
		effectRow(effBeyond, "Beyond_Effect", st{"TickHitsIds": list{hitID(hitExert)}}),
		effectRow(effDodgeHit, "Dodge_Hit_Effect", nil),
		effectRow(7000000000000000099, "Unrelated_Effect", st{"TickHitsIds": list{hitID(hitExert)}}),
		effectRow(effRest, "General_Rest", st{"bInfiniteDuration": true, "EffectDuration": "360", "StatModsIds": list{modID(modRestHealth), modID(modRestMana)}}),
		effectRow(effDrain, "General_Sprint_StaminaDrain", st{"bInfiniteDuration": true, "TickTimer": float32(0.25), "bIgnoreInitialTick": true,
			"TickHitsIds": list{hitID(hitDrain)}}),
	}
}

func syntheticAbilityRows() []designdb.Row {
	sprintCost := st{"Enabled": "IsMoving(GetOwner())", "ChargeCost": "0", "bTriggersCooldown": false,
		"StatCosts": list{st{"Stat": statID(content.GuidStamina), "Value": sprintCostExpr}}, "EffectCosts": list{}}
	dodgeCost := st{"Enabled": "", "ChargeCost": "0", "bTriggersCooldown": true,
		"StatCosts": list{st{"Stat": statID(content.GuidStamina), "Value": dodgeCostExpr}}, "EffectCosts": list{}}
	return []designdb.Row{
		abilityRow(1, "Some_Other_Ability", nil),
		abilityRow(content.GuidSprint, "General_Sprint", st{
			"AbilityName": txt("Sprint"), "ActivationRequirements": "GetStat(GetSource(), $#5365104778484440215:6064629575339934476$) >= 1.0",
			"HeldAbilityBehavior": "EHeldAbilityPressBehavior::Smart", "bOverrideHeldBehavior": true, "bHoldAnimation": true,
			"ComboInputs": list{"EComboButtons::BUTTON_Sprint"}, "Costs": list{sprintCost, st{"Enabled": "", "ChargeCost": "1",
				"bTriggersCooldown": true, "StatCosts": list{}, "EffectCosts": list{}}},
			"EffectsIds": designdb.Pairs{{Key: int64(1), Value: effectID(effSprint)}}, "CancelSectionName": "End",
			"AnimMontages": list{st{"AbilityAnimationSoftPaths": list{"/Game/Characters/Animations/00_ABLT/CM_M_ABLT_Sprint_Montage.CM_M_ABLT_Sprint_Montage"}}},
		}),
		abilityRow(content.GuidDodge, "General_Dodge", st{
			"AbilityName": txt("Dodge"), "AbilityTags": list{st{"TagName": "ability.dodge"}}, "Cooldown": dodgeCooldownExpr,
			"bDisableDuringCooldown": true, "ComboInputs": list{"EComboButtons::BUTTON_Dodge"}, "NetworkingMaxSpeed": float32(2500),
			"Costs":      list{dodgeCost},
			"EffectsIds": designdb.Pairs{{Key: int64(1), Value: effectID(effEvasion)}, {Key: int64(2), Value: effectID(effTracker)}},
			"HitsIds":    designdb.Pairs{{Key: int64(1), Value: hitID(hitDodge)}},
		}),
		abilityRow(content.GuidRest, "General_Rest", st{
			"AbilityName": txt("Rest"), "ActivationRequirements": "!IsInCombat(GetInstigator())", "HeldAbilityBehavior": "EHeldAbilityPressBehavior::Toggle",
			"ApplicableTargets": int64(1), "CancelSectionName": "SitEnd", "Cooldown": "",
			"AnimMontages": list{st{"AbilityAnimationSoftPaths": list{"/Game/Characters/Animations/00_ABLT/CM_M_ABLT_General_Rest_Montage.CM_M_ABLT_General_Rest_Montage"}}},
			"EffectsIds":   designdb.Pairs{{Key: int64(1), Value: effectID(effRest)}},
		}),
	}
}

func syntheticSource() *source {
	src := &source{rows: map[string][]designdb.Row{}, typeIDs: map[string]uint64{}}
	for schema, id := range recordTypes {
		src.typeIDs[schema] = id
		src.rows[schema] = nil
	}
	src.rows["StatTypeDefRecordBase"] = syntheticStatRows()
	src.rows["StatCurveRecordBase"] = syntheticCurveRows()
	inits, classes := syntheticInitAndClassRows()
	src.rows["StatInitializerListRecordBase"] = inits
	src.rows["CharacterClassesRecordBase"] = classes
	mods, hits := syntheticStatModAndHitRows()
	src.rows["StatModRecordBase"] = mods
	src.rows["AbilityHitRecordBase"] = hits
	src.rows["EffectRecordBase"] = syntheticEffectRows()
	src.rows["AoCAbilityRecordBase"] = syntheticAbilityRows()
	return src
}

// == C O A | C | Y | B ==

func buildSynthetic(t *testing.T, src *source) (*content.DB, error) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "content.db")
	sdb, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	defer sdb.Close()
	sdb.SetMaxOpenConns(1)
	if err := content.Create(sdb); err != nil {
		t.Fatal(err)
	}
	tx, err := sdb.BeginTx(context.Background(), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	w := &writer{tx: tx, stmts: map[string]*sql.Stmt{}}
	meta := map[string]string{}
	if err := buildTyped(w, src, &Options{Log: io.Discard}, meta); err != nil {
		return nil, err
	}
	if err := w.close(); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(`INSERT INTO meta VALUES (?, ?), (?, ?)`, content.MetaSchemaVersion, "1", content.MetaRecordEncoding, content.EncodingJSON); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	sdb.Close()
	db, err := content.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	if meta["typed.stat_def"] != "14" || meta["typed.class_stat_base"] != "2400" {
		t.Errorf("typed meta = %v", meta)
	}
	return db, nil
}

func verifySyntheticStatsAndCurves(t *testing.T, db *content.DB) {
	t.Helper()
	s, err := db.StatDefByName("Stamina")
	if err != nil || s.Replication != 1 || s.RepField != "StatRepInt32Owner" || s.Audience != "Rep_Autonomous" ||
		s.OnSpawn != "EStatResetBehavior::Reset_SetMax" || !strings.Contains(s.MaxExpr, "6064631747701440948") {
		t.Errorf("Stamina = %+v, %v", s, err)
	}
	if s, _ := db.StatDefByName("MoveSpeedMult"); s.Replication != 7 || s.RepField != "StatRepInt32EveryoneProxy" {
		t.Errorf("MoveSpeedMult = %+v", s)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM stat_curve`); n != 6*50 {
		t.Errorf("stat_curve has %d keys, want %d", n, 6*50)
	}
	var keyCount int
	var external string
	if err := db.SQL().QueryRow(`SELECT key_count, external_curve FROM stat_curve_def WHERE guid = 5100`).Scan(&keyCount, &external); err != nil || keyCount != 0 || external != "/Game/Curves/X.X" {
		t.Errorf("empty curve = %d, %q, %v", keyCount, external, err)
	}
	curves, err := db.Curves()
	if err != nil || len(curves.Curves) != 6 {
		t.Errorf("Curves = %d curves, %v", len(curves.Curves), err)
	}
}

func verifyMageLevelStats(t *testing.T, db *content.DB, level int, want float64) {
	t.Helper()
	got, err := db.ClassStatBase("Mage", level)
	if err != nil || len(got) != 6 {
		t.Fatalf("Mage level %d = %v, %v", level, got, err)
	}
	for stat, v := range got {
		if v.Value != want || v.RoundingAmbiguous != (level == 20) {
			t.Errorf("Mage level %d %s = %+v, want %v (ambiguous at level 20 only)", level, stat, v, want)
		}
	}
}

func verifySyntheticClassesAndStats(t *testing.T, db *content.DB) {
	t.Helper()
	classes, err := db.Classes()
	if err != nil || len(classes) != 8 {
		t.Fatalf("Classes = %d, %v", len(classes), err)
	}
	for i, c := range classes {
		if c.Name != content.PlayerClasses[i] || c.ArchetypeID != int64(17747+i) || c.StatInitListName != "Player_Base_"+c.Name ||
			c.StatInitListGuid != int64(6000+i) || c.SkillTreeName != c.Name+"_SkillTable" || c.PrefersMelee != (i%2 == 0) {
			t.Errorf("class %d = %+v", i, c)
		}
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM class_stat_base`); n != 2400 {
		t.Errorf("class_stat_base has %d rows", n)
	}
	for level, want := range map[int]float64{1: 130, 19: 130, 20: 436, 21: 130, 50: 130} {
		verifyMageLevelStats(t, db, level, want)
	}
	amb, err := db.RoundingAmbiguous()
	if err != nil || len(amb) != 8*6 {
		t.Errorf("RoundingAmbiguous = %d values, %v; want 48", len(amb), err)
	}
	for _, a := range amb {
		if a.Level != 20 {
			t.Errorf("ambiguous value %+v", a)
		}
	}
}

func verifySyntheticAbilities(t *testing.T, db *content.DB) {
	t.Helper()
	sprint, err := db.Ability(content.GuidSprint)
	if err != nil {
		t.Fatal(err)
	}
	if len(sprint.Costs) != 2 || sprint.Costs[0].StatName != "Stamina" || sprint.Costs[0].EnabledExpr != "IsMoving(GetOwner())" ||
		sprint.Costs[1].StatOrd != -1 || !sprint.Costs[1].TriggersCooldown || sprint.Costs[1].ChargeCostExpr != "1" {
		t.Errorf("Sprint costs = %+v", sprint)
	}
	if !sprint.OverrideHeldBehavior || len(sprint.Effects) != 1 || sprint.Effects[0].Name != "General_Sprint" ||
		sprint.DisplayName != "Sprint" || sprint.Tags == nil || len(sprint.Tags) != 0 {
		t.Errorf("Sprint = %+v", sprint)
	}
	dodge, err := db.AbilityByName("General_Dodge")
	if err != nil {
		t.Fatal(err)
	}
	if len(dodge.Effects) != 2 || dodge.Effects[1].Ord != 2 || len(dodge.Hits) != 1 || dodge.Hits[0].Name != "Dodge_Hit" ||
		fmt.Sprint(dodge.Tags) != "[ability.dodge]" || dodge.NetworkingMaxSpeed != 2500 || !dodge.Costs[0].TriggersCooldown {
		t.Errorf("Dodge = %+v", dodge)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM ability`); n != 3 {
		t.Errorf("%d abilities (Sprint, Dodge and Rest; Some_Other_Ability is not one of them)", n)
	}
}

func queryOrderedNames(t *testing.T, db *content.DB, query string) []string {
	t.Helper()
	rows, err := db.SQL().Query(query)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	var got []string
	for rows.Next() {
		var n string
		rows.Scan(&n)
		got = append(got, n)
	}
	return got
}

func verifySyntheticEffectsAndHits(t *testing.T, db *content.DB) {
	t.Helper()
	gotEffects := queryOrderedNames(t, db, `SELECT name FROM effect ORDER BY name`)
	wantEffects := "[Dodge_Hit_Effect General_Dodge_Evasion General_Guard General_Guard_GuardBreak_Stun General_Rest General_Sprint General_Sprint_StaminaDrain General_Stamina_DisableRegen General_Stamina_Dodge_EvasiveManeuver_Tracker Sprint_Tick_Effect]"
	if fmt.Sprint(gotEffects) != wantEffects {
		t.Errorf("effects = %v\nwant     %s", gotEffects, wantEffects)
	}
	gotHits := queryOrderedNames(t, db, `SELECT name FROM hit ORDER BY name`)
	if want := "[Dodge_Hit Exertion_Hit General_Sprint_StaminaDrain General_Stamina_Dodge_EndHit General_Stamina_Sprint_EndHit Sprint_Tick_Hit Tick_End_Hit]"; fmt.Sprint(gotHits) != want {
		t.Errorf("hits = %v, want %s", gotHits, want)
	}
	sprintEffect, err := db.EffectByName("General_Sprint")
	if err != nil {
		t.Fatal(err)
	}
	if !sprintEffect.InfiniteDuration || sprintEffect.TickTimer != 1 || !sprintEffect.IgnoreInitialTick || fmt.Sprint(sprintEffect.Tags) != "[Effect.DisableStamRegen]" {
		t.Errorf("Sprint effect flags = %+v", sprintEffect)
	}
	if fmt.Sprint(sprintEffect.EffectTags) != "[Effect.DisableStamRegen]" || len(sprintEffect.Hits) != 2 || sprintEffect.Hits[0].Phase != "Tick" || sprintEffect.Hits[1].Phase != "End" ||
		len(sprintEffect.StatMods) != 1 || sprintEffect.StatMods[0].StatName != "MoveSpeedMult" || sprintEffect.StatMods[0].ValueExpr != "0.2" {
		t.Errorf("Sprint effect hits/mods = %+v", sprintEffect)
	}
}

func verifySyntheticHitEffectsAndStamina(t *testing.T, db *content.DB) {
	t.Helper()
	type he struct{ kind, effect, req, stack, dur string }
	var hes []he
	hrows, err := db.SQL().Query(`SELECT kind, effect_name, requirement_expr, stack_count_expr, duration_expr FROM hit_effect
		WHERE hit_guid = ? ORDER BY kind, ord`, hitEnd)
	if err != nil {
		t.Fatal(err)
	}
	for hrows.Next() {
		var x he
		hrows.Scan(&x.kind, &x.effect, &x.req, &x.stack, &x.dur)
		hes = append(hes, x)
	}
	hrows.Close()
	if fmt.Sprint(hes) != "[{apply Beyond_Effect x > 1 1 2} {self General_Stamina_DisableRegen  3 }]" {
		t.Errorf("hit_effect of the end hit = %v", hes)
	}
	if mods, err := db.StatMods("hit", hitEnd); err != nil || len(mods) != 1 || mods[0].StatName != "Stamina" || mods[0].ValueExpr != "0.001" {
		t.Errorf("hit stat mods = %+v, %v", mods, err)
	}
	verifySyntheticStaminaRuleValues(t, db)
}

func verifySyntheticStaminaRuleValues(t *testing.T, db *content.DB) {
	t.Helper()
	rules, err := db.StaminaRules()
	if err != nil {
		t.Fatal(err)
	}
	for key, want := range map[string]string{
		content.RuleStaminaInitial: "100", content.RuleMaxStaminaInitial: "100", content.RuleRegenRateEnabled: "5",
		content.RuleRegenRateDisabled: "0", content.RuleSprintCostOutOfCombat: "1.25", content.RuleSprintCostInCombat: "2.5",
		content.RuleDodgeCostBase: "50", content.RuleDodgeCostSkilled: "30", content.RuleDodgeCooldownBase: "0.65",
		content.RuleDodgeCooldownSkilled: "0.4", content.RuleSprintChargeInterval: "0.25",
		content.RuleStatRegenTime: "1", "sprint.effect_tick_timer": "1", "sprint.cooldown": "0", "dodge.skill_effect_name": "Rogue_Advantage",
		"dodge.triggers_cooldown": "1", "sprint.triggers_cooldown": "0", "stamina.rep_field": "StatRepInt32Owner",
	} {
		if rules[key].Value != want || rules[key].Source == "" {
			t.Errorf("rule %s = %+v, want %q with non-empty source", key, rules[key], want)
		}
	}
	if !strings.Contains(rules[content.RuleStatRegenTime].Source, "no client Config directory") {
		t.Errorf("stat_regen_time source = %q", rules[content.RuleStatRegenTime].Source)
	}
	if src := rules[content.RuleSprintChargeInterval].Source; !strings.Contains(src, "General_Sprint_StaminaDrain") || !strings.Contains(src, "no record references that effect") {
		t.Errorf("charge interval source = %q", src)
	}
	dis, err := db.StaminaRegenDisables()
	if err != nil || len(dis) != 5 || dis[0].EffectName != "General_Sprint" || dis[0].DisablesStanding || !dis[0].DisablesMoving ||
		dis[4].EffectName != "General_Stamina_DisableRegen" || !dis[4].DisablesStanding {
		t.Errorf("regen disables = %+v, %v", dis, err)
	}
}

func TestSyntheticTypedTables(t *testing.T) {
	db, err := buildSynthetic(t, syntheticSource())
	if err != nil {
		t.Fatal(err)
	}
	verifySyntheticStatsAndCurves(t, db)
	verifySyntheticClassesAndStats(t, db)
	verifySyntheticAbilities(t, db)
	verifySyntheticEffectsAndHits(t, db)
	verifySyntheticHitEffectsAndStamina(t, db)
}

func verifySyntheticSprintAndDodgeRuntime(t *testing.T, sprint, dodge content.AbilityRuntime) {
	t.Helper()
	if sprint.HeldBehavior != "Smart" || sprint.MinStamina != 1 || sprint.CostAmount != 1.25 || !sprint.CostRequiresMoving || sprint.ChargeInterval != 0.25 {
		t.Errorf("Sprint cost/behavior = %+v", sprint)
	}
	if sprint.RequiresOutOfCombat || sprint.CancelSection != "End" || sprint.EffectGuid != effSprint || sprint.TriggersCooldown || !strings.Contains(sprint.MontagePath, "Sprint_Montage") {
		t.Errorf("Sprint runtime = %+v", sprint)
	}
	if dodge.HeldBehavior != "Naive" || dodge.CostAmount != 50 || dodge.CostRequiresMoving || dodge.ChargeInterval != 0 || dodge.Cooldown != 0.65 {
		t.Errorf("Dodge cost/cooldown = %+v", dodge)
	}
	if !dodge.TriggersCooldown || !dodge.DisableDuringCooldown || dodge.EffectGuid != effEvasion || dodge.MinStamina != 0 || dodge.MontagePath != "" || dodge.CancelSection != "" {
		t.Errorf("Dodge runtime = %+v", dodge)
	}
}

func verifySyntheticEffectRuntimes(t *testing.T, db *content.DB) {
	t.Helper()
	effects, err := db.EffectRuntimes()
	if err != nil || len(effects) != 4 {
		t.Fatalf("effects (%d, %v): %+v", len(effects), err, effects)
	}
	if e := effects[effSprint]; !e.Infinite || e.TickTimer != 1 || e.EndEffectGuid != effExertion || e.EndEffectDuration != 1 || e.StopsRegenStanding || !e.StopsRegenMoving {
		t.Errorf("Sprint effect = %+v", e)
	}
	if e := effects[effEvasion]; e.Infinite || e.Duration != 0.5 || e.EndEffectGuid != effExertion || !e.StopsRegenStanding {
		t.Errorf("Evasion = %+v", e)
	}
	if e := effects[effRest]; !e.Infinite || e.Duration != 0 || e.EndEffectGuid != 0 {
		t.Errorf("Rest effect = %+v", e)
	}
	if e := effects[effExertion]; e.Infinite || e.Duration != 1 {
		t.Errorf("exertion = %+v", e)
	}
}

func verifySyntheticEffectStatRuntimes(t *testing.T, db *content.DB) {
	t.Helper()
	mods, err := db.EffectStatRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	rm := mods[effRest]
	if len(rm) != 2 || rm[0].StatGuid != statHealthReg || rm[0].SourceStatGuid != statMaxHealth || rm[0].Factor != 0.01 || rm[0].Addend != 1 {
		t.Errorf("Rest health mod = %+v", rm)
	}
	if rm[1].StatGuid != statManaReg || rm[1].SourceStatGuid != statMaxMana || rm[1].Factor != 0.01 || rm[1].Addend != 1 {
		t.Errorf("Rest mana mod = %+v", rm)
	}
	if sm := mods[effSprint]; len(sm) != 1 || sm[0].Addend != 0.2 || sm[0].SourceStatGuid != 0 || sm[0].Factor != 0 {
		t.Errorf("Sprint mods = %+v", sm)
	}
}

func TestSyntheticAbilityRuntime(t *testing.T) {
	db, err := buildSynthetic(t, syntheticSource())
	if err != nil {
		t.Fatal(err)
	}
	abilities, err := db.AbilityRuntimes()
	if err != nil {
		t.Fatal(err)
	}
	verifySyntheticSprintAndDodgeRuntime(t, abilities[content.GuidSprint], abilities[content.GuidDodge])
	rest := abilities[content.GuidRest]
	if rest.HeldBehavior != "Toggle" || !rest.RequiresOutOfCombat || rest.CostStatGuid != 0 || !rest.TargetsSelf || rest.EffectGuid != effRest ||
		rest.CancelSection != "SitEnd" || !strings.Contains(rest.MontagePath, "General_Rest_Montage") || rest.Cooldown != 0 {
		t.Errorf("Rest = %+v", rest)
	}
	verifySyntheticEffectRuntimes(t, db)
	verifySyntheticEffectStatRuntimes(t, db)
}
