// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
)

func (f *fnc) combatCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetAbilityProgressRatio":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no running ability)", e.Name)
		}
		group := ""
		if len(e.Args) != 0 {
			if err := f.checkCallArgs(e, args, tStr); err != nil {
				return val{}, true, err
			}
			group = args[0].str
		}
		i, err := f.c.strOperand(group)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, _ := ctxVal(OpProgress, tNum, i)
		v.float = true
		return v, true, nil
	case "GetEffectCountByRecordAndInstigator":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no instigator of an effect)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tObj, tRef, tObj); err != nil {
			return val{}, true, err
		}
		if args[0].role != aocexpr.RoleCaster || args[2].role != aocexpr.RoleCaster {
			v, err := f.hostCall(e, aocexpr.RoleFn(args[0].role, aocexpr.RoleOpEffectInst), strconv.FormatInt(args[1].ref.Guid, 10), tNum, val{t: tNum, n: nConst{float64(args[2].role)}})
			return v, true, err
		}
		i, err := f.c.ref(args[1].ref)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, err := ctxVal(OpEffectInst, tNum, i)
		return v, true, err
	case "RandDoubleRange":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no random source)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tNum, tNum); err != nil {
			return val{}, true, err
		}
		r, _ := ctxVal(OpRand, tNum)
		return val{t: tNum, n: nFn3{OpLerp, args[0].n, args[1].n, r.n}, float: true}, true, nil
	case "RandDouble":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no random source)", e.Name)
		}
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		v, _ := ctxVal(OpRand, tNum)
		v.float = true
		return v, true, nil
	}
	return f.hitContextCall(e, args)
}

func (f *fnc) hitContextCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetHitInProgress":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := argc(e, 0); err != nil {
			return val{}, true, err
		}
		return val{t: tObj}, true, nil
	case "CheckForDamageTag", "HitHasTag":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tObj, tStr); err != nil {
			return val{}, true, err
		}
		i, err := f.c.strOperand(args[1].str)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, err := ctxVal(OpHitTag, tBool, i)
		return v, true, err
	case "GetHitFlags":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if len(e.Args) == 1 {
			if err := f.checkCallArgs(e, args, tObj); err != nil {
				return val{}, true, err
			}
			return val{t: tFlags}, true, nil
		}
		if err := f.checkCallArgs(e, args, tObj, tObj); err != nil {
			return val{}, true, err
		}
		return val{t: tFlags}, true, nil
	}
	return f.hitFlagsAndTargetsCall(e, args)
}

func (f *fnc) hitFlagsAndTargetsCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "HitFlagsContains":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tFlags, tEnum); err != nil {
			return val{}, true, err
		}
		if args[1].enum.Type != "EHitFlag" {
			return val{}, true, errAt(e.Args[1].Pos(), "HitFlagsContains: %v is not an EHitFlag", args[1].enum)
		}
		i, err := f.c.strOperand(args[1].enum.Value)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, err := ctxVal(OpHitFlag, tBool, i)
		return v, true, err
	case "GetNumTargets":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tObj); err != nil {
			return val{}, true, err
		}
		v, err := ctxVal(OpHitTargets, tNum)
		return v, true, err
	case "GetXPDebtRatio":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no experience debt)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tActor); err != nil {
			return val{}, true, err
		}
		v, _ := ctxVal(OpXpDebt, tNum)
		v.float = true
		return v, true, nil
	}
	return val{}, false, nil
}

func (f *fnc) stateReadCall(e aocexpr.Call, args []val) (val, bool, error) {
	if v, ok, err := f.npcAndLootCall(e, args); ok {
		return v, true, err
	}
	if v, ok, err := f.evalAndTermCall(e, args); ok {
		return v, true, err
	}
	return f.effectAndEquipCall(e, args)
}

func (f *fnc) npcAndLootCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetPlayerLevel":
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostPlayerLevel, "", tNum)
		return v, true, err
	case "GetNarrativeContentLevel":
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostNarrativeLevel, "", tNum)
		return v, true, err
	case "DoesNPCContainTag":
		if err := f.checkCallArgs(e, args, tStr); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostNpcTag, args[0].str, tBool)
		return v, true, err
	case "GetPlayerLootMaxLevel", "GetPlayerLootAvgLuck":
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		fn := aocexpr.HostLootMaxLevel
		if e.Name == "GetPlayerLootAvgLuck" {
			fn = aocexpr.HostLootAvgLuck
		}
		v, err := f.hostCall(e, fn, "", tNum)
		v.float = true
		return v, true, err
	}
	return f.npcLevelAndDistCall(e, args)
}

