// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
)

// Options configure a compile.
type Options struct {
	Mode Mode
}

type vtype uint8

const (
	tNum vtype = iota
	tBool
	tObj
	tRef
	tStr
	tEnum
	tActor
	tFlags
	tPlayer
)

func (t vtype) String() string {
	return [...]string{"a number", "a boolean", "an object", "a record reference", "a string", "an enum value", "an actor id", "the flags of a hit", "the player"}[t]
}

// == C | Y | B | C O A ==

type (
	node   interface{}
	nConst struct{ f float64 }
	nLoad  struct{ slot int }
	nStat  struct{ idx int }
	nTotal struct{}
	nLevel struct{}
	nUn    struct {
		op byte
		x  node
	}
	nBin struct {
		op   byte
		l, r node
	}
	nAnd struct{ l, r node }
	nOr  struct{ l, r node }
	nSel struct{ c, a, b node }
	nFn1 struct {
		op byte
		x  node
	}
	nFn2 struct {
		op   byte
		a, b node
	}
	nFn3 struct {
		op      byte
		a, b, c node
	}
	nCurve struct {
		idx int
		x   node
	}
	nCtx struct {
		op  byte
		imm [2]uint16
	}
	nHost struct {
		op      byte
		fn, str uint16
		args    []node
	}
	nBlock struct {
		stmts  []stmt
		result node
	}
	nActor struct {
		role int
		x    node
	}
)

type (
	stmt   interface{}
	sStore struct {
		slot int
		x    node
	}
	sIf struct {
		cond      node
		then, els []stmt
	}
	sDrop struct{ x node }
)

type val struct {
	t     vtype
	n     node
	float bool // provably a float (the division rule)
	ref   aocexpr.Ref
	str   string
	enum  aocexpr.Enum
	role  int
	etype string
}

const etypeNode = "CityNode"

func (v val) constant() (float64, bool) {
	c, ok := v.n.(nConst)
	return c.f, ok
}

type binding struct {
	t      vtype
	konst  bool
	v      val
	slot   int
	float  bool
	isSlot bool
	etype  string
}

type scope struct {
	vars    map[string]*binding
	barrier bool // a called equation does not see its caller's variables
}

type unitKind uint8

const (
	unitInit unitKind = iota
	unitBase
	unitLimit
	unitRegen
	unitExpr
)

type compiler struct {
	g      *Graph
	single bool
	b      *Bundle

	statIdx  map[int64]int
	queue    []int
	qpos     int
	constIdx map[uint64]int
	strIdx   map[string]int
	refIdx   map[Ref]int
	curveIdx map[int64]int
	progIdx  map[string]int
	parsed   map[string]*aocexpr.Program
}

// Compile compiles the player stat graph and evaluation order.
func Compile(g *Graph, opt Options) (*Bundle, error) {
	b, _, err := CompileExprs(g, opt, nil)
	return b, err
}

// Expr is an ability expression to compile.
type Expr struct {
	What  string
	Text  string
	Terms map[int64]string
}

// ExprResult is what compiling one Expr gave.
type ExprResult struct {
	Program    int
	Err        string
	UsesTarget bool
}

// CoA + C-Y-B

// CompileExprs compiles the stat graph and ability expressions into the same bundle.
func CompileExprs(g *Graph, opt Options, exprs []Expr) (*Bundle, []ExprResult, error) {
	c, err := compileGraph(g, opt)
	if err != nil {
		return nil, nil, err
	}
	results := make([]ExprResult, len(exprs))
	var progs []int
	for i, ex := range exprs {
		results[i] = c.expr(ex)
		if results[i].Program >= 0 {
			progs = append(progs, results[i].Program)
		}
	}
	sort.Ints(progs)
	for i, p := range progs {
		if i == 0 || p != progs[i-1] {
			c.b.Exprs = append(c.b.Exprs, uint32(p))
		}
	}
	if err := c.order(); err != nil {
		return nil, nil, err
	}
	return c.b, results, nil
}

