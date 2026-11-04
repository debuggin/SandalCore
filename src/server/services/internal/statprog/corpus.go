// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"hash/crc32"
	"math"
	"sort"
	"strings"

	"aocrop/services/internal/aocexpr"
)

// Corpus is a parsed corpus file shared between Go and C++ stat engine tests.
type Corpus struct {
	Bundles     map[string][]byte
	Derived     []DerivedBundle
	Cases       []CorpusCase
	Hostiles    []CorpusHostile
	Refusals    []CorpusRefusal
	Fuzzes      []CorpusFuzz
	Profiles    []CorpusProfile
	Views       []CorpusView
	Damages     []CorpusDamage
	NodeHosts   []NodeHostCase
	GatherHosts []GatherHostCase
}

// CorpusFuzz is a differential fuzz check over N deterministic bundle mutations.
type CorpusFuzz struct {
	Bundle   string
	Seed     uint64
	N        int
	Accepted int
	Hash     uint64
}

// CorpusRefusal is a character the engine must refuse to compute.
type CorpusRefusal struct {
	Name, Bundle string
	Class, Level int
}

// DerivedBundle is a bundle switched to ModeDouble.
type DerivedBundle struct{ Name, Of string }

// CorpusCase is one character and its expected stats.
type CorpusCase struct {
	Name, Bundle string
	In           Inputs
	Gear         *CorpusGear
	State        *TableState
	Want         []CorpusWant
	Digest       uint64
	Members      int
	Faults       int
	Ignored      int
	Caps         []CorpusCap
	Fx           *CorpusFx
	Actors       []CorpusActor
}

// CorpusActor is another actor of an fxcase's hit: the inputs and state it is run with, by role.
type CorpusActor struct {
	Role  int
	In    Inputs
	State *TableState
}

func setActors(b *Bundle, ev *Evaluator, actors []CorpusActor) error {
	for _, a := range actors {
		var st aocexpr.State
		if a.State != nil {
			st = a.State
		}
		res, err := b.Run(a.In, st)
		if err != nil {
			return fmt.Errorf("actor %d: %w", a.Role, err)
		}
		if err := ev.SetActor(a.Role, res, a.In, st); err != nil {
			return err
		}
	}
	return nil
}

// CorpusFx holds the host state and expected program outputs for an fxcase.
type CorpusFx struct {
	Host       TableHostSpec
	Progs      []CorpusProg
	Retail     []CorpusRetail
	IgnoredFx  int
	EvalFaults int
}

// CorpusRetail is a number from retail captures an fxcase must reproduce.
type CorpusRetail struct {
	Kind   string
	ID     int64
	Value  float64
	Digits int
}

func roundTo(x float64, digits int) float64 {
	p := math.Pow(10, float64(digits))
	return math.Round(x*p) / p
}

func (r CorpusRetail) holds(got float64) bool {
	if r.Kind == "stat" {
		return roundTo(got, r.Digits) == r.Value
	}
	return got == r.Value
}

// TableHostSpec specifies the deterministic host inputs for expression evaluation.
type TableHostSpec struct {
	Seed        uint64
	Progress    map[string]float64
	Inst        map[aocexpr.Ref]float64
	Tags, Flags map[string]bool
	Targets     float64
	XPDebt      float64
	Fns         map[string]float64
}

// Host builds the TableHost a replay runs expressions with.
func (h TableHostSpec) Host() *TableHost {
	return &TableHost{
		Progress:     h.Progress,
		ByInstigator: h.Inst,
		Rand:         SplitMix{State: h.Seed},
		Tags:         h.Tags,
		Flags:        h.Flags,
		Targets:      h.Targets,
		XPDebt:       h.XPDebt,
		Fns:          h.Fns,
	}
}

// CorpusProg is the expected value of an expression program.
type CorpusProg struct {
	Index int
	Value uint64
}

// CorpusGear is the worn items a case's mods and tags come from, and the rounding of their values.
type CorpusGear struct {
	Rounding Rounding
	Items    []Item
}

// CorpusWant is the expected value of a stat.
type CorpusWant struct {
	Guid  int64
	Value uint64
}

// CorpusCap is how a value compares with a captured float32.
type CorpusCap struct {
	Guid  int64
	Bits  uint32
	Match string
}

// CorpusHostile is a blob a reader must refuse, as an edit of the "tiny" bundle.
type CorpusHostile struct {
	Name  string
	Fix   bool
	Blob  bool
	Pos   int
	Del   int
	Ins   []byte
	Whole []byte
}

