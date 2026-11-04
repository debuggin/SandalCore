// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package aocexpr

import (
	"fmt"
	"strings"
)

// State is what the formulas read of the character's state beyond its class,
// level and equipment.
type State interface {
	EffectCount(effect Ref) float64
	EffectCountByTag(tag string) float64
	SkillPurchased(class, skill Ref) bool
	InCombat() bool
	Moving() bool
	Unkillable() bool
	EquippedItemInSlotHasTag(slot Enum, tag string) bool
	ActorGuid() float64
	ProfessionLevel(actor float64, profession string) float64
	ConsumedItemStat(stat Ref) float64
	NpcEliteRank() float64
	NpcStatDistribution(index int) float64
}

// StatDistributionIndex returns the EStatDistributionStat enum index, or -1 if unknown.
func StatDistributionIndex(name string) int {
	for i, n := range []string{"Constitution", "Strength", "Dexterity", "Mentality", "Intelligence", "Wisdom", "MaxHealth", "MaxMana"} {
		if n == name {
			return i
		}
	}
	return -1
}

// NoState is the default State of a character with no active effects, skills, or items.
type NoState struct{}

func (NoState) EffectCount(Ref) float64                    { return 0 }
func (NoState) EffectCountByTag(string) float64            { return 0 }
func (NoState) SkillPurchased(Ref, Ref) bool               { return false }
func (NoState) InCombat() bool                             { return false }
func (NoState) Moving() bool                               { return false }
func (NoState) Unkillable() bool                           { return false }
func (NoState) EquippedItemInSlotHasTag(Enum, string) bool { return false }
func (NoState) ActorGuid() float64                         { return 0 }
func (NoState) ProfessionLevel(float64, string) float64    { return 1 }
func (NoState) ConsumedItemStat(Ref) float64               { return 0 }
func (NoState) NpcEliteRank() float64                      { return 0 }
func (NoState) NpcStatDistribution(int) float64            { return 0 }

// StatContext is what a stat formula runs against: the stat engine's own functions and State.
type StatContext interface {
	State
	Stat(stat Ref) (float64, error)
	InitRunningTotal() (float64, error)
	Equation(equation Ref) (float64, error)
	Formula(formula Ref) (float64, error)
	Term(term Ref) (float64, error)
	CharacterLevel() float64
}

// StatFloatFuncs are the stat environment's host functions whose result is a float.
var StatFloatFuncs = map[string]bool{
	"GetStat":                 true,
	"GetStatInitRunningTotal": true,
	"EvalE":                   true,
	"EvalEquation":            true,
	"EvalFormula":             true,
	"EvalF":                   true,
	"GetTerm":                 true,
	"GetConsumedItemStat":     true,
	"GetNpcStatDistribution":  true,
}

// HasValidEquipmentTag is the root item tag queried by HasValidEquipment.
const HasValidEquipmentTag = "Item"

// StatFuncNames lists the host functions StatEnv supplies.
func StatFuncNames() []string {
	return []string{
		"GetOwner", "GetInstigator", "GetSource", "GetSelf",
		"GetCharacterLevel", "GetNPCLevel", "GetStat", "GetStatInitRunningTotal", "EvalE", "EvalEquation", "EvalFormula", "EvalF", "GetTerm",
		"GetEffectCount", "GetEffectCountByTag", "GetSkillPurchased", "IsInCombat", "IsMoving", "IsUnkillable",
		"EquippedItemInSlotHasTag", "HasValidEquipment", "GetActorGuid", "GetPlayerProfessionLevel", "GetConsumedItemStat", "GetNpcEliteRank", "GetNpcStatDistribution",
	}
}

func contextOf(ctx StatContext, o Object) StatContext {
	if o.Role != RoleCaster {
		if rc, ok := ctx.(RoleContexter); ok {
			if c := rc.RoleContext(o.Role); c != nil {
				return c
			}
		}
	}
	return ctx
}

func checkStatArgs(args []Value, sig string) error {
	if len(args) != len(sig) {
		return fmt.Errorf("takes %d argument(s), got %d", len(sig), len(args))
	}
	for i, c := range sig {
		ok := false
		switch c {
		case 'o':
			_, ok = args[i].(Object)
		case 'r':
			_, ok = args[i].(Ref)
		case 's':
			_, ok = args[i].(string)
		case 'e':
			_, ok = args[i].(Enum)
		case 'n':
			_, ok = args[i].(float64)
		}
		if !ok {
			return fmt.Errorf("argument %d is %s", i+1, describeValue(args[i]))
		}
	}
	return nil
}

func numStatFunc(f func(args []Value) (float64, error)) Func {
	return func(args []Value) (Value, error) {
		v, err := f(args)
		if err != nil {
			return nil, err
		}
		return v, nil
	}
}

func boolStatFunc(sig string, f func(args []Value) bool) Func {
	return func(args []Value) (Value, error) {
		if err := checkStatArgs(args, sig); err != nil {
			return nil, err
		}
		return f(args), nil
	}
}

func roleBoolStatFunc(ctx StatContext, read func(c StatContext) bool) Func {
	return func(args []Value) (Value, error) {
		if err := checkStatArgs(args, "o"); err != nil {
			return nil, err
		}
		return read(contextOf(ctx, args[0].(Object))), nil
	}
}

// StatEnv returns the environment of a player's stat formulas over ctx.
func StatEnv(ctx StatContext, curves CurveSource) *Env {
	funcs := make(map[string]Func, 26)
	addCoreStatFuncs(funcs, ctx)
	addStateEffectFuncs(funcs, ctx)
	addStateActorFuncs(funcs, ctx)
	return &Env{
		Curves:     curves,
		FloatFuncs: StatFloatFuncs,
		Funcs:      funcs,
	}
}

