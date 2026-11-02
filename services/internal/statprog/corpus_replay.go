// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"bufio"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"math"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
)

type corpusParser struct {
	c         *Corpus
	cur       *CorpusCase
	actor     *CorpusActor
	dmg       damageCursor
	nh        *NodeHostCase
	gh        *GatherHostCase
	curBundle string
	b64       strings.Builder
	bundleLen int
	lineNo    int
}

func (p *corpusParser) fail(format string, args ...any) error {
	return fmt.Errorf("corpus line %d: %s", p.lineNo, fmt.Sprintf(format, args...))
}

// ParseCorpus reads a corpus file.
func ParseCorpus(r io.Reader) (*Corpus, error) {
	p := &corpusParser{c: &Corpus{Bundles: map[string][]byte{}}}
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		p.lineNo++
		line := sc.Text()
		if line == "" || line[0] == '#' {
			continue
		}
		if err := p.step(strings.Fields(line)); err != nil {
			return nil, err
		}
	}
	if p.curBundle != "" || p.cur != nil || p.actor != nil || p.dmg.open() || p.nh != nil || p.gh != nil {
		return nil, fmt.Errorf("corpus: unterminated bundle or case")
	}
	return p.c, sc.Err()
}

func (p *corpusParser) step(f []string) error {
	if p.curBundle != "" {
		if f[0] == "endbundle" {
			raw, err := base64.StdEncoding.DecodeString(p.b64.String())
			if err != nil || len(raw) != p.bundleLen {
				return p.fail("bundle %s is not %d bytes of base64 (%v)", p.curBundle, p.bundleLen, err)
			}
			p.c.Bundles[p.curBundle] = raw
			p.curBundle = ""
			return nil
		}
		p.b64.WriteString(f[0])
		return nil
	}
	if ok, err := p.parseCaseBlock(f); ok || err != nil {
		return err
	}
	if ok, err := p.parseHostOrDamageBlock(f); ok || err != nil {
		return err
	}
	if ok, err := p.parseTailDirective(f); ok || err != nil {
		return err
	}
	if p.actor != nil {
		sub := CorpusCase{In: p.actor.In, State: p.actor.State}
		if err := parseCaseLine(&sub, f); err != nil {
			return p.fail("%v", err)
		}
		p.actor.In, p.actor.State = sub.In, sub.State
		return nil
	}
	if p.cur == nil {
		return p.fail("%q outside a case", f[0])
	}
	if err := parseCaseLine(p.cur, f); err != nil {
		return p.fail("%v", err)
	}
	return nil
}

func (p *corpusParser) parseCaseBlock(f []string) (bool, error) {
	switch f[0] {
	case "bundle":
		if len(f) != 3 {
			return true, p.fail("bundle NAME LEN")
		}
		n, err := strconv.Atoi(f[2])
		if err != nil {
			return true, p.fail("bad length")
		}
		p.curBundle, p.bundleLen = f[1], n
		p.b64.Reset()
		return true, nil
	case "derive":
		if len(f) != 4 || f[3] != "double" {
			return true, p.fail("derive NAME OF double")
		}
		p.c.Derived = append(p.c.Derived, DerivedBundle{f[1], f[2]})
		return true, nil
	case "case", "fxcase":
		if len(f) != 5 {
			return true, p.fail("%s NAME BUNDLE CLASS LEVEL", f[0])
		}
		cl, e1 := strconv.Atoi(f[3])
		lv, e2 := strconv.Atoi(f[4])
		if e1 != nil || e2 != nil {
			return true, p.fail("bad class or level")
		}
		cs := CorpusCase{Name: f[1], Bundle: f[2], In: Inputs{Class: cl, Level: lv}}
		if f[0] == "fxcase" {
			cs.Fx = &CorpusFx{}
		}
		p.c.Cases = append(p.c.Cases, cs)
		p.cur = &p.c.Cases[len(p.c.Cases)-1]
		return true, nil
	case "actor":
		if p.cur == nil || p.cur.Fx == nil || len(f) != 4 {
			return true, p.fail("actor ROLE CLASS LEVEL inside an fxcase")
		}
		role, e1 := strconv.Atoi(f[1])
		cl, e2 := strconv.Atoi(f[2])
		lv, e3 := strconv.Atoi(f[3])
		if e1 != nil || e2 != nil || e3 != nil {
			return true, p.fail("bad actor line")
		}
		p.actor = &CorpusActor{Role: role, In: Inputs{Class: cl, Level: lv}}
		return true, nil
	case "endactor":
		if p.actor == nil {
			return true, p.fail("endactor outside an actor")
		}
		p.cur.Actors = append(p.cur.Actors, *p.actor)
		p.actor = nil
		return true, nil
	}
	return false, nil
}