func compileGraph(g *Graph, opt Options) (*compiler, error) {
	c := &compiler{
		g: g, single: opt.Mode == ModeSingle, b: &Bundle{Mode: opt.Mode},
		statIdx: map[int64]int{}, constIdx: map[uint64]int{}, strIdx: map[string]int{},
		refIdx: map[Ref]int{}, curveIdx: map[int64]int{}, progIdx: map[string]int{},
		parsed: map[string]*aocexpr.Program{},
	}
	classes := append([]ClassDef(nil), g.Classes...)
	sort.Slice(classes, func(i, j int) bool { return classes[i].Enum < classes[j].Enum })
	if len(classes) > MaxClasses {
		return nil, fmt.Errorf("statprog: %d classes (the most is %d)", len(classes), MaxClasses)
	}
	if err := c.compileListMembers(classes); err != nil {
		return nil, err
	}
	if err := c.compileListSteps(classes); err != nil {
		return nil, err
	}
	if err := c.drain(); err != nil {
		return nil, err
	}
	return c, nil
}

func (c *compiler) compileListMembers(classes []ClassDef) error {
	b, g := c.b, c.g
	member := func(list string) ([]uint32, error) {
		items, ok := g.Lists[list]
		if !ok {
			return nil, fmt.Errorf("statprog: no initializer list %q", list)
		}
		var out []uint32
		seen := map[uint32]bool{}
		for _, it := range items {
			idx, err := c.stat(it.Stat)
			if err != nil {
				return nil, fmt.Errorf("list %s: %w", list, err)
			}
			if !seen[uint32(idx)] {
				seen[uint32(idx)] = true
				out = append(out, uint32(idx))
			}
		}
		return out, nil
	}
	for _, l := range CommonLists {
		m, err := member(l)
		if err != nil {
			return err
		}
		b.CommonMembers = appendUnique(b.CommonMembers, m)
	}
	for _, cl := range classes {
		var m []uint32
		for _, l := range cl.listNames() {
			ml, err := member(l)
			if err != nil {
				return err
			}
			m = appendUnique(m, ml)
		}
		b.Classes = append(b.Classes, Class{Enum: uint8(cl.Enum), Name: c.str(cl.Name), Members: m})
	}
	return nil
}

func (c *compiler) compileListSteps(classes []ClassDef) error {
	b, g := c.b, c.g
	steps := func(list string) ([]Step, error) {
		var out []Step
		for i, it := range g.Lists[list] {
			if strings.TrimSpace(it.Expr) == "" {
				continue
			}
			idx, _ := c.stat(it.Stat)
			p, _, err := c.program(it.Expr, unitInit, fmt.Sprintf("list %s entry %d (%s)", list, i, g.Stats[it.Stat].Name), nil)
			if err != nil {
				return nil, err
			}
			out = append(out, Step{Stat: uint32(idx), Prog: uint32(p)})
		}
		return out, nil
	}
	for _, l := range CommonLists {
		s, err := steps(l)
		if err != nil {
			return err
		}
		b.CommonSteps = append(b.CommonSteps, s...)
	}
	for i, cl := range classes {
		for _, l := range cl.listNames() {
			s, err := steps(l)
			if err != nil {
				return err
			}
			b.Classes[i].Steps = append(b.Classes[i].Steps, s...)
		}
	}
	return nil
}

func (c *compiler) drain() error {
	b, g := c.b, c.g
	for ; c.qpos < len(c.queue); c.qpos++ {
		idx := c.queue[c.qpos]
		sd := g.Stats[b.Stats[idx].Guid]
		for _, f := range []struct {
			text string
			kind unitKind
			what string
		}{
			{sd.Base, unitBase, "Base"},
			{sd.Min, unitLimit, "ValueMin"},
			{sd.Max, unitLimit, "ValueMax"},
			{sd.Regen, unitRegen, "RegenRate"},
		} {
			if strings.TrimSpace(f.text) == "" {
				continue
			}
			p, _, err := c.program(f.text, f.kind, fmt.Sprintf("%s of %s", f.what, sd.Name), nil)
			if err != nil {
				return err
			}
			b.Stats[idx].setProg(f.what, int32(p))
		}
	}
	return nil
}

// (Cyb + C.O.A)

type snapshot struct{ consts, strs, refs, curves, progs, stats, queue int }

func (c *compiler) snap() snapshot {
	return snapshot{len(c.b.Consts), len(c.b.Strings), len(c.b.Refs), len(c.b.Curves), len(c.b.Progs), len(c.b.Stats), len(c.queue)}
}

