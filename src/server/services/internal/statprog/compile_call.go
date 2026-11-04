// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"math"
	"strconv"

	"aocrop/services/internal/aocexpr"
)

// C O A and C-Y-B

func (f *fnc) needCallArg(e aocexpr.Call, args []val, i int, t vtype) error {
	if t == tNum {
		args[i] = f.loose(args[i])
	}
	if args[i].t != t {
		return errAt(e.Args[i].Pos(), "%s: argument %d is %v, not %v", e.Name, i+1, args[i].t, t)
	}
	return nil
}

func (f *fnc) checkCallArgs(e aocexpr.Call, args []val, types ...vtype) error {
	if err := argc(e, len(types)); err != nil {
		return err
	}
	for i, t := range types {
		if err := f.needCallArg(e, args, i, t); err != nil {
			return err
		}
	}
	return nil
}

func ctxVal(op byte, t vtype, imm ...uint16) (val, error) {
	n := nCtx{op: op}
	copy(n.imm[:], imm)
	return val{t: t, n: n}, nil
}

func (f *fnc) call(e aocexpr.Call) (val, error) {
	args := make([]val, len(e.Args))
	for i, a := range e.Args {
		v, err := f.expr(a)
		if err != nil {
			return val{}, err
		}
		args[i] = v
	}
	for _, fn := range []func(aocexpr.Call, []val) (val, bool, error){
		f.nodeCall,
		f.gatherCall,
		f.reactionCall,
		f.mathCall,
		f.actorTargetCall,
		f.comboHostCall,
		f.hitRecordCall,
		f.actorReadCall,
		f.hostTableCall,
		f.combatCall,
		f.stateReadCall,
	} {
		if v, ok, err := fn(e, args); ok {
			return v, err
		}
	}
	return val{}, unsupportedAt(e.At, "function %q", e.Name)
}

func (f *fnc) mathCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "Round", "Floor", "Ceil":
		if err := f.checkCallArgs(e, args, tNum); err != nil {
			return val{}, true, err
		}
		op := map[string]byte{"Round": OpRound, "Floor": OpFloor, "Ceil": OpCeil}[e.Name]
		if c, ok := args[0].constant(); ok {
			return val{t: tNum, n: nConst{roundOp(op, c)}}, true, nil
		}
		return val{t: tNum, n: nFn1{op, args[0].n}}, true, nil
	case "Float":
		if err := f.checkCallArgs(e, args, tNum); err != nil {
			return val{}, true, err
		}
		args[0].float = true
		return args[0], true, nil
	case "Min", "Max", "min", "max":
		if err := f.checkCallArgs(e, args, tNum, tNum); err != nil {
			return val{}, true, err
		}
		op := OpMin
		if e.Name == "Max" || e.Name == "max" {
			op = OpMax
		}
		return f.minMaxVal(op, args[0], args[1]), true, nil
	case "SelectFloat", "SelectInt", "selectfloat", "selectint":
		return f.selectCall(e, args)
	case "EvalCurve":
		return f.evalCurveCall(e, args)
	case "Lerp":
		return f.lerpCall(e, args)
	case "Abs":
		if err := f.checkCallArgs(e, args, tNum); err != nil {
			return val{}, true, err
		}
		if c, ok := args[0].constant(); ok {
			return val{t: tNum, n: nConst{math.Abs(c)}, float: args[0].float}, true, nil
		}
		return val{t: tNum, n: nFn1{OpAbs, args[0].n}, float: args[0].float}, true, nil
	case "Clamp":
		if err := f.checkCallArgs(e, args, tNum, tNum, tNum); err != nil {
			return val{}, true, err
		}
		lo := f.minMaxVal(OpMax, args[0], args[1])
		return f.minMaxVal(OpMin, lo, args[2]), true, nil
	case "Int":
		if err := f.checkCallArgs(e, args, tNum); err != nil {
			return val{}, true, err
		}
		if c, ok := args[0].constant(); ok {
			return val{t: tNum, n: nConst{roundOp(OpFloor, c)}}, true, nil
		}
		return val{t: tNum, n: nFn1{OpFloor, args[0].n}}, true, nil
	}
	return val{}, false, nil
}

func (f *fnc) selectCall(e aocexpr.Call, args []val) (val, bool, error) {
	if err := f.checkCallArgs(e, args, tBool, tNum, tNum); err != nil {
		return val{}, true, err
	}
	fl := e.Name == "SelectFloat" || e.Name == "selectfloat"
	cc, cok := args[0].constant()
	a, aok := args[1].constant()
	b, bok := args[2].constant()
	if cok && aok && bok {
		x := b
		if cc != 0 {
			x = a
		}
		return val{t: tNum, n: nConst{x}, float: fl}, true, nil
	}
	return val{t: tNum, n: nSel{args[0].n, args[1].n, args[2].n}, float: fl}, true, nil
}