func (p *corpusParser) parseHostOrDamageBlock(f []string) (bool, error) {
	switch f[0] {
	case "nodehost":
		if len(f) != 2 {
			return true, p.fail("nodehost NAME")
		}
		p.c.NodeHosts = append(p.c.NodeHosts, NodeHostCase{Name: f[1]})
		p.nh = &p.c.NodeHosts[len(p.c.NodeHosts)-1]
		return true, nil
	case "gatherhost":
		if len(f) != 2 {
			return true, p.fail("gatherhost NAME")
		}
		p.c.GatherHosts = append(p.c.GatherHosts, GatherHostCase{Name: f[1], Ctx: GatherContext{Ratings: map[int64]float64{}}})
		p.gh = &p.c.GatherHosts[len(p.c.GatherHosts)-1]
		return true, nil
	case "quality", "rarity", "tier", "duration", "rating":
		if p.gh == nil {
			return true, p.fail("%q outside a gatherhost block", f[0])
		}
		if err := p.gh.parseLine(f); err != nil {
			return true, p.fail("%v", err)
		}
		return true, nil
	case "nodes", "node", "ctx", "q":
		if p.gh != nil {
			if err := p.gh.parseLine(f); err != nil {
				return true, p.fail("%v", err)
			}
			return true, nil
		}
		if p.nh != nil {
			if err := p.nh.parseLine(f); err != nil {
				return true, p.fail("%v", err)
			}
			return true, nil
		}
		return true, p.fail("%q outside a nodehost or gatherhost block", f[0])
	case "dprofile", "dp", "de", "dview", "v", "damage":
		if _, err := p.c.parseDamageLine(f, &p.dmg); err != nil {
			return true, p.fail("%v", err)
		}
		return true, nil
	case "end":
		return true, p.closeBlock(f)
	}
	return false, nil
}

func (p *corpusParser) closeBlock(f []string) error {
	if p.nh != nil {
		p.nh = nil
		return nil
	}
	if p.gh != nil {
		p.gh = nil
		return nil
	}
	if p.dmg.open() {
		if _, err := p.c.parseDamageLine(f, &p.dmg); err != nil {
			return p.fail("%v", err)
		}
		return nil
	}
	p.cur = nil
	return nil
}

func (p *corpusParser) parseTailDirective(f []string) (bool, error) {
	switch f[0] {
	case "refuse":
		if len(f) != 5 {
			return true, p.fail("refuse NAME BUNDLE CLASS LEVEL")
		}
		cl, e1 := strconv.Atoi(f[3])
		lv, e2 := strconv.Atoi(f[4])
		if e1 != nil || e2 != nil {
			return true, p.fail("bad class or level")
		}
		p.c.Refusals = append(p.c.Refusals, CorpusRefusal{Name: f[1], Bundle: f[2], Class: cl, Level: lv})
		return true, nil
	case "fuzz":
		if len(f) != 6 {
			return true, p.fail("fuzz BUNDLE SEED N ACCEPTED HASH")
		}
		seed, e1 := strconv.ParseUint(f[2], 10, 64)
		n, e2 := strconv.Atoi(f[3])
		acc, e3 := strconv.Atoi(f[4])
		h, e4 := strconv.ParseUint(f[5], 16, 64)
		if e1 != nil || e2 != nil || e3 != nil || e4 != nil {
			return true, p.fail("bad fuzz line")
		}
		p.c.Fuzzes = append(p.c.Fuzzes, CorpusFuzz{Bundle: f[1], Seed: seed, N: n, Accepted: acc, Hash: h})
		return true, nil
	case "hostile":
		if len(f) != 6 {
			return true, p.fail("hostile NAME FIX POS DEL HEX")
		}
		pos, e1 := strconv.Atoi(f[3])
		del, e2 := strconv.Atoi(f[4])
		var ins []byte
		var e3 error
		if f[5] != "-" {
			ins, e3 = hex.DecodeString(f[5])
		}
		if e1 != nil || e2 != nil || e3 != nil {
			return true, p.fail("bad hostile edit")
		}
		p.c.Hostiles = append(p.c.Hostiles, CorpusHostile{Name: f[1], Fix: f[2] == "1", Blob: f[2] == "2", Pos: pos, Del: del, Ins: ins})
		return true, nil
	}
	return false, nil
}