func (c *compiler) restore(s snapshot) {
	b := c.b
	b.Consts, b.Strings, b.Refs, b.Curves, b.Progs, b.Stats = b.Consts[:s.consts], b.Strings[:s.strs], b.Refs[:s.refs], b.Curves[:s.curves], b.Progs[:s.progs], b.Stats[:s.stats]
	c.queue = c.queue[:s.queue]
	if c.qpos > s.queue {
		c.qpos = s.queue
	}
	for k, i := range c.constIdx {
		if i >= s.consts {
			delete(c.constIdx, k)
		}
	}
	for k, i := range c.strIdx {
		if i >= s.strs {
			delete(c.strIdx, k)
		}
	}
	for k, i := range c.refIdx {
		if i >= s.refs {
			delete(c.refIdx, k)
		}
	}
	for k, i := range c.curveIdx {
		if i >= s.curves {
			delete(c.curveIdx, k)
		}
	}
	for k, i := range c.progIdx {
		if i >= s.progs {
			delete(c.progIdx, k)
		}
	}
	for k, i := range c.statIdx {
		if i >= s.stats {
			delete(c.statIdx, k)
		}
	}
}

func (c *compiler) expr(ex Expr) ExprResult {
	if strings.TrimSpace(ex.Text) == "" {
		return ExprResult{Program: -1, Err: ex.What + ": the expression is empty"}
	}
	s := c.snap()
	p, usesTarget, err := c.program(ex.Text, unitExpr, ex.What, ex.Terms)
	if err == nil {
		err = c.drain()
	}
	if err != nil {
		c.restore(s)
		return ExprResult{Program: -1, Err: err.Error()}
	}
	return ExprResult{Program: p, UsesTarget: usesTarget}
}

func (s *Stat) setProg(what string, p int32) {
	switch what {
	case "Base":
		s.Base = p
	case "ValueMin":
		s.Min = p
	case "ValueMax":
		s.Max = p
	case "RegenRate":
		s.Regen = p
	}
}

func appendUnique(dst, src []uint32) []uint32 {
	have := map[uint32]bool{}
	for _, x := range dst {
		have[x] = true
	}
	for _, x := range src {
		if !have[x] {
			have[x] = true
			dst = append(dst, x)
		}
	}
	return dst
}

func (c *compiler) stat(guid int64) (int, error) {
	if i, ok := c.statIdx[guid]; ok {
		return i, nil
	}
	sd, ok := c.g.Stats[guid]
	if !ok {
		return 0, fmt.Errorf("no stat %d", guid)
	}
	if len(c.b.Stats) >= MaxStats {
		return 0, fmt.Errorf("more than %d stats", MaxStats)
	}
	if sd.Replication < 0 || sd.Replication > 7 {
		return 0, fmt.Errorf("stat %s: replication %d", sd.Name, sd.Replication)
	}
	var fl uint8
	if sd.MultiplyMods {
		fl |= StatMulMods
	}
	if sd.ClampFinal {
		fl |= StatClampFinal
	}
	switch sd.OnSpawn {
	case "Reset_SetMax":
		fl |= StatSpawnMax
	case "Reset_SetMin":
		fl |= StatSpawnMin
	}
	if sd.ReplicatesBuckets {
		fl |= StatBuckets
	}
	if sd.MultiplyCompliment {
		fl |= StatMulCompl
	}
	if sd.StatType == "Type_FloatArray" {
		fl |= StatFloatArray
	}
	idx := len(c.b.Stats)
	c.b.Stats = append(c.b.Stats, Stat{
		Guid: guid, Name: c.str(sd.Name), Replication: uint8(sd.Replication), Flags: fl,
		Base: -1, Min: -1, Max: -1, Regen: -1,
	})
	c.statIdx[guid] = idx
	c.queue = append(c.queue, idx)
	return idx, nil
}

// [Cyb + C | O | A]

func (c *compiler) str(s string) uint32 {
	if i, ok := c.strIdx[s]; ok {
		return uint32(i)
	}
	i := len(c.b.Strings)
	c.b.Strings = append(c.b.Strings, s)
	c.strIdx[s] = i
	return uint32(i)
}

func (c *compiler) strOperand(s string) (uint16, error) {
	if len(s) > MaxStringLen {
		return 0, fmt.Errorf("a string of %d bytes (the most is %d)", len(s), MaxStringLen)
	}
	i := c.str(s)
	if i >= MaxStrings {
		return 0, fmt.Errorf("more than %d strings", MaxStrings)
	}
	return uint16(i), nil
}

