// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/chardef"
	"aocrop/services/internal/content"
)

// [CoA + C.Y.B]

// Captured level-1 Bard stats from replay_data.bin packet 107 (StatRepInt32Owner fast array).
var (
	capturedBardLevel1 = map[string]float64{
		"Strength": 24, "Dexterity": 24, "Constitution": 30, "Mentality": 30, "Wisdom": 36,
	}
	capturedBardLevel1Intelligence = 60.0
	capturedStamina                = map[string]float64{"Stamina": 100, "MaxStamina": 100, "StaminaRegeneration": 5}
)

func TestRealGoldenBard(t *testing.T) {
	db := builtDB(t)
	got, err := db.ClassStatBase("Bard", 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 6 {
		t.Errorf("Bard level 1 has %d attributes, want 6: %v", len(got), got)
	}
	for stat, want := range capturedBardLevel1 {
		if got[stat].Value != want {
			t.Errorf("Bard level 1 %s = %v, the capture has %v", stat, got[stat].Value, want)
		}
		if got[stat].RoundingAmbiguous {
			t.Errorf("Bard level 1 %s is flagged as depending on the arithmetic's width", stat)
		}
	}
}

// Player_Base_Bard Round(curve * 1.3) at level 1 gives 39 while packet 107 sent 60 (spec §6 open item).
func TestKnownDifferenceBardIntelligence(t *testing.T) {
	db := builtDB(t)
	got, err := db.ClassStatBase("Bard", 1)
	if err != nil {
		t.Fatal(err)
	}
	if got["Intelligence"].Value != 39 {
		t.Errorf("Bard level 1 Intelligence = %v, the formula gives 39", got["Intelligence"].Value)
	}
	if got["Intelligence"].Value == capturedBardLevel1Intelligence {
		t.Errorf("Intelligence now matches the capture (%v): the open item is closed, update spec §6 and this test", capturedBardLevel1Intelligence)
	}
	t.Logf("KNOWN DIFFERENCE: Bard level 1 Intelligence is %v by Player_Base_Bard's formula and %v in the capture (packet 107)",
		got["Intelligence"].Value, capturedBardLevel1Intelligence)
}

var guidDefaultClasses = regexp.MustCompile(`\((\w+), \(Guid=(\d+)\)\)`)

func verifyDefaultClassesINI(t *testing.T, config string, byName map[string]content.Class) {
	t.Helper()
	if config == "" {
		t.Log("AOC_CLIENT_CONFIG is not set: the class Guids are not checked against DefaultAoCRecordConstants.ini")
		return
	}
	data, err := os.ReadFile(filepath.Join(config, "DefaultAoCRecordConstants.ini"))
	if err != nil {
		t.Logf("%v: the class Guids are not checked against the ini", err)
		return
	}
	line := ""
	for _, l := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(l, "DefaultClasses=") {
			line = l
		}
	}
	matches := guidDefaultClasses.FindAllStringSubmatch(line, -1)
	if len(matches) != 8 {
		t.Fatalf("DefaultClasses in the ini lists %d classes: %q", len(matches), line)
	}
	for _, m := range matches {
		g, _ := strconv.ParseInt(m[2], 10, 64)
		if byName[m[1]].Guid != g {
			t.Errorf("%s: content.db has Guid %d, DefaultClasses has %d", m[1], byName[m[1]].Guid, g)
		}
	}
}

