// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package designdb

import (
	"fmt"
	"io"
	"sync"

	"aocrop/services/internal/usmap"
)

// Value is a decoded property value.
type Value = any

// Struct is a decoded struct mapping property name to value.
type Struct map[string]Value

// Text is a decoded FText.
type Text struct {
	History   int8
	Namespace string
	Key       string
	Source    string
	Table     string
}

// Pairs is a decoded Map, entries in file order.
type Pairs []Pair

// Pair is one entry of a Map.
type Pair struct {
	Key   Value
	Value Value
}

// Row is a decoded record.
type Row struct {
	Offset     int64
	End        int64
	Guid       int64
	ParentGuid int64
	Name       string
	DevOnly    bool
	Fields     Struct
}

// DecodeError reports where a table's walk failed.
type DecodeError struct {
	Table     int
	Schema    string
	Row       int
	RowOffset int64
	Offset    int64
	Path      string
	Msg       string
}

func (e *DecodeError) Error() string {
	s := fmt.Sprintf("designdb: table %d as %s: row %d (starts at %#x), byte %#x", e.Table, e.Schema, e.Row, e.RowOffset, e.Offset)
	if e.Path != "" {
		s += ", " + e.Path
	}
	return s + ": " + e.Msg
}

// Decoder walks tables with the record structs of a mappings file.
type Decoder struct {
	db       *DB
	m        *usmap.Map
	results  []PatchResult
	known    map[uint64]string
	facts    *Facts
	children map[string][]string

	mu    sync.Mutex
	plans map[planKey]*plan
	sizes map[string]int
}

// Facts holds property flags, struct alignment, and struct serializers read from the executable.
type Facts struct {
	NotStored       map[string]bool
	RowAlign        map[string]int
	NativeSerialize map[string]bool
}

// ExeFacts returns the facts read from the client executable (exe_gen.go).
func ExeFacts() *Facts {
	return &Facts{NotStored: exeNotStored, RowAlign: exeRowAlign, NativeSerialize: exeNativeSerialize}
}

// Options configures a Decoder besides the database and mappings.
type Options struct {
	Patches    []Patch
	KnownTypes map[uint64]string
	Facts      *Facts
}

// Decoder returns a decoder for db's tables using standard Patches, KnownTypes, and ExeFacts.
func (db *DB) Decoder(m *usmap.Map) *Decoder {
	return db.DecoderWith(m, Options{Patches: Patches(), KnownTypes: KnownTypes()})
}

// DecoderWith returns a Decoder configured with explicit options.
func (db *DB) DecoderWith(m *usmap.Map, opt Options) *Decoder {
	facts := opt.Facts
	if facts == nil {
		facts = ExeFacts()
	}
	pm, results := applyPatches(m, opt.Patches)
	known := make(map[uint64]string, len(opt.KnownTypes))
	for id, s := range opt.KnownTypes {
		known[id] = s
	}
	children := map[string][]string{}
	for _, sc := range pm.Schemas {
		if sc.Super != "" {
			children[sc.Super] = append(children[sc.Super], sc.Name)
		}
	}
	return &Decoder{
		db: db, m: pm, results: results, known: known, facts: facts, children: children,
		plans: map[planKey]*plan{}, sizes: map[string]int{},
	}
}

// Applied lists the patches that changed a struct of the mappings.
func (d *Decoder) Applied() []Patch {
	var out []Patch
	for _, r := range d.results {
		if r.Status == PatchApplied {
			out = append(out, r.Patch)
		}
	}
	return out
}

// PatchResults returns what became of every patch in the order given.
func (d *Decoder) PatchResults() []PatchResult { return append([]PatchResult(nil), d.results...) }

// CheckPatches verifies that every patch either applied or found its property already present.
func (d *Decoder) CheckPatches() error {
	for _, r := range d.results {
		if r.Status == PatchNoStruct || r.Status == PatchNoAnchor {
			return fmt.Errorf("designdb: mappings patch %s: %s (%s)", r.Patch, r.Status, r.Patch.Why)
		}
	}
	return nil
}

const (
	recordRoot      = "DesignDataRecordBase"
	idRoot          = "DesignDataObjectId"
	instancedStruct = "InstancedStruct"
)

type planKey struct {
	name   string
	record bool
}

type step struct {
	name     string
	t        *usmap.Type
	byteEnum bool
}

type plan struct {
	name   string
	steps  []step
	record bool
	isID   bool
}

func (d *Decoder) leaves(name string) ([]*usmap.Schema, error) {
	var out []*usmap.Schema
	for n := name; ; {
		kids := d.children[n]
		if len(kids) == 0 {
			break
		}
		if len(kids) > 1 {
			return nil, fmt.Errorf("%s has %d derived structs (%s, ...): which one the rows are is not known", n, len(kids), kids[0])
		}
		n = kids[0]
		out = append([]*usmap.Schema{d.m.Schema(n)}, out...)
		if len(out) > 64 {
			return nil, fmt.Errorf("the structs derived from %s nest deeper than 64", name)
		}
	}
	return out, nil
}

