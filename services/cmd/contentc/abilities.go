// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
)

// The ability tables (plan D2, contentc version 6): every ability of every class and what it reaches, as plain data and as
// expressions (package content, abilities.go, has the tables). The expressions are written as text; addStatProgram compiles them into
// the stat program's bundle afterwards and fills ab_expr.program.
//
// What is in them: the abilities are the ranks of the skill trees (plan D1's skill_rank: there is one table of ranks, and a class's abilities are those of its tree). Each ability's closure
// is what it reaches by itself: its own effects, its hits (directly, through its projectiles, through its lingering zones), and
// from there the effects the hits apply, the hits those effects fire in each phase, the projectiles and zones a hit fires or spawns.
// The effects the ranks themselves grant (passives) and their hits are in too. Reactions, cooldown modifiers and the like are stored
// as guid lists (the extra columns) and not followed: a part-2 runtime leaves them inert.

//go:embed montage_ids.tsv
var montageIDs string

// abilityNames lists the abilities of the retail captures that no skill tree grants (plan D2, stage 2): the weapon combos, the general abilities
// (Guard) and the consumables' abilities. They are seeds of the closure like the ranks' abilities, found by name.
//
//go:embed ability_names.tsv
var abilityNames string

// seedByName reads ability_names.tsv and adds every ability row of each name to the seed; a name no row has is an error (the list is stale).
func seedByName(abilities map[int64]*designdb.Row, seed map[int64]bool) (int, error) {
	byName := map[string][]int64{}
	for g, r := range abilities {
		byName[r.Name] = append(byName[r.Name], g)
	}
	n := 0
	for i, line := range strings.Split(abilityNames, "\n") {
		line = strings.TrimRight(line, "\r")
		if line == "" || line[0] == '#' {
			continue
		}
		f := strings.Split(line, "\t")
		if len(f) != 2 {
			return 0, fmt.Errorf("ability_names.tsv line %d: %d columns, want 2", i+1, len(f))
		}
		guids := byName[f[0]]
		if len(guids) == 0 {
			return 0, fmt.Errorf("ability_names.tsv line %d: no Ability row named %q", i+1, f[0])
		}
		for _, g := range guids {
			if !seed[g] {
				seed[g] = true
				n++
			}
		}
	}
	return n, nil
}

// -- CoA . C:Y:B --

// exprSet collects expressions once each (the same text and terms share an id) in the order they are met.
type exprSet struct {
	w    *writer
	ids  map[string]int64
	next int64
	// named is the record guid of each name by kind ("Skill", "SkillTree", "Effect", "Ability"), 0 for a name two rows share: a reference
	// written $Skill:Bard_Passive_QuickWit_Jest$ is rewritten to the number form before the expression is stored and compiled (the compiler
	// takes a reference as its type and guid: a named one would compile as the null reference and the expression could never be true).
	named map[string]map[string]int64
}

var commentOnly = regexp.MustCompile(`(?s)//[^\n]*|/\*.*?\*/`)

// namedRef matches a comment (left as it is) or a reference by name.
var namedRef = regexp.MustCompile(`(?s)//[^\n]*|/\*.*?\*/|\$(SkillTree|Skill|Effect|Ability|F):([^$#][^$]*)\$`)

// refTypes is the type id each named kind stands for.
var refTypes = map[string]int64{"SkillTree": content.TypeSkillTree, "Skill": content.TypeSkill, "Effect": content.TypeEffect, "Ability": content.TypeAbility, "F": 4547065559311335368} // F: a StatFormulaType, named in a reward table ($F:Recipe_Gear_Tier1$)

// recordByName matches a comment (left as it is) or GetAbilityRecordByName("Name") / GetEffectRecordByName("Name"): the call is rewritten to the
// record's reference.
var recordByName = regexp.MustCompile(`(?s)//[^\n]*|/\*.*?\*/|Get(Ability|Effect)RecordByName\(\s*"([^"]+)"\s*\)`)