func addCoreStatFuncs(funcs map[string]Func, ctx StatContext) {
	obj := func(kind string) Func {
		return func(args []Value) (Value, error) {
			if len(args) != 0 {
				return nil, fmt.Errorf("takes no arguments, got %d", len(args))
			}
			return Object{Kind: kind}, nil
		}
	}
	level := func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "o"); err != nil {
			return 0, err
		}
		return contextOf(ctx, a[0].(Object)).CharacterLevel(), nil
	}
	formula := func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "r"); err != nil {
			return 0, err
		}
		return ctx.Formula(a[0].(Ref))
	}
	equation := func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "r"); err != nil {
			return 0, err
		}
		return ctx.Equation(a[0].(Ref))
	}
	funcs["GetOwner"] = obj("owner")
	funcs["GetInstigator"] = obj("instigator")
	funcs["GetSource"] = obj("source")
	funcs["GetSelf"] = obj("self")
	funcs["GetCharacterLevel"] = numStatFunc(level)
	funcs["GetNPCLevel"] = numStatFunc(level)
	funcs["GetStat"] = numStatFunc(func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "or"); err != nil {
			return 0, err
		}
		return contextOf(ctx, a[0].(Object)).Stat(a[1].(Ref))
	})
	funcs["GetStatInitRunningTotal"] = numStatFunc(func(a []Value) (float64, error) {
		if err := checkStatArgs(a, ""); err != nil {
			return 0, err
		}
		return ctx.InitRunningTotal()
	})
	funcs["EvalE"] = numStatFunc(equation)
	funcs["EvalEquation"] = numStatFunc(equation)
	funcs["EvalFormula"] = numStatFunc(formula)
	funcs["EvalF"] = numStatFunc(formula)
	funcs["GetTerm"] = numStatFunc(func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "r"); err != nil {
			return 0, err
		}
		return ctx.Term(a[0].(Ref))
	})
}

func addStateEffectFuncs(funcs map[string]Func, ctx StatContext) {
	funcs["GetEffectCount"] = numStatFunc(func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "or"); err != nil {
			return 0, err
		}
		return contextOf(ctx, a[0].(Object)).EffectCount(a[1].(Ref)), nil
	})
	funcs["GetEffectCountByTag"] = numStatFunc(func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "os"); err != nil {
			return 0, err
		}
		return contextOf(ctx, a[0].(Object)).EffectCountByTag(a[1].(string)), nil
	})
	funcs["GetSkillPurchased"] = func(a []Value) (Value, error) {
		if err := checkStatArgs(a, "orr"); err != nil {
			return nil, err
		}
		return contextOf(ctx, a[0].(Object)).SkillPurchased(a[1].(Ref), a[2].(Ref)), nil
	}
	funcs["IsInCombat"] = roleBoolStatFunc(ctx, func(c StatContext) bool { return c.InCombat() })
	funcs["IsMoving"] = roleBoolStatFunc(ctx, func(c StatContext) bool { return c.Moving() })
	funcs["IsUnkillable"] = roleBoolStatFunc(ctx, func(c StatContext) bool { return c.Unkillable() })
	funcs["EquippedItemInSlotHasTag"] = boolStatFunc("es", func(a []Value) bool {
		return ctx.EquippedItemInSlotHasTag(a[0].(Enum), a[1].(string))
	})
	funcs["HasValidEquipment"] = boolStatFunc("oe", func(a []Value) bool {
		return contextOf(ctx, a[0].(Object)).EquippedItemInSlotHasTag(a[1].(Enum), HasValidEquipmentTag)
	})
}

func addStateActorFuncs(funcs map[string]Func, ctx StatContext) {
	funcs["GetActorGuid"] = numStatFunc(func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "o"); err != nil {
			return 0, err
		}
		if a[0].(Object).Role != RoleCaster {
			return 0, fmt.Errorf("the actor id of another actor is not modelled")
		}
		return ctx.ActorGuid(), nil
	})
	funcs["GetPlayerProfessionLevel"] = numStatFunc(func(a []Value) (float64, error) {
		if len(a) == 2 {
			if o, ok := a[0].(Object); ok && o.Role == RolePlayer {
				a = []Value{ctx.ActorGuid(), a[1]}
			}
		}
		if err := checkStatArgs(a, "ns"); err != nil {
			return 0, err
		}
		return ctx.ProfessionLevel(a[0].(float64), a[1].(string)), nil
	})
	funcs["GetConsumedItemStat"] = numStatFunc(func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "r"); err != nil {
			return 0, err
		}
		return ctx.ConsumedItemStat(a[0].(Ref)), nil
	})
	funcs["GetNpcEliteRank"] = numStatFunc(func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "o"); err != nil {
			return 0, err
		}
		return contextOf(ctx, a[0].(Object)).NpcEliteRank(), nil
	})
	funcs["GetNpcStatDistribution"] = numStatFunc(func(a []Value) (float64, error) {
		if err := checkStatArgs(a, "oe"); err != nil {
			return 0, err
		}
		e := a[1].(Enum)
		i := StatDistributionIndex(e.Value)
		if e.Type != "EStatDistributionStat" || i < 0 {
			return 0, fmt.Errorf("%v is not an EStatDistributionStat", e)
		}
		if a[0].(Object).Role != RoleCaster {
			return 0, fmt.Errorf("the stat distribution of another actor is not modelled")
		}
		return ctx.NpcStatDistribution(i), nil
	})
}

// TagMatches reports whether a gameplay tag matches a wanted tag case-insensitively or as a descendant.
func TagMatches(tag, want string) bool {
	if len(tag) == len(want) {
		return strings.EqualFold(tag, want)
	}
	return len(tag) > len(want) && tag[len(want)] == '.' && strings.EqualFold(tag[:len(want)], want)
}
