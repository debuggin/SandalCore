// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"math"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/designdb"
)

// Town vendors (vendors milestone, step 0; contentc version 18): npc_vendor_link, vendor_def_data, vendor_def_modifier, vendor_def_sell_filter, vendor_modifier,
// vendor_sell_filter, base_item_value, item_base_value, vendor_offer, vendor_availability and vendor_price (internal/content/vendors.go says what each holds).
//
// The price of a listing, per currency, per unit (what vendor_price holds):
//
//	P0 = ceil(the listing's price expression)           the offer's expression when the listing has an offer, else the item's base value expression
//	P  = P0 through the def's price modifiers           in order; a modifier applies to a buy when its contexts hold the buy bit, its item filter holds for the
//	                                                    item and (for an Add, Multiply or Set) its currency list names the currency
//	price = ceil(max(P, 0))
//
// The node tax is added at the sale from the node's table (price + ceil(price * percent + flat), then times the quantity). A listing whose expression
// or modifier needs more than the item (a player's effects, a fish's weight, a factor this builder does not read) gets no row: the buy refuses it, and the
// build says how many and why.

var vendorRecordTypes = []string{"VendorDefRecordBase", "VendorInventoryDefRecordBase", "VendorOfferRecordBase", "VendorSellFilterRecordBase", "FVendorPriceModifierRecordBase",
	"BaseItemValueRecordBase", "ItemRecordBase", "ItemVariationRecordBase", "QualityCurveRecordBase", "PlayerInteractionDefRecordBase", "ActionResponseRecordBase",
	"PopulationInstanceRecordBase", "PopulationAssetRecordBase"}

// vendorBuyBit is the buy operation in a modifier's contexts and a def's supported operations.
const vendorBuyBit = 1

// tagQuery is a gameplay tag query as the data holds it: the token stream and the tag dictionary.
type tagQuery struct {
	Tok  []int64  `json:"tok"`
	Tags []string `json:"tags"`
}

// readTagQuery reads an ItemFilter struct; nil when it holds no query.
func readTagQuery(v designdb.Value) *tagQuery {
	s, ok := v.(designdb.Struct)
	if !ok {
		return nil
	}
	ts, _ := s["QueryTokenStream"].([]designdb.Value)
	if len(ts) < 2 {
		return nil
	}
	q := &tagQuery{}
	for _, t := range ts {
		q.Tok = append(q.Tok, intOf(t))
	}
	if q.Tok[1] == 0 {
		return nil
	}
	dict, _ := s["TagDictionary"].([]designdb.Value)
	for _, t := range dict {
		switch x := t.(type) {
		case string:
			q.Tags = append(q.Tags, x)
		case designdb.Struct:
			q.Tags = append(q.Tags, strOf(x["TagName"]))
		}
	}
	return q
}

// matches evaluates the query against a set of lower-case tags (each tag with all its parents). An unreadable stream matches nothing.
func (q *tagQuery) matches(have map[string]bool) bool {
	pos := 2
	r, ok := q.evalTok(&pos, have)
	return r && ok
}

func (q *tagQuery) evalTok(pos *int, have map[string]bool) (bool, bool) {
	if *pos+1 >= len(q.Tok) {
		return false, false
	}
	kind, n := q.Tok[*pos], int(q.Tok[*pos+1])
	*pos += 2
	switch kind {
	case 1, 2, 3: // any, all, none of the tags
		if n < 0 || *pos+n > len(q.Tok) {
			return false, false
		}
		any, all := false, true
		for i := 0; i < n; i++ {
			idx := q.Tok[*pos+i]
			if idx >= 0 && int(idx) < len(q.Tags) && have[strings.ToLower(q.Tags[idx])] {
				any = true
			} else {
				all = false
			}
		}
		*pos += n
		return evalTagKind(kind, any, all), true
	case 4, 5, 6: // any, all, none of the sub-expressions
		any, all := false, true
		for i := 0; i < n; i++ {
			r, good := q.evalTok(pos, have)
			if !good {
				return false, false
			}
			if r {
				any = true
			} else {
				all = false
			}
		}
		return evalTagKind(kind-3, any, all), true
	}
	return false, false
}

func evalTagKind(kind int64, any, all bool) bool {
	switch kind {
	case 1:
		return any
	case 2:
		return all
	}
	return !any
}

