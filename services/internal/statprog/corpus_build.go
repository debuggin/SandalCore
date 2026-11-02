// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"embed"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/content"
)

//go:embed testdata/bard_l1.json testdata/ranger_l20.json
var captures embed.FS

type corpusBundles struct {
	g                          *Graph
	gear                       *Gear
	exprs                      []Expr
	exprIDs                    []int64
	mainEnc, tinyEnc, tinySEnc []byte
	mainB, doubleB             *Bundle
	tinyB, tinySB              *Bundle
	mainRes, tinyRes, tinySRes []ExprResult
	kitItems                   map[string][]Item
}

func loadCorpusBundles(dbPath string) (*corpusBundles, error) {
	g, err := LoadContentDB(dbPath)
	if err != nil {
		return nil, err
	}
	gear, err := OpenGear(dbPath)
	if err != nil {
		return nil, err
	}
	exprs, exprIDs, err := LoadExprs(dbPath)
	if err != nil {
		gear.Close()
		return nil, err
	}
	main, mainRes, err := CompileExprs(g, Options{}, exprs)
	if err != nil {
		gear.Close()
		return nil, err
	}
	tinyF, tinyRes, err := TinyBundle(true, ModeFloat32)
	if err != nil {
		gear.Close()
		return nil, err
	}
	tinyS, tinySRes, err := TinyBundle(false, ModeSingle)
	if err != nil {
		gear.Close()
		return nil, err
	}
	cb := &corpusBundles{
		g: g, gear: gear, exprs: exprs, exprIDs: exprIDs,
		mainEnc: main.Encode(), tinyEnc: tinyF.Encode(), tinySEnc: tinyS.Encode(),
		mainRes: mainRes, tinyRes: tinyRes, tinySRes: tinySRes,
		kitItems: map[string][]Item{},
	}
	if err := cb.decodeAll(); err != nil {
		gear.Close()
		return nil, err
	}
	return cb, nil
}

func (cb *corpusBundles) decodeAll() error {
	var err error
	mustDecode := func(enc []byte) *Bundle {
		b, derr := Decode(enc)
		if derr != nil && err == nil {
			err = derr
		}
		return b
	}
	cb.mainB = mustDecode(cb.mainEnc)
	cb.tinyB = mustDecode(cb.tinyEnc)
	cb.tinySB = mustDecode(cb.tinySEnc)
	doubleEnc := append([]byte(nil), cb.mainEnc...)
	binary.LittleEndian.PutUint16(doubleEnc[6:], FlagDouble)
	cb.doubleB = mustDecode(doubleEnc)
	return err
}

func (cb *corpusBundles) kitInputs(class string, level int, rd Rounding) (Inputs, error) {
	kit, err := cb.gear.StartingKit(class)
	if err != nil {
		return Inputs{}, err
	}
	cb.kitItems[class] = kit
	mods, err := cb.gear.Mods(kit, rd)
	if err != nil {
		return Inputs{}, err
	}
	tags, err := cb.gear.Tags(kit)
	if err != nil {
		return Inputs{}, err
	}
	return Inputs{Class: classEnum(class), Level: level, Mods: mods, Equipped: tags}, nil
}

// BuildCorpus compiles the real graph of the content.db at dbPath and writes the corpus.
func BuildCorpus(dbPath string) ([]byte, error) {
	cb, err := loadCorpusBundles(dbPath)
	if err != nil {
		return nil, err
	}
	defer cb.gear.Close()

	var w corpusWriter
	w.line("# statprog corpus v1, written by `go run ./internal/statprog/cmd/statprog corpus` (see internal/statprog/corpus.go).")
	w.line("# Do not edit: `corpus --check` fails when this file is not what the compiler and the engine produce.")
	w.bundle("main", cb.mainEnc)
	w.line("derive main_double main double")
	w.bundle("tiny", cb.tinyEnc)
	w.bundle("tiny_single", cb.tinySEnc)
	w.bundle("abuse", AbuseBundle())
	w.line("refuse abuse_run abuse 1 1")

	if err := w.writeCaptureCases(cb); err != nil {
		return nil, err
	}
	if err := w.writeKitAndBareCases(cb); err != nil {
		return nil, err
	}
	if err := w.writeTinyAndRealFxCases(dbPath, cb); err != nil {
		return nil, err
	}
	npcIn, npcSt, err := w.writeNpcAndActorCases(dbPath, cb)
	if err != nil {
		return nil, err
	}
	if err := w.writeDamageCorpusCases(dbPath, cb, npcIn, npcSt); err != nil {
		return nil, err
	}
	w.writeNodeAndGatherCases()
	if err := w.writeFuzzAndHostileCases(cb); err != nil {
		return nil, err
	}
	return w.Bytes(), nil
}

