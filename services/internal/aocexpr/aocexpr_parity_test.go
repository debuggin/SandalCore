// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"errors"
	"testing"
)

type parityAbilityCtx struct {
	NoState
	withNodes bool
	targetCtx StatContext
	zeroMaxHP bool
}

func (c *parityAbilityCtx) Stat(r Ref) (float64, error) {
	if r.Name == "Health" {
		return 50, nil
	}
	if r.Name == "MaxHealth" {
		if c.zeroMaxHP {
			return 0, nil
		}
		return 100, nil
	}
	return 25, nil
}

func (c *parityAbilityCtx) InitRunningTotal() (float64, error)      { return 10, nil }
func (c *parityAbilityCtx) Equation(Ref) (float64, error)           { return 20, nil }
func (c *parityAbilityCtx) Formula(Ref) (float64, error)            { return 30, nil }
func (c *parityAbilityCtx) Term(Ref) (float64, error)               { return 40, nil }
func (c *parityAbilityCtx) CharacterLevel() float64                 { return 15 }
func (c *parityAbilityCtx) ActorGuid() float64                      { return 777 }
func (c *parityAbilityCtx) NpcStatDistribution(idx int) float64     { return float64(idx + 10) }
func (c *parityAbilityCtx) AbilityProgress(g string) float64        { return float64(len(g) + 1) }
func (c *parityAbilityCtx) EffectCountByInstigator(Ref) float64     { return 3 }
func (c *parityAbilityCtx) RandDouble() float64                     { return 0.5 }
func (c *parityAbilityCtx) HitHasTag(tag string) bool               { return tag == "fire" }
func (c *parityAbilityCtx) HitHasFlag(flag string) bool             { return flag == "Critical" }
func (c *parityAbilityCtx) HitTargets() float64                     { return 4 }
func (c *parityAbilityCtx) XPDebtRatio() float64                    { return 0.25 }
func (c *parityAbilityCtx) HostFn(fn int, _ string, _ []float64) float64 {
	if fn == HostNodeBiome || fn == HostNodeType || fn == HostNodeLevel {
		return 1
	}
	if fn == HostNodeRecord || fn == HostCurrentNode {
		return 2
	}
	return float64(fn)
}

func (c *parityAbilityCtx) RoleContext(role int) StatContext {
	if role == RoleTarget {
		return c.targetCtx
	}
	return nil
}

func (c *parityAbilityCtx) NodeRank(guid int64, name string) (int, bool) {
	if !c.withNodes || (guid == 999 && name == "") {
		return 0, false
	}
	return 2, true
}

func (c *parityAbilityCtx) NodeGuid(rank int) (int64, bool) {
	if !c.withNodes || rank < 0 {
		return 0, false
	}
	return int64(1000 + rank), true
}

func evalInEnv(t *testing.T, env *Env, src string) Value {
	t.Helper()
	prog, err := Parse(src)
	if err != nil {
		t.Fatalf("Parse(%q): %v", src, err)
	}
	v, err := prog.Eval(env)
	if err != nil {
		t.Fatalf("Eval(%q): %v", src, err)
	}
	return v
}

func evalMustFail(t *testing.T, env *Env, src string) {
	t.Helper()
	prog, err := Parse(src)
	if err != nil {
		t.Fatalf("Parse(%q): %v", src, err)
	}
	if _, err := prog.Eval(env); err == nil {
		t.Fatalf("expected Eval(%q) to fail", src)
	}
}

