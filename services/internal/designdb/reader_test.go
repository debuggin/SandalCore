// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package designdb

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"math"
	"strings"
	"testing"

	"aocrop/services/internal/usmap"
)

type pool struct {
	buf  bytes.Buffer
	refs map[string]uint32
}

func newPool() *pool {
	p := &pool{refs: map[string]uint32{}}
	p.ref("")
	p.ref("None")
	return p
}

func (p *pool) ref(s string) uint32 {
	if r, ok := p.refs[s]; ok {
		return r
	}
	r := uint32(p.buf.Len())
	_ = binary.Write(&p.buf, binary.LittleEndian, uint32(len(s)+1))
	p.buf.WriteString(s)
	p.buf.WriteByte(0)
	p.refs[s] = r
	return r
}

type rowWriter struct {
	base int
	buf  bytes.Buffer
	pool *pool
}

func (w *rowWriter) row() { w.rowAligned(8) }

func (w *rowWriter) rowAligned(align int) {
	for (w.base+w.buf.Len())%align != 0 {
		w.buf.WriteByte(0)
	}
}
func (w *rowWriter) u8(v uint8)    { w.buf.WriteByte(v) }
func (w *rowWriter) bool(v bool)   { w.buf.WriteByte(map[bool]byte{false: 0, true: 1}[v]) }
func (w *rowWriter) u16(v uint16)  { _ = binary.Write(&w.buf, binary.LittleEndian, v) }
func (w *rowWriter) u32(v uint32)  { _ = binary.Write(&w.buf, binary.LittleEndian, v) }
func (w *rowWriter) i32(v int32)   { _ = binary.Write(&w.buf, binary.LittleEndian, v) }
func (w *rowWriter) i64(v int64)   { _ = binary.Write(&w.buf, binary.LittleEndian, v) }
func (w *rowWriter) u64(v uint64)  { _ = binary.Write(&w.buf, binary.LittleEndian, v) }
func (w *rowWriter) f32(v float32) { w.u32(math.Float32bits(v)) }
func (w *rowWriter) f64(v float64) { w.u64(math.Float64bits(v)) }
func (w *rowWriter) str(s string)  { w.u32(w.pool.ref(s)) }
func (w *rowWriter) fstr(s string) {
	if s == "" {
		w.i32(0)
		return
	}
	w.i32(int32(len(s) + 1))
	w.buf.WriteString(s)
	w.buf.WriteByte(0)
}

// A row with a non-zero parent ends with 4 trailing zero bytes in the wire format.
func (w *rowWriter) tail(guid, parent int64, name string, devOnly bool) {
	w.i64(guid)
	w.i64(parent)
	w.str(name)
	w.bool(devOnly)
	if parent != 0 {
		w.u32(0)
	}
}

type tableSpec struct {
	typeID uint64
	rows   uint32
	write  func(w *rowWriter)
}

func buildDBC(p *pool, specs ...tableSpec) []byte {
	dataStart := headerSize + dirEntrySize*len(specs)
	var tables [][]byte
	off := dataStart
	var dir bytes.Buffer
	for _, s := range specs {
		w := &rowWriter{base: off, pool: p}
		s.write(w)
		data := w.buf.Bytes()
		_ = binary.Write(&dir, binary.LittleEndian, s.typeID)
		_ = binary.Write(&dir, binary.LittleEndian, uint64(off))
		_ = binary.Write(&dir, binary.LittleEndian, uint64(len(data)))
		_ = binary.Write(&dir, binary.LittleEndian, s.rows)
		_ = binary.Write(&dir, binary.LittleEndian, crc32.ChecksumIEEE(data))
		tables = append(tables, data)
		off += len(data)
	}
	poolOff := (off + 7) &^ 7
	head := make([]byte, headerSize)
	copy(head, magic)
	binary.LittleEndian.PutUint64(head[0x08:], 1768873989)
	binary.LittleEndian.PutUint32(head[0x14:], uint32(len(specs)))
	binary.LittleEndian.PutUint64(head[0x28:], uint64(poolOff))
	binary.LittleEndian.PutUint32(head[0x30:], uint32(p.buf.Len()))
	binary.LittleEndian.PutUint32(head[0x34:], crc32.ChecksumIEEE(p.buf.Bytes()))
	binary.LittleEndian.PutUint32(head[0x38:], uint32(dataStart))

	out := append(head, dir.Bytes()...)
	for _, t := range tables {
		out = append(out, t...)
	}
	for len(out) < poolOff {
		out = append(out, 0)
	}
	out = append(out, p.buf.Bytes()...)
	binary.LittleEndian.PutUint32(out[0x04:], crc32.ChecksumIEEE(out[8:dataStart]))
	return out
}

