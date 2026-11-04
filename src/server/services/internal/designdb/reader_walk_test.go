// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package designdb

import (
	"reflect"
	"strings"
	"testing"

	"aocrop/services/internal/usmap"
)

func TestClassify(t *testing.T) {
	const (
		holderType = 10
		widgetType = 20
		gadgetType = 30
		stray      = 40
	)
	holder := tableSpec{holderType, 2, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.i64(int64(i))
			w.i64(widgetType)
			w.str("None")
			w.tail(int64(i), 0, "h", false)
		}
	}}
	widgets := widgetTable(widgetType, 3)
	gadgets := widgetTable(gadgetType, 2)
	garbage := tableSpec{stray, 1, func(w *rowWriter) {
		w.row()
		for i := 0; i < 4; i++ {
			w.u64(0xdeadbeefdeadbeef)
		}
	}}

	db, err := Parse(buildDBC(newPool(), holder, widgets, gadgets, garbage))
	if err != nil {
		t.Fatal(err)
	}
	checkClassifyResults(t, db)
	known := map[uint64]string{stray: "KitchenRecordBase", gadgetType: "GadgetRecordBase"}
	cl := db.DecoderWith(testMap(), Options{KnownTypes: known}).Classify()
	if c := cl[3]; c.Schema != "KitchenRecordBase" || c.Decodes || c.Basis != BasisTypeID {
		t.Errorf("garbage with a known type = %+v", c)
	}
	if c := cl[2]; c.Schema != "GadgetRecordBase" || !c.Decodes || c.Basis != BasisTypeID {
		t.Errorf("gadgets with a known type = %+v", c)
	}
}

func checkClassifyResults(t *testing.T, db *DB) {
	t.Helper()
	cl := db.Decoder(testMap()).Classify()
	if len(cl) != 4 {
		t.Fatalf("classifications = %d", len(cl))
	}
	if c := cl[0]; !reflect.DeepEqual(c.Candidates, []string{"HolderRecordBase"}) || c.Schema != "HolderRecordBase" || !c.Decodes || c.Rows != 2 {
		t.Errorf("holder = %+v", c)
	}
	if c := cl[1]; !reflect.DeepEqual(c.Candidates, []string{"GadgetRecordBase", "WidgetRecordBase"}) || c.Schema != "WidgetRecordBase" || !c.Decodes {
		t.Errorf("widgets = %+v", c)
	}
	if c := cl[2]; len(c.Candidates) != 2 || c.Schema != "" || c.Decodes {
		t.Errorf("gadgets = %+v", c)
	}
	if c := cl[3]; len(c.Candidates) != 0 || c.Schema != "" || c.Decodes {
		t.Errorf("garbage = %+v", c)
	}
	if got := db.Decoder(testMap()).Candidates(1); !reflect.DeepEqual(got, []string{"GadgetRecordBase", "WidgetRecordBase"}) {
		t.Errorf("Candidates(1) = %v", got)
	}
	if got := db.Decoder(testMap()).Candidates(9); got != nil {
		t.Errorf("Candidates(9) = %v", got)
	}
}

func oneTableMap() *usmap.Map {
	m := testMap()
	return usmap.New(m.Enums, append(append([]*usmap.Schema{}, m.Schemas...),
		schema("ThingId", "DesignDataObjectId"),
		schema("BoxId", "DesignDataObjectId"),
		schema("ThingRecordBase", "DesignDataRecordBase", prop("A", usmap.KindInt), prop("B", usmap.KindInt), prop("C", usmap.KindInt)),
		schema("BoxRecordBase", "DesignDataRecordBase", prop("A", usmap.KindInt), prop("B", usmap.KindInt), prop("C", usmap.KindInt), prop("D", usmap.KindInt)),
		schema("RefRecordBase", "DesignDataRecordBase", structProp("Thing", "ThingId"), structProp("Box", "BoxId")),
	))
}

func thingTable(typeID uint64, n int, prefix string) tableSpec {
	return tableSpec{typeID, uint32(n), func(w *rowWriter) {
		for i := 0; i < n; i++ {
			w.row()
			w.i32(1)
			w.i32(2)
			w.i32(3)
			w.tail(int64(2000+i), 0, prefix+"_"+string(rune('0'+i)), false)
		}
	}}
}