func (w *corpusWriter) writeCaptureCases(cb *corpusBundles) error {
	for _, cp := range []struct {
		file, name string
		rd         Rounding
	}{{"testdata/bard_l1.json", "bard_l1", RoundNone}, {"testdata/ranger_l20.json", "ranger_l20", RoundHalfUp}} {
		raw, err := captures.ReadFile(cp.file)
		if err != nil {
			return err
		}
		var c capture
		if err := json.Unmarshal(raw, &c); err != nil {
			return err
		}
		items := c.gearItems()
		mods, err := cb.gear.Mods(items, cp.rd)
		if err != nil {
			return err
		}
		tags, err := cb.gear.Tags(items)
		if err != nil {
			return err
		}
		in := Inputs{Class: classEnum(c.Class), Level: c.Level, Mods: mods, Equipped: tags}
		res, err := cb.mainB.Run(in, nil)
		if err != nil {
			return err
		}
		got := map[int64]float64{}
		for _, s := range res.Stats {
			got[s.Guid] = s.Value
		}
		var caps []CorpusCap
		for _, st := range c.Stats {
			m := [...]string{"exact", "near", "miss"}[compare(got[st.Guid], f32(st.Bits))]
			caps = append(caps, CorpusCap{st.Guid, st.Bits, m})
		}
		gear := []CorpusGear{{Rounding: cp.rd, Items: items}}
		if err := w.caseOf(caseWriteSpec{name: cp.name, bundleName: "main", b: cb.mainB, in: in, caps: caps, full: true, gear: gear}); err != nil {
			return err
		}
		if err := w.caseOf(caseWriteSpec{name: cp.name + "_double", bundleName: "main_double", b: cb.doubleB, in: in}); err != nil {
			return err
		}
	}
	return nil
}

func (w *corpusWriter) writeKitAndBareCases(cb *corpusBundles) error {
	for _, cl := range cb.mainB.Classes {
		if int(cl.Enum) >= NpcClassBase {
			continue
		}
		cname := cb.mainB.Strings[cl.Name]
		for _, level := range []int{1, 20} {
			in, err := cb.kitInputs(cname, level, RoundNone)
			if err != nil {
				return err
			}
			gear := []CorpusGear{{Rounding: RoundNone, Items: cb.kitItems[cname]}}
			spec := caseWriteSpec{
				name: fmt.Sprintf("kit_%s_l%d", strings.ToLower(cname), level), bundleName: "main",
				b: cb.mainB, in: in, full: cname == "Tank" && level == 1, gear: gear,
			}
			if err := w.caseOf(spec); err != nil {
				return err
			}
		}
	}
	for _, bare := range []struct {
		class string
		level int
	}{{"Mage", 50}, {"Tank", 7}, {"Cleric", 33}, {"Summoner", 2}, {"Rogue", 19}, {"Fighter", 13}, {"Bard", MaxLevel}} {
		in := Inputs{Class: classEnum(bare.class), Level: bare.level}
		name := fmt.Sprintf("bare_%s_l%d", strings.ToLower(bare.class), bare.level)
		if err := w.caseOf(caseWriteSpec{name: name, bundleName: "main", b: cb.mainB, in: in}); err != nil {
			return err
		}
	}
	fighter, err := cb.kitInputs("Fighter", 20, RoundNone)
	if err != nil {
		return err
	}
	fighter.Mods = append(fighter.Mods, Mod{Stat: 1, Value: 5}, Mod{Stat: fighter.Mods[0].Stat, Value: 2.5}, Mod{Stat: fighter.Mods[0].Stat, Value: -0.5})
	st := &TableState{FlagCombat: true, FlagMoving: true, Professions: map[string]float64{"Fishing": 6}, Actor: 7}
	return w.caseOf(caseWriteSpec{name: "fighter_l20_state", bundleName: "main", b: cb.mainB, in: fighter, st: st})
}

