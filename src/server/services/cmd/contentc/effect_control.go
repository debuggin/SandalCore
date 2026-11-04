// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"strconv"
	"strings"

	"aocrop/services/internal/designdb"
)

// Effect-control export (contentc version 39) runs after all other ability, reward, and gathering tables
// so newly added expressions receive trailing IDs without shifting existing expression IDs.

func (f *fields) optBool(name string) bool {
	if _, ok := f.get(name); !ok {
		return false
	}
	return f.boolean(name)
}

func (f *fields) optStr(name string) string {
	if _, ok := f.get(name); !ok {
		return ""
	}
	return f.str(name)
}

func (f *fields) optNumber(name string) float64 {
	if _, ok := f.get(name); !ok {
		return 0
	}
	return f.number(name)
}

func optRefGuids(f *fields, name string) []int64 {
	if _, ok := f.get(name); !ok {
		return nil
	}
	return refGuids(f, name)
}

func (b *abBuilder) writeEffectControl(opt *Options, meta map[string]string, effects []int64) error {
	ccs := indexRows(b.src.rows["CrowdControlRecordBase"])
	disables := indexRows(b.src.rows["AbilityDisableRecordBase"])
	frags := indexRows(b.src.rows["FragileEffectDefRecordBase"])
	ccDone, disableDone, fragDone := map[int64]bool{}, map[int64]bool{}, map[int64]bool{}
	for _, g := range effects {
		r := b.effects[g]
		if r == nil {
			return fmt.Errorf("no Effect row %d", g)
		}
		f := newFields(fmt.Sprintf("Effect %s (%d)", r.Name, g), r.Fields)
		compare, recalc, lostOnElapse := f.optBool("bCompareSourceWhenStacking"), f.optBool("bRecalculateHitOnTick"), f.optBool("bNumStacksLostOnDurationElapse")
		extendedMax := f.optNumber("ExtendedMaximum")
		lostText, extensionText := f.optStr("NumStacksLostOnDurationElapse"), f.optStr("ExtensionAmount")
		cc := optID(f, "CrowdControlId")
		fragilities := optRefGuids(f, "FragilityDefinitionsIds")
		if f.err != nil {
			return f.err
		}
		ids, err := b.exprIDs(lostText, extensionText)
		if err != nil {
			return err
		}
		if err := b.w.exec(`INSERT INTO ab_effect_control (effect_guid, compare_source, recalc_on_tick, stacks_lost_on_elapse, stacks_lost_expr, extended_max, extension_expr)
			VALUES (?, ?, ?, ?, ?, ?, ?)`, g, b2i(compare), b2i(recalc), b2i(lostOnElapse), ids[0], extendedMax, ids[1]); err != nil {
			return err
		}
		if cc != 0 && !ccDone[cc] {
			row := ccs[cc]
			if row == nil {
				return fmt.Errorf("%s: no CrowdControl row %d", f.where, cc)
			}
			ccDone[cc] = true
			if err := b.writeCrowdControl(row, disables, disableDone); err != nil {
				return err
			}
		}
		for i, fg := range fragilities {
			if !fragDone[fg] {
				row := frags[fg]
				if row == nil {
					return fmt.Errorf("%s: no FragileEffectDef row %d", f.where, fg)
				}
				fragDone[fg] = true
				if err := b.writeFragility(row); err != nil {
					return err
				}
			}
			if err := b.w.exec(`INSERT INTO ab_effect_fragility (effect_guid, ord, fragility_guid) VALUES (?, ?, ?)`, g, i, fg); err != nil {
				return err
			}
		}
	}
	meta["ab.crowd_controls"] = strconv.Itoa(len(ccDone))
	meta["ab.ability_disables"] = strconv.Itoa(len(disableDone))
	meta["ab.fragilities"] = strconv.Itoa(len(fragDone))
	opt.logf("ab_effect: the effect-control columns of %d effects; %d crowd-control, %d ability-disable and %d fragility records; %d expressions in all", len(effects), len(ccDone), len(disableDone), len(fragDone), b.exprs.next)
	return nil
}