func namedWidgetTable(typeID uint64, n int, prefix string) tableSpec {
	return tableSpec{typeID, uint32(n), func(w *rowWriter) {
		for i := 0; i < n; i++ {
			w.row()
			w.i32(int32(i + 1))
			w.tail(int64(3000+i), 0, prefix+"_"+string(rune('0'+i)), false)
		}
	}}
}

func refTable(typeID, thing, box uint64) tableSpec {
	return tableSpec{typeID, 1, func(w *rowWriter) {
		w.row()
		w.i64(1)
		w.i64(int64(thing))
		w.str("None")
		w.i64(2)
		w.i64(int64(box))
		w.str("None")
		w.tail(4000, 0, "ref", false)
	}}
}

type classifyOneTableCase struct {
	name   string
	specs  []tableSpec
	schema []string
	basis  []string
}

func classifyOneTableCases() []classifyOneTableCase {
	const undecided = ""
	return append(classifyOneTableIdAndStemCases(undecided),
		classifyOneTableCase{
			name:   "an Id struct that agrees changes nothing",
			specs:  []tableSpec{thingTable(200, 2, "x"), refTable(201, 200, 300)},
			schema: []string{"ThingRecordBase", "RefRecordBase"},
			basis:  []string{BasisSingleFit, BasisSingleFit},
		},
		classifyOneTableCase{
			name:   "tables several structs fit are told apart by their rows' names",
			specs:  []tableSpec{namedWidgetTable(100, 3, "Gadget"), namedWidgetTable(101, 2, "Widget")},
			schema: []string{"GadgetRecordBase", "WidgetRecordBase"},
			basis:  []string{BasisNameStem, BasisNameStem},
		},
		classifyOneTableCase{
			name:   "two such tables named for one struct are a tie: neither is settled",
			specs:  []tableSpec{namedWidgetTable(100, 3, "Gadget"), namedWidgetTable(101, 2, "Gadget")},
			schema: []string{undecided, undecided},
			basis:  []string{"", ""},
		},
		classifyOneTableCase{
			name: "a TypeId beats the rows' names",
			specs: []tableSpec{
				{10, 1, func(w *rowWriter) {
					w.row()
					w.i64(1)
					w.i64(20)
					w.str("None")
					w.tail(1, 0, "h", false)
				}},
				namedWidgetTable(20, 2, "Gadget"),
			},
			schema: []string{"HolderRecordBase", "WidgetRecordBase"},
			basis:  []string{BasisSingleFit, BasisTypeID},
		},
	)
}

func classifyOneTableIdAndStemCases(undecided string) []classifyOneTableCase {
	return []classifyOneTableCase{
		{
			name:   "the table an Id struct names wins, the other is undecided",
			specs:  []tableSpec{thingTable(100, 2, "x"), thingTable(101, 2, "y"), refTable(102, 101, 999)},
			schema: []string{undecided, "ThingRecordBase", "RefRecordBase"},
			basis:  []string{"", BasisSingleFit, BasisSingleFit},
		},
		{
			name:   "the Id struct beats the rows' names",
			specs:  []tableSpec{thingTable(100, 2, "Thing"), thingTable(101, 2, "Other"), refTable(102, 101, 999)},
			schema: []string{undecided, "ThingRecordBase", "RefRecordBase"},
			basis:  []string{"", BasisSingleFit, BasisSingleFit},
		},
		{
			name:   "without an Id struct, the table whose rows are named for the struct wins",
			specs:  []tableSpec{thingTable(100, 2, "Other"), thingTable(101, 2, "Thing")},
			schema: []string{undecided, "ThingRecordBase"},
			basis:  []string{"", BasisSingleFit},
		},
		{
			name:   "with neither, neither is settled",
			specs:  []tableSpec{thingTable(100, 2, "a"), thingTable(101, 2, "b")},
			schema: []string{undecided, undecided},
			basis:  []string{"", ""},
		},
		{
			name:   "a name that is only partly the stem does not count",
			specs:  []tableSpec{thingTable(100, 2, "Thing"), thingTable(101, 2, "Thingamajig"), thingTable(102, 1, "Thing")},
			schema: []string{undecided, undecided, undecided},
			basis:  []string{"", "", ""},
		},
		{
			name:   "a lone table is not settled when an Id struct gives its TypeId to another struct",
			specs:  []tableSpec{thingTable(200, 2, "Thing"), refTable(201, 300, 200)},
			schema: []string{undecided, "RefRecordBase"},
			basis:  []string{"", BasisSingleFit},
		},
	}
}