func progsOf(res []ExprResult) []int {
	var out []int
	for _, r := range res {
		if r.Program >= 0 {
			out = append(out, r.Program)
		}
	}
	return out
}

func (w *corpusWriter) writeTinyAndRealFxCases(dbPath string, cb *corpusBundles) error {
	for _, tc := range []struct {
		name         string
		class, level int
		rich         bool
	}{{"tiny_ranger_l20", 1, 20, false}, {"tiny_bard_l20", 2, 20, false}, {"tiny_ranger_l5", 1, 5, false}, {"tiny_ranger_l40", 1, 40, false},
		{"tiny_bard_l11_rich", 2, 11, true}, {"tiny_ranger_l50_rich", 1, 50, true}, {"tiny_bard_l1_rich", 2, 1, true}} {
		in := tinyInputs(tc.class, tc.level, tc.rich)
		var st *TableState
		if tc.rich {
			st = richState()
		}
		if err := w.caseOf(caseWriteSpec{name: tc.name, bundleName: "tiny", b: cb.tinyB, in: in, st: st, full: true}); err != nil {
			return err
		}
	}
	for _, level := range []int{1, 20, 41} {
		name := fmt.Sprintf("tiny_single_bard_l%d", level)
		if err := w.caseOf(caseWriteSpec{name: name, bundleName: "tiny_single", b: cb.tinySB, in: tinyInputs(2, level, true), st: richState(), full: true}); err != nil {
			return err
		}
	}
	for _, fc := range tinyFxCases() {
		b, res, name := cb.tinyB, cb.tinyRes, "tiny"
		if fc.single {
			b, res, name = cb.tinySB, cb.tinySRes, "tiny_single"
		}
		if err := w.fxCaseOf(fxWriteSpec{name: fc.name, bundleName: name, b: b, in: fc.in, st: fc.state, full: true, spec: fc.host, progs: progsOf(res)}); err != nil {
			return err
		}
	}
	if len(cb.exprs) == 0 {
		return nil
	}
	return w.writeRealFxList(dbPath, cb)
}

func (w *corpusWriter) writeRealFxList(dbPath string, cb *corpusBundles) error {
	real, err := realFxCases(dbPath, cb.mainB, cb.mainRes, cb.exprIDs)
	if err != nil {
		return err
	}
	var progs []int
	for _, p := range cb.mainB.Exprs {
		progs = append(progs, int(p))
	}
	for _, rc := range real {
		b := cb.mainB
		if rc.bundle == "main_double" {
			b = cb.doubleB
		}
		spec := fxWriteSpec{
			name: rc.name, bundleName: rc.bundle, b: b, in: rc.in, st: rc.state,
			full: rc.name == "real_bard_l25_sprint", spec: rc.host, progs: progs, retail: rc.retail,
		}
		if err := w.fxCaseOf(spec); err != nil {
			return err
		}
	}
	return nil
}