func TestRealClasses(t *testing.T) {
	_, _, config := realInputs(t)
	db := builtDB(t)
	classes, err := db.Classes()
	if err != nil {
		t.Fatal(err)
	}
	if len(classes) != 8 {
		t.Fatalf("%d classes: %+v", len(classes), classes)
	}
	wantOrder := []string{"Bard", "Cleric", "Fighter", "Mage", "Ranger", "Rogue", "Summoner", "Tank"}
	for i, c := range classes {
		enum, _ := chardef.ClassByName(c.Name)
		if c.Name != wantOrder[i] || c.ArchetypeID != int64(17747+i) || int64(chardef.ArchetypeID(enum)) != c.ArchetypeID {
			t.Errorf("class %d = %s with archetype %d (chardef says %d), want %s %d", i, c.Name, c.ArchetypeID, chardef.ArchetypeID(enum), wantOrder[i], 17747+i)
		}
		if c.ClassEnum != "ECharacterClass::"+c.Name || c.StatInitListName != "Player_Base_"+c.Name || c.StatInitListGuid == 0 {
			t.Errorf("class %+v", c)
		}
		if c.LevelTypeName != "Adventuring" || c.LevelTypeGuid != 108651085518458 {
			t.Errorf("%s level type = %s (%d)", c.Name, c.LevelTypeName, c.LevelTypeGuid)
		}
	}
	byName := map[string]content.Class{}
	for _, c := range classes {
		byName[c.Name] = c
	}
	for name, want := range map[string]struct {
		guid  int64
		melee bool
	}{
		"Bard": {108640158570105, false}, "Tank": {108640201692798, true}, "Mage": {108640210802303, false},
	} {
		if c := byName[name]; c.Guid != want.guid || c.PrefersMelee != want.melee {
			t.Errorf("%s = %+v, want Guid %d, melee %v", name, c, want.guid, want.melee)
		}
	}
	verifyDefaultClassesINI(t, config, byName)
}

var attributeFormula = regexp.MustCompile(`^const v = EvalCurve\(\$#7037248826340794574:(\d+)\$, Float\(GetCharacterLevel\(GetOwner\(\)\)\)\);\r\nRound\(v \* ([0-9.]+)\)$`)

type classStatFormulaCounts struct {
	ambiguous         map[[3]string]bool
	halves, differing int
}

func isPrimaryAttrGuid(guid int64) bool {
	for _, a := range content.PrimaryAttributes {
		if a.Guid == guid {
			return true
		}
	}
	return false
}

func verifyClassStatFormulaLevels(t *testing.T, db *content.DB, c content.Class, in content.StatInit, counts *classStatFormulaCounts) {
	t.Helper()
	m := attributeFormula.FindStringSubmatch(in.Expression)
	if m == nil {
		t.Errorf("%s %s: formula %q has an unexpected shape", c.Name, in.StatName, in.Expression)
		return
	}
	curve, _ := strconv.ParseInt(m[1], 10, 64)
	mult, _ := strconv.ParseFloat(m[2], 64)
	for level := 1; level <= 50; level++ {
		var v float64
		if err := db.SQL().QueryRow(`SELECT value FROM stat_curve WHERE guid = ? AND level = ?`, curve, level).Scan(&v); err != nil {
			t.Fatalf("curve %d level %d: %v", curve, level, err)
		}
		x := v * mult
		want64 := math.Floor(x + 0.5)
		x32 := float32(v) * float32(mult)
		want32 := math.Floor(float64(x32 + 0.5))
		var got float64
		var amb int
		if err := db.SQL().QueryRow(`SELECT value, rounding_ambiguous FROM class_stat_base WHERE class_guid = ? AND level = ? AND stat_guid = ?`,
			c.Guid, level, in.StatGuid).Scan(&got, &amb); err != nil {
			t.Fatalf("%s %s level %d: %v", c.Name, in.StatName, level, err)
		}
		if got != want64 {
			t.Errorf("%s %s level %d = %v, want round(%v * %v) = %v", c.Name, in.StatName, level, got, v, mult, want64)
		}
		if got != math.Trunc(got) {
			t.Errorf("%s %s level %d = %v is not an integer", c.Name, in.StatName, level, got)
		}
		if frac := x - math.Floor(x); math.Abs(frac-0.5) < 1e-6 {
			counts.halves++
		}
		if want64 != want32 {
			counts.differing++
			counts.ambiguous[[3]string{c.Name, in.StatName, strconv.Itoa(level)}] = true
		}
		if (amb == 1) != (want64 != want32) {
			t.Errorf("%s %s level %d: rounding_ambiguous = %d, but float64 gives %v and float32 %v", c.Name, in.StatName, level, amb, want64, want32)
		}
	}
}