func TestClassifyOneTablePerSchema(t *testing.T) {
	for _, tt := range classifyOneTableCases() {
		verifyClassifyOneTableCase(t, tt)
	}
}

func verifyClassifyOneTableCase(t *testing.T, tt classifyOneTableCase) {
	t.Helper()
	db, err := Parse(buildDBC(newPool(), tt.specs...))
	if err != nil {
		t.Fatalf("%s: %v", tt.name, err)
	}
	cl := db.Decoder(oneTableMap()).Classify()
	for i := range tt.specs {
		c := cl[i]
		if c.Schema != tt.schema[i] || c.Basis != tt.basis[i] || c.Decodes != (c.Schema != "") {
			t.Errorf("%s: table %d = %+v; want schema %q (%s)", tt.name, i, c, tt.schema[i], tt.basis[i])
		}
	}
	seen := map[string]int{}
	for _, c := range cl {
		if c.Schema == "" {
			continue
		}
		if prev, dup := seen[c.Schema]; dup {
			t.Errorf("%s: tables %d and %d are both %s", tt.name, prev, c.Table, c.Schema)
		}
		seen[c.Schema] = c.Table
	}
}

func TestKnownTypesIsACopy(t *testing.T) {
	got := KnownTypes()
	for id, want := range map[uint64]string{
		7037248826340794574: "StatCurveRecordBase", 1841048597116038327: "AoCAbilityRecordBase",
		3672506661632452365: "AbilityHitRecordBase", 5365104778484440215: "StatTypeDefRecordBase",
		5024462864295026017: "DialogueNode", 2616523457551378613: "ItemRecordBase",
	} {
		if got[id] != want {
			t.Errorf("KnownTypes[%d] = %q, want %q", id, got[id], want)
		}
	}
	if len(got) != 398 {
		t.Errorf("KnownTypes has %d entries, want 398", len(got))
	}
	got[1] = "X"
	delete(got, 7037248826340794574)
	if again := KnownTypes(); len(again) != 398 || again[7037248826340794574] != "StatCurveRecordBase" {
		t.Errorf("KnownTypes after a change = %d entries, StatCurve %q", len(again), again[7037248826340794574])
	}
	f := ExeFacts()
	if !f.NotStored["DesignDataRecordBase.RecordFilename"] || !f.NotStored["DesignDataRecordBase.bIsDirty"] ||
		f.RowAlign["StoryArcPhaseRecordBase"] != 16 || len(f.RowAlign) != 4 || !f.NativeSerialize["RichCurveKey"] {
		t.Errorf("ExeFacts = %+v", f)
	}

	db, err := Parse(buildDBC(newPool(), thingTable(5, 1, "Thing")))
	if err != nil {
		t.Fatal(err)
	}
	known := map[uint64]string{5: "KitchenRecordBase"}
	d := db.DecoderWith(oneTableMap(), Options{KnownTypes: known})
	known[5] = "BoxRecordBase"
	if c := d.Classify()[0]; c.Schema != "" || c.Decodes {
		t.Errorf("table = %+v", c)
	}
}

func rulesMap() *usmap.Map {
	return usmap.New(
		[]*usmap.Enum{{Name: "EMode", Values: []usmap.EnumValue{{Name: "Off", Value: 0}, {Name: "On", Value: 1}}}},
		[]*usmap.Schema{
			schema("DesignDataRecordBase", "",
				prop("Guid", usmap.KindInt64), prop("ParentGuid", usmap.KindInt64), prop("Name", usmap.KindName),
				prop("bDevOnly", usmap.KindBool), prop("RecordFilename", usmap.KindStr), prop("bIsDirty", usmap.KindBool)),
			schema("InstancedStruct", ""),
			schema("Payload", "", prop("P", usmap.KindInt), prop("Q", usmap.KindBool)),
			schema("Native", "", enumProp("Mode", "EMode"), prop("V", usmap.KindFloat)),
			schema("ThingRecordBase", "DesignDataRecordBase", prop("B", usmap.KindInt), prop("Skip", usmap.KindInt), structProp("N", "Native")),
			schema("ThingRecord", "ThingRecordBase", prop("L", usmap.KindBool)),
			schema("InstRecordBase", "DesignDataRecordBase", structProp("Inst", "InstancedStruct"),
				usmap.Prop{ArrayDim: 1, Name: "Opt", Type: &usmap.Type{Kind: usmap.KindOptional, Inner: tInt()}}),
			schema("WideRecordBase", "DesignDataRecordBase", prop("W", usmap.KindInt)),
		})
}