// resolve rewrites the named references of text (outside comments) to $#<type>:<guid>$; a name that is no record, or two, stays as written.
// GetAbilityRecordByName("Name") becomes the ability's reference the same way (the compiler takes a reference, not a call that looks one up).
func (e *exprSet) resolve(text string) string {
	if e.named == nil {
		return text
	}
	if strings.Contains(text, "GetAbilityRecordByName") || strings.Contains(text, "GetEffectRecordByName") {
		text = recordByName.ReplaceAllStringFunc(text, func(m string) string {
			sub := recordByName.FindStringSubmatch(m)
			if len(sub) < 3 || sub[2] == "" {
				return m // a comment
			}
			guid := e.named[sub[1]][sub[2]]
			if guid == 0 {
				return m
			}
			return fmt.Sprintf("$#%d:%d$", refTypes[sub[1]], guid)
		})
	}
	if !strings.Contains(text, "$") {
		return text
	}
	var out strings.Builder
	last := 0
	for _, m := range namedRef.FindAllStringSubmatchIndex(text, -1) {
		if m[2] < 0 {
			continue // a comment
		}
		kind, name := text[m[2]:m[3]], strings.TrimSpace(text[m[4]:m[5]])
		guid := e.named[kind][name]
		if guid == 0 {
			continue
		}
		out.WriteString(text[last:m[0]])
		fmt.Fprintf(&out, "$#%d:%d$", refTypes[kind], guid)
		last = m[1]
	}
	out.WriteString(text[last:])
	return out.String()
}

// namedRecords is the guid of every skill tree, skill, effect and ability by name ($Skill:Name$ in an expression); a name two rows share is 0.
func namedRecords(src *source) map[string]map[string]int64 {
	out := map[string]map[string]int64{}
	for kind, schema := range map[string]string{"SkillTree": "SkillTreeRecordBase", "Skill": "SkillRecordBase", "Effect": "EffectRecordBase", "Ability": "AoCAbilityRecordBase", "F": "StatFormulaTypeRecordBase"} {
		m := map[string]int64{}
		for _, r := range src.rows[schema] {
			if _, dup := m[r.Name]; dup {
				m[r.Name] = 0
				continue
			}
			m[r.Name] = r.Guid
		}
		out[kind] = m
	}
	return out
}

// id is the ab_expr id of the text with its terms, 0 for a text with nothing in it.
func (e *exprSet) id(text string, terms map[int64]string) (int64, error) {
	if strings.TrimSpace(commentOnly.ReplaceAllString(text, "")) == "" {
		return 0, nil
	}
	text = e.resolve(text)
	if len(terms) > 0 {
		resolved := make(map[int64]string, len(terms))
		for k, v := range terms {
			resolved[k] = e.resolve(v)
		}
		terms = resolved
	}
	tj := "{}"
	if len(terms) > 0 {
		m := make(map[string]string, len(terms))
		for k, v := range terms {
			m[strconv.FormatInt(k, 10)] = v
		}
		b, err := json.Marshal(m) // sorted by key
		if err != nil {
			return 0, err
		}
		tj = string(b)
	}
	key := text + "\x00" + tj
	if id, ok := e.ids[key]; ok {
		return id, nil
	}
	e.next++
	if err := e.w.exec(`INSERT INTO ab_expr (id, text, terms, program, uses_target, error) VALUES (?, ?, ?, -1, 0, '')`, e.next, text, tj); err != nil {
		return 0, err
	}
	e.ids[key] = e.next
	return e.next, nil
}

func (b *abBuilder) exprIDs(texts ...string) ([]int64, error) {
	ids := make([]int64, len(texts))
	for i, t := range texts {
		id, err := b.exprs.id(t, nil)
		if err != nil {
			return nil, err
		}
		ids[i] = id
	}
	return ids, nil
}

// abBuilder is the state of one buildAbilities.
type abBuilder struct {
	w         *writer
	src       *source
	statNames map[int64]string
	exprs     *exprSet

	abilities, effects, hits, mods, projectiles, zones map[int64]*designdb.Row
	targetingNames                                     map[int64]string
	reactions, cooldowns, targetingRows                map[int64]*designdb.Row
}

