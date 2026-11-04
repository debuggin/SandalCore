// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"
	"testing"
)

func corpusEffectLimitGraph() (*Graph, []Expr) {
	g := TinyGraph(false)
	stat := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatDef, 1000+n) }
	term := func(n int64) string { return fmt.Sprintf("$#%d:%d$", TypeTerm, n) }
	eq := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatEquation, n) }
	fm := func(n int) string { return fmt.Sprintf("$#%d:%d$", TypeStatFormula, n) }
	g.Equations[105] = "GetTerm(" + term(310) + ") * (GetStat(GetOwner(), " + stat(1) + ") + 5.0)"
	g.Clamps[105] = Clamp{HasMax: true, Max: 0}
	g.Formulas[210] = Formula{Equation: 105, Terms: map[int64]string{310: "-1.0"}}
	g.Formulas[211] = Formula{Equation: 105, Terms: map[int64]string{310: "1.0"}}
	g.Equations[106] = "GetStat(GetOwner(), " + stat(1) + ") - 100.0"
	g.Clamps[106] = Clamp{HasMin: true, Min: 0, HasMax: true, Max: 15}
	exprs := []Expr{
		{What: "damage formula", Text: "EvalFormula(" + fm(210) + ")"},
		{What: "burn formula", Text: "EvalFormula(" + fm(211) + ")"},
		{What: "equation over upper limit", Text: "EvalE(" + eq(103) + ")"},
		{What: "equation under lower limit", Text: "EvalE(" + eq(106) + ")"},
		{What: "formula over equation upper limit", Text: "EvalFormula(" + fm(201) + ")"},
	}
	return g, exprs
}

func verifyCorpusEffectMode(t *testing.T, mode Mode) {
	t.Helper()
	g, exprs := corpusEffectLimitGraph()
	b, res, err := CompileExprs(g, Options{Mode: mode}, exprs)
	if err != nil {
		t.Fatalf("%v: %v", mode, err)
	}
	in := tinyInputs(2, 20, false)
	run, err := b.Run(in, nil)
	if err != nil {
		t.Fatal(err)
	}
	str := statValue(t, run, 1)
	ev, err := b.NewEvaluator(run, in, nil)
	if err != nil {
		t.Fatal(err)
	}
	wants := []float64{-(str + 5), str + 5, str - 20, str - 100, str * 0.01}
	for i, want := range wants {
		if res[i].Program < 0 {
			t.Fatalf("%v: expression %d (%q) failed: %s", mode, i, exprs[i].What, res[i].Err)
		}
		got, err := ev.Eval(res[i].Program, nil)
		if err != nil {
			t.Fatal(err)
		}
		if math.Abs(got-want) > 1e-5*math.Max(1, math.Abs(want)) {
			t.Errorf("%v: %s = %v, want %v", mode, exprs[i].What, got, want)
		}
	}
	if ev.Faults() != 0 {
		t.Errorf("%v: %d faults", mode, ev.Faults())
	}
	if got, want := statValue(t, run, 18), (str-20)+(str*0.01)*100; math.Abs(got-want) > 1e-3 {
		t.Errorf("%v: Constrained stat = %v, want %v", mode, got, want)
	}
}

func TestCorpusEffectsUnclampedEquationsAndFormulas(t *testing.T) {
	for _, mode := range []Mode{ModeFloat32, ModeDouble} {
		verifyCorpusEffectMode(t, mode)
	}
}

func TestCorpusEffectsRefEngineConstrainedParity(t *testing.T) {
	g := TinyGraph(false)
	if len(g.Clamps) == 0 {
		t.Fatal("TinyGraph has no equation clamps")
	}
	in := tinyInputs(2, 20, false)
	ref, err := NewRefEngine(g, in, nil, ModeFloat32)
	if err != nil {
		t.Fatal(err)
	}
	guid, ok := g.StatByName("Constrained")
	if !ok {
		t.Fatal("missing Constrained stat in TinyGraph")
	}
	got, err := ref.Value(guid)
	if err != nil {
		t.Fatal(err)
	}
	if want := (80.0 - 20) + (80.0*0.01)*100; math.Abs(got-want) > 1e-3 {
		t.Errorf("RefEngine Value(Constrained) = %v, want %v", got, want)
	}
}