func parseCaseLine(c *CorpusCase, f []string) error {
	u := func(s string, base, bits int) uint64 {
		v, err := strconv.ParseUint(s, base, bits)
		if err != nil {
			panic(fmt.Errorf("bad number %q", s))
		}
		return v
	}
	var err error
	func() {
		defer func() {
			if r := recover(); r != nil {
				err = fmt.Errorf("%v in %v", r, f)
			}
		}()
		parseCaseFields(c, f, u)
	}()
	return err
}

func parseCaseFields(c *CorpusCase, f []string, u func(string, int, int) uint64) {
	switch f[0] {
	case "mod":
		c.In.Mods = append(c.In.Mods, Mod{Stat: int64(u(f[1], 10, 64)), Value: math.Float64frombits(u(f[2], 16, 64))})
	case "tag":
		c.In.Equipped = append(c.In.Equipped, EquippedTag{Slot: f[1], Tag: f[2]})
	case "rounding", "item":
		parseGearLine(c, f, u)
	case "state":
		parseStateLine(c, f, u)
	case "fx", "fxtag", "fxmod", "rank", "ignoredfx", "evalfaults", "prog", "retail":
		parseFxLine(c, f, u)
	case "host":
		parseHostLine(&c.Fx.Host, f, u)
	case "want":
		c.Want = append(c.Want, CorpusWant{Guid: int64(u(f[1], 10, 64)), Value: u(f[2], 16, 64)})
	case "digest":
		c.Digest = u(f[1], 16, 64)
		c.Members, c.Faults, c.Ignored = int(u(f[2], 10, 32)), int(u(f[3], 10, 32)), int(u(f[4], 10, 32))
	case "cap":
		c.Caps = append(c.Caps, CorpusCap{Guid: int64(u(f[1], 10, 64)), Bits: uint32(u(f[2], 16, 32)), Match: f[3]})
	default:
		panic(fmt.Errorf("unknown line %q", f[0]))
	}
}

func parseGearLine(c *CorpusCase, f []string, u func(string, int, int) uint64) {
	if c.Gear == nil {
		c.Gear = &CorpusGear{}
	}
	if f[0] == "item" {
		c.Gear.Items = append(c.Gear.Items, Item{Record: int64(u(f[1], 10, 64)), Quality: int(u(f[2], 10, 32)), Slot: f[3]})
		return
	}
	switch f[1] {
	case "none":
		c.Gear.Rounding = RoundNone
	case "halfup":
		c.Gear.Rounding = RoundHalfUp
	default:
		panic(fmt.Errorf("unknown rounding %q", f[1]))
	}
}

func parseStateLine(c *CorpusCase, f []string, u func(string, int, int) uint64) {
	if c.State == nil {
		c.State = &TableState{}
	}
	s := c.State
	switch f[1] {
	case "effect":
		if s.Effects == nil {
			s.Effects = map[aocexpr.Ref]float64{}
		}
		s.Effects[aocexpr.Ref{TypeID: u(f[2], 10, 64), Guid: int64(u(f[3], 10, 64))}] = math.Float64frombits(u(f[4], 16, 64))
	case "tag":
		if s.Tags == nil {
			s.Tags = map[string]float64{}
		}
		s.Tags[f[2]] = math.Float64frombits(u(f[3], 16, 64))
	case "skill":
		if s.Skills == nil {
			s.Skills = map[[2]aocexpr.Ref]bool{}
		}
		s.Skills[[2]aocexpr.Ref{{TypeID: u(f[2], 10, 64), Guid: int64(u(f[3], 10, 64))}, {TypeID: u(f[4], 10, 64), Guid: int64(u(f[5], 10, 64))}}] = true
	case "combat":
		s.FlagCombat = true
	case "moving":
		s.FlagMoving = true
	case "unkillable":
		s.FlagUnkillable = true
	case "actor", "prof", "consumed", "npc", "dist":
		parseStateExtra(s, f, u)
	default:
		panic(fmt.Errorf("unknown state %q", f[1]))
	}
}