func (w *corpusWriter) writeNpcAndActorCases(dbPath string, cb *corpusBundles) (map[string]Inputs, map[string]*TableState, error) {
	dists := [][8]float64{
		{0.25, 0.125, 0.125, 0.125, 0.125, 0.0625, 0.0625, 0.125},
		{0.5, 0.0625, 0.0625, 0.0625, 0.0625, 0.0625, 0.0625, 0.125},
		{0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125},
	}
	npcIn := map[string]Inputs{}
	npcSt := map[string]*TableState{}
	for _, cl := range cb.mainB.Classes {
		if int(cl.Enum) < NpcClassBase {
			continue
		}
		cname := cb.mainB.Strings[cl.Name]
		for k, level := range []int{1, 10, 25} {
			in := Inputs{Class: int(cl.Enum), Level: level}
			st := &TableState{EliteRank: float64(k), NpcDist: dists[k]}
			name := fmt.Sprintf("npc_%s_l%d", strings.ToLower(cname), level)
			if err := w.caseOf(caseWriteSpec{name: name, bundleName: "main", b: cb.mainB, in: in, st: st, full: k == 1}); err != nil {
				return nil, nil, err
			}
			npcIn[name], npcSt[name] = in, st
		}
	}
	if err := w.writeActorFxCases(dbPath, cb, npcIn, npcSt); err != nil {
		return nil, nil, err
	}
	return npcIn, npcSt, nil
}

func (w *corpusWriter) writeActorFxCases(dbPath string, cb *corpusBundles, npcIn map[string]Inputs, npcSt map[string]*TableState) error {
	for i, fc := range tinyFxCases() {
		if i > 1 || fc.single {
			continue
		}
		actors := []CorpusActor{
			{Role: aocexpr.RoleTarget, In: tinyInputs(1, 10, true), State: richState()},
			{Role: aocexpr.RoleActiveHarm, In: tinyInputs(2, 5, false)},
		}
		spec := fxWriteSpec{
			name: fc.name + "_actors", bundleName: "tiny", b: cb.tinyB,
			in: fc.in, st: fc.state, actors: actors, full: true, spec: fc.host, progs: progsOf(cb.tinyRes),
		}
		if err := w.fxCaseWith(spec); err != nil {
			return err
		}
	}
	if len(cb.exprs) == 0 {
		return nil
	}
	real, err := realFxCases(dbPath, cb.mainB, cb.mainRes, cb.exprIDs)
	if err != nil {
		return err
	}
	var progs []int
	for _, p := range cb.mainB.Exprs {
		progs = append(progs, int(p))
	}
	npcMid := "npc_npc_l10"
	for _, rc := range real {
		if rc.name != "real_bard_l25_sprint" && rc.name != "real_fighter_l50" {
			continue
		}
		actors := []CorpusActor{
			{Role: aocexpr.RoleTarget, In: npcIn[npcMid], State: npcSt[npcMid]},
			{Role: aocexpr.RoleActiveHarm, In: Inputs{Class: classEnum("Fighter"), Level: 20}},
		}
		spec := fxWriteSpec{
			name: rc.name + "_vs_npc", bundleName: rc.bundle, b: cb.mainB,
			in: rc.in, st: rc.state, actors: actors, spec: rc.host, progs: progs,
		}
		if err := w.fxCaseWith(spec); err != nil {
			return err
		}
	}
	return nil
}

func (w *corpusWriter) writeDamageCorpusCases(dbPath string, cb *corpusBundles, npcIn map[string]Inputs, npcSt map[string]*TableState) error {
	cdb, err := content.Open(dbPath)
	if err != nil {
		return err
	}
	combat, err := cdb.Combat()
	cdb.Close()
	if err != nil {
		return err
	}
	profile, err := ProfileFromCombat(combat)
	if err != nil {
		return err
	}
	viewOf := func(in Inputs, st *TableState) (MapView, error) {
		var state aocexpr.State
		if st != nil {
			state = st
		}
		res, err := cb.mainB.Run(in, state)
		if err != nil {
			return nil, err
		}
		v := MapView{}
		for _, s := range res.Stats {
			v[s.Guid] = s.Value
		}
		return v, nil
	}
	fighterKit, err := cb.kitInputs("Fighter", 20, RoundNone)
	if err != nil {
		return err
	}
	mageKit, err := cb.kitInputs("Mage", 25, RoundNone)
	if err != nil {
		return err
	}
	var views []CorpusView
	for _, v := range []struct {
		name string
		in   Inputs
		st   *TableState
	}{{"fighter_l20", fighterKit, nil}, {"mage_l25", mageKit, nil}, {"npc_l10", npcIn["npc_npc_l10"], npcSt["npc_npc_l10"]}, {"npc_l25", npcIn["npc_npc_l25"], npcSt["npc_npc_l25"]}} {
		mv, err := viewOf(v.in, v.st)
		if err != nil {
			return err
		}
		views = append(views, CorpusView{Name: v.name, V: mv})
	}
	views = append(views, CorpusView{Name: "synthetic_a", V: syntheticView(profile, 0xA11CE)}, CorpusView{Name: "synthetic_b", V: syntheticView(profile, 0xB0B)})
	w.damageCases("real", profile, views)
	return nil
}