func (b *abBuilder) writeAbilityDisable(where string, disable int64, disables map[int64]*designdb.Row, disableDone map[int64]bool) error {
	if disable == 0 || disableDone[disable] {
		return nil
	}
	row := disables[disable]
	if row == nil {
		return fmt.Errorf("%s: no AbilityDisable row %d", where, disable)
	}
	df := newFields(fmt.Sprintf("AbilityDisable %s (%d)", row.Name, disable), row.Fields)
	filter := df.str("Filter")
	if df.err != nil {
		return df.err
	}
	if err := b.w.exec(`INSERT INTO ab_ability_disable (guid, name, filter) VALUES (?, ?, ?)`, disable, row.Name, filter); err != nil {
		return err
	}
	disableDone[disable] = true
	return nil
}

func (b *abBuilder) writeCrowdControl(r *designdb.Row, disables map[int64]*designdb.Row, disableDone map[int64]bool) error {
	f := newFields(fmt.Sprintf("CrowdControl %s (%d)", r.Name, r.Guid), r.Fields)
	status, priority := f.int("AppliedStatus"), f.int("Priority")
	animation := f.str("AnimationRequest")
	disable := optID(f, "AbilitiesToDisableId")
	face, ignore, restart, override := f.boolean("bFaceSource"), f.boolean("bIgnoreNonCombatantImmunity"), f.boolean("bRestartMontageOnReapply"), f.boolean("bOverrideClientAuthMovement")
	param, magnitude, duration := f.str("PushParameterization"), f.str("PushMagnitude"), f.str("PushDuration")
	arrival, endAtArrival := f.number("PushArrivalDistance"), f.boolean("bEndPushAtArrival")
	dir := newFields(f.where+": PushDirection", f.strukt("PushDirection"))
	dx, dy, dz := dir.number("X"), dir.number("Y"), dir.number("Z")
	snare := strings.TrimSpace(f.str("SnareStrength"))
	if f.err != nil {
		return f.err
	}
	// SnareStrength is a literal number in design data; non-numeric expressions are rejected rather than coerced to 0.
	strength := 0.0
	if snare != "" {
		v, err := strconv.ParseFloat(snare, 64)
		if err != nil {
			return fmt.Errorf("%s: SnareStrength %q is no number", f.where, snare)
		}
		strength = v
	}
	if err := b.writeAbilityDisable(f.where, disable, disables, disableDone); err != nil {
		return err
	}
	return b.w.exec(`INSERT INTO ab_crowd_control (guid, name, applied_status, snare_strength, animation_request, priority, disable_guid, face_source, ignore_non_combatant,
		restart_montage, override_client_move, push_parameterization, push_magnitude, push_duration, push_dir_x, push_dir_y, push_dir_z, push_arrival_distance, end_push_at_arrival)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		r.Guid, r.Name, status, strength, animation, priority, disable, b2i(face), b2i(ignore), b2i(restart), b2i(override), param, magnitude, duration, dx, dy, dz, arrival, b2i(endAtArrival))
}

func (b *abBuilder) writeFragility(r *designdb.Row) error {
	f := newFields(fmt.Sprintf("FragileEffectDef %s (%d)", r.Name, r.Guid), r.Fields)
	kind := f.str("EffectFragilityType")
	threshold, rate := f.str("EffectBreakThreshold"), f.str("EffectBreakThresholdAccumulationRate")
	frequency := f.number("EvaluationTickFrequency")
	immediately, evaluate := f.str("ShouldBreakEffectImmediately"), f.str("ShouldEvaluateThresholdAccumulation")
	bindings := f.strukt("EventBindings")
	if f.err != nil {
		return f.err
	}
	bits := map[string]int64{}
	for k, v := range bindings {
		if n, ok := v.(int64); ok {
			bits[k] = n
		}
	}
	return b.w.exec(`INSERT INTO ab_fragility (guid, name, fragility_type, break_threshold, accumulation_rate, tick_frequency, break_immediately, evaluate_accumulation, bindings)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`, r.Guid, r.Name, kind, threshold, rate, frequency, immediately, evaluate, jsonOf(bits))
}