func TestEnumsHostFnAndCurveString(t *testing.T) {
	if ord, ok := EnumOrdinal("ESeasons", "Summer"); !ok || ord != 2 {
		t.Fatalf("EnumOrdinal ESeasons::Summer = (%d, %v)", ord, ok)
	}
	if _, ok := EnumOrdinal("ESeason", "NoSuch"); ok {
		t.Fatal("expected unknown enum value to fail")
	}
	if name, ok := EnumName("ESeasons", 1); !ok || name != "Spring" {
		t.Fatalf("EnumName ESeasons 1 = (%q, %v)", name, ok)
	}
	if _, ok := EnumName("ESeason", -1); ok {
		t.Fatal("expected negative ordinal to fail")
	}
	if typ, ord, ok := EnumOrdinalOf("ESeasons::Fall"); !ok || typ != "ESeason" || ord != 3 {
		t.Fatalf("EnumOrdinalOf ESeasons::Fall = (%q, %d, %v)", typ, ord, ok)
	}
	if _, _, ok := EnumOrdinalOf("MissingDelimiters"); ok {
		t.Fatal("expected missing :: to fail")
	}
	if got := RoleFn(RoleCaster, RoleOpHasAlignment); got != HostRoleBase+RoleOpHasAlignment {
		t.Fatalf("RoleFn caster = %d", got)
	}
	if got := RoleFn(RoleEffectInstigator, RoleOpEffectInst); got < 256 {
		t.Fatalf("RoleFn high role = %d", got)
	}
	if RoleOf(Object{Role: RoleTarget}) != RoleTarget {
		t.Fatal("RoleOf mismatch")
	}
	if a, b, err := ParseRolePair(RolePair(2, 5)); err != nil || a != 2 || b != 5 {
		t.Fatalf("ParseRolePair = (%d, %d, %v)", a, b, err)
	}
	for _, bad := range []string{"no_comma", "x,1", "1,y"} {
		if _, _, err := ParseRolePair(bad); err == nil {
			t.Fatalf("expected ParseRolePair(%q) to fail", bad)
		}
	}
	if InterpLinear.String() != "Linear" || InterpConstant.String() != "Constant" || InterpCubic.String() != "Cubic" || Interp(9).String() != "Interp(9)" {
		t.Fatal("Interp.String mismatch")
	}
}

func TestASTPositionsExportAndStatEnvExtras(t *testing.T) {
	exprs := []Expr{
		Num{At: Pos{1, 1}}, Bool{At: Pos{1, 2}}, RefLit{At: Pos{1, 3}}, Str{At: Pos{1, 4}}, EnumLit{At: Pos{1, 5}},
		Var{At: Pos{1, 6}}, Unary{At: Pos{1, 7}}, Binary{At: Pos{1, 8}}, Call{At: Pos{1, 9}},
	}
	for i, e := range exprs {
		if e.Pos() != (Pos{1, i + 1}) {
			t.Fatalf("Expr %d Pos() = %v", i, e.Pos())
		}
	}
	stmts := []Stmt{Decl{At: Pos{1, 10}}, Set{At: Pos{1, 11}}, If{At: Pos{1, 12}}, ExprStmt{X: Num{At: Pos{1, 13}}}}
	for i, s := range stmts {
		if s.Pos() != (Pos{1, i + 10}) {
			t.Fatalf("Stmt %d Pos() = %v", i, s.Pos())
		}
	}
	if CanonicalFunc("unknown_fn") != "unknown_fn" || CanonicalFunc("getowner") != "GetOwner" {
		t.Fatal("CanonicalFunc mismatch")
	}
	if StatDistributionIndex("Strength") != 1 || StatDistributionIndex("Unknown") != -1 {
		t.Fatal("StatDistributionIndex mismatch")
	}
	var ns NoState
	if ns.NpcStatDistribution(0) != 0 || ns.ProfessionLevel(0, "Mining") != 1 {
		t.Fatal("NoState defaults mismatch")
	}
	p1, err := Parse(`var a = true; const b = 1; set a = !false; if (1 < 2) { set a = false; } else { set a = true; } "x"`)
	if err != nil || len(p1.Tree().Stmts) != 4 {
		t.Fatalf("Parse/Tree: %v", err)
	}
	p2, err := Parse(`Lerp($#10:20$, ESeason::Winter, 0.5)`)
	if err != nil || p2.Tree().Result == nil {
		t.Fatalf("Parse/Tree call/ref: %v", err)
	}
}

