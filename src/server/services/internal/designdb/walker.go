// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package designdb

import (
	"encoding/binary"
	"fmt"
	"io"
	"math"
	"strconv"
	"strings"
	"unicode/utf8"

	"aocrop/services/internal/usmap"
)

type failure struct {
	off int
	msg string
}

type pathElem struct {
	name string
	idx  int
}

type walker struct {
	d         *Decoder
	db        *DB
	data      []byte
	o         int
	end       int
	build     bool
	strs      map[uint32]string
	path      []pathElem
	ids       map[string]uint64
	trace     io.Writer
	traceFrom int
	row       int
}

func (d *Decoder) newWalker(t TableInfo, opt walkOpts) *walker {
	w := &walker{
		d: d, db: d.db, data: d.db.data,
		o: int(t.Offset), end: int(t.End()),
		build: opt.build, ids: opt.ids,
		trace: opt.trace, traceFrom: opt.traceFrom,
	}
	if opt.build {
		w.strs = map[uint32]string{}
	}
	if w.ids == nil {
		w.ids = map[string]uint64{}
	}
	return w
}

func (w *walker) failTableEnd(rows uint32) {
	diff := w.end - w.o
	how := fmt.Sprintf("%d bytes short of", diff)
	if diff < 0 {
		how = fmt.Sprintf("%d bytes past", -diff)
	}
	panic(&failure{off: w.o, msg: fmt.Sprintf("the %d rows end at %#x, %s the table end %#x", rows, w.o, how, w.end)})
}

func (w *walker) tracing() bool { return w.trace != nil && w.row >= w.traceFrom }

func (w *walker) traced(at int, what string, v Value) {
	s := fmt.Sprintf("%v", v)
	if len(s) > 80 {
		s = s[:77] + "..."
	}
	fmt.Fprintf(w.trace, "%#x row %d %s: %s = %s\n", at, w.row, w.pathString(), what, s)
}

func (w *walker) fail(format string, args ...any) {
	panic(&failure{off: w.o, msg: fmt.Sprintf(format, args...)})
}

func (w *walker) need(n int) {
	if n < 0 || w.o+n > w.end {
		w.fail("a %d-byte read runs past the end of the table (%#x)", n, w.end)
	}
}

func (w *walker) u8() uint8 {
	w.need(1)
	v := w.data[w.o]
	w.o++
	return v
}

func (w *walker) u32() uint32 {
	w.need(4)
	v := binary.LittleEndian.Uint32(w.data[w.o:])
	w.o += 4
	return v
}

func (w *walker) u64() uint64 {
	w.need(8)
	v := binary.LittleEndian.Uint64(w.data[w.o:])
	w.o += 8
	return v
}

func (w *walker) u16() uint16 {
	w.need(2)
	v := binary.LittleEndian.Uint16(w.data[w.o:])
	w.o += 2
	return v
}

func (w *walker) pool() string {
	at := w.o
	ref := w.u32()
	if !w.db.validPoolRef(ref) {
		w.o = at
		w.fail("pool reference %#x does not point at a string", ref)
	}
	if !w.build {
		return ""
	}
	if s, ok := w.strs[ref]; ok {
		return s
	}
	s, _ := w.db.poolString(ref)
	if !utf8.ValidString(s) {
		s = strings.ToValidUTF8(s, "\uFFFD")
	}
	w.strs[ref] = s
	return s
}

func (w *walker) fstring() string { return w.fstringIf(w.build) }

func (w *walker) fstringIf(want bool) string {
	at := w.o
	n := int32(w.u32())
	if n == 0 {
		return ""
	}
	if n < 0 {
		w.o = at
		w.fail("FString with negative length %d (UTF-16 is not supported)", n)
	}
	w.need(int(n))
	b := w.data[w.o : w.o+int(n)]
	if b[n-1] != 0 {
		w.o = at
		w.fail("FString of %d bytes is not NUL-terminated", n)
	}
	w.o += int(n)
	if !want {
		return ""
	}
	s := string(b[:n-1])
	if !utf8.ValidString(s) {
		s = strings.ToValidUTF8(s, "\uFFFD")
	}
	return s
}