// Digest computes the FNV-1a 64 hash over every stat result in order.
func Digest(res *Result) uint64 {
	h := uint64(14695981039346656037)
	put := func(b []byte) {
		for _, c := range b {
			h ^= uint64(c)
			h *= 1099511628211
		}
	}
	var buf [8]byte
	u64 := func(v uint64) {
		binary.LittleEndian.PutUint64(buf[:], v)
		put(buf[:])
	}
	for _, s := range res.Stats {
		u64(uint64(s.Guid))
		u64(math.Float64bits(s.Value))
		u64(math.Float64bits(s.Base))
		u64(math.Float64bits(s.Equip))
		if s.HasRegen {
			put([]byte{1})
			u64(math.Float64bits(s.Regen))
		} else {
			put([]byte{0})
		}
	}
	return h
}

type corpusWriter struct{ bytes.Buffer }

func (w *corpusWriter) line(format string, args ...any) { fmt.Fprintf(w, format+"\n", args...) }

func (w *corpusWriter) bundle(name string, enc []byte) {
	w.line("bundle %s %d", name, len(enc))
	s := base64.StdEncoding.EncodeToString(enc)
	for len(s) > 0 {
		n := min(len(s), 100)
		w.line("%s", s[:n])
		s = s[n:]
	}
	w.line("endbundle")
}

func f64hex(f float64) string { return fmt.Sprintf("%016x", math.Float64bits(f)) }

type caseWriteSpec struct {
	name, bundleName string
	b                *Bundle
	in               Inputs
	st               *TableState
	caps             []CorpusCap
	full             bool
	gear             []CorpusGear
}

func (w *corpusWriter) caseOf(cs caseWriteSpec) error {
	var state aocexpr.State
	if cs.st != nil {
		state = cs.st
	}
	res, err := cs.b.Run(cs.in, state)
	if err != nil {
		return fmt.Errorf("case %s: %w", cs.name, err)
	}
	w.line("case %s %s %d %d", cs.name, cs.bundleName, cs.in.Class, cs.in.Level)
	for _, m := range cs.in.Mods {
		w.line("mod %d %s", m.Stat, f64hex(m.Value))
	}
	for _, t := range cs.in.Equipped {
		w.line("tag %s %s", t.Slot, t.Tag)
	}
	for _, g := range cs.gear {
		w.line("rounding %s", [...]string{"none", "halfup"}[g.Rounding])
		for _, it := range g.Items {
			w.line("item %d %d %s", it.Record, it.Quality, it.Slot)
		}
	}
	if cs.st != nil {
		writeState(w, cs.st)
	}
	if cs.full {
		for _, s := range res.Stats {
			w.line("want %d %s", s.Guid, f64hex(s.Value))
		}
	}
	w.line("digest %016x %d %d %d", Digest(res), len(res.Stats), res.Faults, res.IgnoredMods)
	for _, c := range cs.caps {
		w.line("cap %d %08x %s", c.Guid, c.Bits, c.Match)
	}
	w.line("end")
	return nil
}

type fxWriteSpec struct {
	name, bundleName string
	b                *Bundle
	in               Inputs
	st               *TableState
	actors           []CorpusActor
	full             bool
	spec             TableHostSpec
	progs            []int
	retail           []CorpusRetail
}

func (w *corpusWriter) fxCaseOf(fs fxWriteSpec) error {
	return w.fxCaseWith(fs)
}

func (w *corpusWriter) fxCaseWith(fs fxWriteSpec) error {
	var state aocexpr.State
	if fs.st != nil {
		state = fs.st
	}
	res, err := fs.b.Run(fs.in, state)
	if err != nil {
		return fmt.Errorf("fxcase %s: %w", fs.name, err)
	}
	w.line("fxcase %s %s %d %d", fs.name, fs.bundleName, fs.in.Class, fs.in.Level)
	writeInputs(w, fs.in, fs.st)
	if fs.full {
		for _, s := range res.Stats {
			w.line("want %d %s", s.Guid, f64hex(s.Value))
		}
	}
	w.line("digest %016x %d %d %d", Digest(res), len(res.Stats), res.Faults, res.IgnoredMods)
	w.line("ignoredfx %d", res.IgnoredEffectMods)
	for _, a := range fs.actors {
		w.line("actor %d %d %d", a.Role, a.In.Class, a.In.Level)
		writeInputs(w, a.In, a.State)
		w.line("endactor")
	}
	if len(fs.progs) > 0 {
		w.writeHostSpec(fs.spec)
		if err := w.evalCaseProgs(fs, res, state); err != nil {
			return err
		}
	}
	for _, r := range fs.retail {
		if r.Kind != "stat" {
			continue
		}
		i, ok := fs.b.StatIndex(r.ID)
		if !ok || !r.holds(res.Values[i]) {
			return fmt.Errorf("fxcase %s: the retail value %v of stat %d is %v here", fs.name, r.Value, r.ID, res.Values[i])
		}
		w.line("retail stat %d %s %d", r.ID, f64hex(r.Value), r.Digits)
	}
	w.line("end")
	return nil
}