func TestEvalBuiltinsLooseBoolAndIntDivide(t *testing.T) {
	ctx := &parityAbilityCtx{withNodes: true, targetCtx: &parityAbilityCtx{}}
	env := AbilityEnv(ctx, nil)
	env.IntDivide = true
	if got := evalInEnv(t, env, `Lerp(10, 20, 0.25)`); got != 12.5 {
		t.Fatalf("Lerp = %v", got)
	}
	if got := evalInEnv(t, env, `Clamp(-5, 0, 10) + Clamp(5, 0, 10) + Clamp(15, 0, 10)`); got != 15.0 {
		t.Fatalf("Clamp = %v", got)
	}
	if got := evalInEnv(t, env, `Abs(-4.5) + Abs(2.5)`); got != 7.0 {
		t.Fatalf("Abs = %v", got)
	}
	if got := evalInEnv(t, env, `7 / 2`); got != 3.0 {
		t.Fatalf("IntDivide 7/2 = %v", got)
	}
	if got := evalInEnv(t, env, `var out = 0; if ((true > 0.0) && (false < 1.0)) { set out = Abs(true) + 8; } else { set out = 0; } out`); got != 9.0 {
		t.Fatalf("LooseBool = %v", got)
	}
	if got := evalInEnv(t, env, `GetOwner() == GetTarget()`); got != true {
		t.Fatalf("SameActor Object == Object = %v", got)
	}
	rawEnv := &Env{Funcs: map[string]Func{}}
	evalMustFail(t, rawEnv, `RandDoubleRange(2, 8)`)
	rawEnv.Funcs["RandDouble"] = func([]Value) (Value, error) { return 0.5, nil }
	if got := evalInEnv(t, rawEnv, `RandDoubleRange(2, 8)`); got != 5.0 {
		t.Fatalf("RandDoubleRange custom = %v", got)
	}
	rawEnv.Funcs["RandDouble"] = func([]Value) (Value, error) { return nil, errors.New("rng fail") }
	evalMustFail(t, rawEnv, `RandDoubleRange(2, 8)`)
	for _, bad := range []string{`Lerp(1, 2)`, `Clamp(1, 2)`, `Abs(1, 2)`, `RandDoubleRange(1)`} {
		evalMustFail(t, env, bad)
	}
}

func TestStatAndAbilityTargetHitCombatFuncs(t *testing.T) {
	ctx := &parityAbilityCtx{withNodes: true, targetCtx: &parityAbilityCtx{}}
	env := AbilityEnv(ctx, nil)
	if len(AbilityFuncNames()) < 50 || len(StatFuncNames()) < 20 {
		t.Fatal("unexpected function name list length")
	}
	exprs := []string{
		`EvalEquation($#1:1$) + EvalFormula($#1:2$) + EvalF($#1:2$) + GetTerm($#1:3$)`,
		`GetNPCLevel(GetTarget()) + GetActorGuid(GetOwner()) + GetPlayerProfessionLevel(GetPlayer(), "Mining")`,
		`GetNpcStatDistribution(GetOwner(), EStatDistributionStat::Strength)`,
		`HasValidEquipment(GetOwner(), EEquipSlot::Helm)`,
		`GetAbilityProgressRatio() + GetAbilityProgressRatio("cast")`,
		`GetEffectCountByRecordAndInstigator(GetOwner(), $#1:10$, GetOwner()) + GetEffectCountByRecordAndInstigator(GetTarget(), $#1:10$, GetOwner())`,
		`CheckForDamageTag(GetHitInProgress(), "fire") && HitHasTag(GetHitInProgress(), "fire")`,
		`HitFlagsContains(GetHitFlags(GetHitInProgress(), GetTarget()), EHitFlag::Critical)`,
		`GetNumTargets(GetHitInProgress()) + GetXPDebtRatio(1) + RandDouble()`,
		`GetRemainingAbilityCharges($#1:10$) + GetRemainingAbilityCooldown($#1:10$)`,
		`GetActiveTarget(GetOwner(), ETargetingIntention::Harm) == GetActiveTarget(GetOwner(), ETargetingIntention::Support)`,
		`GetCreatureOwner(GetOwner()) == GetCreatureOwner(GetTarget())`,
	}
	for _, src := range exprs {
		_ = evalInEnv(t, env, src)
	}
	for _, bad := range []string{
		`GetActorGuid(GetTarget())`,
		`GetNpcStatDistribution(GetTarget(), EStatDistributionStat::Strength)`,
		`GetNpcStatDistribution(GetOwner(), EStatDistributionStat::Unknown)`,
		`GetActiveTarget(GetOwner(), ETargetingIntention::Other)`,
		`GetCreatureOwner(GetPlayer())`,
	} {
		evalMustFail(t, env, bad)
	}
}