func (w *walker) instanced() Value {
	path := w.fstringIf(true)
	at := w.o
	size := int32(w.u32())
	if size < 0 {
		w.o = at
		w.fail("InstancedStruct size %d", size)
	}
	if path == "" {
		if size != 0 {
			w.o = at
			w.fail("InstancedStruct of no struct with %d bytes of data", size)
		}
		return nil
	}
	name := path[strings.LastIndexAny(path, "./:")+1:]
	if w.d.m.Schema(name) == nil {
		w.o = at
		w.fail("InstancedStruct of %s: the mappings have no struct %s", path, name)
	}
	start := w.o
	w.push("(" + name + ")")
	v := w.readStruct(name)
	w.pop()
	if got := w.o - start; got != int(size) {
		w.o = start
		w.fail("InstancedStruct of %s: its properties take %d bytes, the size says %d", path, got, size)
	}
	if !w.build {
		return nil
	}
	return Struct{"Type": path, "Value": v}
}

func (w *walker) text() Value {
	w.u32()
	switch h := int8(w.u8()); h {
	case 0:
		ns, key, src := w.fstring(), w.fstring(), w.fstring()
		return Text{Namespace: ns, Key: key, Source: src}
	case 11:
		table := w.pool()
		return Text{History: 11, Table: table, Key: w.fstring()}
	case -1:
		t := Text{History: -1}
		if w.u32() != 0 {
			t.Source = w.fstring()
		}
		return t
	default:
		w.o--
		w.fail("FText history type %d is not supported", h)
		return nil
	}
}

func (w *walker) removed(key *usmap.Type) {
	n := w.count(w.d.minSize(key))
	for i := 0; i < n; i++ {
		w.pushIdx(i)
		w.readValue(key, false)
		w.pop()
	}
}

func (w *walker) count(elemSize int) int {
	at := w.o
	n := w.u32()
	if need := uint64(n) * uint64(elemSize); need > uint64(w.end-w.o) {
		left := w.end - w.o
		w.o = at
		w.fail("count %d needs at least %d bytes, %d are left", n, need, left)
	}
	if w.tracing() {
		w.traced(at, "count", int64(n))
	}
	return int(n)
}

func (w *walker) push(name string)   { w.path = append(w.path, pathElem{name: name, idx: -1}) }
func (w *walker) pushIdx(i int)      { w.path = append(w.path, pathElem{idx: i}) }
func (w *walker) pop()               { w.path = w.path[:len(w.path)-1] }
func (w *walker) pathString() string { return formatPath(w.path) }

func formatPath(p []pathElem) string {
	var b strings.Builder
	for _, e := range p {
		if e.idx >= 0 {
			b.WriteString("[" + strconv.Itoa(e.idx) + "]")
			continue
		}
		if b.Len() > 0 {
			b.WriteByte('.')
		}
		b.WriteString(e.name)
	}
	return b.String()
}

func (w *walker) readValue(t *usmap.Type, byteEnum bool) Value {
	switch t.Kind {
	case usmap.KindBool:
		b := w.u8()
		if b > 1 {
			w.o--
			w.fail("bool byte %d", b)
		}
		return b == 1
	case usmap.KindByte:
		return int64(w.u8())
	case usmap.KindInt8:
		return int64(int8(w.u8()))
	case usmap.KindInt16:
		return int64(int16(w.u16()))
	case usmap.KindUInt16:
		return int64(w.u16())
	case usmap.KindInt:
		return int64(int32(w.u32()))
	case usmap.KindUInt32:
		return int64(w.u32())
	case usmap.KindInt64:
		return int64(w.u64())
	case usmap.KindUInt64:
		return w.u64()
	case usmap.KindFloat:
		return math.Float32frombits(w.u32())
	case usmap.KindDouble:
		return math.Float64frombits(w.u64())
	case usmap.KindName, usmap.KindStr, usmap.KindUnknown, usmap.KindSoftObject, usmap.KindObject:
		return w.pool()
	case usmap.KindEnum:
		return w.readEnumValue(t, byteEnum)
	case usmap.KindText:
		return w.text()
	default:
		return w.readContainerValue(t)
	}
}