// vitem is what a price needs of an item.
type vitem struct {
	guid        int64
	name        string
	level       int64
	quality     int64
	qualityCurv int64
	rarityMin   int
	rarityMax   int
	baseValue   int64
	vendorable  bool            // Vendorable: a shop may buy it
	blacklisted bool            // Blacklisted
	tags        map[string]bool // lower case, each tag with its parents
}

type curExpr struct {
	currency int64
	expr     string
}

type baseValue struct {
	guid, buyCur, sellCur int64
	name                  string
	buy, sell             []curExpr
	override              bool
}

type vmodifier struct {
	guid                             int64
	name                             string
	contexts                         int64
	operation, source, factor, exprS string
	curve                            *aocexpr.Curve
	curveKeys                        [][3]any
	filter                           *tagQuery
	currencies                       []int64
	children                         []int64
}

type vpredicate struct {
	expr, failure string
	hidden        bool
}

type vlisting struct {
	item, variation int64
	offers          []int64
	preds           []vpredicate
}

type vinventory struct {
	name     string
	listings []vlisting
	preds    []vpredicate
}

type vdef struct {
	guid        int64
	name        string
	ops         int64
	blacklisted bool
	tax         string
	fromWh      bool
	invs        []int64
	mods        []int64
	filters     []int64
}

func readCurExprs(f *fields, name string) []curExpr {
	var out []curExpr
	for i, p := range f.pairs(name) {
		ks, ok := p.Key.(designdb.Struct)
		val, vok := p.Value.(string)
		if !ok || !vok {
			f.fail(name, "map of id to expression", p, true)
			return nil
		}
		g, _ := idOf(f, fmt.Sprintf("%s: %s[%d]", f.where, name, i), ks)
		out = append(out, curExpr{currency: g, expr: val})
	}
	return out
}

func readPredicates(v designdb.Value) []vpredicate {
	var out []vpredicate
	l, _ := v.([]designdb.Value)
	for _, e := range l {
		s, ok := e.(designdb.Struct)
		if !ok {
			continue
		}
		p := vpredicate{expr: strOf(s["AvailabilityPredicate"])}
		if t, ok := s["FailureReason"].(designdb.Text); ok {
			p.failure = t.Source
			if p.failure == "" {
				p.failure = t.Key
			}
		}
		if b, ok := s["bHiddenIfUnavailable"].(bool); ok {
			p.hidden = b
		}
		out = append(out, p)
	}
	return out
}

// priceGen prices the listings.
type priceGen struct {
	curves   aocexpr.CurveSource
	items    map[int64]*vitem
	bands    map[int64][]qualityBand
	varQual  map[int64]string
	bases    map[int64]*baseValue
	offers   map[int64]*baseValue
	mods     map[int64]*vmodifier
	programs map[string]*aocexpr.Program
	skipped  map[string]int
	example  map[string]string
}

func (g *priceGen) skip(reason, where string) {
	g.skipped[reason]++
	if _, ok := g.example[reason]; !ok {
		g.example[reason] = where
	}
}

// rarityIndex is the ordinal of a rarity name (Common 0 ... Artifact 6); Common for a name that is none.
func rarityIndex(name string) int {
	for i, r := range rarities {
		if strings.EqualFold(r, name) {
			return i
		}
	}
	return 0
}

// rarityOf is the rarity ordinal of a quality on an item's quality curve (Common 0 ... Artifact 6), held inside the item's own range (RarityMin to RarityMax: an item
// that is never below Rare reads Rare at quality 1); an item with no curve is Common.
func (g *priceGen) rarityOf(it *vitem, quality int64) int {
	band := g.band(it.qualityCurv, quality)
	if band < it.rarityMin {
		band = it.rarityMin
	}
	if band > it.rarityMax {
		band = it.rarityMax
	}
	return band
}

func (g *priceGen) band(curve, quality int64) int {
	bands := g.bands[curve]
	if len(bands) != len(rarities) {
		return 0
	}
	band := 0
	for i, b := range bands {
		if float64(quality) >= b.X {
			band = i
		}
		if float64(quality) >= b.X && float64(quality) <= b.Y {
			break
		}
	}
	return band
}

type evalCtx struct {
	item    *vitem
	quality int64
	rarity  int
}

func (g *priceGen) program(src string) (*aocexpr.Program, error) {
	if p, ok := g.programs[src]; ok {
		return p, nil
	}
	p, err := aocexpr.Parse(src)
	if err != nil {
		return nil, err
	}
	g.programs[src] = p
	return p, nil
}