func indexRows(rows []designdb.Row) map[int64]*designdb.Row {
	m := make(map[int64]*designdb.Row, len(rows))
	for i := range rows {
		m[rows[i].Guid] = &rows[i]
	}
	return m
}

// montageNetGuids reads montage_ids.tsv: the NetGUID of a montage by ability name and AnimationIndex, where a capture names exactly one.
func montageNetGuids() (map[string]map[int]uint64, error) {
	out := map[string]map[int]uint64{}
	for i, line := range strings.Split(montageIDs, "\n") {
		line = strings.TrimRight(line, "\r")
		if line == "" || line[0] == '#' || strings.HasPrefix(line, "ability\t") {
			continue
		}
		f := strings.Split(line, "\t")
		if len(f) != 5 {
			return nil, fmt.Errorf("montage_ids.tsv line %d: %d columns, want 5", i+1, len(f))
		}
		idx, e1 := strconv.Atoi(f[1])
		guid, e2 := strconv.ParseUint(f[2], 10, 64)
		distinct, e3 := strconv.Atoi(f[4])
		if e1 != nil || e2 != nil || e3 != nil {
			return nil, fmt.Errorf("montage_ids.tsv line %d: bad number", i+1)
		}
		if distinct != 1 {
			continue // two captures name two ids for the same index: the data does not have it
		}
		if out[f[0]] == nil {
			out[f[0]] = map[int]uint64{}
		}
		out[f[0]][idx] = guid
	}
	return out, nil
}

// CoA.C_Y_B

// refGuids reads a list of Id structs, or a map of keys to Id structs (the design data uses either for the same kind of reference), into
// guids (zeros dropped).
func refGuids(f *fields, name string) []int64 {
	v, ok := f.get(name)
	if !ok {
		f.fail(name, "array or map of Id structs", v, ok)
		return nil
	}
	var items []designdb.Value
	switch x := v.(type) {
	case []designdb.Value:
		items = x
	case designdb.Pairs:
		for _, p := range x {
			items = append(items, p.Value)
		}
	default:
		f.fail(name, "array or map of Id structs", v, true)
		return nil
	}
	var out []int64
	for i, it := range items {
		s, ok := it.(designdb.Struct)
		if !ok {
			f.fail(name, "Id structs", it, true)
			return nil
		}
		g, _ := idOf(f, fmt.Sprintf("%s: %s[%d]", f.where, name, i), s)
		if g != 0 {
			out = append(out, g)
		}
	}
	return out
}

// jsonOf marshals v (nil is "{}"... never: callers pass a map or a slice).
func jsonOf(v any) string {
	b, _ := json.Marshal(v)
	return string(b)
}

// C Y B / C O A

var zoneHitRoles = []string{"InitialHitsIds", "LingeringHitsIds", "OnBeginOverlapHitsIds", "OnEndOverlapHitsIds", "EndHitsIds"}

type abClosure struct {
	b                                               *abBuilder
	abilitySet, effectSet, hitSet, projSet, zoneSet map[int64]bool
	reactionSet, cooldownSet, targetingSet          map[int64]bool
	effQ, hitQ                                      []int64
}

func newAbBuilder(w *writer, src *source) (*abBuilder, error) {
	for _, schema := range []string{"ProjectileRecordBase", "LingeringEffectRecordBase", "AbilityTargetingRecordBase", "StatTypeDefRecordBase", "ReactionRecordBase", "ModifyCooldownRecordBase"} {
		if _, err := src.need(schema); err != nil {
			return nil, err
		}
	}
	statNames := map[int64]string{}
	for _, r := range src.rows["StatTypeDefRecordBase"] {
		statNames[r.Guid] = r.Name
	}
	return &abBuilder{
		w: w, src: src, statNames: statNames, exprs: &exprSet{w: w, ids: map[string]int64{}, named: namedRecords(src)},
		abilities: indexRows(src.rows["AoCAbilityRecordBase"]), effects: indexRows(src.rows["EffectRecordBase"]),
		hits: indexRows(src.rows["AbilityHitRecordBase"]), mods: indexRows(src.rows["StatModRecordBase"]),
		projectiles: indexRows(src.rows["ProjectileRecordBase"]), zones: indexRows(src.rows["LingeringEffectRecordBase"]),
		reactions: indexRows(src.rows["ReactionRecordBase"]), cooldowns: indexRows(src.rows["ModifyCooldownRecordBase"]),
		targetingRows: indexRows(src.rows["AbilityTargetingRecordBase"]),
	}, nil
}