func (f *fnc) npcLevelAndDistCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetCharacterLevel", "GetNPCLevel":
		if len(e.Args) == 0 && e.Name == "GetNPCLevel" {
			return val{t: tNum, n: nLevel{}}, true, nil
		}
		if err := f.checkCallArgs(e, args, tObj); err != nil {
			return val{}, true, err
		}
		return f.actor(args[0].role, val{t: tNum, n: nLevel{}}), true, nil
	case "GetNpcStatDistribution":
		if err := f.checkCallArgs(e, args, tObj, tEnum); err != nil {
			return val{}, true, err
		}
		i := aocexpr.StatDistributionIndex(args[1].enum.Value)
		if args[1].enum.Type != "EStatDistributionStat" || i < 0 {
			return val{}, true, errAt(e.Args[1].Pos(), "GetNpcStatDistribution: %v is not an EStatDistributionStat", args[1].enum)
		}
		if args[0].role != aocexpr.RoleCaster {
			return val{}, true, unsupportedAt(e.At, "%v of another actor", e.Name)
		}
		v, _ := ctxVal(OpNpcDist, tNum, uint16(i))
		v.float = true
		return v, true, nil
	case "GetNpcEliteRank":
		if err := f.checkCallArgs(e, args, tObj); err != nil {
			return val{}, true, err
		}
		v, _ := ctxVal(OpNpcRank, tNum)
		return f.actor(args[0].role, v), true, nil
	}
	return val{}, false, nil
}

func (f *fnc) evalAndTermCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetStat":
		if err := f.checkCallArgs(e, args, tObj, tRef); err != nil {
			return val{}, true, err
		}
		guid, err := f.statRef(args[1].ref)
		if err != nil {
			return val{}, true, errAt(e.Args[1].Pos(), "GetStat: %v", err)
		}
		idx, err := f.c.stat(guid)
		if err != nil {
			return val{}, true, errAt(e.At, "GetStat: %v", err)
		}
		return f.actor(args[0].role, val{t: tNum, n: nStat{idx}, float: true}), true, nil
	case "GetStatInitRunningTotal":
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		if f.kind != unitInit {
			return val{}, true, errAt(e.At, "GetStatInitRunningTotal outside a stat initializer")
		}
		return val{t: tNum, n: nTotal{}, float: true}, true, nil
	case "EvalE", "EvalFormula", "EvalEquation", "EvalF":
		if err := f.checkCallArgs(e, args, tRef); err != nil {
			return val{}, true, err
		}
		r := args[0].ref
		isFormula := e.Name == "EvalFormula" || e.Name == "EvalF"
		want := uint64(TypeStatEquation)
		if isFormula {
			want = TypeStatFormula
		}
		if r.Kind != "" || r.TypeID != want {
			return val{}, true, unsupportedAt(e.Args[0].Pos(), "%s of %v (only $#<TypeId>:<Guid>$ of the %s table)", e.Name, r, map[bool]string{true: "formula", false: "equation"}[isFormula])
		}
		v, err := f.inline(e, r.Guid, isFormula)
		return v, true, err
	case "GetTerm":
		if err := f.checkCallArgs(e, args, tRef); err != nil {
			return val{}, true, err
		}
		v, err := f.term(e, args[0].ref)
		return v, true, err
	}
	return val{}, false, nil
}

func (f *fnc) effectAndEquipCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetEffectCount":
		if err := f.checkCallArgs(e, args, tObj, tRef); err != nil {
			return val{}, true, err
		}
		i, err := f.c.ref(args[1].ref)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, _ := ctxVal(OpEffect, tNum, i)
		return f.actor(args[0].role, v), true, nil
	case "GetEffectCountByTag":
		if err := f.checkCallArgs(e, args, tObj, tStr); err != nil {
			return val{}, true, err
		}
		i, err := f.c.strOperand(args[1].str)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, _ := ctxVal(OpEffectTag, tNum, i)
		return f.actor(args[0].role, v), true, nil
	case "GetSkillPurchased":
		if err := f.checkCallArgs(e, args, tObj, tRef, tRef); err != nil {
			return val{}, true, err
		}
		a, err := f.c.ref(args[1].ref)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		b, err := f.c.ref(args[2].ref)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, _ := ctxVal(OpSkill, tBool, a, b)
		return f.actor(args[0].role, v), true, nil
	case "IsInCombat", "IsMoving", "IsUnkillable":
		if err := f.checkCallArgs(e, args, tObj); err != nil {
			return val{}, true, err
		}
		v, _ := ctxVal(map[string]byte{"IsInCombat": OpCombat, "IsMoving": OpMoving, "IsUnkillable": OpUnkillable}[e.Name], tBool)
		return f.actor(args[0].role, v), true, nil
	case "EquippedItemInSlotHasTag", "HasValidEquipment", "GetActorGuid", "GetPlayerProfessionLevel", "GetConsumedItemStat":
		return f.equipAndProfCall(e, args)
	}
	return val{}, false, nil
}