func verifySurveyClassStatSamples(t *testing.T, db *content.DB) {
	t.Helper()
	for _, w := range []struct {
		class string
		level int
		stat  string
		want  float64
	}{
		{"Tank", 1, "Constitution", 39}, {"Tank", 2, "Constitution", 88}, {"Tank", 3, "Constitution", 153},
		{"Tank", 4, "Constitution", 198}, {"Tank", 50, "Constitution", 671},
		{"Bard", 2, "Constitution", 68}, {"Bard", 50, "Constitution", 516},
		{"Mage", 1, "Intelligence", 39}, {"Mage", 1, "Strength", 24},
	} {
		got, err := db.ClassStatBase(w.class, w.level)
		if err != nil || got[w.stat].Value != w.want {
			t.Errorf("%s level %d %s = %v, %v; want %v", w.class, w.level, w.stat, got[w.stat].Value, err, w.want)
		}
	}
}

func TestRealClassStatBase(t *testing.T) {
	db := builtDB(t)
	classes, err := db.Classes()
	if err != nil {
		t.Fatal(err)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM class_stat_base`); n != 2400 {
		t.Fatalf("class_stat_base has %d rows, want 2400", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM stat_curve_def WHERE name LIKE 'Player_Base_Attrib_%' AND key_count = 50 AND external_curve = ''`); n != 6 {
		t.Errorf("%d of the 6 Player_Base_Attrib curves have 50 keys and no external curve", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM stat_curve WHERE name LIKE 'Player_Base_Attrib_%' AND interp != 'Linear'`); n != 0 {
		t.Errorf("%d keys of the attribute curves are not linear", n)
	}
	counts := &classStatFormulaCounts{ambiguous: map[[3]string]bool{}}
	for _, c := range classes {
		inits, err := db.StatInits(c.StatInitListName)
		if err != nil {
			t.Fatal(err)
		}
		attrs := 0
		for _, in := range inits {
			if !isPrimaryAttrGuid(in.StatGuid) {
				continue
			}
			attrs++
			verifyClassStatFormulaLevels(t, db, c, in, counts)
		}
		if attrs != 6 {
			t.Errorf("%s: %d attribute formulas, want 6", c.Name, attrs)
		}
	}
	if counts.halves != 80 || counts.differing != 7 {
		t.Errorf("%d values on a half, %d where float32 differs; the survey found 80 and 7", counts.halves, counts.differing)
	}
	amb, err := db.RoundingAmbiguous()
	if err != nil {
		t.Fatal(err)
	}
	if len(amb) != 7 {
		t.Errorf("RoundingAmbiguous lists %d values, want 7: %+v", len(amb), amb)
	}
	for _, a := range amb {
		if a.Level != 20 || a.Value != 436 || !counts.ambiguous[[3]string{a.Class, a.Stat, strconv.Itoa(a.Level)}] {
			t.Errorf("unexpected ambiguous value %+v", a)
		}
	}
	verifySurveyClassStatSamples(t, db)
}

// [C_Y_B :: CoA]

func verifyCapturedFieldNames(t *testing.T, db *content.DB, field string, names []string) {
	t.Helper()
	for _, name := range names {
		s, err := db.StatDefByName(name)
		if err != nil {
			t.Errorf("%s: %v", name, err)
			continue
		}
		if s.RepField != field {
			t.Errorf("%s: the capture sends it in %s, content.db says %q (replication %d)", name, field, s.RepField, s.Replication)
		}
	}
}

func verifyCapturedStatRepFields(t *testing.T, db *content.DB) {
	t.Helper()
	capture := map[string][]string{
		"StatRepInt32EveryoneProxy": {"CharacterType", "Corruption", "GravityScale", "Health", "MagicalPenetration", "Mana",
			"MaxHealth", "MaxMana", "MoveSpeedMult", "PhysicalPenetration", "StealthLevel"},
		"StatRepInt32OwnerProxy": {"ArcaneDamage", "ArcaneDamageAddMult", "ArcaneDamageMult", "ArcaneMitigation", "BleedDamage",
			"BlockChance", "BlockMitigation", "BludgeonDamage"},
		"StatRepInt32Owner": {"Accuracy", "AccuracyRating", "AdventuringXPAddMult", "ArcaneDamageRating", "ArcaneMitigationRating",
			"Strength", "Dexterity", "Constitution", "Mentality", "Intelligence", "Wisdom", "Stamina", "MaxStamina", "StaminaRegeneration"},
	}
	for field, names := range capture {
		verifyCapturedFieldNames(t, db, field, names)
	}
}

func TestRealStatDefs(t *testing.T) {
	db := builtDB(t)
	for _, w := range []struct {
		name         string
		guid         int64
		replication  int
		field        string
		min, max     string
		onSpawn      string
		regenContain string
	}{
		{"Stamina", content.GuidStamina, 1, "StatRepInt32Owner", "0", "GetStat(GetInstigator(), $#5365104778484440215:6064631747701440948$)", "EStatResetBehavior::Reset_SetMax", "$#5365104778484440215:6064631663405498419$"},
		{"MaxStamina", content.GuidMaxStamina, 1, "StatRepInt32Owner", "1", "", "EStatResetBehavior::Reset_None", ""},
		{"StaminaRegeneration", content.GuidStaminaRegeneration, 1, "StatRepInt32Owner", "0", "", "EStatResetBehavior::Reset_None", ""},
		{"Strength", content.GuidStrength, 1, "StatRepInt32Owner", "0", "", "EStatResetBehavior::Reset_None", ""},
	} {
		s, err := db.StatDefByName(w.name)
		if err != nil {
			t.Fatal(err)
		}
		if s.Guid != w.guid || s.Replication != w.replication || s.RepField != w.field || s.MinExpr != w.min || s.MaxExpr != w.max ||
			s.OnSpawn != w.onSpawn || !strings.Contains(s.RegenExpr, w.regenContain) || s.StatType != "EStatRecordType::Type_Float" {
			t.Errorf("%s = %+v", w.name, s)
		}
	}
	verifyCapturedStatRepFields(t, db)
	if n := scalarInt(t, db, `SELECT count(*) FROM stat_def WHERE rep_field IS NOT NULL AND replication NOT IN (1, 5, 7)`); n != 0 {
		t.Errorf("%d stats have a rep_field with another replication value", n)
	}
	if s, _ := db.StatDefByName("Health"); s.Audience != "Rep_Autonomous|Rep_Simulated|Rep_Proxy" || s.Replication != 7 {
		t.Errorf("Health = %+v", s)
	}
}

// (Cyb : C.O.A)

func verifyCapturedStaminaRules(t *testing.T, db *content.DB) {
	t.Helper()
	for name, want := range capturedStamina {
		var rule string
		switch name {
		case "Stamina":
			rule = content.RuleStaminaInitial
		case "MaxStamina":
			rule = content.RuleMaxStaminaInitial
		case "StaminaRegeneration":
			rule = content.RuleRegenRateEnabled
		}
		rules, err := db.StaminaRules()
		if err != nil {
			t.Fatal(err)
		}
		if v, err := rules[rule].Float(); err != nil || v != want {
			t.Errorf("%s: rule %s = %q, the capture has %v", name, rule, rules[rule].Value, want)
		}
	}
	inits, err := db.StatInits("Player_Base")
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, in := range inits {
		got[in.StatName] = in.Expression
	}
	if got["MaxStamina"] != "100" || got["Stamina"] != "100" || got["StaminaRegeneration"] != "" {
		t.Errorf("Player_Base: MaxStamina %q, Stamina %q, StaminaRegeneration %q", got["MaxStamina"], got["Stamina"], got["StaminaRegeneration"])
	}
}

func verifyRealStaminaRuleMap(t *testing.T, rules map[string]content.Rule, config string) {
	t.Helper()
	num := func(key string) float64 {
		t.Helper()
		r, ok := rules[key]
		if !ok {
			t.Fatalf("no stamina rule %s", key)
		}
		v, err := r.Float()
		if err != nil {
			t.Fatalf("rule %s = %q: %v", key, r.Value, err)
		}
		if r.Source == "" {
			t.Errorf("rule %s has no source", key)
		}
		return v
	}
	for key, want := range map[string]float64{
		content.RuleStaminaInitial:        100,
		content.RuleMaxStaminaInitial:     100,
		content.RuleRegenRateEnabled:      5,
		content.RuleRegenRateDisabled:     0,
		content.RuleSprintCostOutOfCombat: 1.25,
		content.RuleSprintCostInCombat:    2.5,
		"sprint.cooldown":                 0,
		"sprint.effect_tick_timer":        1,
		content.RuleDodgeCostBase:         50,
		content.RuleDodgeCostSkilled:      30,
		content.RuleDodgeCooldownBase:     0.65,
		content.RuleDodgeCooldownSkilled:  0.4,
		content.RuleStatRegenTime:         1,
	} {
		if got := num(key); got != want {
			t.Errorf("rule %s = %v, want %v", key, got, want)
		}
	}
	if v, err := aocexpr.EvalFloat(rules[content.RuleRegenBaseExpr].Value, ruleEnv{}.env()); err != nil || v != 5 {
		t.Errorf("StaminaRegeneration base expression = %v, %v; want 5", v, err)
	}
	if v, err := aocexpr.EvalFloat(rules[content.RuleRegenBaseExpr].Value, ruleEnv{moving: true}.with(6064631641826394197).env()); err != nil || v != 0 {
		t.Errorf("StaminaRegeneration while sprinting = %v, %v; want 0", v, err)
	}
	src := rules[content.RuleStatRegenTime].Source
	if _, err := os.Stat(filepath.Join(config, "DefaultCombat.ini")); config != "" && err == nil {
		if !strings.HasPrefix(src, "DefaultCombat.ini") {
			t.Errorf("stat_regen_time source = %q, want DefaultCombat.ini", src)
		}
	} else if !strings.Contains(src, "the default 1 s") {
		t.Errorf("stat_regen_time source = %q, want the documented default", src)
	}
}

func TestRealStamina(t *testing.T) {
	_, _, config := realInputs(t)
	db := builtDB(t)
	verifyCapturedStaminaRules(t, db)
	rules, err := db.StaminaRules()
	if err != nil {
		t.Fatal(err)
	}
	verifyRealStaminaRuleMap(t, rules, config)
	if r := rules[content.RuleSprintChargeInterval]; r.Value != "0.25" || !strings.Contains(r.Source, "General_Sprint_StaminaDrain") ||
		!strings.Contains(r.Source, "no record references that effect") {
		t.Errorf("sprint charge interval = %+v", r)
	}
	dis, err := db.StaminaRegenDisables()
	if err != nil {
		t.Fatal(err)
	}
	wantDis := []content.RegenDisable{
		{Ord: 0, EffectGuid: 6064631641826394197, EffectName: "General_Sprint", DisablesStanding: false, DisablesMoving: true},
		{Ord: 1, EffectGuid: 6064631667969556480, EffectName: "General_Guard", DisablesStanding: true, DisablesMoving: true},
		{Ord: 2, EffectGuid: 106223481149050, EffectName: "General_Dodge_Evasion", DisablesStanding: true, DisablesMoving: true},
		{Ord: 3, EffectGuid: 6064631799761208354, EffectName: "General_Guard_GuardBreak_Stun", DisablesStanding: true, DisablesMoving: true},
		{Ord: 4, EffectGuid: 6064631668308443138, EffectName: "General_Stamina_DisableRegen", DisablesStanding: true, DisablesMoving: true},
	}
	if fmt.Sprint(dis) != fmt.Sprint(wantDis) {
		t.Errorf("regen disables = %+v\nwant %+v", dis, wantDis)
	}
}

// (Cyb + C O A)

func verifyRealSprintAbilityFields(t *testing.T, sprint content.Ability) {
	t.Helper()
	if sprint.Guid != content.GuidSprint || sprint.DisplayName != "Sprint" || sprint.CooldownExpr != "0" ||
		sprint.HeldBehavior != "EHeldAbilityPressBehavior::Smart" || !sprint.OverrideHeldBehavior || !sprint.HoldAnimation ||
		sprint.DisableDuringCooldown || !sprint.UsableInAir || sprint.UsableWhileSwimming || !sprint.UsableWhileWalking ||
		sprint.UsableWhileMounted || fmt.Sprint(sprint.ComboInputs) != "[EComboButtons::BUTTON_Sprint]" ||
		sprint.ActivationRequirements != "GetStat(GetSource(), $#5365104778484440215:6064629575339934476$) >= 1.0" {
		t.Errorf("Sprint = %+v", sprint)
	}
}

func verifyRealSprintAbilityCostAndEffects(t *testing.T, sprint content.Ability) {
	t.Helper()
	if len(sprint.Costs) != 1 {
		t.Fatalf("Sprint has %d costs: %+v", len(sprint.Costs), sprint.Costs)
	}
	sc := sprint.Costs[0]
	if sc.EnabledExpr != "IsMoving(GetOwner())" || sc.TriggersCooldown || sc.StatGuid != content.GuidStamina || sc.StatName != "Stamina" ||
		sc.ChargeCostExpr != "0" || !strings.Contains(sc.Expression, "var Cost = 1.25;") || !strings.Contains(sc.Expression, "set Cost = 2.5;") ||
		!strings.Contains(sc.Expression, "IsInCombat(GetSource())") {
		t.Errorf("Sprint cost = %+v", sc)
	}
	if len(sprint.Effects) != 1 || sprint.Effects[0].Name != "General_Sprint" || sprint.Effects[0].Guid != 6064631641826394197 || len(sprint.Hits) != 0 {
		t.Errorf("Sprint effects = %+v, hits = %+v", sprint.Effects, sprint.Hits)
	}
}

func verifyRealDodgeAbilityFields(t *testing.T, dodge content.Ability) {
	t.Helper()
	if dodge.Guid != content.GuidDodge || dodge.DisplayName != "Dodge" || !dodge.DisableDuringCooldown || dodge.HeldBehavior != "EHeldAbilityPressBehavior::Naive" ||
		dodge.OverrideHeldBehavior || fmt.Sprint(dodge.Tags) != "[ability.dodge]" || dodge.NetworkingMaxSpeed != 2500 ||
		!dodge.UsableInAir || !dodge.UsableWhileSwimming || dodge.UsableWhileMounted {
		t.Errorf("Dodge = %+v", dodge)
	}
	if !strings.HasPrefix(dodge.CooldownExpr, "SelectFloat(") || !strings.HasSuffix(dodge.CooldownExpr, "0.4,\r\n0.65)") {
		t.Errorf("Dodge cooldown = %q", dodge.CooldownExpr)
	}
}

func verifyRealDodgeAbilityCostAndEffects(t *testing.T, dodge content.Ability) {
	t.Helper()
	if len(dodge.Costs) != 1 {
		t.Fatalf("Dodge has %d costs", len(dodge.Costs))
	}
	dc := dodge.Costs[0]
	if dc.EnabledExpr != "" || !dc.TriggersCooldown || dc.StatName != "Stamina" || !strings.Contains(dc.Expression, "30,\r\n50) * mult") ||
		!strings.Contains(dc.Expression, "Min((snareStacks + chilled),3)") {
		t.Errorf("Dodge cost = %+v", dc)
	}
	var names []string
	for _, e := range dodge.Effects {
		names = append(names, e.Name)
	}
	if fmt.Sprint(names) != "[General_Dodge_Evasion General_Stamina_Dodge_EvasiveManeuver_Tracker]" {
		t.Errorf("Dodge effects = %v", names)
	}
}

func effectHitsString(e content.Effect) string {
	var parts []string
	for _, h := range e.Hits {
		parts = append(parts, fmt.Sprintf("%s:%s", h.Phase, h.Name))
	}
	return strings.Join(parts, " ")
}

func verifyRealSprintEffect(t *testing.T, db *content.DB) {
	t.Helper()
	e, err := db.EffectByName("General_Sprint")
	if err != nil {
		t.Fatal(err)
	}
	if !e.InfiniteDuration || e.TickTimer != 1 || !e.IgnoreInitialTick || fmt.Sprint(e.Tags) != "[Effect.DisableStamRegen]" ||
		fmt.Sprint(e.EffectTags) != "[Effect.DisableStamRegen]" || e.DisplayName != "Sprint" || e.Category != "EEffectCategory::EFFECT_Buff" ||
		effectHitsString(e) != "Tick:General_Stamina_Sprint_StartMoving_Hit Tick:General_Stamina_Sprint_SecondWind_Charge_Hit Tick:General_Sprint_StopMoving_Exertion End:General_Stamina_Sprint_EndHit" {
		t.Errorf("effect General_Sprint = %+v (hits %s)", e, effectHitsString(e))
	}
	if len(e.StatMods) != 1 || e.StatMods[0].StatName != "MoveSpeedMult" || !strings.HasPrefix(e.StatMods[0].ValueExpr, "const skilled1 = GetSkillPurchased(") ||
		!strings.HasSuffix(e.StatMods[0].ValueExpr, "SelectFloat(skilled3, 0.05, 0.0)") {
		t.Errorf("General_Sprint stat mods = %+v", e.StatMods)
	}
}

func verifyRealNotMovingAndExertionEffects(t *testing.T, db *content.DB) {
	t.Helper()
	e, err := db.EffectByName("General_Sprint_NotMoving")
	if err != nil {
		t.Fatal(err)
	}
	if !e.InfiniteDuration || e.TickTimer != 0.25 || !e.IgnoreInitialTick || e.GlobalStackLimit != 1 || e.Category != "EEffectCategory::EFFECT_Hidden" {
		t.Errorf("effect General_Sprint_NotMoving = %+v", e)
	}
	e, err = db.EffectByName("General_Stamina_DisableRegen")
	if err != nil {
		t.Fatal(err)
	}
	if e.DisplayName != "Exertion" || e.InfiniteDuration || e.DurationExpr != "1" || e.TickTimer != 0 ||
		effectHitsString(e) != "Apply:General_Stamina_StamRegenDisable_ApplyHit End:General_Stamina_StamRegenDisable_EndHit" {
		t.Errorf("effect General_Stamina_DisableRegen = %+v (hits %s)", e, effectHitsString(e))
	}
	applyHit, err := db.StatMods("hit", scalarGuid(t, db, "hit", "General_Stamina_StamRegenDisable_ApplyHit"))
	if err != nil || len(applyHit) != 1 || applyHit[0].StatName != "Stamina" || applyHit[0].ValueExpr != "0.001" {
		t.Errorf("DisableRegen apply hit stat mods = %+v, %v", applyHit, err)
	}
}

func verifyRealDodgeEffectsAndRefs(t *testing.T, db *content.DB) {
	t.Helper()
	e, err := db.EffectByName("General_Dodge_Evasion")
	if err != nil {
		t.Fatal(err)
	}
	if e.DisplayName != "Evading" || e.InfiniteDuration || e.DurationExpr != "0.5" ||
		effectHitsString(e) != "PreApply:General_Stamina_Dodge_EvasiveManeuver_Apply_Hit Apply:General_Stamina_Dodge_Recuperation_Counter_Apply Apply:General_Stamina_Dodge_GracefulReflexes_Cleanse_Hit End:General_Stamina_Dodge_EndHit" {
		t.Errorf("effect General_Dodge_Evasion = %+v (hits %s)", e, effectHitsString(e))
	}
	if len(e.StatMods) != 2 || e.StatMods[0].StatName != "MagicalEvasion" || e.StatMods[1].StatName != "PhysicalEvasion" ||
		!strings.HasPrefix(e.StatMods[1].ValueExpr, "1.0") {
		t.Errorf("General_Dodge_Evasion stat mods = %+v", e.StatMods)
	}
	e, err = db.EffectByName("General_Stamina_Dodge_EvasiveManeuver_Tracker")
	if err != nil {
		t.Fatal(err)
	}
	if e.DurationExpr != "0.5" || !strings.HasPrefix(e.ApplicationRequirements, "GetSkillPurchased(GetOwner(),") {
		t.Errorf("effect Evasive Maneuver Tracker = %+v", e)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM effect`); n < 6 {
		t.Errorf("only %d effects", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM effect_hit WHERE hit_guid NOT IN (SELECT guid FROM hit)`); n != 0 {
		t.Errorf("%d effect hits have no hit row", n)
	}
	if n := scalarInt(t, db, `SELECT count(*) FROM hit_effect WHERE effect_guid NOT IN (SELECT guid FROM record WHERE type_id = ?)`, int64(content.TypeEffect)); n != 0 {
		t.Errorf("%d hit effects name no Effect record", n)
	}
}

func TestRealSprintAndDodge(t *testing.T) {
	db := builtDB(t)
	sprint, err := db.AbilityByName("General_Sprint")
	if err != nil {
		t.Fatal(err)
	}
	verifyRealSprintAbilityFields(t, sprint)
	verifyRealSprintAbilityCostAndEffects(t, sprint)

	dodge, err := db.AbilityByName("General_Dodge")
	if err != nil {
		t.Fatal(err)
	}
	verifyRealDodgeAbilityFields(t, dodge)
	verifyRealDodgeAbilityCostAndEffects(t, dodge)

	verifyRealSprintEffect(t, db)
	verifyRealNotMovingAndExertionEffects(t, db)
	verifyRealDodgeEffectsAndRefs(t, db)
}

func verifyGoldenCurveKeys(t *testing.T, db *content.DB, c struct {
	guid int64
	name string
}) {
	t.Helper()
	var name string
	var keys int
	if err := db.SQL().QueryRow(`SELECT name, key_count FROM stat_curve_def WHERE guid = ?`, c.guid).Scan(&name, &keys); err != nil || name != c.name || keys != 50 {
		t.Errorf("curve %d = %q with %d keys, %v; want %s with 50", c.guid, name, keys, err, c.name)
		return
	}
	for i, want := range goldenCurveValues {
		var level, value float64
		var interp string
		if err := db.SQL().QueryRow(`SELECT level, value, interp FROM stat_curve WHERE guid = ? AND ord = ?`, c.guid, i).Scan(&level, &value, &interp); err != nil ||
			level != float64(i+1) || value != want || interp != "Linear" {
			t.Errorf("curve %s key %d = (%v, %v, %s), %v; want (%d, %v, Linear)", c.name, i, level, value, interp, err, i+1, want)
			break
		}
	}
}

func verifyGoldenFormulaLevels(t *testing.T, db *content.DB, f struct {
	class      string
	stat       string
	curve      int64
	multiplier string
}) int {
	t.Helper()
	listGuid := scalarInt(t, db, `SELECT stat_init_list_guid FROM class WHERE name = ?`, f.class)
	statGuid := scalarInt(t, db, `SELECT guid FROM stat_def WHERE name = ?`, f.stat)
	var expr string
	if err := db.SQL().QueryRow(`SELECT expression FROM stat_init WHERE list_guid = ? AND stat_guid = ?`, listGuid, statGuid).Scan(&expr); err != nil {
		t.Errorf("%s %s: %v", f.class, f.stat, err)
		return 0
	}
	if want := formula(f.curve, f.multiplier); expr != want {
		t.Errorf("%s %s: formula %q, golden %q", f.class, f.stat, expr, want)
	}
	checked := 0
	for level := 1; level <= 50; level++ {
		want64, want32, _ := goldenValue(f.multiplier, level)
		var got float64
		var amb bool
		if err := db.SQL().QueryRow(`SELECT value, rounding_ambiguous FROM class_stat_base WHERE class_name = ? AND stat_name = ? AND level = ?`,
			f.class, f.stat, level).Scan(&got, &amb); err != nil {
			t.Fatalf("%s %s level %d: %v", f.class, f.stat, level, err)
		}
		if got != want64 || amb != (want64 != want32) {
			t.Errorf("%s %s level %d = %v (ambiguous %v); golden %v, float32 %v", f.class, f.stat, level, got, amb, want64, want32)
		}
		checked++
	}
	return checked
}

func TestRealClassStatBaseMatchesTheGoldenData(t *testing.T) {
	db := builtDB(t)
	for _, c := range goldenCurves {
		verifyGoldenCurveKeys(t, db, c)
	}
	checked := 0
	for _, f := range goldenFormulas {
		checked += verifyGoldenFormulaLevels(t, db, f)
	}
	if checked != 2400 || len(goldenFormulas) != 48 {
		t.Errorf("%d values of %d formulas compared, want 2400 of 48", checked, len(goldenFormulas))
	}
}