func (w *walker) readEnumValue(t *usmap.Type, byteEnum bool) Value {
	if !byteEnum {
		return w.pool()
	}
	v := int64(w.u8())
	if e := w.d.m.Enum(t.Enum); e != nil {
		if name, ok := e.ValueName(v); ok {
			return t.Enum + "::" + name
		}
	}
	return v
}

func (w *walker) readContainerValue(t *usmap.Type) Value {
	switch t.Kind {
	case usmap.KindOptional:
		at := w.o
		switch set := w.u32(); set {
		case 0:
			return nil
		case 1:
			return w.readValue(t.Inner, false)
		default:
			w.o = at
			w.fail("optional set flag %d", set)
		}
	case usmap.KindArray, usmap.KindSet:
		if t.Kind == usmap.KindSet {
			w.removed(t.Inner)
		}
		n := w.count(w.d.minSize(t.Inner))
		var out []Value
		if w.build {
			out = make([]Value, 0, n)
		}
		for i := 0; i < n; i++ {
			w.pushIdx(i)
			v := w.readValue(t.Inner, false)
			w.pop()
			if w.build {
				out = append(out, v)
			}
		}
		return out
	case usmap.KindMap:
		return w.readMapValue(t)
	case usmap.KindStruct:
		return w.readStruct(t.Struct)
	}
	w.fail("property type %s is not supported", t.Kind)
	return nil
}

func (w *walker) readMapValue(t *usmap.Type) Value {
	w.removed(t.Inner)
	n := w.count(w.d.minSize(t.Inner) + w.d.minSize(t.Value))
	var out Pairs
	if w.build {
		out = make(Pairs, 0, n)
	}
	for i := 0; i < n; i++ {
		w.pushIdx(i)
		w.push("key")
		k := w.readValue(t.Inner, false)
		w.pop()
		w.push("value")
		v := w.readValue(t.Value, false)
		w.pop()
		w.pop()
		if w.build {
			out = append(out, Pair{k, v})
		}
	}
	return out
}

func (w *walker) readStruct(name string) Value {
	switch name {
	case "AoCExpression", "SoftObjectPath", "SoftClassPath":
		return w.pool()
	case instancedStruct:
		return w.instanced()
	case "GameplayTagContainer":
		n := w.count(4)
		var out []Value
		if w.build {
			out = make([]Value, 0, n)
		}
		for i := 0; i < n; i++ {
			w.pushIdx(i)
			s := w.pool()
			w.pop()
			if w.build {
				out = append(out, s)
			}
		}
		return out
	}
	p, err := w.d.plan(name, false)
	if err != nil {
		w.fail("%v", err)
	}
	return w.readPlan(p)
}

func (w *walker) readPlan(p *plan) Value {
	var out Struct
	if w.build {
		out = make(Struct, len(p.steps))
	}
	inherits := false
	for _, s := range p.steps {
		w.push(s.name)
		at := w.o
		var v Value
		switch {
		case p.isID && s.name == "TypeId" && s.t.Kind == usmap.KindInt64:
			tid := w.u64()
			if tid != 0 {
				w.ids[p.name] = tid
			}
			v = int64(tid)
		case p.record && s.name == "ParentGuid" && s.t.Kind == usmap.KindInt64:
			pg := w.u64()
			inherits = pg != 0
			v = int64(pg)
		default:
			v = w.readValue(s.t, s.byteEnum)
		}
		if w.tracing() && s.t.Kind != usmap.KindStruct {
			w.traced(at, s.t.Kind.String(), v)
		}
		w.pop()
		if w.build {
			out[s.name] = v
		}
	}
	if inherits {
		at := w.o
		if tail := w.u32(); tail != 0 {
			w.o = at
			w.fail("the inherited row's tail is %#x; only 0 is known", tail)
		}
		if w.tracing() {
			w.traced(at, "inherited row tail", int64(0))
		}
	}
	if !w.build {
		return nil
	}
	return out
}
