// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"fmt"
	"math"
	"strconv"
)

// AbilityContext is what an ability's expressions run against.
type AbilityContext interface {
	StatContext
	HostCaller
	AbilityProgress(group string) float64
	EffectCountByInstigator(effect Ref) float64
	RandDouble() float64
	HitHasTag(tag string) bool
	HitHasFlag(flag string) bool
	HitTargets() float64
	XPDebtRatio() float64
}

// AbilityFloatFuncs are the host functions of an ability expression that return a float.
var AbilityFloatFuncs = map[string]bool{
	"GetAbilityProgressRatio":             true,
	"RandDouble":                          true,
	"GetXPDebtRatio":                      true,
	"GetRemainingAbilityCharges":          true,
	"GetStatDeltaOverTime":                true,
	"GetStatModValue":                     true,
	"GetStatModEffectiveValue":            true,
	"GetAngleBetween":                     true,
	"Now":                                 true,
	"GetProgressDuration":                 true,
	"GetDistanceBetween":                  true,
	"GetHealthPercentage":                 true,
	"GetEffectRemainingDuration":          true,
	"GetRemainingAbilityCooldown":         true,
	"GetHitTriggerCount":                  true,
	"GetAbilityHitCount":                  true,
	"GetAbilityComboCount":                true,
	"RandIntRange":                        true,
	"GetArtisanshipModifiedStat":          true,
	"GetGatheringInteractionBaseDuration": true,
	"CalculateDamage":                     true,
	"CalculateDamageMultiplier":           true,
	"CalculateDamageAdditiveMultiplier":   true,
	"CalculateDamageMitigation":           true,
	"CalculateDamageCritical":             true,
	"CalculateDamagePenetration":          true,
	"CalculateDamageDampening":            true,
}

// AbilityFuncNames lists the host functions AbilityEnv supplies besides StatFuncNames.
func AbilityFuncNames() []string {
	return []string{"GetTarget", "GetActiveTarget", "GetCreatureOwner", "GetAbilityProgressRatio", "GetEffectCountByRecordAndInstigator", "RandDouble",
		"GetHitInProgress", "CheckForDamageTag", "HitHasTag", "GetHitFlags", "HitFlagsContains", "GetNumTargets", "GetXPDebtRatio",
		"GetRemainingAbilityCharges", "IsApplicableTarget", "IsAnyMovementKeyPressed", "GetStatDeltaOverTime", "GetHitEvent", "GetStatModValue",
		"GetStatModEffectiveValue", "IsNeutral", "IsEnemy", "GetAngleBetween", "Now", "GetProgressDuration",
		"GetDistanceBetween", "GetHealthPercentage", "HasAlignment", "GetEffectRemainingDuration", "GetRemainingAbilityCooldown", "IsClient", "RandIntRange",
		"GetHitTriggerCount", "GetAbilityHitCount", "GetAbilityComboCount", "IsComboButtonPressed", "GetCurrentWeaponSkilltree", "DoesHitContainActor", "GetClosestTargetToActorInHit", "GetHitRecord", "GetHitSourceRecord", "GetRecordTypeId", "GetProjectileTypeId", "CalculateDamage", "CalculateDamageMultiplier", "CalculateDamageAdditiveMultiplier",
		"CalculateDamageMitigation", "CalculateDamageCritical", "CalculateDamagePenetration", "CalculateDamageDampening", "Int", "GetPlayerLootMaxLevel", "GetPlayerLootAvgLuck", "DoesNPCContainTag", "GetPlayerLevel", "GetNarrativeContentLevel",
		"GetPlayer", "GetPlayerGuid", "GetNodeBiome", "GetNodeRecord", "GetCurrentNode", "Node_GetNodeType", "Node_GetLevel", "Node_InLevelRangeInclusive",
		"Node_IsPlayerCitizen", "Node_IsPlayerCitizenOfNode", "Node_IsPlayerMayor", "EnumInt",
		"GetArtisanshipModifiedStat", "GetRolledItemQuality", "GetItemRarity", "GetSourceCertificationTier", "GetGatheringInteractionBaseDuration"}
}