func prop(name string, k usmap.Kind) usmap.Prop {
	return usmap.Prop{ArrayDim: 1, Name: name, Type: &usmap.Type{Kind: k}}
}

func structProp(name, st string) usmap.Prop {
	return usmap.Prop{ArrayDim: 1, Name: name, Type: &usmap.Type{Kind: usmap.KindStruct, Struct: st}}
}

func arrayOf(name string, in *usmap.Type) usmap.Prop {
	return usmap.Prop{ArrayDim: 1, Name: name, Type: &usmap.Type{Kind: usmap.KindArray, Inner: in}}
}

func tInt() *usmap.Type             { return &usmap.Type{Kind: usmap.KindInt} }
func tStructT(n string) *usmap.Type { return &usmap.Type{Kind: usmap.KindStruct, Struct: n} }

func enumProp(name, enum string) usmap.Prop {
	return usmap.Prop{ArrayDim: 1, Name: name, Type: &usmap.Type{Kind: usmap.KindEnum, Enum: enum, Inner: &usmap.Type{Kind: usmap.KindByte}}}
}

func schema(name, super string, props ...usmap.Prop) *usmap.Schema {
	return &usmap.Schema{Name: name, Super: super, PropCount: uint16(len(props)), Props: props}
}

func testMap() *usmap.Map {
	return usmap.New(
		[]*usmap.Enum{
			{Name: "ERichCurveInterpMode", Values: []usmap.EnumValue{{Name: "RCIM_Linear", Value: 0}, {Name: "RCIM_Constant", Value: 1}, {Name: "RCIM_Cubic", Value: 2}}},
		},
		[]*usmap.Schema{
			schema("DesignDataRecordBase", "",
				prop("Guid", usmap.KindInt64), prop("ParentGuid", usmap.KindInt64), prop("Name", usmap.KindName),
				prop("bDevOnly", usmap.KindBool), prop("RecordFilename", usmap.KindStr), prop("bIsDirty", usmap.KindBool)),
			schema("DesignDataObjectId", "",
				prop("Guid", usmap.KindInt64), prop("TypeId", usmap.KindInt64), prop("Name", usmap.KindName)),
			schema("WidgetId", "DesignDataObjectId"),
			schema("AoCExpression", "", prop("Expression", usmap.KindStr)),
			schema("SoftObjectPath", "", structProp("AssetPath", "TopLevelAssetPath"), prop("SubPathString", usmap.KindUnknown)),
			schema("GameplayTag", "", prop("TagName", usmap.KindName)),
			schema("GameplayTagContainer", "", arrayOf("GameplayTags", tStructT("GameplayTag")), arrayOf("ParentTags", tStructT("GameplayTag"))),
			schema("RichCurveKey", "", enumProp("InterpMode", "ERichCurveInterpMode"), prop("Time", usmap.KindFloat)),
			schema("Sub", "", prop("A", usmap.KindInt), arrayOf("Keys", tStructT("RichCurveKey"))),
			schema("WidgetRecordBase", "DesignDataRecordBase", prop("Count", usmap.KindInt)),
			schema("GadgetRecordBase", "DesignDataRecordBase", prop("Level", usmap.KindInt)),
			schema("HolderRecordBase", "DesignDataRecordBase", structProp("Target", "WidgetId")),
			kitchenRecordSchema(),
			schema("NotARecord", "", prop("X", usmap.KindInt)),
		})
}