func (d *Decoder) plan(name string, record bool) (*plan, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	key := planKey{name, record}
	if p := d.plans[key]; p != nil {
		return p, nil
	}
	chain, err := d.m.Chain(name)
	if err != nil {
		return nil, err
	}
	if record {
		leaves, err := d.leaves(name)
		if err != nil {
			return nil, err
		}
		chain = append(leaves, chain...)
	}
	p := &plan{name: name, record: record}
	seen := map[string]string{}
	for _, sc := range chain {
		for _, pr := range sc.Props {
			if d.facts.NotStored[sc.Name+"."+pr.Name] {
				continue
			}
			if pr.ArrayDim != 1 {
				return nil, fmt.Errorf("%s.%s is a fixed array of %d (not supported)", sc.Name, pr.Name, pr.ArrayDim)
			}
			if from, dup := seen[pr.Name]; dup {
				return nil, fmt.Errorf("%s.%s repeats %s.%s", sc.Name, pr.Name, from, pr.Name)
			}
			seen[pr.Name] = sc.Name
			p.steps = append(p.steps, step{name: pr.Name, t: pr.Type, byteEnum: d.facts.NativeSerialize[sc.Name]})
		}
	}
	root := chain[len(chain)-1].Name
	p.isID = root == idRoot && name != idRoot
	d.plans[key] = p
	return p, nil
}

func (d *Decoder) minSize(t *usmap.Type) int {
	switch t.Kind {
	case usmap.KindBool, usmap.KindByte, usmap.KindInt8:
		return 1
	case usmap.KindInt16, usmap.KindUInt16:
		return 2
	case usmap.KindInt, usmap.KindFloat, usmap.KindUInt32, usmap.KindName, usmap.KindStr,
		usmap.KindUtf8Str, usmap.KindAnsiStr, usmap.KindClass, usmap.KindSoftClass,
		usmap.KindEnum, usmap.KindArray, usmap.KindUnknown, usmap.KindObject, usmap.KindSoftObject, usmap.KindOptional:
		return 4
	case usmap.KindMap, usmap.KindSet, usmap.KindInt64, usmap.KindUInt64, usmap.KindDouble:
		return 8
	case usmap.KindText:
		return 5
	case usmap.KindStruct:
		return d.structMinSize(t.Struct)
	}
	return 0
}

func (d *Decoder) structMinSize(structName string) int {
	switch structName {
	case "AoCExpression", "SoftObjectPath", "SoftClassPath", "GameplayTagContainer":
		return 4
	case instancedStruct:
		return 8
	}
	d.mu.Lock()
	n, ok := d.sizes[structName]
	d.mu.Unlock()
	if ok {
		return n
	}
	chain, err := d.m.Chain(structName)
	if err != nil {
		return 0
	}
	d.mu.Lock()
	d.sizes[structName] = 0
	d.mu.Unlock()
	for _, sc := range chain {
		for _, p := range sc.Props {
			if d.facts.NotStored[sc.Name+"."+p.Name] {
				continue
			}
			if p.Type.Kind == usmap.KindEnum && d.facts.NativeSerialize[sc.Name] {
				n++
				continue
			}
			n += d.minSize(p.Type)
		}
	}
	d.mu.Lock()
	d.sizes[structName] = n
	d.mu.Unlock()
	return n
}

var recordProps = []struct {
	name string
	kind usmap.Kind
}{
	{"Guid", usmap.KindInt64}, {"ParentGuid", usmap.KindInt64}, {"Name", usmap.KindName}, {"bDevOnly", usmap.KindBool},
}

func (d *Decoder) recordPlan(schema string) (*plan, error) {
	chain, err := d.m.Chain(schema)
	if err != nil {
		return nil, err
	}
	if len(chain) < 2 || chain[len(chain)-1].Name != recordRoot {
		return nil, fmt.Errorf("designdb: %s is not a record schema (its chain does not end in %s)", schema, recordRoot)
	}
	root := chain[len(chain)-1]
	for _, want := range recordProps {
		found := false
		for _, p := range root.Props {
			found = found || p.Name == want.name && p.Type.Kind == want.kind
		}
		if !found {
			return nil, fmt.Errorf("designdb: %s has no %s %s", recordRoot, want.kind, want.name)
		}
	}
	return d.plan(schema, true)
}

type walkOpts struct {
	build     bool
	ids       map[string]uint64
	trace     io.Writer
	traceFrom int
}

func (d *Decoder) walk(t TableInfo, schema string, p *plan, build bool, ids map[string]uint64) (rows []Row, err error) {
	return d.walkTraced(t, schema, p, walkOpts{build: build, ids: ids})
}