// seedClosure reads the ranks of every skill tree (plan D1's skill_rank: the one table of ranks); the abilities and effects they grant seed the closure.
func (b *abBuilder) seedClosure(opt *Options) (map[int64]bool, map[int64]bool, error) {
	abilitySeed, effectSeed := map[int64]bool{}, map[int64]bool{}
	rankRows, err := b.w.tx.Query(`SELECT DISTINCT ability_guid, effect_guid FROM skill_rank`)
	if err != nil {
		return nil, nil, fmt.Errorf("read skill_rank (buildTalents must run before buildAbilities): %w", err)
	}
	nranks := 0
	for rankRows.Next() {
		var ag, eg int64
		if err := rankRows.Scan(&ag, &eg); err != nil {
			rankRows.Close()
			return nil, nil, err
		}
		if b.abilities[ag] != nil {
			abilitySeed[ag] = true
		}
		if b.effects[eg] != nil {
			effectSeed[eg] = true
		}
		nranks++
	}
	if err := rankRows.Err(); err != nil {
		return nil, nil, err
	}
	rankRows.Close()
	// The abilities every character has and no tree grants (plan A7's Sprint, Dodge and Rest), and the Sprint's drain, which no record references.
	for _, g := range []int64{content.GuidSprint, content.GuidDodge, content.GuidRest} {
		if b.abilities[g] == nil {
			return nil, nil, fmt.Errorf("no Ability row %d", g)
		}
		abilitySeed[g] = true
	}
	if b.effects[content.GuidSprintStaminaDrain] == nil {
		return nil, nil, fmt.Errorf("no Effect row %d (the Sprint's stamina drain)", content.GuidSprintStaminaDrain)
	}
	effectSeed[content.GuidSprintStaminaDrain] = true
	// ... and the abilities of the retail captures by name (weapon combos, Guard, consumables)
	named, err := seedByName(b.abilities, abilitySeed)
	if err != nil {
		return nil, nil, err
	}
	opt.logf("ability_names.tsv: %d more abilities", named)
	opt.logf("skill_rank: %d distinct ability/effect pairs, %d abilities, %d effects granted by ranks (and the always-on ones)", nranks, len(abilitySeed), len(effectSeed))
	return abilitySeed, effectSeed, nil
}

func (c *abClosure) addEffect(g int64) error {
	if g == 0 || c.effectSet[g] {
		return nil
	}
	if c.b.effects[g] == nil {
		return fmt.Errorf("no Effect row %d", g)
	}
	c.effectSet[g] = true
	c.effQ = append(c.effQ, g)
	return nil
}

func (c *abClosure) addHit(g int64) error {
	if g == 0 || c.hitSet[g] {
		return nil
	}
	if c.b.hits[g] == nil {
		return fmt.Errorf("no AbilityHit row %d", g)
	}
	c.hitSet[g] = true
	c.hitQ = append(c.hitQ, g)
	return nil
}

func addAll(guids []int64, fn func(int64) error) error {
	for _, g := range guids {
		if err := fn(g); err != nil {
			return err
		}
	}
	return nil
}

func (c *abClosure) addProjHits(g int64) error {
	if g == 0 {
		return nil
	}
	p := c.b.projectiles[g]
	if p == nil {
		return fmt.Errorf("no Projectile row %d", g)
	}
	c.projSet[g] = true
	pf := newFields(fmt.Sprintf("Projectile %s (%d)", p.Name, g), p.Fields)
	hs := refGuids(pf, "HitsIds")
	if pf.err != nil {
		return pf.err
	}
	return addAll(hs, c.addHit)
}