func isObjectVal(v Value) bool {
	_, ok := v.(Object)
	return ok
}

func checkArgCount(args []Value, n int) error {
	if len(args) != n {
		return fmt.Errorf("takes %d argument(s), got %d", n, len(args))
	}
	return nil
}

// AbilityEnv returns the environment of an ability expression over ctx.
func AbilityEnv(ctx AbilityContext, curves CurveSource) *Env {
	env := StatEnv(ctx, curves)
	env.LooseBool = true
	env.FloatFuncs = map[string]bool{}
	for k := range StatFloatFuncs {
		env.FloatFuncs[k] = true
	}
	for k := range AbilityFloatFuncs {
		env.FloatFuncs[k] = true
	}
	env.SameActor = func(a, b int) bool { return ctx.HostFn(HostSameActor, RolePair(a, b), nil) != 0 }
	addAbilityTargetFuncs(env, ctx)
	addAbilityHitFuncs(env, ctx)
	addAbilityHitFlagFuncs(env, ctx)
	addAbilityCombatFuncs(env, ctx)
	addAbilityHostStateFuncs(env, ctx)
	addAbilityRecordFuncs(env, ctx)
	addAbilityHitRecordFuncs(env, ctx)
	addAbilityActorFuncs(env, ctx)
	addAbilityActorStateFuncs(env, ctx)
	addAbilityTimeAndDamageFuncs(env, ctx)
	addNodeFuncs(env, ctx)
	addGatherFuncs(env, ctx)
	return env
}

func addAbilityTargetFuncs(env *Env, ctx AbilityContext) {
	env.Funcs["GetTarget"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return Object{Kind: "target", Role: RoleTarget}, nil
	}
	env.Funcs["GetActiveTarget"] = func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		o, ok := args[0].(Object)
		e, ok2 := args[1].(Enum)
		if !ok || !ok2 || e.Type != "ETargetingIntention" || o.Role != RoleCaster {
			return nil, fmt.Errorf("takes the caster and an ETargetingIntention")
		}
		switch e.Value {
		case "Harm":
			return Object{Kind: "activetarget", Role: RoleActiveHarm}, nil
		case "Support":
			return Object{Kind: "activetarget", Role: RoleActiveSupport}, nil
		}
		return nil, fmt.Errorf("the targeting intention %v is not modelled", e)
	}
	env.Funcs["GetCreatureOwner"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		o, ok := args[0].(Object)
		if !ok {
			return nil, fmt.Errorf("takes an actor")
		}
		switch o.Role {
		case RoleCaster:
			return Object{Kind: "owner", Role: RoleCasterOwner}, nil
		case RoleTarget:
			return Object{Kind: "owner", Role: RoleTargetOwner}, nil
		}
		return nil, fmt.Errorf("the owner of that actor is not modelled")
	}
	env.Funcs["GetAbilityProgressRatio"] = func(args []Value) (Value, error) {
		if len(args) == 0 {
			return ctx.AbilityProgress(""), nil
		}
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 0 or 1 argument(s), got %d", len(args))
		}
		g, ok := args[0].(string)
		if !ok {
			return nil, fmt.Errorf("argument 1 is %s", describeValue(args[0]))
		}
		return ctx.AbilityProgress(g), nil
	}
}

func addAbilityHitFuncs(env *Env, ctx AbilityContext) {
	env.Funcs["GetEffectCountByRecordAndInstigator"] = func(args []Value) (Value, error) {
		if len(args) != 3 {
			return nil, fmt.Errorf("takes 3 argument(s), got %d", len(args))
		}
		t, o1 := args[0].(Object)
		r, ok := args[1].(Ref)
		i, o2 := args[2].(Object)
		if !o1 || !ok || !o2 {
			return nil, fmt.Errorf("takes an object, a record reference and an object")
		}
		if t.Role == RoleCaster && i.Role == RoleCaster {
			return ctx.EffectCountByInstigator(r), nil
		}
		return ctx.HostFn(RoleFn(t.Role, RoleOpEffectInst), strconv.FormatInt(r.Guid, 10), []float64{float64(i.Role)}), nil
	}
	env.Funcs["GetHitInProgress"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return Object{Kind: "hit"}, nil
	}
	hitTag := func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		_, ok := args[0].(Object)
		tag, ok2 := args[1].(string)
		if !ok || !ok2 {
			return nil, fmt.Errorf("takes the hit and a tag")
		}
		return ctx.HitHasTag(tag), nil
	}
	env.Funcs["CheckForDamageTag"] = hitTag
	env.Funcs["HitHasTag"] = hitTag
}