func (f *fnc) evalCurveCall(e aocexpr.Call, args []val) (val, bool, error) {
	if err := f.checkCallArgs(e, args, tRef, tNum); err != nil {
		return val{}, true, err
	}
	r := args[0].ref
	if r.Kind != "" || r.TypeID != TypeStatCurve {
		return val{}, true, errAt(e.Args[0].Pos(), "EvalCurve: %v is not a StatCurve reference", r)
	}
	idx, err := f.c.curve(r.Guid)
	if err != nil {
		return val{}, true, errAt(e.At, "EvalCurve %v: %v", r, err)
	}
	return val{t: tNum, n: nCurve{idx, args[1].n}, float: true}, true, nil
}

func (f *fnc) lerpCall(e aocexpr.Call, args []val) (val, bool, error) {
	if err := f.checkCallArgs(e, args, tNum, tNum, tNum); err != nil {
		return val{}, true, err
	}
	a, aok := args[0].constant()
	b, bok := args[1].constant()
	t, tok := args[2].constant()
	if aok && bok && tok {
		d := arith(OpSub, b, a, f.c.single)
		p := arith(OpMul, t, d, f.c.single)
		if r := arith(OpAdd, a, p, f.c.single); !math.IsInf(r, 0) && !math.IsNaN(r) {
			return val{t: tNum, n: nConst{r}, float: true}, true, nil
		}
	}
	return val{t: tNum, n: nFn3{OpLerp, args[0].n, args[1].n, args[2].n}, float: true}, true, nil
}

func (f *fnc) actorTargetCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetOwner", "GetInstigator", "GetSource", "GetSelf":
		if err := argc(e, 0); err != nil {
			return val{}, true, err
		}
		return val{t: tObj}, true, nil
	case "GetTarget":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no target)", e.Name)
		}
		if err := argc(e, 0); err != nil {
			return val{}, true, err
		}
		f.usesTarget = true
		return val{t: tObj, role: aocexpr.RoleTarget}, true, nil
	case "GetActiveTarget":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no target)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tObj, tEnum); err != nil {
			return val{}, true, err
		}
		if args[1].enum.Type != "ETargetingIntention" || args[0].role != aocexpr.RoleCaster {
			return val{}, true, unsupportedAt(e.At, "%v of that actor", e.Name)
		}
		switch args[1].enum.Value {
		case "Harm":
			f.usesTarget = true
			return val{t: tObj, role: aocexpr.RoleActiveHarm}, true, nil
		case "Support":
			f.usesTarget = true
			return val{t: tObj, role: aocexpr.RoleActiveSupport}, true, nil
		}
		return val{}, true, unsupportedAt(e.At, "targeting intention %v", args[1].enum)
	case "GetCreatureOwner":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no pet)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tObj); err != nil {
			return val{}, true, err
		}
		switch args[0].role {
		case aocexpr.RoleCaster:
			return val{t: tObj, role: aocexpr.RoleCasterOwner}, true, nil
		case aocexpr.RoleTarget:
			f.usesTarget = true
			return val{t: tObj, role: aocexpr.RoleTargetOwner}, true, nil
		}
		return val{}, true, unsupportedAt(e.At, "the owner of that actor")
	}
	return val{}, false, nil
}

func (f *fnc) comboHostCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "IsClient":
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostIsClient, "", tBool)
		return v, true, err
	case "GetAbilityComboCount":
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostComboCount, "", tNum)
		v.float = false
		return v, true, err
	case "IsComboButtonPressed":
		if err := f.checkCallArgs(e, args, tEnum); err != nil {
			return val{}, true, err
		}
		if args[0].enum.Type != "EComboButtons" {
			return val{}, true, errAt(e.Args[0].Pos(), "IsComboButtonPressed: %v is not an EComboButtons", args[0].enum)
		}
		v, err := f.hostCall(e, aocexpr.HostComboButton, args[0].enum.Value, tBool)
		return v, true, err
	}
	return val{}, false, nil
}