func TestAbilityHostRecordActorAndDamageFuncs(t *testing.T) {
	ctx := &parityAbilityCtx{withNodes: true, targetCtx: &parityAbilityCtx{}}
	env := AbilityEnv(ctx, nil)
	exprs := []string{
		`IsApplicableTarget(GetTarget(), EAffectedTarget::Enemy) && IsAnyMovementKeyPressed() && IsClient()`,
		`Int(3.9) + RandIntRange(1, 5) + GetStatDeltaOverTime(GetOwner(), 5, $#105:1$)`,
		`GetStatModValue(GetHitEvent(), $#105:1$) + GetStatModEffectiveValue(GetHitEvent(), GetOwner(), $#105:1$)`,
		`GetAbilityComboCount() + GetRecordTypeId(GetHitSourceRecord(GetHitInProgress())) + GetRecordTypeId($#42:99$) + GetProjectileTypeId()`,
		`IsComboButtonPressed(EComboButtons::Primary) && DoesHitContainActor(GetHitInProgress(), GetTarget())`,
		`GetHitRecord(GetHitInProgress()) == GetClosestTargetToActorInHit(GetHitInProgress(), GetOwner())`,
		`GetCurrentWeaponSkilltree(GetOwner()) == $#0:1$`,
		`IsNeutral(GetOwner(), GetTarget()) && IsEnemy(GetOwner(), GetTarget()) && HasAlignment(GetOwner(), $#1:2$)`,
		`GetAngleBetween(GetOwner(), GetTarget()) + GetDistanceBetween(GetOwner(), GetTarget()) + GetHealthPercentage(GetOwner())`,
		`GetEffectRemainingDuration(GetOwner(), $#1:3$) + GetHitTriggerCount(GetOwner(), $#1:4$) + GetAbilityHitCount(GetOwner(), $#1:4$)`,
		`Now() + GetProgressDuration("g") + CalculateDamage() + CalculateDamageMultiplier() + CalculateDamageAdditiveMultiplier()`,
		`CalculateDamageMitigation() + CalculateDamageCritical() + CalculateDamagePenetration() + CalculateDamageDampening()`,
	}
	for _, src := range exprs {
		_ = evalInEnv(t, env, src)
	}
	ctx.zeroMaxHP = true
	if got := evalInEnv(t, env, `GetHealthPercentage(GetOwner())`); got != 0.0 {
		t.Fatalf("expected 0 health percentage when max=0, got %v", got)
	}
}

func TestAbilityGatherAndNodeFuncs(t *testing.T) {
	ctx := &parityAbilityCtx{withNodes: true}
	env := AbilityEnv(ctx, nil)
	exprs := []string{
		`GetArtisanshipModifiedStat(GetOwner(), $#105:1$) + GetRolledItemQuality() + GetItemRarity()`,
		`GetSourceCertificationTier() + GetGatheringInteractionBaseDuration()`,
		`GetNodeBiome() == EBiomeType::AlpineForest`,
		`Node_GetNodeType(GetNodeRecord()) == ENodeType::Scientific`,
		`Node_GetLevel(GetCurrentNode(GetOwner())) == ENodeLevel::Crossroads`,
		`Node_GetLevel(GetCurrentNode(GetPlayer())) == ENodeLevel::Crossroads`,
		`Node_IsPlayerCitizen(GetPlayer()) && Node_IsPlayerCitizenOfNode(GetPlayer(), GetNodeRecord()) && Node_IsPlayerMayor(GetPlayer())`,
		`Node_InLevelRangeInclusive(GetNodeRecord(), ENodeLevel::Wilderness, ENodeLevel::Metropolis)`,
		`EnumInt(ENodeLevel::Village) + (GetPlayerGuid() == GetPlayer())`,
	}
	for _, src := range exprs {
		_ = evalInEnv(t, env, src)
	}
	noNodeEnv := AbilityEnv(&parityAbilityCtx{withNodes: false}, nil)
	for _, bad := range []string{
		`GetNodeRecord()`,
		`Node_GetLevel($CityNode:Unknown$)`,
		`Node_IsPlayerCitizen(GetOwner())`,
		`Node_InLevelRangeInclusive(GetNodeRecord(), ENodeLevel::Wilderness, ENodeLevel::Bogus)`,
		`EnumInt(123)`,
		`EnumInt(ENodeLevel::Bogus)`,
	} {
		evalMustFail(t, noNodeEnv, bad)
	}
}