func addAbilityHitFlagFuncs(env *Env, ctx AbilityContext) {
	env.Funcs["GetHitFlags"] = func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		if !isObjectVal(args[0]) || !isObjectVal(args[1]) {
			return nil, fmt.Errorf("takes the hit and the target")
		}
		return Object{Kind: "hitflags"}, nil
	}
	env.Funcs["HitFlagsContains"] = func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		o, ok := args[0].(Object)
		e, ok2 := args[1].(Enum)
		if !ok || o.Kind != "hitflags" || !ok2 || e.Type != "EHitFlag" {
			return nil, fmt.Errorf("takes the flags of a hit and an EHitFlag")
		}
		return ctx.HitHasFlag(e.Value), nil
	}
	env.Funcs["GetNumTargets"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		if !isObjectVal(args[0]) {
			return nil, fmt.Errorf("takes the hit")
		}
		return ctx.HitTargets(), nil
	}
}

func addAbilityCombatFuncs(env *Env, ctx AbilityContext) {
	env.Funcs["GetXPDebtRatio"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		if _, ok := args[0].(float64); !ok {
			return nil, fmt.Errorf("takes an actor id")
		}
		return ctx.XPDebtRatio(), nil
	}
	env.Funcs["RandDouble"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return ctx.RandDouble(), nil
	}
	env.Funcs["GetRemainingAbilityCharges"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		r, ok := args[0].(Ref)
		if !ok {
			return nil, fmt.Errorf("takes an ability reference")
		}
		return ctx.HostFn(HostRemainingCharges, fmt.Sprint(r.Guid), nil), nil
	}
	env.Funcs["GetRemainingAbilityCooldown"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		r, ok := args[0].(Ref)
		if !ok {
			return nil, fmt.Errorf("takes an ability reference")
		}
		return ctx.HostFn(HostCooldownLeft, fmt.Sprint(r.Guid), nil), nil
	}
}

func addAbilityHostStateFuncs(env *Env, ctx AbilityContext) {
	env.Funcs["IsApplicableTarget"] = func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		e, ok := args[1].(Enum)
		if !isObjectVal(args[0]) || !ok || e.Type != "EAffectedTarget" {
			return nil, fmt.Errorf("takes an actor and an EAffectedTarget")
		}
		return ctx.HostFn(HostApplicableTarget, e.Value, nil) != 0, nil
	}
	env.Funcs["IsAnyMovementKeyPressed"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return ctx.HostFn(HostMovementKey, "", nil) != 0, nil
	}
	env.Funcs["IsClient"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return ctx.HostFn(HostIsClient, "", nil) != 0, nil
	}
	env.Funcs["Int"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		x, ok := args[0].(float64)
		if !ok {
			return nil, fmt.Errorf("takes a number")
		}
		return math.Floor(x), nil
	}
	env.Funcs["RandIntRange"] = func(args []Value) (Value, error) {
		if len(args) != 2 {
			return nil, fmt.Errorf("takes 2 argument(s), got %d", len(args))
		}
		lo, ok := args[0].(float64)
		hi, ok2 := args[1].(float64)
		if !ok || !ok2 {
			return nil, fmt.Errorf("takes two numbers")
		}
		return ctx.HostFn(HostRandIntRange, "", []float64{lo, hi}), nil
	}
}

