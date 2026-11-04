// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package designdb

import (
	"math"
	"reflect"
	"strings"
	"testing"

	"aocrop/services/internal/usmap"
)

func kitchenTable(typeID uint64) tableSpec {
	return tableSpec{typeID, 1, func(w *rowWriter) {
		w.row()
		w.bool(true)
		w.u8(200)
		w.u8(0xFE)
		w.u16(0xFFFF)
		w.u16(65535)
		w.i32(-7)
		w.u32(4000000000)
		w.i64(-1 << 40)
		w.u64(math.MaxUint64)
		w.f32(1.5)
		w.f64(-2.25)
		w.str("a name")
		w.str("a string")
		w.str("EColor::Green")
		w.u32(0)
		w.u8(0)
		w.fstr("ns")
		w.fstr("key")
		w.fstr("Source text")
		writeKitchenContainersAndTail(w)
	}}
}

func writeKitchenContainersAndTail(w *rowWriter) {
	w.u32(3)
	w.i32(10)
	w.i32(20)
	w.i32(30)
	w.u32(0)
	w.u32(2)
	w.str("x")
	w.f32(1)
	w.str("y")
	w.f32(2)
	w.u32(0)
	w.u32(2)
	w.i32(5)
	w.i32(6)
	w.i32(9)
	w.u32(2)
	w.u8(2)
	w.f32(0.5)
	w.u8(0)
	w.f32(1.5)
	w.i64(42)
	w.i64(777)
	w.str("None")
	w.str("1 + 2")
	w.str("/Game/Foo.Foo")
	w.u32(2)
	w.str("tag.a")
	w.str("tag.b")
	w.str("/Game/Obj")
	w.str("/Game/Soft")
	w.tail(5000, 6000, "kitchen", true)
}

func wantKitchenStruct() Struct {
	return Struct{
		"B": true, "By": int64(200), "I8": int64(-2), "I16": int64(-1), "U16": int64(65535), "I": int64(-7),
		"U32": int64(4000000000), "L": int64(-1 << 40), "U64": uint64(math.MaxUint64),
		"F": float32(1.5), "D": float64(-2.25), "N": "a name", "S": "a string", "E": "EColor::Green",
		"Tx":  Text{Namespace: "ns", Key: "key", Source: "Source text"},
		"Arr": []Value{int64(10), int64(20), int64(30)},
		"M":   Pairs{{"x", float32(1)}, {"y", float32(2)}},
		"Set": []Value{int64(5), int64(6)},
		"Sub": Struct{"A": int64(9), "Keys": []Value{
			Struct{"InterpMode": "ERichCurveInterpMode::RCIM_Cubic", "Time": float32(0.5)},
			Struct{"InterpMode": "ERichCurveInterpMode::RCIM_Linear", "Time": float32(1.5)},
		}},
		"Ref":     Struct{"Guid": int64(42), "TypeId": int64(777), "Name": "None"},
		"Expr":    "1 + 2",
		"Soft":    "/Game/Foo.Foo",
		"Tags":    []Value{"tag.a", "tag.b"},
		"Obj":     "/Game/Obj",
		"SoftObj": "/Game/Soft",
	}
}

func TestDecodeEveryValueKind(t *testing.T) {
	db, err := Parse(buildDBC(newPool(), kitchenTable(9)))
	if err != nil {
		t.Fatal(err)
	}
	rows, err := db.Decoder(testMap()).Decode(0, "KitchenRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 1 {
		t.Fatalf("rows = %d", len(rows))
	}
	r := rows[0]
	if r.Guid != 5000 || r.ParentGuid != 6000 || r.Name != "kitchen" || !r.DevOnly {
		t.Errorf("row tail = %+v", r)
	}
	if r.Offset%8 != 0 {
		t.Errorf("row offset %#x is not 8-aligned", r.Offset)
	}
	for _, name := range []string{"Guid", "ParentGuid", "Name", "bDevOnly", "RecordFilename", "bIsDirty"} {
		if _, in := r.Fields[name]; in {
			t.Errorf("Fields holds %s", name)
		}
	}
	want := wantKitchenStruct()
	for k, v := range want {
		if !reflect.DeepEqual(r.Fields[k], v) {
			t.Errorf("%s = %#v, want %#v", k, r.Fields[k], v)
		}
	}
}

