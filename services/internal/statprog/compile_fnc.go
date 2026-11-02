// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"fmt"
	"math"

	"aocrop/services/internal/aocexpr"
)

type fnc struct {
	c          *compiler
	kind       unitKind
	scopes     []*scope
	nlocals    int
	inlining   []string
	terms      []map[int64]string
	topTerms   bool
	usesTarget bool
}

const maxInline = 16

func (f *fnc) pushScope(barrier bool) {
	f.scopes = append(f.scopes, &scope{vars: map[string]*binding{}, barrier: barrier})
}
func (f *fnc) popScope() { f.scopes = f.scopes[:len(f.scopes)-1] }

func (f *fnc) lookup(name string) *binding {
	for i := len(f.scopes) - 1; i >= 0; i-- {
		if b, ok := f.scopes[i].vars[name]; ok {
			return b
		}
		if f.scopes[i].barrier {
			break
		}
	}
	return nil
}

func (f *fnc) newSlot() int {
	s := f.nlocals
	f.nlocals++
	return s
}

func errAt(pos aocexpr.Pos, format string, args ...any) error {
	return fmt.Errorf("%v: %s", pos, fmt.Sprintf(format, args...))
}

func unsupportedAt(pos aocexpr.Pos, format string, args ...any) error {
	return fmt.Errorf("%v: unsupported %s", pos, fmt.Sprintf(format, args...))
}

const divisionConstruct = "division where no operand is proven to be a float (integer division is not modelled; write 2.0 or Float(x))"

func (f *fnc) tree(t aocexpr.Tree) (val, error) {
	var out []stmt
	for _, s := range t.Stmts {
		ss, err := f.stmt(s)
		if err != nil {
			return val{}, err
		}
		out = append(out, ss...)
	}
	r, err := f.expr(t.Result)
	if err != nil {
		return val{}, err
	}
	if len(out) > 0 && (r.t == tNum || r.t == tBool) {
		r.n = nBlock{stmts: out, result: r.n}
	}
	return r, nil
}

func (f *fnc) stmts(list []aocexpr.Stmt) ([]stmt, error) {
	f.pushScope(false)
	defer f.popScope()
	var out []stmt
	for _, s := range list {
		ss, err := f.stmt(s)
		if err != nil {
			return nil, err
		}
		out = append(out, ss...)
	}
	return out, nil
}

// -- C | Y | B . CoA --

func (f *fnc) stmt(s aocexpr.Stmt) ([]stmt, error) {
	switch s := s.(type) {
	case aocexpr.Decl:
		return f.declStmt(s)
	case aocexpr.Set:
		b := f.lookup(s.Name)
		if b == nil {
			return nil, errAt(s.At, "set of undeclared variable %q", s.Name)
		}
		if b.konst {
			return nil, errAt(s.At, "set of const %q", s.Name)
		}
		v, err := f.expr(s.X)
		if err != nil {
			return nil, err
		}
		if v.t != b.t {
			return nil, unsupportedAt(s.At, "set of %q (%v) to %v", s.Name, b.t, v.t)
		}
		b.float = b.float && v.float
		return []stmt{sStore{b.slot, v.n}}, nil
	case aocexpr.If:
		return f.ifStmt(s)
	case aocexpr.ExprStmt:
		v, err := f.expr(s.X)
		if err != nil {
			return nil, err
		}
		if _, isConst := v.constant(); (v.t == tNum || v.t == tBool) && !isConst {
			return []stmt{sDrop{v.n}}, nil
		}
		return nil, nil
	}
	return nil, fmt.Errorf("internal error: statement %T", s)
}

func (f *fnc) declStmt(s aocexpr.Decl) ([]stmt, error) {
	v, err := f.expr(s.X)
	if err != nil {
		return nil, err
	}
	sc := f.scopes[len(f.scopes)-1]
	if _, dup := sc.vars[s.Name]; dup {
		return nil, errAt(s.At, "%q is already declared in this block", s.Name)
	}
	switch v.t {
	case tNum, tBool:
		if _, isConst := v.constant(); isConst && s.Const {
			sc.vars[s.Name] = &binding{t: v.t, konst: true, v: v, float: v.float}
			return nil, nil
		}
		slot := f.newSlot()
		sc.vars[s.Name] = &binding{t: v.t, slot: slot, isSlot: true, float: v.float, etype: v.etype}
		return []stmt{sStore{slot, v.n}}, nil
	default:
		if !s.Const {
			return nil, unsupportedAt(s.At, "var %q holds %v (only numbers and booleans can change)", s.Name, v.t)
		}
		sc.vars[s.Name] = &binding{t: v.t, konst: true, v: v}
		return nil, nil
	}
}

func (f *fnc) ifStmt(s aocexpr.If) ([]stmt, error) {
	cv, err := f.expr(s.Cond)
	if err != nil {
		return nil, err
	}
	if cv.t != tBool {
		return nil, errAt(s.At, "the condition of an if is %v, not a boolean", cv.t)
	}
	if c, ok := cv.constant(); ok {
		if c != 0 {
			return f.stmts(s.Then)
		}
		return f.stmts(s.Else)
	}
	then, err := f.stmts(s.Then)
	if err != nil {
		return nil, err
	}
	els, err := f.stmts(s.Else)
	if err != nil {
		return nil, err
	}
	return []stmt{sIf{cond: cv.n, then: then, els: els}}, nil
}