func addAbilityRecordFuncs(env *Env, ctx AbilityContext) {
	env.Funcs["GetStatDeltaOverTime"] = func(args []Value) (Value, error) {
		if len(args) != 3 {
			return nil, fmt.Errorf("takes 3 argument(s), got %d", len(args))
		}
		secs, ok := args[1].(float64)
		r, ok2 := args[2].(Ref)
		if !isObjectVal(args[0]) || !ok || !ok2 {
			return nil, fmt.Errorf("takes an actor, a number of seconds and a stat")
		}
		return ctx.HostFn(HostStatDelta, fmt.Sprint(r.Guid), []float64{secs}), nil
	}
	env.Funcs["GetHitEvent"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return Object{Kind: "hitevent"}, nil
	}
	statMod := func(fn int) func(args []Value) (Value, error) {
		return func(args []Value) (Value, error) {
			if len(args) != 2 && len(args) != 3 {
				return nil, fmt.Errorf("takes 2 or 3 arguments, got %d", len(args))
			}
			r, ok := args[len(args)-1].(Ref)
			if o, isO := args[0].(Object); !isO || o.Kind != "hitevent" || !ok || (len(args) == 3 && !isObjectVal(args[1])) {
				return nil, fmt.Errorf("takes the hit event, an optional actor and a stat")
			}
			return ctx.HostFn(fn, fmt.Sprint(r.Guid), nil), nil
		}
	}
	env.Funcs["GetStatModValue"] = statMod(HostHitStatMod)
	env.Funcs["GetStatModEffectiveValue"] = statMod(HostHitStatEffective)
	env.Funcs["GetAbilityComboCount"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return ctx.HostFn(HostComboCount, "", nil), nil
	}
	env.Funcs["IsComboButtonPressed"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		e, ok := args[0].(Enum)
		if !ok || e.Type != "EComboButtons" {
			return nil, fmt.Errorf("takes an EComboButtons value")
		}
		return ctx.HostFn(HostComboButton, e.Value, nil) != 0, nil
	}
}

func addAbilityHitRecordFuncs(env *Env, ctx AbilityContext) {
	env.Funcs["GetHitRecord"] = func(args []Value) (Value, error) {
		if len(args) != 1 || !isObjectVal(args[0]) {
			return nil, fmt.Errorf("takes the hit")
		}
		return Object{Kind: "hit"}, nil
	}
	env.Funcs["GetHitSourceRecord"] = func(args []Value) (Value, error) {
		if len(args) != 1 || !isObjectVal(args[0]) {
			return nil, fmt.Errorf("takes the hit")
		}
		return HitSourceRef, nil
	}
	env.Funcs["GetRecordTypeId"] = func(args []Value) (Value, error) {
		r, ok := args[0].(Ref)
		if len(args) != 1 || !ok {
			return nil, fmt.Errorf("takes a record reference")
		}
		if r == HitSourceRef {
			return ctx.HostFn(HostHitSourceType, "", nil), nil
		}
		return float64(r.TypeID), nil
	}
	env.Funcs["GetProjectileTypeId"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return float64(TypeProjectile), nil
	}
	env.Funcs["GetCurrentWeaponSkilltree"] = func(args []Value) (Value, error) {
		if len(args) != 1 || !isObjectVal(args[0]) {
			return nil, fmt.Errorf("takes an actor")
		}
		return WeaponSkilltreeRef, nil
	}
}