func (w *corpusWriter) writeHostSpec(spec TableHostSpec) {
	w.line("host seed %d", spec.Seed)
	var groups []string
	for g := range spec.Progress {
		groups = append(groups, g)
	}
	sort.Strings(groups)
	for _, g := range groups {
		name := g
		if name == "" {
			name = "-"
		}
		w.line("host progress %s %s", name, f64hex(spec.Progress[g]))
	}
	for _, r := range sortedStateRefs(spec.Inst) {
		w.line("host inst %d %d %s", r.TypeID, r.Guid, f64hex(spec.Inst[r]))
	}
	writeHostFlagSet(w, "hittag", spec.Tags)
	writeHostFlagSet(w, "hitflag", spec.Flags)
	if spec.Targets != 0 {
		w.line("host targets %s", f64hex(spec.Targets))
	}
	if spec.XPDebt != 0 {
		w.line("host xpdebt %s", f64hex(spec.XPDebt))
	}
	var fkeys []string
	for k := range spec.Fns {
		fkeys = append(fkeys, k)
	}
	sort.Strings(fkeys)
	for _, k := range fkeys {
		fn, name, _ := strings.Cut(k, "|")
		if name == "" {
			name = "-"
		}
		w.line("host fn %s %s %s", fn, name, f64hex(spec.Fns[k]))
	}
}

func writeHostFlagSet(w *corpusWriter, kind string, set map[string]bool) {
	var names []string
	for n, on := range set {
		if on {
			names = append(names, n)
		}
	}
	sort.Strings(names)
	for _, n := range names {
		w.line("host %s %s", kind, n)
	}
}

func (w *corpusWriter) evalCaseProgs(fs fxWriteSpec, res *Result, state aocexpr.State) error {
	ev, err := fs.b.NewEvaluator(res, fs.in, state)
	if err != nil {
		return fmt.Errorf("fxcase %s: %w", fs.name, err)
	}
	if err := setActors(fs.b, ev, fs.actors); err != nil {
		return fmt.Errorf("fxcase %s: %w", fs.name, err)
	}
	host := fs.spec.Host()
	got := map[int]float64{}
	for _, p := range fs.progs {
		v, err := ev.Eval(p, host)
		if err != nil {
			return fmt.Errorf("fxcase %s: program %d: %w", fs.name, p, err)
		}
		got[p] = v
		w.line("prog %d %s", p, f64hex(v))
	}
	w.line("evalfaults %d", ev.Faults())
	for _, r := range fs.retail {
		if r.Kind != "prog" {
			continue
		}
		v, ok := got[int(r.ID)]
		if !ok || !r.holds(v) {
			return fmt.Errorf("fxcase %s: the retail value %v of program %d is %v here (evaluated: %v)", fs.name, r.Value, r.ID, v, ok)
		}
		w.line("retail prog %d %s", r.ID, f64hex(r.Value))
	}
	return nil
}

func writeInputs(w *corpusWriter, in Inputs, st *TableState) {
	for _, m := range in.Mods {
		w.line("mod %d %s", m.Stat, f64hex(m.Value))
	}
	for _, t := range in.Equipped {
		w.line("tag %s %s", t.Slot, t.Tag)
	}
	if st != nil {
		writeState(w, st)
	}
	for _, e := range in.Effects {
		w.line("fx %d %d %d", e.Effect.TypeID, e.Effect.Guid, e.Stacks)
		for _, t := range e.Tags {
			w.line("fxtag %s", t)
		}
		for _, m := range e.Mods {
			w.line("fxmod %d %s", m.Stat, f64hex(m.Value))
		}
	}
	for _, r := range in.Ranks {
		w.line("rank %d %d %d %d", r.Class.TypeID, r.Class.Guid, r.Skill.TypeID, r.Skill.Guid)
	}
}

func sortedStateRefs(m map[aocexpr.Ref]float64) []aocexpr.Ref {
	var out []aocexpr.Ref
	for r := range m {
		out = append(out, r)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].TypeID != out[j].TypeID {
			return out[i].TypeID < out[j].TypeID
		}
		return out[i].Guid < out[j].Guid
	})
	return out
}