func rnd(x float64) float64 { return float64(float32(x)) }

func (f *fnc) num(x float64) float64 {
	if f.c.single {
		return rnd(x)
	}
	return x
}

func b2f(b bool) float64 {
	if b {
		return 1
	}
	return 0
}

func (f *fnc) expr(e aocexpr.Expr) (val, error) {
	switch e := e.(type) {
	case aocexpr.Num:
		n := f.num(e.V)
		if math.IsInf(n, 0) {
			return val{}, errAt(e.At, "the number %v does not fit a float32 (single arithmetic)", e.V)
		}
		return val{t: tNum, n: nConst{n}, float: e.Float}, nil
	case aocexpr.Bool:
		return val{t: tBool, n: nConst{b2f(e.V)}}, nil
	case aocexpr.RefLit:
		return val{t: tRef, ref: e.Ref}, nil
	case aocexpr.Str:
		return val{t: tStr, str: e.V}, nil
	case aocexpr.EnumLit:
		return val{t: tEnum, enum: e.V}, nil
	case aocexpr.Var:
		b := f.lookup(e.Name)
		if b == nil {
			return val{}, errAt(e.At, "unknown variable %q", e.Name)
		}
		if b.konst {
			return b.v, nil
		}
		return val{t: b.t, n: nLoad{b.slot}, float: b.float, etype: b.etype}, nil
	case aocexpr.Unary:
		x, err := f.expr(e.X)
		if err != nil {
			return val{}, err
		}
		if e.Op == "-" {
			if x.t != tNum {
				return val{}, errAt(e.At, "unary - needs a number, got %v", x.t)
			}
			if c, ok := x.constant(); ok {
				return val{t: tNum, n: nConst{f.num(-c)}, float: x.float}, nil
			}
			return val{t: tNum, n: nUn{OpNeg, x.n}, float: x.float}, nil
		}
		if x.t != tBool {
			return val{}, errAt(e.At, "! needs a boolean, got %v", x.t)
		}
		if c, ok := x.constant(); ok {
			return val{t: tBool, n: nConst{b2f(c == 0)}}, nil
		}
		return val{t: tBool, n: nUn{OpNot, x.n}}, nil
	case aocexpr.Binary:
		return f.binary(e)
	case aocexpr.Call:
		return f.call(e)
	}
	return val{}, fmt.Errorf("internal error: expression %T", e)
}

// (C | O | A) (Cyb)

var binOps = map[string]byte{
	"+": OpAdd, "-": OpSub, "*": OpMul, "/": OpDiv, "%": OpMod,
	"<": OpLt, "<=": OpLe, ">": OpGt, ">=": OpGe, "==": OpEq, "!=": OpNe,
}

func (f *fnc) binary(e aocexpr.Binary) (val, error) {
	l, err := f.expr(e.L)
	if err != nil {
		return val{}, err
	}
	if e.Op == "&&" || e.Op == "||" {
		return f.logicalBinary(e, l)
	}
	r, err := f.expr(e.R)
	if err != nil {
		return val{}, err
	}
	op := binOps[e.Op]
	l, r = f.loose(l), f.loose(r)
	if l, r, err = f.enumPair(e, l, r); err != nil {
		return val{}, err
	}
	if op == OpEq || op == OpNe {
		return f.equality(e, op, l, r)
	}
	if l.t != tNum || r.t != tNum {
		return val{}, errAt(e.At, "%s needs numbers, got %v and %v", e.Op, l.t, r.t)
	}
	return f.numericBinary(e, op, l, r)
}

func (f *fnc) logicalBinary(e aocexpr.Binary, l val) (val, error) {
	r, err := f.expr(e.R)
	if err != nil {
		return val{}, err
	}
	if l.t != tBool || r.t != tBool {
		return val{}, errAt(e.At, "%s needs booleans, got %v and %v", e.Op, l.t, r.t)
	}
	if c, ok := l.constant(); ok {
		if (e.Op == "&&") == (c != 0) {
			return r, nil
		}
		return val{t: tBool, n: nConst{b2f(e.Op == "||")}}, nil
	}
	if e.Op == "&&" {
		return val{t: tBool, n: nAnd{l.n, r.n}}, nil
	}
	return val{t: tBool, n: nOr{l.n, r.n}}, nil
}