func addAbilityActorFuncs(env *Env, ctx AbilityContext) {
	env.Funcs["DoesHitContainActor"] = func(args []Value) (Value, error) {
		if len(args) != 2 || !isObjectVal(args[0]) || !isObjectVal(args[1]) {
			return nil, fmt.Errorf("takes the hit and an actor")
		}
		return ctx.HostFn(HostHitContains, strconv.Itoa(args[1].(Object).Role), nil) != 0, nil
	}
	env.Funcs["GetClosestTargetToActorInHit"] = func(args []Value) (Value, error) {
		if len(args) != 2 || !isObjectVal(args[0]) || !isObjectVal(args[1]) || args[1].(Object).Role != RoleCaster {
			return nil, fmt.Errorf("takes the hit and the caster")
		}
		return Object{Kind: "closest", Role: RoleClosestInHit}, nil
	}
	relation := func(fn int) func(args []Value) (Value, error) {
		return func(args []Value) (Value, error) {
			a, ok := args[0].(Object)
			b, ok2 := args[1].(Object)
			if len(args) != 2 || !ok || !ok2 {
				return nil, fmt.Errorf("takes two actors")
			}
			return ctx.HostFn(fn, RolePair(a.Role, b.Role), nil) != 0, nil
		}
	}
	env.Funcs["IsNeutral"] = relation(HostIsNeutral)
	env.Funcs["IsEnemy"] = relation(HostIsEnemy)
	env.Funcs["GetAngleBetween"] = func(args []Value) (Value, error) {
		if len(args) != 2 || !isObjectVal(args[0]) || !isObjectVal(args[1]) {
			return nil, fmt.Errorf("takes two actors")
		}
		return ctx.HostFn(HostAngleBetween, RolePair(args[0].(Object).Role, args[1].(Object).Role), nil), nil
	}
	env.Funcs["GetDistanceBetween"] = func(args []Value) (Value, error) {
		if len(args) != 2 || !isObjectVal(args[0]) || !isObjectVal(args[1]) {
			return nil, fmt.Errorf("takes two actors")
		}
		return ctx.HostFn(HostDistanceBetween, RolePair(args[0].(Object).Role, args[1].(Object).Role), nil), nil
	}
}

func addAbilityActorStateFuncs(env *Env, ctx AbilityContext) {
	actorRead := func(op int, ref bool) func(args []Value) (Value, error) {
		return func(args []Value) (Value, error) {
			name := ""
			switch {
			case ref && len(args) == 2 && isObjectVal(args[0]):
				r, ok := args[1].(Ref)
				if !ok {
					return nil, fmt.Errorf("takes an actor and a record reference")
				}
				name = strconv.FormatInt(r.Guid, 10)
			case !ref && len(args) == 1 && isObjectVal(args[0]):
			default:
				return nil, fmt.Errorf("takes the wrong arguments")
			}
			return ctx.HostFn(RoleFn(args[0].(Object).Role, op), name, nil), nil
		}
	}
	env.Funcs["GetHealthPercentage"] = func(args []Value) (Value, error) {
		if len(args) != 1 || !isObjectVal(args[0]) {
			return nil, fmt.Errorf("takes an actor")
		}
		c := contextOf(ctx, args[0].(Object))
		health, err := c.Stat(Ref{Kind: "Stat", Name: "Health"})
		if err != nil {
			return nil, err
		}
		max, err := c.Stat(Ref{Kind: "Stat", Name: "MaxHealth"})
		if err != nil {
			return nil, err
		}
		if max == 0 {
			return 0.0, nil
		}
		return health / max, nil
	}
	env.Funcs["HasAlignment"] = func(args []Value) (Value, error) {
		v, err := actorRead(RoleOpHasAlignment, true)(args)
		if err != nil {
			return nil, err
		}
		return v.(float64) != 0, nil
	}
	env.Funcs["GetEffectRemainingDuration"] = actorRead(RoleOpEffectRemaining, true)
}

func addAbilityTimeAndDamageFuncs(env *Env, ctx AbilityContext) {
	actorAbilityHost := func(fn int) func(args []Value) (Value, error) {
		return func(args []Value) (Value, error) {
			if len(args) != 2 || !isObjectVal(args[0]) {
				return nil, fmt.Errorf("takes an actor and an ability")
			}
			r, ok := args[1].(Ref)
			if !ok {
				return nil, fmt.Errorf("takes an actor and an ability")
			}
			return ctx.HostFn(fn, strconv.FormatInt(r.Guid, 10), []float64{float64(args[0].(Object).Role)}), nil
		}
	}
	env.Funcs["GetHitTriggerCount"] = actorAbilityHost(HostHitTriggerCount)
	env.Funcs["GetAbilityHitCount"] = actorAbilityHost(HostAbilityHitCount)
	env.Funcs["Now"] = func(args []Value) (Value, error) {
		if len(args) != 0 {
			return nil, fmt.Errorf("takes no arguments, got %d", len(args))
		}
		return ctx.HostFn(HostNow, "", nil), nil
	}
	env.Funcs["GetProgressDuration"] = func(args []Value) (Value, error) {
		if len(args) != 1 {
			return nil, fmt.Errorf("takes 1 argument(s), got %d", len(args))
		}
		g, ok := args[0].(string)
		if !ok {
			return nil, fmt.Errorf("argument 1 is %s", describeValue(args[0]))
		}
		return ctx.HostFn(HostProgressDuration, g, nil), nil
	}
	for i, name := range DamageFnNames {
		fn := HostCalcDamage + i
		env.Funcs[name] = func(args []Value) (Value, error) {
			if len(args) != 0 {
				return nil, fmt.Errorf("takes no arguments, got %d", len(args))
			}
			return ctx.HostFn(fn, "", nil), nil
		}
	}
}