func kitchenRecordSchema() *usmap.Schema {
	return schema("KitchenRecordBase", "DesignDataRecordBase",
		prop("B", usmap.KindBool), prop("By", usmap.KindByte), prop("I8", usmap.KindInt8),
		prop("I16", usmap.KindInt16), prop("U16", usmap.KindUInt16), prop("I", usmap.KindInt),
		prop("U32", usmap.KindUInt32), prop("L", usmap.KindInt64), prop("U64", usmap.KindUInt64),
		prop("F", usmap.KindFloat), prop("D", usmap.KindDouble), prop("N", usmap.KindName),
		prop("S", usmap.KindStr), enumProp("E", "EColor"), prop("Tx", usmap.KindText),
		arrayOf("Arr", tInt()),
		usmap.Prop{ArrayDim: 1, Name: "M", Type: &usmap.Type{Kind: usmap.KindMap, Inner: &usmap.Type{Kind: usmap.KindName}, Value: &usmap.Type{Kind: usmap.KindFloat}}},
		usmap.Prop{ArrayDim: 1, Name: "Set", Type: &usmap.Type{Kind: usmap.KindSet, Inner: tInt()}},
		structProp("Sub", "Sub"), structProp("Ref", "WidgetId"), structProp("Expr", "AoCExpression"),
		structProp("Soft", "SoftObjectPath"), structProp("Tags", "GameplayTagContainer"),
		prop("Obj", usmap.KindObject), prop("SoftObj", usmap.KindSoftObject))
}

func widgetTable(typeID uint64, n int) tableSpec {
	return tableSpec{typeID, uint32(n), func(w *rowWriter) {
		for i := 0; i < n; i++ {
			w.row()
			w.i32(int32(i + 1))
			w.tail(int64(1000+i), 0, "w"+string(rune('a'+i)), false)
		}
	}}
}

func TestParseHeaderAndDirectory(t *testing.T) {
	p := newPool()
	data := buildDBC(p, widgetTable(111, 2), widgetTable(222, 3))
	db, err := Parse(data)
	if err != nil {
		t.Fatal(err)
	}
	h := db.Header()
	if h.Tables != 2 || h.Generated != 1768873989 || h.DataStart != headerSize+2*dirEntrySize ||
		h.PoolSize != uint32(p.buf.Len()) {
		t.Errorf("header = %+v", h)
	}
	if got := h.GeneratedAt().Format("2006-01-02 15:04:05"); got != "2026-01-20 01:53:09" {
		t.Errorf("GeneratedAt = %s", got)
	}
	tabs := db.Tables()
	if len(tabs) != 2 || tabs[0].TypeID != 111 || tabs[0].Rows != 2 || tabs[1].TypeID != 222 || tabs[1].Rows != 3 {
		t.Fatalf("tables = %+v", tabs)
	}
	if tabs[0].Offset != uint64(h.DataStart) || tabs[1].Offset != tabs[0].End() {
		t.Errorf("tables are not one run: %+v", tabs)
	}
	checkTableLookupsAndVerify(t, db, len(data), int(tabs[1].Size))
}

func checkTableLookupsAndVerify(t *testing.T, db *DB, wantSize, wantTab1Size int) {
	t.Helper()
	if ti, ok := db.TableByTypeID(222); !ok || ti.Index != 1 {
		t.Errorf("TableByTypeID(222) = %+v, %v", ti, ok)
	}
	if _, ok := db.TableByTypeID(999); ok {
		t.Error("TableByTypeID(999) found a table")
	}
	if _, ok := db.Table(2); ok {
		t.Error("Table(2) found a table")
	}
	if db.Size() != wantSize || len(db.TableBytes(1)) != wantTab1Size {
		t.Error("Size / TableBytes")
	}
	if err := db.Verify(); err != nil {
		t.Errorf("Verify: %v", err)
	}
}

func TestPoolStrings(t *testing.T) {
	p := newPool()
	hello := p.ref("héllo")
	bad := p.ref("x")
	db, err := Parse(buildDBC(p, widgetTable(1, 1)))
	if err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		ref  uint32
		want string
		ok   bool
	}{
		{0, "", true}, {p.ref("None"), "None", true}, {hello, "héllo", true},
		{0xFFFFFFFF, "", true}, {hello + 1, "", false}, {bad + 100000, "", false},
	} {
		got, ok := db.poolString(c.ref)
		if got != c.want || ok != c.ok {
			t.Errorf("poolString(%#x) = %q, %v; want %q, %v", c.ref, got, ok, c.want, c.ok)
		}
	}
}