// WalkFrom reads up to rows rows of table as schema starting at file offset.
func (d *Decoder) WalkFrom(table int, schema string, offset uint64, rows uint32) ([]Row, error) {
	t, ok := d.db.Table(table)
	if !ok {
		return nil, fmt.Errorf("designdb: no table %d", table)
	}
	if offset < t.Offset || offset > t.End() {
		return nil, fmt.Errorf("designdb: offset %#x is outside table %d", offset, table)
	}
	p, err := d.recordPlan(schema)
	if err != nil {
		return nil, err
	}
	part := TableInfo{Index: t.Index, TypeID: t.TypeID, Offset: offset, Size: t.End() - offset, Rows: rows}
	got, err := d.walkTraced(part, schema, p, walkOpts{build: true})
	if de, ok := err.(*DecodeError); ok && de.Row == int(rows) && de.Path == "" {
		return got, nil
	}
	return got, err
}

// Trace writes one line per value read from fromRow onward to w.
func (d *Decoder) Trace(table int, schema string, offset uint64, fromRow int, w io.Writer) error {
	t, ok := d.db.Table(table)
	if !ok {
		return fmt.Errorf("designdb: no table %d", table)
	}
	if offset != 0 {
		if offset < t.Offset || offset > t.End() {
			return fmt.Errorf("designdb: offset %#x is outside table %d", offset, table)
		}
		t = TableInfo{Index: t.Index, TypeID: t.TypeID, Offset: offset, Size: t.End() - offset, Rows: t.Rows}
	}
	p, err := d.recordPlan(schema)
	if err != nil {
		return err
	}
	_, err = d.walkTraced(t, schema, p, walkOpts{build: true, trace: w, traceFrom: fromRow})
	return err
}

func (d *Decoder) walkTraced(t TableInfo, schema string, p *plan, opt walkOpts) (rows []Row, err error) {
	w := d.newWalker(t, opt)
	if opt.build {
		rows = make([]Row, 0, t.Rows)
	}
	align := 8
	if a := d.facts.RowAlign[schema]; a > 8 {
		align = a
	}
	row, rowStart := 0, w.o
	defer func() {
		if r := recover(); r != nil {
			f, ok := r.(*failure)
			if !ok {
				panic(r)
			}
			err = &DecodeError{
				Table: t.Index, Schema: schema, Row: row,
				RowOffset: int64(rowStart), Offset: int64(f.off),
				Path: w.pathString(), Msg: f.msg,
			}
		}
	}()
	for ; row < int(t.Rows); row++ {
		w.o = (w.o + align - 1) &^ (align - 1)
		rowStart = w.o
		w.row = row
		w.path = w.path[:0]
		if w.tracing() {
			fmt.Fprintf(w.trace, "%#x row %d starts\n", w.o, row)
		}
		v := w.readPlan(p)
		if opt.build {
			rows = append(rows, makeRow(int64(rowStart), int64(w.o), v.(Struct)))
		}
	}
	w.path = w.path[:0]
	if w.o != w.end {
		rowStart = w.o
		w.failTableEnd(t.Rows)
	}
	return rows, nil
}

func makeRow(off, end int64, s Struct) Row {
	r := Row{Offset: off, End: end, Fields: s}
	r.Guid, _ = s["Guid"].(int64)
	r.ParentGuid, _ = s["ParentGuid"].(int64)
	r.Name, _ = s["Name"].(string)
	r.DevOnly, _ = s["bDevOnly"].(bool)
	delete(s, "Guid")
	delete(s, "ParentGuid")
	delete(s, "Name")
	delete(s, "bDevOnly")
	return r
}

// Decode reads every row of table as schema, failing if rows do not end on the table's last byte.
func (d *Decoder) Decode(table int, schema string) ([]Row, error) {
	rows, err := d.DecodePartial(table, schema)
	if err != nil {
		return nil, err
	}
	return rows, nil
}

// DecodePartial decodes table as schema, returning rows read prior to any failure alongside the error.
func (d *Decoder) DecodePartial(table int, schema string) ([]Row, error) {
	t, ok := d.db.Table(table)
	if !ok {
		return nil, fmt.Errorf("designdb: no table %d", table)
	}
	p, err := d.recordPlan(schema)
	if err != nil {
		return nil, err
	}
	return d.walk(t, schema, p, true, nil)
}

// Check walks table as schema without allocating decoded values.
func (d *Decoder) Check(table int, schema string) error {
	t, ok := d.db.Table(table)
	if !ok {
		return fmt.Errorf("designdb: no table %d", table)
	}
	p, err := d.recordPlan(schema)
	if err != nil {
		return err
	}
	_, err = d.walk(t, schema, p, false, nil)
	return err
}