func (c *compiler) ref(r aocexpr.Ref) (uint16, error) {
	k := Ref{r.TypeID, r.Guid}
	if i, ok := c.refIdx[k]; ok {
		return uint16(i), nil
	}
	if len(c.b.Refs) >= MaxRefs {
		return 0, fmt.Errorf("more than %d references", MaxRefs)
	}
	i := len(c.b.Refs)
	c.b.Refs = append(c.b.Refs, k)
	c.refIdx[k] = i
	return uint16(i), nil
}

func (c *compiler) constant(f float64) (uint16, error) {
	bits := math.Float64bits(f)
	if i, ok := c.constIdx[bits]; ok {
		return uint16(i), nil
	}
	if len(c.b.Consts) >= MaxConsts {
		return 0, fmt.Errorf("more than %d constants", MaxConsts)
	}
	i := len(c.b.Consts)
	c.b.Consts = append(c.b.Consts, f)
	c.constIdx[bits] = i
	return uint16(i), nil
}

// -- C-Y-B & C.O.A --

func (c *compiler) curve(guid int64) (int, error) {
	if i, ok := c.curveIdx[guid]; ok {
		return i, nil
	}
	cv, ok := c.g.Curves[guid]
	if !ok {
		return 0, fmt.Errorf("no curve %d", guid)
	}
	if len(cv.Keys) == 0 {
		return 0, fmt.Errorf("curve %d has no keys", guid)
	}
	if len(c.b.Curves) >= MaxCurves || len(cv.Keys) > MaxCurveKeys {
		return 0, fmt.Errorf("curve %d: too many curves or keys", guid)
	}
	out := Curve{Keys: make([]CurveKey, len(cv.Keys))}
	for i, k := range cv.Keys {
		ck := CurveKey{Time: k.Time, Value: k.Value}
		switch k.Interp {
		case aocexpr.InterpLinear:
			ck.Interp = InterpLinear
		case aocexpr.InterpConstant:
			ck.Interp = InterpConstant
		default:
			ck.Interp = InterpCubic
		}
		ck.Arrive, ck.Leave = k.Arrive, k.Leave
		out.Keys[i] = ck
	}
	i := len(c.b.Curves)
	c.b.Curves = append(c.b.Curves, out)
	c.curveIdx[guid] = i
	return i, nil
}

func (c *compiler) parse(text string) (*aocexpr.Program, error) {
	if p, ok := c.parsed[text]; ok {
		return p, nil
	}
	p, err := aocexpr.Parse(text)
	if err != nil {
		return nil, err
	}
	c.parsed[text] = p
	return p, nil
}

func (c *compiler) program(text string, kind unitKind, what string, terms map[int64]string) (int, bool, error) {
	p, err := c.parse(text)
	if err != nil {
		return 0, false, fmt.Errorf("%s: %w", what, err)
	}
	f := &fnc{c: c, kind: kind}
	if terms != nil {
		f.terms = []map[int64]string{terms}
		f.topTerms = true
	}
	f.pushScope(false)
	v, err := f.tree(p.Tree())
	if err != nil {
		return 0, false, fmt.Errorf("%s: %w", what, err)
	}
	if v.t != tNum && v.t != tBool {
		return 0, false, fmt.Errorf("%s: the formula's value is %v, not a number", what, v.t)
	}
	em := &emitter{c: c}
	em.node(v.n)
	em.op(OpRet)
	if em.err != nil {
		return 0, false, fmt.Errorf("%s: %w", what, em.err)
	}
	if len(em.code) > MaxCodeBytes {
		return 0, false, fmt.Errorf("%s: %d bytes of code (the most is %d)", what, len(em.code), MaxCodeBytes)
	}
	if f.nlocals > MaxLocals {
		return 0, false, fmt.Errorf("%s: %d local variables (the most is %d)", what, f.nlocals, MaxLocals)
	}
	if em.maxH > MaxStack {
		return 0, false, fmt.Errorf("%s: the stack gets %d deep (the most is %d)", what, em.maxH, MaxStack)
	}
	key := strconv.Itoa(f.nlocals) + ":" + string(em.code)
	if i, ok := c.progIdx[key]; ok {
		return i, f.usesTarget, nil
	}
	if len(c.b.Progs) >= MaxPrograms {
		return 0, false, fmt.Errorf("more than %d programs", MaxPrograms)
	}
	i := len(c.b.Progs)
	c.b.Progs = append(c.b.Progs, Program{Locals: uint16(f.nlocals), Code: em.code})
	c.progIdx[key] = i
	return i, f.usesTarget, nil
}