func (w *corpusWriter) writeNodeAndGatherCases() {
	for _, nc := range nodeHostCases() {
		w.line("nodehost %s", nc.Name)
		w.line("nodes %d", len(nc.World.Types))
		for r := range nc.World.Types {
			w.line("node %d %d %d", r, nc.World.Types[r], nc.World.Levels[r])
		}
		w.line("ctx %d %d %d %d", nc.World.Node, nc.World.Biome, nc.World.ActorNode, nc.World.PlayerNode)
		for _, q := range nc.Queries {
			args := ""
			for _, a := range q.Args {
				args += " " + strconv.Itoa(a)
			}
			w.line("q %d%s = %d", q.Fn, args, q.Want)
		}
		w.line("end")
	}
	for _, gc := range gatherHostCases() {
		w.line("gatherhost %s", gc.Name)
		w.line("quality %s", f64hex(gc.Ctx.Quality))
		w.line("rarity %d", gc.Ctx.ItemRarity)
		w.line("tier %d", gc.Ctx.SourceTier)
		w.line("duration %s", f64hex(gc.Ctx.BaseDuration))
		var stats []int64
		for g := range gc.Ctx.Ratings {
			stats = append(stats, g)
		}
		sort.Slice(stats, func(i, j int) bool { return stats[i] < stats[j] })
		for _, g := range stats {
			w.line("rating %d %s", g, f64hex(gc.Ctx.Ratings[g]))
		}
		for _, q := range gc.Queries {
			name := q.Name
			if name == "" {
				name = "-"
			}
			w.line("q %d %s %d = %s", q.Fn, name, q.Role, f64hex(q.Want))
		}
		w.line("end")
	}
}

func (w *corpusWriter) writeFuzzAndHostileCases(cb *corpusBundles) error {
	for _, fz := range []struct {
		name string
		enc  []byte
		seed uint64
		n    int
	}{{"tiny", cb.tinyEnc, 0x9E3779B97F4A7C15, 20000}, {"tiny_single", cb.tinySEnc, 12345, 5000}, {"main", cb.mainEnc, 88172645463325252, 3000}} {
		acc, h := FuzzParity(fz.enc, fz.seed, fz.n)
		w.line("fuzz %s %d %d %d %016x", fz.name, fz.seed, fz.n, acc, h)
	}
	hostile, err := HostileCases()
	if err != nil {
		return err
	}
	for _, h := range hostile {
		w.hostileLine(cb.tinyEnc, h)
	}
	return nil
}

func classEnum(name string) int {
	return map[string]int{"Ranger": 1, "Bard": 2, "Cleric": 3, "Fighter": 4, "Rogue": 5, "Summoner": 6, "Tank": 7, "Mage": 8}[name]
}

type capture struct {
	Class  string `json:"class"`
	Level  int    `json:"level"`
	Source string `json:"source"`
	Items  []struct {
		Slot    int   `json:"slot"`
		Record  int64 `json:"record"`
		Quality int   `json:"quality"`
	} `json:"items"`
	Stats []struct {
		Guid      int64  `json:"guid"`
		Array     string `json:"array"`
		Bits      uint32 `json:"bits"`
		BaseBits  uint32 `json:"base_bits"`
		EquipBits uint32 `json:"equip_bits"`
		HasBucket bool
	} `json:"stats"`
}