func (f *fnc) numericBinary(e aocexpr.Binary, op byte, l, r val) (val, error) {
	lc, lok := l.constant()
	rc, rok := r.constant()
	switch op {
	case OpMod:
		if lok && rok && int64(rc) != 0 {
			return val{t: tNum, n: nConst{float64(int64(lc) % int64(rc))}}, nil
		}
		return val{t: tNum, n: nBin{op, l.n, r.n}}, nil
	case OpAdd, OpSub, OpMul, OpDiv:
		fl := l.float || r.float
		if op == OpDiv && !fl {
			return val{}, unsupportedAt(e.At, "%s", divisionConstruct)
		}
		if lok && rok && !(op == OpDiv && rc == 0) {
			if res := arith(op, lc, rc, f.c.single); !math.IsInf(res, 0) && !math.IsNaN(res) {
				return val{t: tNum, n: nConst{res}, float: fl}, nil
			}
		}
		return val{t: tNum, n: nBin{op, l.n, r.n}, float: fl}, nil
	default:
		if lok && rok {
			var res bool
			switch op {
			case OpLt:
				res = lc < rc
			case OpLe:
				res = lc <= rc
			case OpGt:
				res = lc > rc
			case OpGe:
				res = lc >= rc
			}
			return val{t: tBool, n: nConst{b2f(res)}}, nil
		}
		return val{t: tBool, n: nBin{op, l.n, r.n}}, nil
	}
}

func (f *fnc) enumPair(e aocexpr.Binary, l, r val) (val, val, error) {
	conv := func(num, lit val, litPos aocexpr.Pos) (val, error) {
		switch {
		case lit.t == tEnum && num.etype != "" && num.etype != etypeNode && lit.enum.Type == num.etype:
			ord, ok := aocexpr.EnumOrdinal(lit.enum.Type, lit.enum.Value)
			if !ok {
				return lit, errAt(litPos, "%v is not a value of %s", lit.enum, lit.enum.Type)
			}
			return val{t: tNum, n: nConst{float64(ord)}, etype: num.etype}, nil
		case lit.t == tRef && num.etype == etypeNode:
			rank, err := f.c.g.nodeRank(lit.ref)
			if err != nil {
				return lit, errAt(litPos, "%v", err)
			}
			return val{t: tNum, n: nConst{float64(rank)}, etype: etypeNode}, nil
		}
		return lit, nil
	}
	var err error
	if l.t == tNum && l.etype != "" {
		if r, err = conv(l, r, e.R.Pos()); err != nil {
			return l, r, err
		}
	}
	if r.t == tNum && r.etype != "" {
		if l, err = conv(r, l, e.L.Pos()); err != nil {
			return l, r, err
		}
	}
	return l, r, nil
}

func (f *fnc) minMaxVal(op byte, a, b val) val {
	fl := a.float || b.float
	ac, aok := a.constant()
	bc, bok := b.constant()
	if aok && bok {
		return val{t: tNum, n: nConst{minmax(op, ac, bc)}, float: fl}
	}
	return val{t: tNum, n: nFn2{op, a.n, b.n}, float: fl}
}

func (f *fnc) loose(v val) val {
	if v.t == tBool && f.kind == unitExpr {
		v.t = tNum
		v.float = false
	}
	return v
}

// C O A x Cyb

func (f *fnc) equality(e aocexpr.Binary, op byte, l, r val) (val, error) {
	want := op == OpEq
	switch {
	case l.t == r.t && (l.t == tNum || l.t == tBool):
		lc, lok := l.constant()
		rc, rok := r.constant()
		if lok && rok {
			return val{t: tBool, n: nConst{b2f((lc == rc) == want)}}, nil
		}
		return val{t: tBool, n: nBin{op, l.n, r.n}}, nil
	case l.t == tStr && r.t == tStr:
		return val{t: tBool, n: nConst{b2f((l.str == r.str) == want)}}, nil
	case l.t == tEnum && r.t == tEnum:
		return val{t: tBool, n: nConst{b2f((l.enum == r.enum) == want)}}, nil
	case l.t == tRef && r.t == tRef:
		if v, ok, err := f.markerEquality(e, l, r, want); ok {
			return v, err
		}
		return val{t: tBool, n: nConst{b2f((l.ref == r.ref) == want)}}, nil
	case (l.t == tObj && r.t == tRef) || (l.t == tRef && r.t == tObj):
		if v, ok, err := f.hitRecordEquality(e, l, r, want); ok {
			return v, err
		}
	case l.t == tObj && r.t == tObj:
		if l.role == r.role {
			return val{t: tBool, n: nConst{b2f(want)}}, nil
		}
		if f.kind != unitExpr {
			return val{}, errAt(e.At, "two actors in a stat formula")
		}
		same, err := f.hostCall(aocexpr.Call{Name: "==", At: e.At}, aocexpr.HostSameActor, aocexpr.RolePair(l.role, r.role), tBool)
		if err != nil {
			return val{}, err
		}
		if !want {
			same = val{t: tBool, n: nUn{OpNot, same.n}}
		}
		return same, nil
	}
	return val{}, errAt(e.At, "cannot compare %v with %v", l.t, r.t)
}

func arith(op byte, a, b float64, single bool) float64 {
	var r float64
	switch op {
	case OpAdd:
		r = a + b
	case OpSub:
		r = a - b
	case OpMul:
		r = a * b
	case OpDiv:
		r = a / b
	}
	if single {
		r = float64(float32(r))
	}
	return r
}

func argc(e aocexpr.Call, n int) error {
	if len(e.Args) != n {
		return errAt(e.At, "%s takes %d argument(s), got %d", e.Name, n, len(e.Args))
	}
	return nil
}
