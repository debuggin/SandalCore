// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"encoding/binary"
	"fmt"
	"math"
	"sort"
	"strings"
)

type emitter struct {
	c    *compiler
	code []byte
	h    int
	maxH int
	err  error
}

func (e *emitter) fail(err error) {
	if e.err == nil {
		e.err = err
	}
}

func (e *emitter) push(n int) {
	e.h += n
	if e.h > e.maxH {
		e.maxH = e.h
	}
}

func (e *emitter) op(op byte, operands ...uint16) {
	if e.err != nil {
		return
	}
	e.code = append(e.code, op)
	switch operandBytes(op) {
	case 1:
		e.code = append(e.code, byte(operands[0]))
	case 2, 4:
		for _, o := range operands {
			e.code = binary.LittleEndian.AppendUint16(e.code, o)
		}
	}
	pop, push := stackEffect(op)
	if e.h < pop {
		e.fail(fmt.Errorf("internal error: stack underflow emitting %#x", op))
		return
	}
	e.h -= pop
	e.push(push)
}

func (e *emitter) jump(op byte) int {
	e.op(op, 0)
	return len(e.code) - 2
}

func (e *emitter) patch(at int) {
	if len(e.code) > math.MaxUint16 {
		e.fail(fmt.Errorf("code too long to jump over"))
		return
	}
	binary.LittleEndian.PutUint16(e.code[at:], uint16(len(e.code)))
}

// ~ C-O-A & C:Y:B ~

func (e *emitter) node(n node) {
	if e.err != nil {
		return
	}
	if e.emitLeafNode(n) || e.emitLogicalNode(n) || e.emitOpNode(n) {
		return
	}
	e.fail(fmt.Errorf("internal error: node %T", n))
}

func (e *emitter) emitLeafNode(n node) bool {
	switch n := n.(type) {
	case nConst:
		i, err := e.c.constant(n.f)
		if err != nil {
			e.fail(err)
			return true
		}
		e.op(OpConst, i)
	case nLoad:
		e.op(OpLoad, uint16(n.slot))
	case nStat:
		e.op(OpStat, uint16(n.idx))
	case nTotal:
		e.op(OpTotal)
	case nLevel:
		e.op(OpLevel)
	case nCtx:
		switch operandBytes(n.op) {
		case 0:
			e.op(n.op)
		case 1, 2:
			e.op(n.op, n.imm[0])
		default:
			e.op(n.op, n.imm[0], n.imm[1])
		}
	default:
		return false
	}
	return true
}

func (e *emitter) emitLogicalNode(n node) bool {
	switch n := n.(type) {
	case nAnd:
		e.node(n.l)
		jf := e.jump(OpJz)
		e.node(n.r)
		je := e.jump(OpJmp)
		e.patch(jf)
		e.h--
		i, err := e.c.constant(0)
		if err != nil {
			e.fail(err)
			return true
		}
		e.op(OpConst, i)
		e.patch(je)
	case nOr:
		e.node(n.l)
		jt := e.jump(OpJnz)
		e.node(n.r)
		je := e.jump(OpJmp)
		e.patch(jt)
		e.h--
		i, err := e.c.constant(1)
		if err != nil {
			e.fail(err)
			return true
		}
		e.op(OpConst, i)
		e.patch(je)
	case nSel:
		e.node(n.c)
		e.node(n.a)
		e.node(n.b)
		e.op(OpSel)
	case nBlock:
		for _, s := range n.stmts {
			e.stmt(s)
		}
		e.node(n.result)
	default:
		return false
	}
	return true
}

func (e *emitter) emitOpNode(n node) bool {
	switch n := n.(type) {
	case nUn:
		e.node(n.x)
		e.op(n.op)
	case nBin:
		e.node(n.l)
		e.node(n.r)
		e.op(n.op)
	case nFn1:
		e.node(n.x)
		e.op(n.op)
	case nFn2:
		e.node(n.a)
		e.node(n.b)
		e.op(n.op)
	case nFn3:
		e.node(n.a)
		e.node(n.b)
		e.node(n.c)
		e.op(n.op)
	case nCurve:
		e.node(n.x)
		e.op(OpCurve, uint16(n.idx))
	case nHost:
		for _, a := range n.args {
			e.node(a)
		}
		e.op(n.op, n.fn, n.str)
	case nActor:
		e.op(OpActor, uint16(n.role))
		e.node(n.x)
		e.op(OpActor, 0)
	default:
		return false
	}
	return true
}