type match int

const (
	matchExact match = iota
	matchNear
	matchMiss
)

func compare(got, want float64) match {
	g := float64(float32(got))
	if g == want {
		return matchExact
	}
	if math.Abs(g-want) <= 1e-6*math.Max(1, math.Abs(want)) {
		return matchNear
	}
	return matchMiss
}

func f32(bits uint32) float64 { return float64(math.Float32frombits(bits)) }

func (c *capture) gearItems() []Item {
	out := make([]Item, len(c.Items))
	for i, it := range c.Items {
		out[i] = Item{Record: it.Record, Quality: it.Quality, Slot: SlotName(it.Slot)}
	}
	return out
}

type tinyFxCase struct {
	name   string
	in     Inputs
	state  *TableState
	host   TableHostSpec
	single bool
}

func tinyFxCases() []tinyFxCase {
	stat := func(n int) int64 { return int64(1000 + n) }
	eff := func(n int64) aocexpr.Ref { return aocexpr.Ref{TypeID: tinyEffectType, Guid: n} }
	rank := func(skill int64) SkillRank {
		return SkillRank{Class: aocexpr.Ref{TypeID: tinyClassType, Guid: 2}, Skill: aocexpr.Ref{TypeID: tinySkillType, Guid: skill}}
	}
	forms := func(in Inputs) Inputs {
		in.Effects = []ActiveEffect{
			{Effect: eff(7), Stacks: 2, Tags: []string{"pet.summon.wolf"}, Mods: []EffectMod{{stat(19), 0.25}, {stat(20), 1.05}, {stat(17), 0.1}, {stat(1), 4}}},
			{Effect: eff(8), Stacks: 1, Mods: []EffectMod{{stat(19), 0.125}, {stat(20), 1.1}, {stat(17), 0.5}, {424242, 7}}},
			{Effect: eff(9), Stacks: 0, Mods: []EffectMod{{stat(19), 100}}},
		}
		return in
	}
	hosts := tinyHosts()
	return []tinyFxCase{
		{name: "tiny_fx_none_bard_l20", in: tinyInputs(2, 20, false), host: hosts[0]},
		{name: "tiny_fx_forms_bard_l20", in: forms(tinyInputs(2, 20, false)), host: hosts[1]},
		{name: "tiny_fx_forms_ranger_l11_rich", in: func() Inputs {
			in := forms(tinyInputs(1, 11, true))
			in.Ranks = []SkillRank{rank(3), rank(5)}
			return in
		}(), state: richState(), host: hosts[2]},
		{name: "tiny_fx_ranks_bard_l41", in: func() Inputs {
			in := tinyInputs(2, 41, false)
			in.Ranks = []SkillRank{rank(3), rank(4), rank(5)}
			in.Effects = []ActiveEffect{{Effect: eff(1), Stacks: 3, Tags: []string{"pet.summon"}, Mods: []EffectMod{{stat(7), 1000}}}}
			return in
		}(), host: hosts[1]},
		{name: "tiny_fx_limits_ranger_l5", in: func() Inputs {
			in := tinyInputs(1, 5, false)
			in.Effects = []ActiveEffect{{Effect: eff(1), Stacks: 4000, Mods: []EffectMod{{stat(19), 1}, {stat(17), 0.999}, {stat(20), 0.0001}}}}
			return in
		}(), host: hosts[0]},
		{name: "tiny_fx_single_bard_l20", in: forms(tinyInputs(2, 20, true)), state: richState(), host: hosts[1], single: true},
		{name: "tiny_fx_single_ranger_l50", in: func() Inputs {
			in := forms(tinyInputs(1, 50, false))
			in.Ranks = []SkillRank{rank(4)}
			return in
		}(), host: hosts[2], single: true},
	}
}