func parseStateExtra(s *TableState, f []string, u func(string, int, int) uint64) {
	switch f[1] {
	case "actor":
		s.Actor = math.Float64frombits(u(f[2], 16, 64))
	case "prof":
		if s.Professions == nil {
			s.Professions = map[string]float64{}
		}
		s.Professions[f[2]] = math.Float64frombits(u(f[3], 16, 64))
	case "consumed":
		if s.Consumed == nil {
			s.Consumed = map[aocexpr.Ref]float64{}
		}
		s.Consumed[aocexpr.Ref{TypeID: u(f[2], 10, 64), Guid: int64(u(f[3], 10, 64))}] = math.Float64frombits(u(f[4], 16, 64))
	case "npc":
		s.EliteRank = math.Float64frombits(u(f[2], 16, 64))
	case "dist":
		i := int(u(f[2], 10, 32))
		if i < 0 || i >= len(s.NpcDist) {
			panic(fmt.Errorf("state dist %d: there are 8 stats", i))
		}
		s.NpcDist[i] = math.Float64frombits(u(f[3], 16, 64))
	}
}

func parseFxLine(c *CorpusCase, f []string, u func(string, int, int) uint64) {
	switch f[0] {
	case "fx":
		c.In.Effects = append(c.In.Effects, ActiveEffect{Effect: aocexpr.Ref{TypeID: u(f[1], 10, 64), Guid: int64(u(f[2], 10, 64))}, Stacks: int(u(f[3], 10, 32))})
	case "fxtag":
		e := &c.In.Effects[len(c.In.Effects)-1]
		e.Tags = append(e.Tags, f[1])
	case "fxmod":
		e := &c.In.Effects[len(c.In.Effects)-1]
		e.Mods = append(e.Mods, EffectMod{Stat: int64(u(f[1], 10, 64)), Value: math.Float64frombits(u(f[2], 16, 64))})
	case "rank":
		c.In.Ranks = append(c.In.Ranks, SkillRank{Class: aocexpr.Ref{TypeID: u(f[1], 10, 64), Guid: int64(u(f[2], 10, 64))}, Skill: aocexpr.Ref{TypeID: u(f[3], 10, 64), Guid: int64(u(f[4], 10, 64))}})
	case "ignoredfx":
		c.Fx.IgnoredFx = int(u(f[1], 10, 32))
	case "evalfaults":
		c.Fx.EvalFaults = int(u(f[1], 10, 32))
	case "prog":
		c.Fx.Progs = append(c.Fx.Progs, CorpusProg{Index: int(u(f[1], 10, 32)), Value: u(f[2], 16, 64)})
	case "retail":
		switch f[1] {
		case "prog":
			c.Fx.Retail = append(c.Fx.Retail, CorpusRetail{Kind: "prog", ID: int64(u(f[2], 10, 64)), Value: math.Float64frombits(u(f[3], 16, 64))})
		case "stat":
			c.Fx.Retail = append(c.Fx.Retail, CorpusRetail{Kind: "stat", ID: int64(u(f[2], 10, 64)), Value: math.Float64frombits(u(f[3], 16, 64)), Digits: int(u(f[4], 10, 32))})
		default:
			panic(fmt.Errorf("unknown retail line %q", f[1]))
		}
	}
}