func (g *priceGen) env(c *evalCtx) *aocexpr.Env {
	return &aocexpr.Env{
		IntDivide: true,
		Curves:    g.curves,
		Funcs: map[string]aocexpr.Func{
			"GetItemLevel":  func([]aocexpr.Value) (aocexpr.Value, error) { return float64(c.item.level), nil },
			"GetItemRarity": func([]aocexpr.Value) (aocexpr.Value, error) { return float64(c.rarity), nil },
			"ItemTypeTagsContains": func(a []aocexpr.Value) (aocexpr.Value, error) {
				if len(a) != 1 {
					return nil, fmt.Errorf("ItemTypeTagsContains takes one tag")
				}
				t, ok := a[0].(string)
				if !ok {
					return nil, fmt.Errorf("ItemTypeTagsContains takes a tag")
				}
				return c.item.tags[strings.ToLower(t)], nil
			},
		},
	}
}

func (g *priceGen) eval(src string, c *evalCtx) (float64, error) {
	p, err := g.program(src)
	if err != nil {
		return 0, err
	}
	return p.EvalFloat(g.env(c))
}

// reason names why an expression could not be priced, for the build's report.
func reasonOf(err error) string {
	if construct, ok := aocexpr.Unsupported(err); ok {
		return "unsupported " + construct
	}
	return "error: " + err.Error()
}

var errNoFactor = fmt.Errorf("a price factor this builder does not read")

func (g *priceGen) modValue(m *vmodifier, c *evalCtx) (float64, error) {
	switch m.source {
	case "Expression":
		return g.eval(m.exprS, c)
	case "Curve":
		var at float64
		switch m.factor {
		case "Quality":
			at = float64(c.quality)
		case "Rarity":
			at = float64(c.rarity)
		default:
			return 0, errNoFactor
		}
		if m.curve == nil {
			return 0, fmt.Errorf("modifier %s has a curve source and no curve", m.name)
		}
		return m.curve.Eval(at)
	default:
		return 0, fmt.Errorf("modifier %s has the source %q", m.name, m.source)
	}
}

// apply runs the modifiers (in order) on a value in a currency, by the rule at the top of this file.
func (g *priceGen) apply(ids []int64, v float64, currency int64, c *evalCtx) (float64, error) {
	for _, id := range ids {
		m := g.mods[id]
		if m == nil {
			return 0, fmt.Errorf("price modifier %d is not in the data", id)
		}
		if m.contexts&vendorBuyBit == 0 {
			continue
		}
		if m.filter != nil && !m.filter.matches(c.item.tags) {
			continue
		}
		if m.source == "PriceModifierSet" {
			var err error
			if v, err = g.apply(m.children, v, currency, c); err != nil {
				return 0, err
			}
			continue
		}
		x, err := g.modValue(m, c)
		if err != nil {
			return 0, err
		}
		named := false
		for _, cur := range m.currencies {
			if cur == currency {
				named = true
			}
		}
		if !named {
			continue
		}
		switch m.operation {
		case "Add":
			v += x
		case "Multiply":
			v *= x
		case "Set":
			v = x
		default:
			return 0, fmt.Errorf("modifier %s has the operation %q", m.name, m.operation)
		}
	}
	return v, nil
}

// price is one currency's unit price of a listing.
func (g *priceGen) price(expr string, currency int64, mods []int64, c *evalCtx) (int64, bool, error) {
	e, err := g.eval(expr, c)
	if err != nil {
		return 0, false, err
	}
	if e < 0 {
		return 0, false, nil // the data's "no price" (a fallback of -1): the listing cannot be bought
	}
	v, err := g.apply(mods, math.Ceil(e), currency, c)
	if err != nil {
		return 0, false, err
	}
	v = math.Ceil(math.Max(v, 0))
	if v > 1e15 {
		return 0, false, fmt.Errorf("a price of %v", v)
	}
	return int64(v), true, nil
}

func tagSet(tags []string) map[string]bool {
	out := map[string]bool{}
	for _, t := range tags {
		t = strings.ToLower(t)
		for {
			out[t] = true
			i := strings.LastIndex(t, ".")
			if i < 0 {
				break
			}
			t = t[:i]
		}
	}
	return out
}

func jsonKeys(keys [][3]any) string { return jsonText(keys) }