func (e *emitter) stmt(s stmt) {
	switch s := s.(type) {
	case sStore:
		e.node(s.x)
		e.op(OpStore, uint16(s.slot))
	case sDrop:
		e.node(s.x)
		e.op(OpPop)
	case sIf:
		e.node(s.cond)
		jz := e.jump(OpJz)
		for _, t := range s.then {
			e.stmt(t)
		}
		if len(s.els) == 0 {
			e.patch(jz)
			return
		}
		je := e.jump(OpJmp)
		e.patch(jz)
		for _, t := range s.els {
			e.stmt(t)
		}
		e.patch(je)
	}
}

// (C O A) (Cyb)

func (c *compiler) buildStatDeps() ([][]uint16, [][]uint16, error) {
	b := c.b
	ns := len(b.Stats)
	infos := make([]progInfo, len(b.Progs))
	for i := range b.Progs {
		inf, err := b.analyze(i)
		if err != nil {
			return nil, nil, fmt.Errorf("internal error: %w", err)
		}
		infos[i] = inf
	}
	initDeps := make([][]uint16, ns)
	for _, s := range b.CommonSteps {
		initDeps[s.Stat] = append(initDeps[s.Stat], infos[s.Prog].stats...)
		if infos[s.Prog].total && b.Stats[s.Stat].Base >= 0 {
			initDeps[s.Stat] = append(initDeps[s.Stat], infos[b.Stats[s.Stat].Base].stats...)
		}
	}
	for _, cl := range b.Classes {
		for _, s := range cl.Steps {
			initDeps[s.Stat] = append(initDeps[s.Stat], infos[s.Prog].stats...)
			if infos[s.Prog].total && b.Stats[s.Stat].Base >= 0 {
				initDeps[s.Stat] = append(initDeps[s.Stat], infos[b.Stats[s.Stat].Base].stats...)
			}
		}
	}
	valueDeps := make([][]uint16, ns)
	for i, s := range b.Stats {
		for _, p := range []int32{s.Base, s.Min, s.Max} {
			if p >= 0 {
				valueDeps[i] = append(valueDeps[i], infos[p].stats...)
			}
		}
	}
	return initDeps, valueDeps, nil
}

// order computes the evaluation order: a depth-first walk of what each node reads, in stat order.
func (c *compiler) order() error {
	initDeps, valueDeps, err := c.buildStatDeps()
	if err != nil {
		return err
	}
	b := c.b
	ns := len(b.Stats)
	state := make([]uint8, 2*ns)
	var stack []string
	var visit func(node int) error
	visit = func(node int) error {
		switch state[node] {
		case 2:
			return nil
		case 1:
			return fmt.Errorf("statprog: the stat formulas depend on themselves: %s -> %s", strings.Join(stack, " -> "), b.Strings[b.Stats[node>>1].Name])
		}
		state[node] = 1
		stack = append(stack, b.Strings[b.Stats[node>>1].Name])
		s := node >> 1
		var deps []uint16
		if node&1 == NodeInit {
			deps = initDeps[s]
		} else {
			if err := visit(2*s + NodeInit); err != nil {
				return err
			}
			deps = valueDeps[s]
		}
		sorted := append([]uint16(nil), deps...)
		sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
		for _, t := range sorted {
			if err := visit(2*int(t) + NodeValue); err != nil {
				return err
			}
		}
		stack = stack[:len(stack)-1]
		state[node] = 2
		b.Order = append(b.Order, uint32(node))
		return nil
	}
	for s := 0; s < ns; s++ {
		if err := visit(2*s + NodeValue); err != nil {
			return err
		}
	}
	return nil
}