func parseHostLine(h *TableHostSpec, f []string, u func(string, int, int) uint64) {
	switch f[1] {
	case "seed":
		h.Seed = u(f[2], 10, 64)
	case "progress":
		if h.Progress == nil {
			h.Progress = map[string]float64{}
		}
		group := f[2]
		if group == "-" {
			group = ""
		}
		h.Progress[group] = math.Float64frombits(u(f[3], 16, 64))
	case "inst":
		if h.Inst == nil {
			h.Inst = map[aocexpr.Ref]float64{}
		}
		h.Inst[aocexpr.Ref{TypeID: u(f[2], 10, 64), Guid: int64(u(f[3], 10, 64))}] = math.Float64frombits(u(f[4], 16, 64))
	case "hittag":
		if h.Tags == nil {
			h.Tags = map[string]bool{}
		}
		h.Tags[f[2]] = true
	case "hitflag":
		if h.Flags == nil {
			h.Flags = map[string]bool{}
		}
		h.Flags[f[2]] = true
	case "targets":
		h.Targets = math.Float64frombits(u(f[2], 16, 64))
	case "xpdebt":
		h.XPDebt = math.Float64frombits(u(f[2], 16, 64))
	case "fn":
		if h.Fns == nil {
			h.Fns = map[string]float64{}
		}
		name := f[3]
		if name == "-" {
			name = ""
		}
		h.Fns[f[2]+"|"+name] = math.Float64frombits(u(f[4], 16, 64))
	default:
		panic(fmt.Errorf("unknown host line %q", f[1]))
	}
}

// Replay checks the corpus the way the C++ test does.
func (c *Corpus) Replay() error {
	for _, nc := range c.NodeHosts {
		if err := nc.check(); err != nil {
			return err
		}
	}
	for _, gc := range c.GatherHosts {
		if err := gc.check(); err != nil {
			return err
		}
	}
	bundles, err := c.decodeCorpusBundles()
	if err != nil {
		return err
	}
	for _, cs := range c.Cases {
		b, ok := bundles[cs.Bundle]
		if !ok {
			return fmt.Errorf("case %s: no bundle %s", cs.Name, cs.Bundle)
		}
		if err := replayCase(cs, b); err != nil {
			return err
		}
	}
	if err := c.replayDamage(); err != nil {
		return err
	}
	return c.replayTailChecks(bundles)
}

func (c *Corpus) decodeCorpusBundles() (map[string]*Bundle, error) {
	bundles := map[string]*Bundle{}
	for name, raw := range c.Bundles {
		b, err := Decode(raw)
		if err != nil {
			return nil, fmt.Errorf("bundle %s: %w", name, err)
		}
		bundles[name] = b
	}
	for _, d := range c.Derived {
		raw, ok := c.Bundles[d.Of]
		if !ok {
			return nil, fmt.Errorf("derive %s: no bundle %s", d.Name, d.Of)
		}
		raw = append([]byte(nil), raw...)
		binary.LittleEndian.PutUint16(raw[6:], FlagDouble)
		b, err := Decode(raw)
		if err != nil {
			return nil, fmt.Errorf("derive %s: %w", d.Name, err)
		}
		bundles[d.Name] = b
	}
	return bundles, nil
}

func replayCase(cs CorpusCase, b *Bundle) error {
	var st aocexpr.State
	if cs.State != nil {
		st = cs.State
	}
	res, err := b.Run(cs.In, st)
	if err != nil {
		return fmt.Errorf("case %s: %w", cs.Name, err)
	}
	if len(res.Stats) != cs.Members || res.Faults != cs.Faults || res.IgnoredMods != cs.Ignored || (len(cs.Want) != 0 && len(cs.Want) != cs.Members) {
		return fmt.Errorf("case %s: %d stats, %d faults, %d ignored mods; the corpus has %d, %d, %d (%d wants)", cs.Name, len(res.Stats), res.Faults, res.IgnoredMods, cs.Members, cs.Faults, cs.Ignored, len(cs.Want))
	}
	got := map[int64]float64{}
	for i, s := range res.Stats {
		if len(cs.Want) != 0 && (cs.Want[i].Guid != s.Guid || cs.Want[i].Value != math.Float64bits(s.Value)) {
			return fmt.Errorf("case %s: stat %d is %016x, the corpus has %d %016x", cs.Name, s.Guid, math.Float64bits(s.Value), cs.Want[i].Guid, cs.Want[i].Value)
		}
		got[s.Guid] = s.Value
	}
	if d := Digest(res); d != cs.Digest {
		return fmt.Errorf("case %s: digest %016x, the corpus has %016x", cs.Name, d, cs.Digest)
	}
	for _, cp := range cs.Caps {
		want := float64(math.Float32frombits(cp.Bits))
		if m := [...]string{"exact", "near", "miss"}[compare(got[cp.Guid], want)]; m != cp.Match {
			return fmt.Errorf("case %s: stat %d compares as %s with the capture, the corpus says %s", cs.Name, cp.Guid, m, cp.Match)
		}
	}
	if cs.Fx != nil {
		return replayFxCase(cs, b, res, st)
	}
	return nil
}