func TestParseErrors(t *testing.T) {
	good := func() []byte { return buildDBC(newPool(), widgetTable(1, 2), widgetTable(2, 1)) }
	le := binary.LittleEndian
	cases := []struct {
		name   string
		mutate func(b []byte) []byte
		want   string
	}{
		{"short file", func(b []byte) []byte { return b[:10] }, "bad magic"},
		{"bad magic", func(b []byte) []byte { b[0] = 'X'; return b }, "bad magic"},
		{"table count too big", func(b []byte) []byte { le.PutUint32(b[0x14:], 100000); return b }, "do not fit"},
		{"data start", func(b []byte) []byte { le.PutUint32(b[0x38:], 0x100); return b }, "data starts at"},
		{"pool not at the end", func(b []byte) []byte { return append(b, 0, 0, 0) }, "does not end"},
		{"table gap", func(b []byte) []byte { le.PutUint64(b[headerSize+dirEntrySize+8:], 0x1000); return b }, "not the next run"},
		{"table past the pool", func(b []byte) []byte { le.PutUint64(b[headerSize+16:], 1<<30); return b }, "not the next run"},
		{"pool string overruns", func(b []byte) []byte {
			poolOff := le.Uint64(b[0x28:])
			le.PutUint32(b[poolOff:], 1<<30)
			return b
		}, "claims"},
		{"pool offset and size that wrap to the file length", mutatePoolWrap, "does not end"},
		{"pool string not terminated", func(b []byte) []byte {
			poolOff := le.Uint64(b[0x28:])
			b[poolOff+4] = 'x'
			return b
		}, "NUL"},
	}
	for _, c := range cases {
		_, err := Parse(c.mutate(good()))
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: err = %v, want it to contain %q", c.name, err, c.want)
		}
	}
	checkVerifyCorruptions(t, good)
}

// off + size is 2^64 + len(b), wrapping to len(b) while stretching the last table to end 5 bytes before off.
func mutatePoolWrap(b []byte) []byte {
	le := binary.LittleEndian
	size := uint32(len(b)) + 8
	off := ^uint64(0) - 7
	le.PutUint64(b[0x28:], off)
	le.PutUint32(b[0x30:], size)
	last := headerSize + dirEntrySize
	le.PutUint64(b[last+16:], off-5-le.Uint64(b[last+8:]))
	return b
}

func checkVerifyCorruptions(t *testing.T, good func() []byte) {
	t.Helper()
	for _, c := range []struct {
		name string
		at   func(db *DB) int
		want string
	}{
		{"header", func(db *DB) int { return 0x20 }, "header crc"},
		{"table", func(db *DB) int { return int(db.tables[1].Offset) + 2 }, "table 1 crc"},
		{"pool", func(db *DB) int { return int(db.hdr.PoolOffset) + 10 }, "pool crc"},
	} {
		data := good()
		db, err := Parse(data)
		if err != nil {
			t.Fatal(err)
		}
		data[c.at(db)] ^= 0x40
		db2, err := Parse(data)
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		if err := db2.Verify(); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("Verify after corrupting the %s: %v, want %q", c.name, err, c.want)
		}
	}
}

// A row takes at least 21 bytes (Guid, ParentGuid, Name, bDevOnly), so Parse rejects impossible counts upfront.
func TestDecodeRejectsAnImpossibleRowCount(t *testing.T) {
	for _, rows := range []uint32{3, 1 << 20, 0xFFFFFFFF} {
		data := buildDBC(newPool(), widgetTable(1, 2))
		binary.LittleEndian.PutUint32(data[headerSize+24:], rows)
		db, err := Parse(data)
		if err == nil || !strings.Contains(err.Error(), "rows") {
			t.Errorf("%d rows in a table of 2: Parse = %v, want an error about the rows", rows, err)
			if err == nil {
				_, _ = db.Decoder(testMap()).Decode(0, "WidgetRecordBase")
			}
		}
	}
	data := buildDBC(newPool(), widgetTable(1, 2), tableSpec{2, 0, func(w *rowWriter) {}})
	if _, err := Parse(data); err != nil {
		t.Errorf("Parse = %v", err)
	}
}