func (f *fnc) hitRecordCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetCurrentWeaponSkilltree":
		if err := f.checkCallArgs(e, args, tObj); err != nil {
			return val{}, true, err
		}
		return val{t: tRef, ref: aocexpr.WeaponSkilltreeRef}, true, nil
	case "GetHitRecord":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tObj); err != nil {
			return val{}, true, err
		}
		return val{t: tObj, etype: etypeHitRecord}, true, nil
	case "GetHitSourceRecord":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tObj); err != nil {
			return val{}, true, err
		}
		return val{t: tRef, ref: aocexpr.HitSourceRef}, true, nil
	case "GetRecordTypeId":
		if err := f.checkCallArgs(e, args, tRef); err != nil {
			return val{}, true, err
		}
		if args[0].ref == aocexpr.HitSourceRef {
			v, err := f.hostCall(e, aocexpr.HostHitSourceType, "", tNum)
			return v, true, err
		}
		return val{t: tNum, n: nConst{float64(args[0].ref.TypeID)}, float: true}, true, nil
	case "GetProjectileTypeId":
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		return val{t: tNum, n: nConst{float64(aocexpr.TypeProjectile)}, float: true}, true, nil
	case "DoesHitContainActor":
		if err := f.checkCallArgs(e, args, tObj, tObj); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostHitContains, strconv.Itoa(args[1].role), tBool)
		return v, true, err
	case "GetClosestTargetToActorInHit":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no hit)", e.Name)
		}
		if err := f.checkCallArgs(e, args, tObj, tObj); err != nil {
			return val{}, true, err
		}
		if args[1].role != aocexpr.RoleCaster {
			return val{}, true, unsupportedAt(e.At, "%v of another actor", e.Name)
		}
		return val{t: tObj, role: aocexpr.RoleClosestInHit}, true, nil
	}
	return val{}, false, nil
}

func (f *fnc) actorReadCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "RandIntRange":
		if err := f.checkCallArgs(e, args, tNum, tNum); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostRandIntRange, "", tNum, args[0], args[1])
		return v, true, err
	case "GetDistanceBetween":
		if err := f.checkCallArgs(e, args, tObj, tObj); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostDistanceBetween, aocexpr.RolePair(args[0].role, args[1].role), tNum)
		return v, true, err
	case "GetHealthPercentage":
		return f.healthPctCall(e, args)
	case "HasAlignment", "GetEffectRemainingDuration":
		if err := f.checkCallArgs(e, args, tObj, tRef); err != nil {
			return val{}, true, err
		}
		if e.Name == "HasAlignment" {
			v, err := f.hostCall(e, aocexpr.RoleFn(args[0].role, aocexpr.RoleOpHasAlignment), strconv.FormatInt(args[1].ref.Guid, 10), tBool)
			return v, true, err
		}
		v, err := f.hostCall(e, aocexpr.RoleFn(args[0].role, aocexpr.RoleOpEffectRemaining), strconv.FormatInt(args[1].ref.Guid, 10), tNum)
		return v, true, err
	case "GetRemainingAbilityCooldown":
		if err := f.checkCallArgs(e, args, tRef); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostCooldownLeft, strconv.FormatInt(args[0].ref.Guid, 10), tNum)
		return v, true, err
	case "GetHitTriggerCount", "GetAbilityHitCount":
		if err := f.checkCallArgs(e, args, tObj, tRef); err != nil {
			return val{}, true, err
		}
		fn := aocexpr.HostHitTriggerCount
		if e.Name == "GetAbilityHitCount" {
			fn = aocexpr.HostAbilityHitCount
		}
		v, err := f.hostCall(e, fn, strconv.FormatInt(args[1].ref.Guid, 10), tNum, val{t: tNum, n: nConst{float64(args[0].role)}})
		return v, true, err
	case "CalculateDamage", "CalculateDamageMultiplier", "CalculateDamageAdditiveMultiplier", "CalculateDamageMitigation", "CalculateDamageCritical",
		"CalculateDamagePenetration", "CalculateDamageDampening":
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		fn := 0
		for i, name := range aocexpr.DamageFnNames {
			if name == e.Name {
				fn = aocexpr.HostCalcDamage + i
			}
		}
		f.usesTarget = true
		v, err := f.hostCall(e, fn, "", tNum)
		return v, true, err
	}
	return val{}, false, nil
}

func (f *fnc) healthPctCall(e aocexpr.Call, args []val) (val, bool, error) {
	if err := f.checkCallArgs(e, args, tObj); err != nil {
		return val{}, true, err
	}
	hg, ok := f.c.g.StatByName("Health")
	mg, ok2 := f.c.g.StatByName("MaxHealth")
	if !ok || !ok2 {
		return val{}, true, unsupportedAt(e.At, "%v (the graph has no Health and MaxHealth)", e.Name)
	}
	hi, err := f.c.stat(hg)
	if err != nil {
		return val{}, true, errAt(e.At, "%v", err)
	}
	mi, err := f.c.stat(mg)
	if err != nil {
		return val{}, true, errAt(e.At, "%v", err)
	}
	health := f.actor(args[0].role, val{t: tNum, n: nStat{hi}, float: true})
	max := f.actor(args[0].role, val{t: tNum, n: nStat{mi}, float: true})
	return val{t: tNum, n: nBin{OpDiv, health.n, max.n}, float: true}, true, nil
}