func writeState(w *corpusWriter, st *TableState) {
	for _, r := range sortedStateRefs(st.Effects) {
		w.line("state effect %d %d %s", r.TypeID, r.Guid, f64hex(st.Effects[r]))
	}
	var tags []string
	for t := range st.Tags {
		tags = append(tags, t)
	}
	sort.Strings(tags)
	for _, t := range tags {
		w.line("state tag %s %s", t, f64hex(st.Tags[t]))
	}
	var skills [][2]aocexpr.Ref
	for k, v := range st.Skills {
		if v {
			skills = append(skills, k)
		}
	}
	sort.Slice(skills, func(i, j int) bool {
		return fmt.Sprint(skills[i]) < fmt.Sprint(skills[j])
	})
	for _, k := range skills {
		w.line("state skill %d %d %d %d", k[0].TypeID, k[0].Guid, k[1].TypeID, k[1].Guid)
	}
	if st.FlagCombat {
		w.line("state combat")
	}
	if st.FlagMoving {
		w.line("state moving")
	}
	if st.FlagUnkillable {
		w.line("state unkillable")
	}
	writeStateProfAndNpc(w, st)
}

func writeStateProfAndNpc(w *corpusWriter, st *TableState) {
	if st.Actor != 0 {
		w.line("state actor %s", f64hex(st.Actor))
	}
	var profs []string
	for p := range st.Professions {
		profs = append(profs, p)
	}
	sort.Strings(profs)
	for _, p := range profs {
		w.line("state prof %s %s", p, f64hex(st.Professions[p]))
	}
	for _, r := range sortedStateRefs(st.Consumed) {
		w.line("state consumed %d %d %s", r.TypeID, r.Guid, f64hex(st.Consumed[r]))
	}
	if st.EliteRank != 0 {
		w.line("state npc %s", f64hex(st.EliteRank))
	}
	for i, d := range st.NpcDist {
		if d != 0 {
			w.line("state dist %d %s", i, f64hex(d))
		}
	}
}

func diffEdit(orig, blob []byte) (pos, del int, ins []byte) {
	p := 0
	for p < len(orig) && p < len(blob) && orig[p] == blob[p] {
		p++
	}
	s := 0
	for s < len(orig)-p && s < len(blob)-p && orig[len(orig)-1-s] == blob[len(blob)-1-s] {
		s++
	}
	return p, len(orig) - p - s, blob[p : len(blob)-s]
}

// ApplyEdit makes the blob of a hostile line from the bundle it edits.
func ApplyEdit(orig []byte, h CorpusHostile) ([]byte, bool) {
	if h.Blob {
		return append([]byte(nil), h.Ins...), true
	}
	if h.Pos < 0 || h.Del < 0 || h.Pos > len(orig) || h.Pos+h.Del > len(orig) {
		return nil, false
	}
	out := append(append(append([]byte(nil), orig[:h.Pos]...), h.Ins...), orig[h.Pos+h.Del:]...)
	if h.Fix && len(out) >= headerSize {
		binary.LittleEndian.PutUint32(out[8:], uint32(len(out)-headerSize))
		binary.LittleEndian.PutUint32(out[12:], crc32.ChecksumIEEE(out[headerSize:]))
	}
	return out, true
}

const maxWholeHostile = 16384

func (w *corpusWriter) hostileLine(tiny []byte, h Hostile) bool {
	well := len(h.Blob) >= headerSize && int64(binary.LittleEndian.Uint32(h.Blob[8:])) == int64(len(h.Blob)-headerSize) &&
		binary.LittleEndian.Uint32(h.Blob[12:]) == crc32.ChecksumIEEE(h.Blob[headerSize:]) && bytes.Equal(h.Blob[:8], tiny[:8])
	var e CorpusHostile
	e.Name = strings.NewReplacer(" ", "_", ":", "").Replace(h.Name)
	if well {
		e.Fix = true
		p, d, ins := diffEdit(tiny[headerSize:], h.Blob[headerSize:])
		e.Pos, e.Del, e.Ins = headerSize+p, d, ins
	} else {
		e.Pos, e.Del, e.Ins = diffEdit(tiny, h.Blob)
	}
	if len(e.Ins) > 512 {
		if len(h.Blob) > maxWholeHostile {
			return false
		}
		e = CorpusHostile{Name: e.Name, Blob: true, Ins: append([]byte(nil), h.Blob...)}
	}
	if got, ok := ApplyEdit(tiny, e); !ok || !bytes.Equal(got, h.Blob) {
		return false
	}
	ins := "-"
	if len(e.Ins) > 0 {
		ins = hex.EncodeToString(e.Ins)
	}
	fix := 0
	if e.Fix {
		fix = 1
	}
	if e.Blob {
		fix = 2
	}
	w.line("hostile %s %d %d %d %s", e.Name, fix, e.Pos, e.Del, ins)
	return true
}