func addGatherFuncs(env *Env, ctx AbilityContext) {
	env.Funcs["GetArtisanshipModifiedStat"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 2); err != nil {
			return nil, err
		}
		o, ok := args[0].(Object)
		r, ok2 := args[1].(Ref)
		if !ok || !ok2 {
			return nil, fmt.Errorf("takes an actor and a stat")
		}
		return ctx.HostFn(HostArtisanStat, fmt.Sprint(r.Guid), []float64{float64(o.Role)}), nil
	}
	for name, fn := range map[string]int{
		"GetRolledItemQuality":                HostRolledItemQuality,
		"GetItemRarity":                       HostItemRarity,
		"GetSourceCertificationTier":          HostSourceCertTier,
		"GetGatheringInteractionBaseDuration": HostGatherBaseSecs,
	} {
		fn := fn
		env.Funcs[name] = func(args []Value) (Value, error) {
			if err := checkArgCount(args, 0); err != nil {
				return nil, err
			}
			return ctx.HostFn(fn, "", nil), nil
		}
	}
}

type nodeEnvHelper struct {
	ctx    AbilityContext
	ranker NodeRanker
}

func (h nodeEnvHelper) rankOf(v Value) (int, error) {
	r, ok := v.(Ref)
	if !ok || (r.Kind != KindCityNode && r.TypeID != TypeCityNode) {
		return 0, fmt.Errorf("argument is %s, not a city node", describeValue(v))
	}
	if h.ranker == nil {
		return 0, fmt.Errorf("the context has no city nodes")
	}
	rank, ok := h.ranker.NodeRank(r.Guid, r.Name)
	if !ok {
		return 0, fmt.Errorf("%v is no city node", r)
	}
	return rank, nil
}

func (h nodeEnvHelper) nodeValue(rank int) (Value, error) {
	if h.ranker == nil {
		return nil, fmt.Errorf("the context has no city nodes")
	}
	g, ok := h.ranker.NodeGuid(rank)
	if !ok {
		return nil, fmt.Errorf("the host answered node %d, which is no city node", rank)
	}
	return Ref{TypeID: TypeCityNode, Guid: g}, nil
}

func (h nodeEnvHelper) enumValue(typ string, ord float64) (Value, error) {
	name, ok := EnumName(typ, int(ord))
	if !ok || float64(int(ord)) != ord {
		return nil, fmt.Errorf("the host answered %v, which is no %s", ord, typ)
	}
	return Enum{Type: typ, Value: name}, nil
}

func (h nodeEnvHelper) requirePlayer(args []Value, i int) error {
	if o, ok := args[i].(Object); !ok || o.Role != RolePlayer {
		return fmt.Errorf("argument %d is %s, not GetPlayer()", i+1, describeValue(args[i]))
	}
	return nil
}

func addNodeFuncs(env *Env, ctx AbilityContext) {
	ranker, _ := ctx.(NodeRanker)
	h := nodeEnvHelper{ctx: ctx, ranker: ranker}
	addNodeQueryFuncs(env, h)
	addNodeCitizenFuncs(env, h)
	addNodeRangeAndEnumFuncs(env, h)
}