func TestDecodeTextHistories(t *testing.T) {
	m := usmap.New(nil, []*usmap.Schema{
		schema("DesignDataRecordBase", "",
			prop("Guid", usmap.KindInt64), prop("ParentGuid", usmap.KindInt64), prop("Name", usmap.KindName),
			prop("bDevOnly", usmap.KindBool), prop("RecordFilename", usmap.KindStr), prop("bIsDirty", usmap.KindBool)),
		schema("TextRecordBase", "DesignDataRecordBase", prop("T", usmap.KindText)),
	})
	cases := []struct {
		name  string
		write func(w *rowWriter)
		want  Text
	}{
		{"none, no string", func(w *rowWriter) { w.u32(0); w.u8(0xFF); w.u32(0) }, Text{History: -1}},
		{"none, culture invariant", func(w *rowWriter) { w.u32(0); w.u8(0xFF); w.u32(1); w.fstr("hi") }, Text{History: -1, Source: "hi"}},
		{"string table entry", func(w *rowWriter) { w.u32(0); w.u8(11); w.str("tbl"); w.fstr("k") }, Text{History: 11, Table: "tbl", Key: "k"}},
		{"base, empty strings", func(w *rowWriter) { w.u32(0x20); w.u8(0); w.fstr(""); w.fstr("k"); w.fstr("") }, Text{Key: "k"}},
	}
	for _, c := range cases {
		spec := tableSpec{1, 1, func(w *rowWriter) {
			w.row()
			c.write(w)
			w.tail(1, 0, "t", false)
		}}
		db, err := Parse(buildDBC(newPool(), spec))
		if err != nil {
			t.Fatal(err)
		}
		rows, err := db.Decoder(m).Decode(0, "TextRecordBase")
		if err != nil {
			t.Errorf("%s: %v", c.name, err)
			continue
		}
		if got := rows[0].Fields["T"]; got != c.want {
			t.Errorf("%s: %#v, want %#v", c.name, got, c.want)
		}
	}
	spec := tableSpec{1, 1, func(w *rowWriter) { w.row(); w.u32(0); w.u8(7); w.tail(1, 0, "t", false) }}
	db, _ := Parse(buildDBC(newPool(), spec))
	if _, err := db.Decoder(m).Decode(0, "TextRecordBase"); err == nil || !strings.Contains(err.Error(), "history type 7") {
		t.Errorf("history 7: err = %v", err)
	}
}

func badTagRefTable() tableSpec {
	return tableSpec{9, 1, func(w *rowWriter) {
		w.row()
		w.bool(false)
		w.u8(0)
		w.u8(0)
		w.u16(0)
		w.u16(0)
		w.i32(0)
		w.u32(0)
		w.i64(0)
		w.u64(0)
		w.f32(0)
		w.f64(0)
		w.str("")
		w.str("")
		w.str("")
		w.u32(0)
		w.u8(0xFF)
		w.u32(0)
		for i := 0; i < 7; i++ {
			w.u32(0)
		}
		w.i64(0)
		w.i64(0)
		w.str("")
		w.str("")
		w.str("")
		w.u32(2)
		w.str("ok")
		w.u32(3)
		w.str("")
		w.str("")
		w.tail(1, 0, "x", false)
	}}
}

func TestDecodeErrorsNameTheProperty(t *testing.T) {
	m := testMap()
	db, err := Parse(buildDBC(newPool(), badTagRefTable()))
	if err != nil {
		t.Fatal(err)
	}
	_, err = db.Decoder(m).Decode(0, "KitchenRecordBase")
	var de *DecodeError
	if !asDecodeError(err, &de) {
		t.Fatalf("err = %v", err)
	}
	if de.Path != "Tags[1]" || !strings.Contains(de.Msg, "does not point at a string") || de.Row != 0 {
		t.Errorf("error = %+v", de)
	}
	if !strings.Contains(err.Error(), "Tags[1]") {
		t.Errorf("Error() = %s", err)
	}
}