func (f *fnc) equipAndProfCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "EquippedItemInSlotHasTag":
		if err := f.checkCallArgs(e, args, tEnum, tStr); err != nil {
			return val{}, true, err
		}
		if args[0].enum.Type != "EEquipSlot" {
			return val{}, true, errAt(e.Args[0].Pos(), "EquippedItemInSlotHasTag: %v is not an EEquipSlot", args[0].enum)
		}
		a, err := f.c.strOperand(args[0].enum.Value)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		b, err := f.c.strOperand(args[1].str)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, err := ctxVal(OpEquipTag, tBool, a, b)
		return v, true, err
	case "HasValidEquipment":
		if err := f.checkCallArgs(e, args, tObj, tEnum); err != nil {
			return val{}, true, err
		}
		if args[1].enum.Type != "EEquipSlot" {
			return val{}, true, errAt(e.Args[1].Pos(), "HasValidEquipment: %v is not an EEquipSlot", args[1].enum)
		}
		a, err := f.c.strOperand(args[1].enum.Value)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		b, err := f.c.strOperand(aocexpr.HasValidEquipmentTag)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, _ := ctxVal(OpEquipTag, tBool, a, b)
		return f.actor(args[0].role, v), true, nil
	}
	return f.profAndConsumedCall(e, args)
}

func (f *fnc) profAndConsumedCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetActorGuid":
		if err := f.checkCallArgs(e, args, tObj); err != nil {
			return val{}, true, err
		}
		return val{t: tActor}, true, nil
	case "GetPlayerProfessionLevel":
		if len(args) == 2 && args[0].t == tPlayer {
			args[0] = val{t: tActor}
		}
		if err := f.checkCallArgs(e, args, tActor, tStr); err != nil {
			return val{}, true, err
		}
		i, err := f.c.strOperand(args[1].str)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, err := ctxVal(OpProfession, tNum, i)
		return v, true, err
	case "GetConsumedItemStat":
		if err := f.checkCallArgs(e, args, tRef); err != nil {
			return val{}, true, err
		}
		i, err := f.c.ref(args[0].ref)
		if err != nil {
			return val{}, true, errAt(e.At, "%v", err)
		}
		v, _ := ctxVal(OpConsumed, tNum, i)
		v.float = true
		return v, true, nil
	}
	return val{}, false, nil
}

func (f *fnc) statRef(r aocexpr.Ref) (int64, error) {
	if r.Kind == "" {
		if r.TypeID != TypeStatDef {
			return 0, fmt.Errorf("%v is not a StatTypeDef reference", r)
		}
		return r.Guid, nil
	}
	if r.Kind != "Stat" {
		return 0, fmt.Errorf("%v is not a stat reference", r)
	}
	guid, ok := f.c.g.StatByName(r.Name)
	if !ok {
		return 0, fmt.Errorf("no stat (or more than one) is called %q", r.Name)
	}
	return guid, nil
}

// CoA.C:Y:B

func (f *fnc) inline(e aocexpr.Call, guid int64, formula bool) (val, error) {
	key := "E" + strconv.FormatInt(guid, 10)
	var terms map[int64]string
	eq := guid
	if formula {
		key = "F" + strconv.FormatInt(guid, 10)
		fm, ok := f.c.g.Formulas[guid]
		if !ok {
			return val{}, errAt(e.At, "%s: no formula %d", e.Name, guid)
		}
		terms, eq = fm.Terms, fm.Equation
	}
	text, ok := f.c.g.Equations[eq]
	if !ok {
		return val{}, errAt(e.At, "%s: no equation %d", e.Name, eq)
	}
	if err := f.enter(key); err != nil {
		return val{}, errAt(e.At, "%s: %v", e.Name, err)
	}
	defer f.leave()
	p, err := f.c.parse(text)
	if err != nil {
		return val{}, errAt(e.At, "%s %d: %v", e.Name, guid, err)
	}
	f.terms = append(f.terms, terms)
	f.pushScope(true)
	v, err := f.tree(p.Tree())
	f.popScope()
	f.terms = f.terms[:len(f.terms)-1]
	if err != nil {
		return val{}, errAt(e.At, "%s %d: %v", e.Name, guid, err)
	}
	if v.t == tBool && f.kind == unitExpr {
		return v, nil
	}
	if v.t != tNum {
		return val{}, errAt(e.At, "%s %d: its value is %v, not a number", e.Name, guid, v.t)
	}
	// The value is the equation's own: the game's evaluator returns an equation's (or a formula's) value as it is, and never applies the limits the equation's
	// record lists (Graph.Clamps). The burn's formula shares the damage equation, whose limit is "not above 0": its pool is a positive number.
	v.float = true // a stat equation is a float (aocexpr.StatFloatFuncs)
	return v, nil
}