func (f *fnc) hostTableCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetRemainingAbilityCharges":
		if err := f.checkCallArgs(e, args, tRef); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostRemainingCharges, strconv.FormatInt(args[0].ref.Guid, 10), tNum)
		return v, true, err
	case "IsApplicableTarget":
		if err := f.checkCallArgs(e, args, tObj, tEnum); err != nil {
			return val{}, true, err
		}
		if args[1].enum.Type != "EAffectedTarget" {
			return val{}, true, errAt(e.Args[1].Pos(), "IsApplicableTarget: %v is not an EAffectedTarget", args[1].enum)
		}
		v, err := f.hostCall(e, aocexpr.HostApplicableTarget, args[1].enum.Value, tBool)
		return v, true, err
	case "IsAnyMovementKeyPressed", "isanymovementkeypressed":
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostMovementKey, "", tBool)
		return v, true, err
	case "GetStatDeltaOverTime":
		if err := f.checkCallArgs(e, args, tObj, tNum, tRef); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostStatDelta, strconv.FormatInt(args[2].ref.Guid, 10), tNum, args[1])
		return v, true, err
	case "GetHitEvent":
		if f.kind != unitExpr {
			return val{}, true, unsupportedAt(e.At, "function %q (a stat formula has no host)", e.Name)
		}
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		return val{t: tObj}, true, nil
	case "GetStatModValue", "GetStatModEffectiveValue", "IsNeutral", "IsEnemy", "GetAngleBetween", "Now", "GetProgressDuration":
		return f.hostTableMiscCall(e, args)
	}
	return val{}, false, nil
}

func (f *fnc) hostTableMiscCall(e aocexpr.Call, args []val) (val, bool, error) {
	switch e.Name {
	case "GetStatModValue", "GetStatModEffectiveValue":
		fn := aocexpr.HostHitStatMod
		if e.Name == "GetStatModEffectiveValue" {
			fn = aocexpr.HostHitStatEffective
		}
		if len(e.Args) == 3 {
			if err := f.checkCallArgs(e, args, tObj, tObj, tRef); err != nil {
				return val{}, true, err
			}
			v, err := f.hostCall(e, fn, strconv.FormatInt(args[2].ref.Guid, 10), tNum)
			return v, true, err
		}
		if err := f.checkCallArgs(e, args, tObj, tRef); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, fn, strconv.FormatInt(args[1].ref.Guid, 10), tNum)
		return v, true, err
	case "IsNeutral", "IsEnemy":
		if err := f.checkCallArgs(e, args, tObj, tObj); err != nil {
			return val{}, true, err
		}
		fn := aocexpr.HostIsNeutral
		if e.Name == "IsEnemy" {
			fn = aocexpr.HostIsEnemy
		}
		v, err := f.hostCall(e, fn, aocexpr.RolePair(args[0].role, args[1].role), tBool)
		return v, true, err
	case "GetAngleBetween":
		if err := f.checkCallArgs(e, args, tObj, tObj); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostAngleBetween, aocexpr.RolePair(args[0].role, args[1].role), tNum)
		return v, true, err
	case "Now":
		if err := f.checkCallArgs(e, args); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostNow, "", tNum)
		return v, true, err
	default:
		if err := f.checkCallArgs(e, args, tStr); err != nil {
			return val{}, true, err
		}
		v, err := f.hostCall(e, aocexpr.HostProgressDuration, args[0].str, tNum)
		return v, true, err
	}
}

// ~ CoA & C | Y | B ~

func (f *fnc) actor(role int, v val) val {
	if role != aocexpr.RoleCaster {
		v.n = nActor{role, v.n}
	}
	return v
}

func (f *fnc) hostCall(e aocexpr.Call, fn int, name string, t vtype, args ...val) (val, error) {
	if f.kind != unitExpr {
		return val{}, unsupportedAt(e.At, "function %q (a stat formula has no host)", e.Name)
	}
	i, err := f.c.strOperand(name)
	if err != nil {
		return val{}, errAt(e.At, "%v", err)
	}
	nodes := make([]node, len(args))
	for k, a := range args {
		nodes[k] = a.n
	}
	op := []byte{OpHost0, OpHost1, OpHost2}[len(args)]
	return val{t: t, n: nHost{op: op, fn: uint16(fn), str: i, args: nodes}, float: true}, nil
}

func roundOp(op byte, x float64) float64 {
	switch op {
	case OpRound:
		return math.Floor(x + 0.5)
	case OpFloor:
		return math.Floor(x)
	}
	return math.Ceil(x)
}

func minmax(op byte, a, b float64) float64 {
	if op == OpMin {
		return math.Min(a, b)
	}
	return math.Max(a, b)
}