func TestStrictValues(t *testing.T) {
	m := testMap()
	spec := tableSpec{1, 1, func(w *rowWriter) {
		w.row()
		w.i32(1)
		w.i64(1)
		w.i64(0)
		w.str("a")
		w.u8(2)
	}}
	db, _ := Parse(buildDBC(newPool(), spec))
	if _, err := db.Decoder(m).Decode(0, "WidgetRecordBase"); err == nil || !strings.Contains(err.Error(), "bool byte 2") {
		t.Errorf("bool 2: %v", err)
	}
	checkStrictMapValues(t)
}

func checkStrictMapValues(t *testing.T) {
	t.Helper()
	m2 := usmap.New(nil, []*usmap.Schema{
		schema("DesignDataRecordBase", "", prop("Guid", usmap.KindInt64), prop("ParentGuid", usmap.KindInt64), prop("Name", usmap.KindName), prop("bDevOnly", usmap.KindBool)),
		schema("MapRecordBase", "DesignDataRecordBase", usmap.Prop{ArrayDim: 1, Name: "M", Type: &usmap.Type{Kind: usmap.KindMap, Inner: &usmap.Type{Kind: usmap.KindInt}, Value: &usmap.Type{Kind: usmap.KindInt}}}),
	})
	spec := tableSpec{1, 1, func(w *rowWriter) {
		w.row()
		w.u32(1)
		w.i32(7)
		w.u32(1)
		w.i32(5)
		w.i32(6)
		w.tail(1, 0, "m", false)
	}}
	db, _ := Parse(buildDBC(newPool(), spec))
	rows, err := db.Decoder(m2).Decode(0, "MapRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if want := (Pairs{{int64(5), int64(6)}}); !reflect.DeepEqual(rows[0].Fields["M"], want) {
		t.Errorf("M = %#v, want %#v", rows[0].Fields["M"], want)
	}
	spec = tableSpec{1, 1, func(w *rowWriter) {
		w.row()
		w.u32(1 << 30)
		w.buf.Write(make([]byte, 24))
	}}
	db, _ = Parse(buildDBC(newPool(), spec))
	if _, err := db.Decoder(m2).Decode(0, "MapRecordBase"); err == nil || !strings.Contains(err.Error(), "needs at least") {
		t.Errorf("huge removal count: %v", err)
	}
	spec = tableSpec{1, 1, func(w *rowWriter) {
		w.row()
		w.u32(0)
		w.u32(1 << 30)
		w.buf.Write(make([]byte, 24))
	}}
	db, _ = Parse(buildDBC(newPool(), spec))
	if _, err := db.Decoder(m2).Decode(0, "MapRecordBase"); err == nil || !strings.Contains(err.Error(), "needs at least") {
		t.Errorf("huge count: %v", err)
	}
}

func TestPatches(t *testing.T) {
	m := usmap.New(nil, []*usmap.Schema{
		schema("DesignDataRecordBase", "", prop("Guid", usmap.KindInt64), prop("ParentGuid", usmap.KindInt64), prop("Name", usmap.KindName), prop("bDevOnly", usmap.KindBool)),
		schema("PatchedRecordBase", "DesignDataRecordBase", prop("A", usmap.KindInt), prop("B", usmap.KindInt)),
	})
	spec := tableSpec{1, 2, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.i32(int32(10 + i))
			w.bool(i == 1)
			w.i32(int32(20 + i))
			w.tail(int64(i), 0, "p", false)
		}
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	patch := Patch{Struct: "PatchedRecordBase", After: "A", Prop: prop("Mid", usmap.KindBool), Why: "test"}

	if _, err := db.DecoderWith(m, Options{}).Decode(0, "PatchedRecordBase"); err == nil {
		t.Error("the unpatched mappings decoded the table")
	}
	d := db.DecoderWith(m, Options{Patches: []Patch{patch}})
	rows, err := d.Decode(0, "PatchedRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if got := rows[1].Fields; got["A"] != int64(11) || got["Mid"] != true || got["B"] != int64(21) {
		t.Errorf("row 1 = %v", got)
	}
	if got := d.Applied(); len(got) != 1 || got[0].Why != "test" {
		t.Errorf("Applied = %+v", got)
	}
	if n := len(m.Schema("PatchedRecordBase").Props); n != 2 {
		t.Errorf("the original schema has %d props", n)
	}
	checkPatchStatuses(t, db, m, patch)
	checkBuiltinPatches(t, db, m, patch)
}