func TestFindInt64(t *testing.T) {
	db, err := Parse(buildDBC(newPool(), widgetTable(1, 3), widgetTable(2, 1)))
	if err != nil {
		t.Fatal(err)
	}
	hits := db.FindInt64(0, 1001)
	if len(hits) != 1 {
		t.Fatalf("hits = %v", hits)
	}
	if got := db.data[hits[0] : hits[0]+8]; binary.LittleEndian.Uint64(got) != 1001 {
		t.Errorf("hit %#x does not hold 1001", hits[0])
	}
	if hits := db.FindInt64(1, 1001); len(hits) != 0 {
		t.Errorf("table 1 has no 1001 row, hits = %v", hits)
	}
	if hits := db.FindInt64(0, 77777); len(hits) != 0 {
		t.Errorf("hits = %v", hits)
	}
}

func TestRowAlignmentAndSelfCheck(t *testing.T) {
	m := testMap()
	p := newPool()
	db, err := Parse(buildDBC(p, widgetTable(1, 4)))
	if err != nil {
		t.Fatal(err)
	}
	d := db.Decoder(m)
	rows, err := d.Decode(0, "WidgetRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 4 {
		t.Fatalf("rows = %d", len(rows))
	}
	for i, r := range rows {
		if r.Offset%8 != 0 {
			t.Errorf("row %d at %#x is not 8-aligned", i, r.Offset)
		}
		if r.Fields["Count"] != int64(i+1) || r.Guid != int64(1000+i) {
			t.Errorf("row %d = %+v", i, r)
		}
		if i > 0 && r.Offset-rows[i-1].Offset != 32 {
			t.Errorf("row %d starts %d bytes after the last", i, r.Offset-rows[i-1].Offset)
		}
	}
	if err := d.Check(0, "WidgetRecordBase"); err != nil {
		t.Errorf("Check: %v", err)
	}
	checkShortAndPartialTable(t, m)
	checkOverCountAndCutTable(t, m)
}

func checkShortAndPartialTable(t *testing.T, m *usmap.Map) {
	t.Helper()
	short := buildDBC(newPool(), tableSpec{1, 2, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.i32(1)
			w.tail(1, 0, "a", false)
		}
		w.u32(0xdeadbeef)
	}})
	db, err := Parse(short)
	if err != nil {
		t.Fatal(err)
	}
	rows, err := db.Decoder(m).Decode(0, "WidgetRecordBase")
	var de *DecodeError
	if rows != nil || !asDecodeError(err, &de) {
		t.Fatalf("Decode = %v, %v; want no rows and a *DecodeError", rows, err)
	}
	if de.Row != 2 || de.Table != 0 || de.Schema != "WidgetRecordBase" || !strings.Contains(de.Msg, "4 bytes short") {
		t.Errorf("error = %+v", de)
	}
	if got := db.Tables()[0].End(); de.Offset != int64(got)-4 {
		t.Errorf("error offset %#x, want %#x", de.Offset, got-4)
	}
	part, err := db.Decoder(m).DecodePartial(0, "WidgetRecordBase")
	if err == nil || len(part) != 2 {
		t.Errorf("DecodePartial = %d rows, %v", len(part), err)
	}
}

func checkOverCountAndCutTable(t *testing.T, m *usmap.Map) {
	t.Helper()
	var de *DecodeError
	over := buildDBC(newPool(), tableSpec{1, 3, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.i32(1)
			w.tail(1, 0, "a", false)
		}
		w.u64(0)
	}})
	db, _ := Parse(over)
	_, err := db.Decoder(m).Decode(0, "WidgetRecordBase")
	if !asDecodeError(err, &de) || de.Row != 2 || !strings.Contains(de.Msg, "past the end") {
		t.Errorf("over-count: %v", err)
	}

	cut := buildDBC(newPool(), tableSpec{1, 2, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.i32(1)
			w.tail(1, 0, "a", false)
		}
		w.buf.Truncate(w.buf.Len() - 3)
	}})
	db, _ = Parse(cut)
	_, err = db.Decoder(m).Decode(0, "WidgetRecordBase")
	if !asDecodeError(err, &de) || de.Row != 1 {
		t.Errorf("cut row: %v", err)
	}
}

func asDecodeError(err error, target **DecodeError) bool {
	de, ok := err.(*DecodeError)
	if ok {
		*target = de
	}
	return ok
}