func (c *abClosure) addZoneHits(g int64) error {
	if g == 0 {
		return nil
	}
	z := c.b.zones[g]
	if z == nil {
		return fmt.Errorf("no LingeringEffect row %d", g)
	}
	c.zoneSet[g] = true
	zf := newFields(fmt.Sprintf("LingeringEffect %s (%d)", z.Name, g), z.Fields)
	lists := map[string][]int64{}
	for _, role := range zoneHitRoles {
		lists[role] = refGuids(zf, role)
	}
	if zf.err != nil {
		return zf.err
	}
	for _, role := range zoneHitRoles {
		if err := addAll(lists[role], c.addHit); err != nil {
			return err
		}
	}
	return nil
}

// reactions (version 35): an effect, a hit or an ability that carries one brings it, the hits it applies and what those reach; a hit's cooldown
// modifiers and the targeting records of its recursive hits come with it
func (c *abClosure) addReaction(g int64) error {
	if g == 0 || c.reactionSet[g] {
		return nil
	}
	r := c.b.reactions[g]
	if r == nil {
		return fmt.Errorf("no Reaction row %d", g)
	}
	c.reactionSet[g] = true
	rf := newFields(fmt.Sprintf("Reaction %s (%d)", r.Name, g), r.Fields)
	if err := addAll(refGuids(rf, "ReactHitsIds"), c.addHit); err != nil {
		return err
	}
	return rf.err
}

func (c *abClosure) addCooldown(g int64) error {
	if g == 0 || c.cooldownSet[g] {
		return nil
	}
	if c.b.cooldowns[g] == nil {
		return fmt.Errorf("no ModifyCooldown row %d", g)
	}
	c.cooldownSet[g] = true
	return nil
}

// seedAbilityOwn adds an ability's own effects and hits (directly, projectiles, zones).
func (c *abClosure) seedAbilityOwn(g int64) error {
	af := newFields(fmt.Sprintf("Ability %s (%d)", c.b.abilities[g].Name, g), c.b.abilities[g].Fields)
	_, effs := af.keyedIds("EffectsIds")
	_, hs := af.keyedIds("HitsIds")
	_, ps := af.keyedIds("ProjectilesIds")
	_, zs := af.keyedIds("LingeringEffectsIds")
	for _, step := range []struct {
		ids []int64
		fn  func(int64) error
	}{
		{refGuids(af, "ReactionsIds"), c.addReaction}, {refGuids(af, "ModifyCooldownsIds"), c.addCooldown},
		{effs, c.addEffect}, {hs, c.addHit}, {ps, c.addProjHits}, {zs, c.addZoneHits},
	} {
		if af.err != nil {
			return af.err
		}
		if err := addAll(step.ids, step.fn); err != nil {
			return err
		}
	}
	return af.err
}

func (c *abClosure) expandEffect(g int64) error {
	ef := newFields(fmt.Sprintf("Effect %s (%d)", c.b.effects[g].Name, g), c.b.effects[g].Fields)
	for _, p := range effectPhases {
		if err := addAll(refGuids(ef, p.field), c.addHit); err != nil {
			return err
		}
	}
	if err := addAll(refGuids(ef, "ReactionsIds"), c.addReaction); err != nil {
		return err
	}
	return ef.err
}