func checkPatchStatuses(t *testing.T, db *DB, m *usmap.Map, patch Patch) {
	t.Helper()
	for _, tt := range []struct {
		p      Patch
		status PatchStatus
		fails  bool
	}{
		{Patch{Struct: "NoSuchRecordBase", After: "A", Prop: prop("Mid", usmap.KindBool), Why: "w"}, PatchNoStruct, true},
		{Patch{Struct: "PatchedRecordBase", After: "A", Prop: prop("B", usmap.KindBool)}, PatchPresent, false},
		{Patch{Struct: "PatchedRecordBase", After: "Z", Prop: prop("Mid", usmap.KindBool)}, PatchNoAnchor, true},
		{patch, PatchApplied, false},
	} {
		d := db.DecoderWith(m, Options{Patches: []Patch{tt.p}})
		res := d.PatchResults()
		if len(res) != 1 || res[0].Status != tt.status || res[0].Patch.Struct != tt.p.Struct {
			t.Errorf("patch %v: results %+v, want status %v", tt.p, res, tt.status)
		}
		if got := d.Applied(); (len(got) == 1) != (tt.status == PatchApplied) {
			t.Errorf("patch %v: Applied = %+v", tt.p, got)
		}
		err := d.CheckPatches()
		if tt.fails != (err != nil) || (err != nil && !strings.Contains(err.Error(), tt.p.String()+": "+tt.status.String())) {
			t.Errorf("patch %v: CheckPatches = %v, want an error naming the patch and %q: %v", tt.p, err, tt.status, tt.fails)
		}
	}
}

func checkBuiltinPatches(t *testing.T, db *DB, m *usmap.Map, patch Patch) {
	t.Helper()
	if s := PatchNoAnchor.String(); s != "anchor missing" || PatchPresent.String() != "already present" ||
		PatchNoStruct.String() != "struct missing" || PatchApplied.String() != "applied" {
		t.Errorf("status names: %q ...", s)
	}
	if got := patch.String(); got != "PatchedRecordBase.Mid after A" {
		t.Errorf("Patch.String = %q", got)
	}
	two := db.DecoderWith(m, Options{Patches: []Patch{patch, {Struct: "PatchedRecordBase", After: "Mid", Prop: prop("Mid2", usmap.KindBool)}}})
	if got := two.Applied(); len(got) != 2 {
		t.Errorf("Applied = %+v", got)
	}
	if props := two.m.Schema("PatchedRecordBase").Props; len(props) != 4 || props[1].Name != "Mid" || props[2].Name != "Mid2" || props[2].Index != props[1].Index+1 {
		t.Errorf("props = %+v", props)
	}

	got := Patches()
	if len(got) != 4 || got[0].Struct != "AbilityHitRecordBase" || got[0].After != "ApplicableLifeStates" ||
		got[0].Prop.Name != "bMayCorrupt" || got[0].Prop.Type.Kind != usmap.KindBool {
		t.Errorf("Patches = %+v", got)
	}
	for _, p := range got {
		if p.Why == "" || p.Struct == "" || p.Prop.Name == "" || p.After == "" || p.Prop.ArrayDim != 1 || p.Prop.Type == nil {
			t.Errorf("patch %+v is incomplete", p)
		}
	}
	got[0].Struct = "Changed"
	if Patches()[0].Struct != "AbilityHitRecordBase" {
		t.Error("Patches returns the list itself")
	}
}

func TestDecodeRejectsWhatIsNotARecord(t *testing.T) {
	db, _ := Parse(buildDBC(newPool(), widgetTable(1, 1)))
	d := db.Decoder(testMap())
	for _, c := range []struct{ schema, want string }{
		{"NotARecord", "not a record schema"},
		{"DesignDataRecordBase", "not a record schema"},
		{"NoSuchRecordBase", "no schema"},
	} {
		if _, err := d.Decode(0, c.schema); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("Decode as %s: %v, want %q", c.schema, err, c.want)
		}
	}
	if _, err := d.Decode(5, "WidgetRecordBase"); err == nil || !strings.Contains(err.Error(), "no table 5") {
		t.Errorf("Decode of table 5: %v", err)
	}
	if err := d.Check(-1, "WidgetRecordBase"); err == nil {
		t.Error("Check of table -1 succeeded")
	}
}