func addNodeQueryFuncs(env *Env, h nodeEnvHelper) {
	for _, name := range []string{"GetPlayer", "GetPlayerGuid"} {
		kind := map[string]string{"GetPlayer": "player", "GetPlayerGuid": "playerguid"}[name]
		env.Funcs[name] = func(args []Value) (Value, error) {
			if err := checkArgCount(args, 0); err != nil {
				return nil, err
			}
			return Object{Kind: kind, Role: RolePlayer}, nil
		}
	}
	env.Funcs["GetNodeBiome"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 0); err != nil {
			return nil, err
		}
		return h.enumValue("EBiomeType", h.ctx.HostFn(HostNodeBiome, "", nil))
	}
	env.Funcs["GetNodeRecord"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 0); err != nil {
			return nil, err
		}
		return h.nodeValue(int(h.ctx.HostFn(HostNodeRecord, "", nil)))
	}
	env.Funcs["GetCurrentNode"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 1); err != nil {
			return nil, err
		}
		o, ok := args[0].(Object)
		if !ok {
			return nil, fmt.Errorf("argument 1 is %s, not an actor", describeValue(args[0]))
		}
		who := float64(o.Role)
		if o.Role == RolePlayer {
			who = ActorPlayer
		}
		return h.nodeValue(int(h.ctx.HostFn(HostCurrentNode, "", []float64{who})))
	}
	env.Funcs["Node_GetNodeType"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 1); err != nil {
			return nil, err
		}
		rank, err := h.rankOf(args[0])
		if err != nil {
			return nil, err
		}
		return h.enumValue("ENodeType", h.ctx.HostFn(HostNodeType, "", []float64{float64(rank)}))
	}
	env.Funcs["Node_GetLevel"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 1); err != nil {
			return nil, err
		}
		rank, err := h.rankOf(args[0])
		if err != nil {
			return nil, err
		}
		return h.enumValue("ENodeLevel", h.ctx.HostFn(HostNodeLevel, "", []float64{float64(rank)}))
	}
}

func addNodeCitizenFuncs(env *Env, h nodeEnvHelper) {
	env.Funcs["Node_IsPlayerCitizen"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 1); err != nil {
			return nil, err
		}
		if err := h.requirePlayer(args, 0); err != nil {
			return nil, err
		}
		return h.ctx.HostFn(HostPlayerCitizen, "", nil) != 0, nil
	}
	env.Funcs["Node_IsPlayerCitizenOfNode"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 2); err != nil {
			return nil, err
		}
		if err := h.requirePlayer(args, 0); err != nil {
			return nil, err
		}
		rank, err := h.rankOf(args[1])
		if err != nil {
			return nil, err
		}
		return h.ctx.HostFn(HostPlayerCitizenOfNode, "", []float64{float64(rank)}) != 0, nil
	}
	env.Funcs["Node_IsPlayerMayor"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 1); err != nil {
			return nil, err
		}
		if err := h.requirePlayer(args, 0); err != nil {
			return nil, err
		}
		return h.ctx.HostFn(HostPlayerMayor, "", nil) != 0, nil
	}
}

func addNodeRangeAndEnumFuncs(env *Env, h nodeEnvHelper) {
	env.Funcs["Node_InLevelRangeInclusive"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 3); err != nil {
			return nil, err
		}
		rank, err := h.rankOf(args[0])
		if err != nil {
			return nil, err
		}
		var bound [2]float64
		for i := 0; i < 2; i++ {
			e, ok := args[1+i].(Enum)
			ord, known := EnumOrdinal(e.Type, e.Value)
			if !ok || e.Type != "ENodeLevel" || !known {
				return nil, fmt.Errorf("argument %d is %s, not an ENodeLevel", 2+i, describeValue(args[1+i]))
			}
			bound[i] = float64(ord)
		}
		level := h.ctx.HostFn(HostNodeLevel, "", []float64{float64(rank)})
		return level >= bound[0] && level <= bound[1], nil
	}
	env.Funcs["EnumInt"] = func(args []Value) (Value, error) {
		if err := checkArgCount(args, 1); err != nil {
			return nil, err
		}
		e, ok := args[0].(Enum)
		if !ok {
			return nil, fmt.Errorf("argument 1 is %s, not an enum value", describeValue(args[0]))
		}
		ord, ok := EnumOrdinal(e.Type, e.Value)
		if !ok {
			return nil, fmt.Errorf("%v is not an enum this evaluation knows", e)
		}
		return float64(ord), nil
	}
}