func replayFxCase(cs CorpusCase, b *Bundle, res *Result, st aocexpr.State) error {
	for _, r := range cs.Fx.Retail {
		if r.Kind != "stat" {
			continue
		}
		if i, ok := b.StatIndex(r.ID); !ok || !r.holds(res.Values[i]) {
			return fmt.Errorf("case %s: the retail value %v of stat %d is %v here", cs.Name, r.Value, r.ID, res.Values[i])
		}
	}
	if res.IgnoredEffectMods != cs.Fx.IgnoredFx {
		return fmt.Errorf("case %s: %d ignored effect mods, the corpus has %d", cs.Name, res.IgnoredEffectMods, cs.Fx.IgnoredFx)
	}
	if len(cs.Fx.Progs) == 0 {
		return nil
	}
	ev, err := b.NewEvaluator(res, cs.In, st)
	if err != nil {
		return fmt.Errorf("case %s: %w", cs.Name, err)
	}
	if err := setActors(b, ev, cs.Actors); err != nil {
		return fmt.Errorf("case %s: %w", cs.Name, err)
	}
	host := cs.Fx.Host.Host()
	progValue := map[int]float64{}
	for _, p := range cs.Fx.Progs {
		v, err := ev.Eval(p.Index, host)
		if err != nil {
			return fmt.Errorf("case %s: program %d: %w", cs.Name, p.Index, err)
		}
		if math.Float64bits(v) != p.Value {
			return fmt.Errorf("case %s: program %d gives %016x, the corpus has %016x", cs.Name, p.Index, math.Float64bits(v), p.Value)
		}
		progValue[p.Index] = v
	}
	for _, r := range cs.Fx.Retail {
		if r.Kind == "prog" {
			if v, ok := progValue[int(r.ID)]; !ok || !r.holds(v) {
				return fmt.Errorf("case %s: the retail value %v of program %d is %v here", cs.Name, r.Value, r.ID, v)
			}
		}
	}
	if ev.Faults() != cs.Fx.EvalFaults {
		return fmt.Errorf("case %s: the evaluations had %d faults, the corpus has %d", cs.Name, ev.Faults(), cs.Fx.EvalFaults)
	}
	return nil
}

func (c *Corpus) replayTailChecks(bundles map[string]*Bundle) error {
	for _, r := range c.Refusals {
		b, ok := bundles[r.Bundle]
		if !ok {
			return fmt.Errorf("refuse %s: no bundle %s", r.Name, r.Bundle)
		}
		if _, err := b.Run(Inputs{Class: r.Class, Level: r.Level}, nil); err == nil {
			return fmt.Errorf("refuse %s: the run finished", r.Name)
		}
	}
	for _, fz := range c.Fuzzes {
		raw, ok := c.Bundles[fz.Bundle]
		if !ok {
			return fmt.Errorf("fuzz: no bundle %s", fz.Bundle)
		}
		if acc, h := FuzzParity(raw, fz.Seed, fz.N); acc != fz.Accepted || h != fz.Hash {
			return fmt.Errorf("fuzz %s: %d accepted with hash %016x; the corpus has %d and %016x", fz.Bundle, acc, h, fz.Accepted, fz.Hash)
		}
	}
	tiny, ok := c.Bundles["tiny"]
	if !ok && len(c.Hostiles) > 0 {
		return fmt.Errorf("hostile lines need the bundle tiny")
	}
	for _, h := range c.Hostiles {
		blob, ok := ApplyEdit(tiny, h)
		if !ok {
			return fmt.Errorf("hostile %s: the edit does not fit the bundle", h.Name)
		}
		if _, err := Decode(blob); err == nil {
			return fmt.Errorf("hostile %s was accepted", h.Name)
		}
	}
	return nil
}