func rulesFacts() *Facts {
	return &Facts{
		NotStored:       map[string]bool{"ThingRecordBase.Skip": true, "DesignDataRecordBase.RecordFilename": true, "DesignDataRecordBase.bIsDirty": true},
		RowAlign:        map[string]int{"WideRecordBase": 16},
		NativeSerialize: map[string]bool{"Native": true},
	}
}

func TestLeafRecordAndFacts(t *testing.T) {
	spec := tableSpec{1, 2, func(w *rowWriter) {
		for i := 0; i < 2; i++ {
			w.row()
			w.bool(i == 1)
			w.i32(int32(7 + i))
			w.u8(1)
			w.f32(2.5)
			w.tail(int64(i+1), 0, "thing", false)
		}
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	d := db.DecoderWith(rulesMap(), Options{Facts: rulesFacts()})
	rows, err := d.Decode(0, "ThingRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	want := Struct{"L": true, "B": int64(8), "N": Struct{"Mode": "EMode::On", "V": float32(2.5)}}
	if !reflect.DeepEqual(rows[1].Fields, want) {
		t.Errorf("row 1 = %#v, want %#v", rows[1].Fields, want)
	}
	cl := d.Classify()
	if c := cl[0]; c.Schema != "ThingRecordBase" || !c.Decodes || len(c.Candidates) != 1 {
		t.Errorf("classified as %+v", c)
	}
	if got := d.recordSchemas(); !reflect.DeepEqual(got, []string{"InstRecordBase", "ThingRecordBase", "WideRecordBase"}) {
		t.Errorf("recordSchemas = %v", got)
	}
	if _, err := db.DecoderWith(rulesMap(), Options{Facts: &Facts{}}).Decode(0, "ThingRecordBase"); err == nil {
		t.Error("decoded without the facts")
	}
	two := usmap.New(nil, append(rulesMap().Schemas, schema("ThingRecord2", "ThingRecordBase")))
	if _, err := db.DecoderWith(two, Options{Facts: rulesFacts()}).Decode(0, "ThingRecordBase"); err == nil || !strings.Contains(err.Error(), "derived structs") {
		t.Errorf("two leaves: %v", err)
	}
}

func TestInheritedRowTail(t *testing.T) {
	spec := tableSpec{1, 3, func(w *rowWriter) {
		w.row()
		w.i32(1)
		w.tail(1, 0, "a", false)
		w.row()
		w.i32(2)
		w.tail(2, 1, "b", false)
		w.row()
		w.i32(3)
		w.tail(3, 1, "c", false)
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	m := usmap.New(nil, append(rulesMap().Schemas, schema("PlainRecordBase", "DesignDataRecordBase", prop("W", usmap.KindInt))))
	rows, err := db.DecoderWith(m, Options{Facts: rulesFacts()}).Decode(0, "PlainRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 3 || rows[1].ParentGuid != 1 || rows[1].End-rows[1].Offset != 4+21+4 || rows[0].End-rows[0].Offset != 4+21 {
		t.Errorf("rows = %+v", rows)
	}
	bad := tableSpec{1, 1, func(w *rowWriter) {
		w.row()
		w.i32(1)
		w.i64(1)
		w.i64(1)
		w.str("x")
		w.bool(false)
		w.u32(5)
	}}
	db, _ = Parse(buildDBC(newPool(), bad))
	if _, err := db.DecoderWith(m, Options{Facts: rulesFacts()}).Decode(0, "PlainRecordBase"); err == nil || !strings.Contains(err.Error(), "inherited row's tail is 0x5") {
		t.Errorf("tail 5: %v", err)
	}
}

func TestRowAlign16(t *testing.T) {
	spec := tableSpec{1, 3, func(w *rowWriter) {
		for i := 0; i < 3; i++ {
			w.rowAligned(16)
			w.i32(int32(i))
			w.tail(int64(i+1), 0, "w", false)
		}
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	rows, err := db.DecoderWith(rulesMap(), Options{Facts: rulesFacts()}).Decode(0, "WideRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	for i, r := range rows {
		if r.Offset%16 != 0 || r.Fields["W"] != int64(i) {
			t.Errorf("row %d at %#x = %v", i, r.Offset, r.Fields)
		}
	}
	if _, err := db.DecoderWith(rulesMap(), Options{Facts: &Facts{}}).Decode(0, "WideRecordBase"); err == nil {
		t.Error("an 8-aligned walk decoded 16-aligned rows")
	}
}

func TestInstancedStructAndOptional(t *testing.T) {
	inst := func(w *rowWriter, path string, size int32, body func()) {
		w.fstr(path)
		w.i32(size)
		if body != nil {
			body()
		}
	}
	spec := tableSpec{1, 2, func(w *rowWriter) {
		w.row()
		inst(w, "/Script/Mod.Payload", 5, func() { w.i32(42); w.bool(true) })
		w.u32(1)
		w.i32(9)
		w.tail(1, 0, "a", false)
		w.row()
		inst(w, "", 0, nil)
		w.u32(0)
		w.tail(2, 0, "b", false)
	}}
	db, err := Parse(buildDBC(newPool(), spec))
	if err != nil {
		t.Fatal(err)
	}
	rows, err := db.DecoderWith(rulesMap(), Options{Facts: rulesFacts()}).Decode(0, "InstRecordBase")
	if err != nil {
		t.Fatal(err)
	}
	want0 := Struct{"Inst": Struct{"Type": "/Script/Mod.Payload", "Value": Struct{"P": int64(42), "Q": true}}, "Opt": int64(9)}
	want1 := Struct{"Inst": nil, "Opt": nil}
	if !reflect.DeepEqual(rows[0].Fields, want0) || !reflect.DeepEqual(rows[1].Fields, want1) {
		t.Errorf("rows = %#v / %#v", rows[0].Fields, rows[1].Fields)
	}
	checkInstancedStructErrors(t, inst)
}

func checkInstancedStructErrors(t *testing.T, inst func(*rowWriter, string, int32, func())) {
	t.Helper()
	for _, c := range []struct {
		name  string
		write func(w *rowWriter)
		want  string
	}{
		{"size mismatch", func(w *rowWriter) {
			inst(w, "/Script/Mod.Payload", 9, func() { w.i32(1); w.bool(false); w.u32(0) })
			w.u32(0)
		}, "take 5 bytes, the size says 9"},
		{"unknown struct", func(w *rowWriter) { inst(w, "/Script/Mod.Nope", 0, nil); w.u32(0) }, "no struct Nope"},
		{"data without a struct", func(w *rowWriter) { inst(w, "", 4, func() { w.i32(0) }); w.u32(0) }, "no struct with 4 bytes"},
		{"optional flag", func(w *rowWriter) { inst(w, "", 0, nil); w.u32(2) }, "optional set flag 2"},
	} {
		spec := tableSpec{1, 1, func(w *rowWriter) { w.row(); c.write(w); w.tail(1, 0, "x", false) }}
		db, _ := Parse(buildDBC(newPool(), spec))
		if _, err := db.DecoderWith(rulesMap(), Options{Facts: rulesFacts()}).Decode(0, "InstRecordBase"); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v, want %q", c.name, err, c.want)
		}
	}
}

func TestClassifyIgnoresEmptyTables(t *testing.T) {
	empty := tableSpec{7, 0, func(w *rowWriter) {}}
	db, err := Parse(buildDBC(newPool(), empty))
	if err != nil {
		t.Fatal(err)
	}
	cl := db.Decoder(testMap()).Classify()
	if len(cl[0].Candidates) != 0 || cl[0].Schema != "" {
		t.Errorf("empty table = %+v", cl[0])
	}
	rows, err := db.Decoder(testMap()).Decode(0, "WidgetRecordBase")
	if err != nil || len(rows) != 0 {
		t.Errorf("Decode of an empty table = %v, %v", rows, err)
	}
}