func (c *abClosure) expandHitEffectsAndProjs(hf *fields) error {
	for i, v := range hf.list("ApplyEffects") {
		s, ok := v.(designdb.Struct)
		if !ok {
			return fmt.Errorf("%s: ApplyEffects[%d] is %T", hf.where, i, v)
		}
		xf := newFields(fmt.Sprintf("%s: ApplyEffects[%d]", hf.where, i), s)
		e, _ := xf.id("EffectId")
		if xf.err != nil {
			return xf.err
		}
		if err := c.addEffect(e); err != nil {
			return err
		}
	}
	for i, v := range hf.list("EffectsAppliedToSelf") {
		s, ok := v.(designdb.Struct)
		if !ok {
			return fmt.Errorf("%s: EffectsAppliedToSelf[%d] is %T", hf.where, i, v)
		}
		xf := newFields(fmt.Sprintf("%s: EffectsAppliedToSelf[%d]", hf.where, i), s)
		e, _ := xf.id("Effect")
		if xf.err != nil {
			return xf.err
		}
		if err := c.addEffect(e); err != nil {
			return err
		}
	}
	for _, field := range []string{"ProjectilesToFire", "ProjectilesToSpawn"} {
		for i, v := range hf.list(field) {
			s, ok := v.(designdb.Struct)
			if !ok {
				return fmt.Errorf("%s: %s[%d] is %T", hf.where, field, i, v)
			}
			xf := newFields(fmt.Sprintf("%s: %s[%d]", hf.where, field, i), s)
			p, _ := xf.id("Projectile")
			if xf.err != nil {
				return xf.err
			}
			if err := c.addProjHits(p); err != nil {
				return err
			}
		}
	}
	return nil
}

func (c *abClosure) expandHitZonesAndExtras(hf *fields) error {
	if err := addAll(refGuids(hf, "LingeringZonesIds"), c.addZoneHits); err != nil {
		return err
	}
	if err := addAll(refGuids(hf, "ReactionsIds"), c.addReaction); err != nil {
		return err
	}
	if err := addAll(refGuids(hf, "ModifyCooldownsIds"), c.addCooldown); err != nil {
		return err
	}
	for i, v := range hf.list("RecursiveHit") {
		s, ok := v.(designdb.Struct)
		if !ok {
			return fmt.Errorf("%s: RecursiveHit[%d] is %T", hf.where, i, v)
		}
		xf := newFields(fmt.Sprintf("%s: RecursiveHit[%d]", hf.where, i), s)
		ch, _ := xf.id("Hit")
		tg, _ := xf.id("Targeting")
		if xf.err != nil {
			return xf.err
		}
		if err := c.addHit(ch); err != nil {
			return err
		}
		if tg != 0 {
			if c.b.targetingRows[tg] == nil {
				return fmt.Errorf("%s: RecursiveHit[%d] names Targeting %d, which is not a row", hf.where, i, tg)
			}
			c.targetingSet[tg] = true
		}
	}
	return hf.err
}

// computeClosure expands the worklist: effects fire hits, hits apply effects and fire projectiles and zones, which fire hits.
func (b *abBuilder) computeClosure(abilitySeed, effectSeed map[int64]bool) (*abClosure, error) {
	c := &abClosure{
		b: b, abilitySet: map[int64]bool{}, effectSet: map[int64]bool{}, hitSet: map[int64]bool{},
		projSet: map[int64]bool{}, zoneSet: map[int64]bool{}, reactionSet: map[int64]bool{},
		cooldownSet: map[int64]bool{}, targetingSet: map[int64]bool{},
	}
	for _, g := range sortedBools(abilitySeed) {
		c.abilitySet[g] = true
	}
	if err := addAll(sortedBools(effectSeed), c.addEffect); err != nil {
		return nil, err
	}
	if err := addAll(sortedBools(c.abilitySet), c.seedAbilityOwn); err != nil {
		return nil, err
	}
	for len(c.effQ) > 0 || len(c.hitQ) > 0 {
		for len(c.effQ) > 0 {
			g := c.effQ[0]
			c.effQ = c.effQ[1:]
			if err := c.expandEffect(g); err != nil {
				return nil, err
			}
		}
		for len(c.hitQ) > 0 {
			g := c.hitQ[0]
			c.hitQ = c.hitQ[1:]
			hf := newFields(fmt.Sprintf("AbilityHit %s (%d)", b.hits[g].Name, g), b.hits[g].Fields)
			if err := c.expandHitEffectsAndProjs(hf); err != nil {
				return nil, err
			}
			if err := c.expandHitZonesAndExtras(hf); err != nil {
				return nil, err
			}
		}
	}
	return c, nil
}