// clamp applies a limit: raised to its Min, then lowered to its Max (Unreal's Clamp order). A term's limits bind its value (an equation's do not: see inline).
func (f *fnc) clamp(v val, cl Clamp) val {
	limit := func(op byte, v val, bound float64) val {
		b := f.num(bound)
		if c, ok := v.constant(); ok {
			return val{t: tNum, n: nConst{minmax(op, c, b)}, float: true}
		}
		return val{t: tNum, n: nFn2{op, v.n, nConst{b}}, float: true}
	}
	if cl.HasMin {
		v = limit(OpMax, v, cl.Min)
	}
	if cl.HasMax {
		v = limit(OpMin, v, cl.Max)
	}
	return v
}

func (f *fnc) term(e aocexpr.Call, r aocexpr.Ref) (val, error) {
	if r.Kind != "" {
		return val{}, errAt(e.At, "GetTerm: the formula has no term %v", r)
	}
	if len(f.terms) == 0 || f.terms[len(f.terms)-1] == nil && !f.anyTerms() {
		return val{}, errAt(e.At, "GetTerm outside a formula")
	}
	idx := -1
	for i := len(f.terms) - 1; i >= 0; i-- {
		if _, ok := f.terms[i][r.Guid]; ok {
			idx = i
			break
		}
	}
	td, known := f.c.g.Terms[r.Guid]
	var text string
	switch {
	case idx >= 0:
		text = f.terms[idx][r.Guid]
		if strings.TrimSpace(text) == "" {
			idx = -1
			if known {
				text = td.Default
			}
		}
	case known:
		text = td.Default
	default:
		return val{}, errAt(e.At, "GetTerm: the formula has no term %v", r)
	}
	var v val
	if idx < 0 && strings.TrimSpace(text) == "" {
		v = val{t: tNum, n: nConst{0}, float: true}
	} else {
		var err error
		v, err = f.evalTermExpr(e, r, idx, text)
		if err != nil {
			return val{}, err
		}
	}
	if known {
		v = f.clamp(v, td.Clamp)
	}
	return v, nil
}

func (f *fnc) evalTermExpr(e aocexpr.Call, r aocexpr.Ref, idx int, text string) (val, error) {
	scopeDepth := idx
	if idx < 0 {
		scopeDepth = 0
	}
	if idx == 0 && f.topTerms {
		scopeDepth = 1
	}
	key := "T" + strconv.FormatInt(r.Guid, 10) + "@" + strconv.Itoa(scopeDepth+1)
	if err := f.enter(key); err != nil {
		return val{}, errAt(e.At, "GetTerm: %v", err)
	}
	defer f.leave()
	p, err := f.c.parse(text)
	if err != nil {
		return val{}, errAt(e.At, "GetTerm %v: %v", r, err)
	}
	saved := f.terms
	f.terms = append([]map[int64]string(nil), saved[:scopeDepth]...)
	f.pushScope(true)
	v, err := f.tree(p.Tree())
	f.popScope()
	f.terms = saved
	if err != nil {
		return val{}, errAt(e.At, "GetTerm %v: %v", r, err)
	}
	if v.t != tNum {
		return val{}, errAt(e.At, "GetTerm %v: its value is %v, not a number", r, v.t)
	}
	v.float = true
	return v, nil
}

// C:Y:B and C.O.A

func (f *fnc) anyTerms() bool {
	for _, t := range f.terms {
		if t != nil {
			return true
		}
	}
	return false
}

func (f *fnc) enter(key string) error {
	for _, k := range f.inlining {
		if k == key {
			return fmt.Errorf("it calls itself")
		}
	}
	if len(f.inlining) >= maxInline {
		return fmt.Errorf("calls nest more than %d deep", maxInline)
	}
	f.inlining = append(f.inlining, key)
	return nil
}

func (f *fnc) leave() { f.inlining = f.inlining[:len(f.inlining)-1] }