func writeClosureSet(set map[int64]bool, rows map[int64]*designdb.Row, fn func(*designdb.Row) error) error {
	for _, g := range sortedBools(set) {
		if err := fn(rows[g]); err != nil {
			return err
		}
	}
	return nil
}

// writeClosureRows writes the closure rows in guid order; the reactions' own rows come after all the others, so that the ab_expr ids of everything above keep their order.
func (b *abBuilder) writeClosureRows(c *abClosure, opt *Options, meta map[string]string) error {
	netGuids, err := montageNetGuids()
	if err != nil {
		return err
	}
	b.targetingNames = map[int64]string{}
	for _, r := range b.src.rows["AbilityTargetingRecordBase"] {
		b.targetingNames[r.Guid] = r.Name
	}
	if err := writeClosureSet(c.abilitySet, b.abilities, func(r *designdb.Row) error { return b.writeAbility(r, netGuids) }); err != nil {
		return err
	}
	if err := writeClosureSet(c.hitSet, b.hits, b.writeHit); err != nil {
		return err
	}
	if err := writeClosureSet(c.projSet, b.projectiles, b.writeProjectile); err != nil {
		return err
	}
	if err := writeClosureSet(c.zoneSet, b.zones, func(r *designdb.Row) error { return b.writeZone(r, zoneHitRoles) }); err != nil {
		return err
	}
	if err := writeClosureSet(c.effectSet, b.effects, b.writeEffect); err != nil {
		return err
	}
	if err := writeClosureSet(c.reactionSet, b.reactions, b.writeReaction); err != nil {
		return err
	}
	if err := writeClosureSet(c.cooldownSet, b.cooldowns, b.writeModifyCooldown); err != nil {
		return err
	}
	if err := writeClosureSet(c.targetingSet, b.targetingRows, b.writeTargetingDef); err != nil {
		return err
	}
	opt.logf("ab_*: %d abilities, %d hits, %d projectiles, %d zones, %d effects, %d reactions, %d cooldown modifiers, %d targeting records, %d expressions", len(c.abilitySet), len(c.hitSet), len(c.projSet), len(c.zoneSet), len(c.effectSet), len(c.reactionSet), len(c.cooldownSet), len(c.targetingSet), b.exprs.next)
	for k, n := range map[string]int{
		"ab.reactions": len(c.reactionSet), "ab.modify_cooldowns": len(c.cooldownSet), "ab.targeting_defs": len(c.targetingSet),
		"ab.abilities": len(c.abilitySet), "ab.hits": len(c.hitSet), "ab.projectiles": len(c.projSet), "ab.zones": len(c.zoneSet), "ab.effects": len(c.effectSet),
	} {
		meta[k] = strconv.Itoa(n)
	}
	meta["ab.exprs"] = strconv.FormatInt(b.exprs.next, 10)
	meta["ab.montage_ids"] = "montage_ids.tsv: retail captures, ability and AnimationIndex to the montage's NetGUID where exactly one id was seen"
	return nil
}

// buildAbilities fills the ab_* tables (and meta's ab.* counts).
func buildAbilities(w *writer, src *source, opt *Options, meta map[string]string) error {
	b, err := newAbBuilder(w, src)
	if err != nil {
		return err
	}
	abilitySeed, effectSeed, err := b.seedClosure(opt)
	if err != nil {
		return err
	}
	c, err := b.computeClosure(abilitySeed, effectSeed)
	if err != nil {
		return err
	}
	if err := b.writeClosureRows(c, opt, meta); err != nil {
		return err
	}
	if err := b.buildLoot(opt, meta); err != nil { // plan E1, stage 3, part 2: the reward tables (their expressions join the set)
		return err
	}
	return b.buildGather(opt, meta) // Gathering stage 2: the gathering tables (their formulas join the set)
}

// [CoA ~ C_Y_B]

func sortedBools(m map[int64]bool) []int64 {
	out := make([]int64, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

func (b *abBuilder) name(guid int64, rows map[int64]*designdb.Row) string {
	if r := rows[guid]; r != nil {
		return r.Name
	}
	return ""
}
