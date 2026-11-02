// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	"aocrop/services/internal/aocexpr"
	"aocrop/services/internal/designdb"
)

func loadVendorItems(g *priceGen, src *source) error {
	for _, r := range src.rows["ItemRecordBase"] {
		f := newFields(fmt.Sprintf("Item %s (%d)", r.Name, r.Guid), r.Fields)
		it := &vitem{guid: r.Guid, name: r.Name, level: f.int("Level"), quality: f.int("DefaultQuality")}
		it.qualityCurv, _ = f.id("QualityCurveId")
		it.rarityMin, it.rarityMax = 0, len(rarities)-1
		if _, has := f.get("RarityMin"); has {
			it.rarityMin = rarityIndex(enumValue(f.str("RarityMin")))
			it.rarityMax = rarityIndex(enumValue(f.str("RarityMax")))
		}
		if _, has := f.get("BaseVendorValueId"); has {
			it.baseValue, _ = f.id("BaseVendorValueId")
		}
		if v, has := f.get("Vendorable"); has {
			it.vendorable, _ = v.(bool)
		}
		if v, has := f.get("Blacklisted"); has {
			it.blacklisted, _ = v.(bool)
		}
		if _, has := f.get("GameplayTags"); has {
			it.tags = tagSet(f.tags("GameplayTags"))
		} else {
			it.tags = map[string]bool{}
		}
		if f.err != nil {
			return f.err
		}
		g.items[r.Guid] = it
	}
	for _, r := range src.rows["ItemVariationRecordBase"] {
		f := newFields(fmt.Sprintf("ItemVariation %s (%d)", r.Name, r.Guid), r.Fields)
		g.varQual[r.Guid] = f.str("Quality")
		if f.err != nil {
			return f.err
		}
	}
	return nil
}

func loadVendorBasesAndOffers(g *priceGen, src *source) ([]int64, error) {
	var baseIDs []int64
	for _, r := range src.rows["BaseItemValueRecordBase"] {
		f := newFields(fmt.Sprintf("BaseItemValue %s (%d)", r.Name, r.Guid), r.Fields)
		b := &baseValue{guid: r.Guid, name: r.Name}
		b.buyCur, _ = f.id("BuyingCurrencyTierId")
		b.sellCur, _ = f.id("SellingCurrencyTierId")
		b.buy = readCurExprs(f, "CurrencyValues")
		b.override = f.boolean("bUsesOverrideSellValues")
		b.sell = readCurExprs(f, "OverrideSellValues")
		if f.err != nil {
			return nil, f.err
		}
		g.bases[r.Guid] = b
		baseIDs = append(baseIDs, r.Guid)
	}
	for _, r := range src.rows["VendorOfferRecordBase"] {
		f := newFields(fmt.Sprintf("VendorOffer %s (%d)", r.Name, r.Guid), r.Fields)
		o := &baseValue{guid: r.Guid, name: r.Name, buy: readCurExprs(f, "CurrencyValues")}
		if f.err != nil {
			return nil, f.err
		}
		g.offers[r.Guid] = o
	}
	return baseIDs, nil
}

func loadVendorModifiers(g *priceGen, src *source) ([]int64, error) {
	var modIDs []int64
	for _, r := range src.rows["FVendorPriceModifierRecordBase"] {
		f := newFields(fmt.Sprintf("VendorPriceModifier %s (%d)", r.Name, r.Guid), r.Fields)
		m := &vmodifier{guid: r.Guid, name: r.Name, contexts: f.int("AffectedContexts"), operation: enumValue(f.str("ArithmeticOperation")),
			source: enumValue(f.str("ModifierValueSource")), factor: enumValue(f.str("PriceFactor")), exprS: f.str("ValueExpression"),
			currencies: f.ids("AffectedCurrenciesIds"), children: f.ids("PriceModifiersIds")}
		m.filter = readTagQuery(f.strukt("ItemFilter"))
		vc := newFields(f.where+": ValueCurve", f.strukt("ValueCurve"))
		ed := newFields(f.where+": ValueCurve.EditorCurveData", vc.strukt("EditorCurveData"))
		keys := ed.list("Keys")
		if err := firstErr(f, vc, ed); err != nil {
			return nil, err
		}
		var ks []aocexpr.Key
		for i, kv := range keys {
			ksr, ok := kv.(designdb.Struct)
			if !ok {
				return nil, fmt.Errorf("%s: curve key %d is %T", f.where, i, kv)
			}
			kf := newFields(fmt.Sprintf("%s: curve key %d", f.where, i), ksr)
			k := aocexpr.Key{Time: kf.float("Time"), Value: kf.float("Value"), Arrive: kf.optFloat("ArriveTangent"), Leave: kf.optFloat("LeaveTangent")}
			mode := kf.str("InterpMode")
			if kf.err != nil {
				return nil, kf.err
			}
			var err error
			if k.Interp, err = aocexpr.ParseInterp(mode); err != nil {
				return nil, fmt.Errorf("%s: curve key %d: %w", f.where, i, err)
			}
			ks = append(ks, k)
			m.curveKeys = append(m.curveKeys, [3]any{k.Time, k.Value, k.Interp.String()})
		}
		if len(ks) > 0 {
			var err error
			if m.curve, err = aocexpr.NewCurve(ks); err != nil {
				return nil, fmt.Errorf("%s: %w", f.where, err)
			}
		}
		g.mods[r.Guid] = m
		modIDs = append(modIDs, r.Guid)
	}
	return modIDs, nil
}

func loadVendorInvsAndDefs(src *source) (map[int64]*vinventory, []*vdef, error) {
	invs := map[int64]*vinventory{}
	for _, r := range src.rows["VendorInventoryDefRecordBase"] {
		f := newFields(fmt.Sprintf("VendorInventoryDef %s (%d)", r.Name, r.Guid), r.Fields)
		inv := &vinventory{name: r.Name}
		if pv, ok := f.get("AvailabilityPredicates"); ok {
			inv.preds = readPredicates(pv)
		}
		for i, lv := range f.list("Listings") {
			ls, ok := lv.(designdb.Struct)
			if !ok {
				return nil, nil, fmt.Errorf("%s: Listings[%d] is %T", f.where, i, lv)
			}
			lf := newFields(fmt.Sprintf("%s: Listings[%d]", f.where, i), ls)
			l := vlisting{preds: readPredicates(ls["AvailabilityPredicates"])}
			l.item, _ = lf.id("ItemId")
			if _, has := lf.get("ItemVariation"); has {
				l.variation, _ = lf.id("ItemVariation")
			}
			l.offers = lf.ids("Offers")
			if lf.err != nil {
				return nil, nil, lf.err
			}
			inv.listings = append(inv.listings, l)
		}
		if f.err != nil {
			return nil, nil, f.err
		}
		invs[r.Guid] = inv
	}
	var defs []*vdef
	for _, r := range src.rows["VendorDefRecordBase"] {
		f := newFields(fmt.Sprintf("VendorDef %s (%d)", r.Name, r.Guid), r.Fields)
		d := &vdef{guid: r.Guid, name: r.Name, ops: f.int("SupportedOperations"), blacklisted: f.boolean("Blacklisted"), fromWh: f.boolean("bCanSellFromWarehouse"),
			invs: f.ids("InventoryIds"), mods: f.ids("PriceModifiersIds"), filters: f.ids("SellFiltersIds")}
		if tag, ok := f.strukt("TaxTag")["TagName"].(string); ok && tag != "None" {
			d.tax = tag
		}
		if f.err != nil {
			return nil, nil, f.err
		}
		defs = append(defs, d)
	}
	return invs, defs, nil
}

// --- the flat tables ---------------------------------------------------------------------------------------------------------------------

func writeVendorDefAndModTables(w *writer, src *source, g *priceGen, defs []*vdef, modIDs []int64) error {
	for _, d := range defs {
		if err := w.exec(`INSERT INTO vendor_def_data (vendor_guid, blacklisted, tax_tag, from_warehouse) VALUES (?, ?, ?, ?)`, d.guid, b2i(d.blacklisted), d.tax, b2i(d.fromWh)); err != nil {
			return err
		}
		for i, m := range d.mods {
			if err := w.exec(`INSERT INTO vendor_def_modifier (vendor_guid, ord, modifier_guid) VALUES (?, ?, ?)`, d.guid, i, m); err != nil {
				return err
			}
		}
		for i, fl := range d.filters {
			if err := w.exec(`INSERT INTO vendor_def_sell_filter (vendor_guid, ord, filter_guid) VALUES (?, ?, ?)`, d.guid, i, fl); err != nil {
				return err
			}
		}
	}
	sort.Slice(modIDs, func(i, j int) bool { return modIDs[i] < modIDs[j] })
	for _, id := range modIDs {
		m := g.mods[id]
		filter := ""
		if m.filter != nil {
			filter = jsonText(m.filter)
		}
		if m.currencies == nil {
			m.currencies = []int64{}
		}
		if m.children == nil {
			m.children = []int64{}
		}
		if m.curveKeys == nil {
			m.curveKeys = [][3]any{}
		}
		if err := w.exec(`INSERT INTO vendor_modifier (guid, name, contexts, operation, source, factor, expression, curve, filter, currencies, children) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			m.guid, m.name, m.contexts, m.operation, m.source, m.factor, m.exprS, jsonKeys(m.curveKeys), filter, jsonText(m.currencies), jsonText(m.children)); err != nil {
			return err
		}
	}
	for _, r := range src.rows["VendorSellFilterRecordBase"] {
		f := newFields(fmt.Sprintf("VendorSellFilter %s (%d)", r.Name, r.Guid), r.Fields)
		q := readTagQuery(f.strukt("ItemFilter"))
		white := f.ids("ItemWhitelistIds")
		if f.err != nil {
			return f.err
		}
		filter := ""
		if q != nil {
			filter = jsonText(q)
		}
		if white == nil {
			white = []int64{}
		}
		if err := w.exec(`INSERT INTO vendor_sell_filter (guid, name, filter, whitelist) VALUES (?, ?, ?, ?)`, r.Guid, r.Name, filter, jsonText(white)); err != nil {
			return err
		}
	}
	return nil
}

func pairsJSON(l []curExpr) string {
	out := make([][2]any, 0, len(l))
	for _, c := range l {
		out = append(out, [2]any{c.currency, c.expr})
	}
	return jsonText(out)
}

func writeVendorValueAndAvailTables(w *writer, g *priceGen, baseIDs []int64, invs map[int64]*vinventory) (int, int, error) {
	sort.Slice(baseIDs, func(i, j int) bool { return baseIDs[i] < baseIDs[j] })
	for _, id := range baseIDs {
		b := g.bases[id]
		if err := w.exec(`INSERT INTO base_item_value (guid, name, buy_currency, sell_currency, buy, override_sell, sell) VALUES (?, ?, ?, ?, ?, ?, ?)`,
			b.guid, b.name, b.buyCur, b.sellCur, pairsJSON(b.buy), b2i(b.override), pairsJSON(b.sell)); err != nil {
			return 0, 0, err
		}
	}
	itemIDs := make([]int64, 0, len(g.items))
	for id, it := range g.items {
		if it.baseValue != 0 {
			itemIDs = append(itemIDs, id)
		}
	}
	sort.Slice(itemIDs, func(i, j int) bool { return itemIDs[i] < itemIDs[j] })
	for _, id := range itemIDs {
		if err := w.exec(`INSERT INTO item_base_value (item_guid, base_value_guid) VALUES (?, ?)`, id, g.items[id].baseValue); err != nil {
			return 0, 0, err
		}
	}
	offerIDs := make([]int64, 0, len(g.offers))
	for id := range g.offers {
		offerIDs = append(offerIDs, id)
	}
	sort.Slice(offerIDs, func(i, j int) bool { return offerIDs[i] < offerIDs[j] })
	for _, id := range offerIDs {
		o := g.offers[id]
		if err := w.exec(`INSERT INTO vendor_offer (guid, name, prices) VALUES (?, ?, ?)`, o.guid, o.name, pairsJSON(o.buy)); err != nil {
			return 0, 0, err
		}
	}
	invIDs := make([]int64, 0, len(invs))
	for id := range invs {
		invIDs = append(invIDs, id)
	}
	sort.Slice(invIDs, func(i, j int) bool { return invIDs[i] < invIDs[j] })
	preds := 0
	for _, id := range invIDs {
		inv := invs[id]
		write := func(listing int, ps []vpredicate) error {
			for i, p := range ps {
				if err := w.exec(`INSERT INTO vendor_availability (inventory_guid, listing_ord, ord, expression, failure, hidden) VALUES (?, ?, ?, ?, ?, ?)`,
					id, listing, i, p.expr, p.failure, b2i(p.hidden)); err != nil {
					return err
				}
				preds++
			}
			return nil
		}
		if err := write(-1, inv.preds); err != nil {
			return 0, 0, err
		}
		for i, l := range inv.listings {
			if err := write(i, l.preds); err != nil {
				return 0, 0, err
			}
		}
	}
	return len(offerIDs), preds, nil
}

// --- the links: which NPC opens which shop ------------------------------------------------------------------------------------------------

func writeNpcVendorLinks(w *writer, src *source, defs []*vdef) (int, error) {
	vendorOf := map[int64]bool{}
	for _, d := range defs {
		vendorOf[d.guid] = true
	}
	interactions := map[int64][]int64{} // PlayerInteractionDef -> its action responses
	for _, r := range src.rows["PlayerInteractionDefRecordBase"] {
		f := newFields(fmt.Sprintf("PlayerInteractionDef %s (%d)", r.Name, r.Guid), r.Fields)
		list := append(f.ids("CompletionResponsesIds"), f.ids("ChannelStartResponseIds")...)
		if f.err != nil {
			return 0, f.err
		}
		interactions[r.Guid] = list
	}
	opens := map[int64]int64{} // ActionResponse -> the VendorDef it opens (OpenCustomVendorWindow)
	for _, r := range src.rows["ActionResponseRecordBase"] {
		f := newFields(fmt.Sprintf("ActionResponse %s (%d)", r.Name, r.Guid), r.Fields)
		if enumValue(f.str("ResponseType")) != "OpenCustomVendorWindow" {
			continue
		}
		vid, _ := f.id("VendorId")
		if f.err != nil {
			return 0, f.err
		}
		if vid != 0 {
			opens[r.Guid] = vid
		}
	}
	shopOf := func(interaction int64) int64 {
		for _, a := range interactions[interaction] {
			if v := opens[a]; v != 0 && vendorOf[v] {
				return v
			}
		}
		return 0
	}
	links := 0
	for _, kind := range []struct{ kind, schema, field string }{{"instance", "PopulationInstanceRecordBase", "InteractResponseId"}, {"asset", "PopulationAssetRecordBase", "InteractionDefId"}} {
		for _, r := range src.rows[kind.schema] {
			f := newFields(fmt.Sprintf("%s %s (%d)", kind.schema, r.Name, r.Guid), r.Fields)
			ref, _ := f.id(kind.field)
			if f.err != nil {
				return 0, f.err
			}
			if v := shopOf(ref); v != 0 {
				if err := w.exec(`INSERT INTO npc_vendor_link (source_kind, source_guid, vendor_guid) VALUES (?, ?, ?)`, kind.kind, r.Guid, v); err != nil {
					return 0, err
				}
				links++
			}
		}
	}
	return links, nil
}

// --- the prices ---------------------------------------------------------------------------------------------------------------------------

type vendorPriceRow struct {
	offer, currency, price int64
}

func (g *priceGen) listingEvalCtx(l vlisting, where string) (*evalCtx, bool) {
	it := g.items[l.item]
	if it == nil {
		g.skip("the listing names an item that is no record", where)
		return nil, false
	}
	quality := it.quality
	if l.variation != 0 {
		q, perr := strconv.ParseFloat(strings.TrimSpace(g.varQual[l.variation]), 64)
		if perr != nil {
			g.skip("a variation whose quality is no plain number", where)
			return nil, false
		}
		quality = int64(q)
	}
	return &evalCtx{item: it, quality: quality, rarity: g.rarityOf(it, quality)}, true
}

func (g *priceGen) priceListing(d *vdef, inv *vinventory, ord int, l vlisting) ([]vendorPriceRow, bool) {
	it := g.items[l.item]
	itemLabel := strconv.FormatInt(l.item, 10)
	if it != nil {
		itemLabel = it.name
	}
	where := fmt.Sprintf("%s listing %d of %s (%s)", d.name, ord, inv.name, itemLabel)
	c, ok := g.listingEvalCtx(l, where)
	if !ok {
		return nil, false
	}
	var out []vendorPriceRow
	offers := l.offers
	if len(offers) == 0 {
		offers = []int64{0}
	}
	for _, offer := range offers {
		var exprs []curExpr
		if offer == 0 {
			b := g.bases[it.baseValue]
			if b == nil {
				g.skip("the item has no base value record", where)
				return nil, false
			}
			exprs = b.buy
		} else {
			o := g.offers[offer]
			if o == nil {
				g.skip("the listing names an offer that is no record", where)
				return nil, false
			}
			exprs = o.buy
		}
		for _, ce := range exprs {
			p, ok, perr := g.price(ce.expr, ce.currency, d.mods, c)
			if perr != nil {
				g.skip(reasonOf(perr), where)
				return nil, false
			}
			if !ok {
				g.skip("the expression gives no price (negative)", where)
				return nil, false
			}
			out = append(out, vendorPriceRow{offer, ce.currency, p})
		}
	}
	return out, true
}

func writeVendorPrices(w *writer, g *priceGen, defs []*vdef, invs map[int64]*vinventory) (int, int, error) {
	rows, listings := 0, 0
	perDef := map[string][2]int{}
	for _, d := range defs {
		if strings.HasPrefix(d.name, "PTR_ONLY_") {
			continue // the test realm's free shops: the GM vendors price nothing
		}
		priced, total := 0, 0
		for _, invID := range d.invs {
			inv := invs[invID]
			if inv == nil {
				continue
			}
			for ord, l := range inv.listings {
				total++
				listings++
				out, ok := g.priceListing(d, inv, ord, l)
				if !ok {
					continue
				}
				for _, rw := range out {
					if err := w.exec(`INSERT OR IGNORE INTO vendor_price (vendor_guid, inventory_guid, ord, item_guid, variation_guid, offer_guid, currency_guid, price) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
						d.guid, invID, ord, l.item, l.variation, rw.offer, rw.currency, rw.price); err != nil {
						return 0, 0, err
					}
					rows++
				}
				priced++
			}
		}
		perDef[d.name] = [2]int{priced, total}
	}
	// the shop the first step opens must price every listing: its three tabs are the proof the rule and the data agree
	if mp, ok := perDef["Marketplace_Vendor"]; !ok || mp[0] != mp[1] || mp[1] == 0 {
		return 0, 0, fmt.Errorf("vendor prices: Marketplace_Vendor priced %d of %d listings (every one must have a price); first problems: %v", mp[0], mp[1], g.example)
	}
	return rows, listings, nil
}

func buildVendors(w *writer, src *source, opt *Options, meta map[string]string) error {
	for _, schema := range vendorRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
	}
	g := &priceGen{curves: src.curves, items: map[int64]*vitem{}, varQual: map[int64]string{}, bases: map[int64]*baseValue{}, offers: map[int64]*baseValue{},
		mods: map[int64]*vmodifier{}, programs: map[string]*aocexpr.Program{}, skipped: map[string]int{}, example: map[string]string{}}
	var err error
	if g.bands, err = readQualityCurves(src.rows["QualityCurveRecordBase"]); err != nil {
		return err
	}
	if err := loadVendorItems(g, src); err != nil {
		return err
	}
	baseIDs, err := loadVendorBasesAndOffers(g, src)
	if err != nil {
		return err
	}
	modIDs, err := loadVendorModifiers(g, src)
	if err != nil {
		return err
	}
	invs, defs, err := loadVendorInvsAndDefs(src)
	if err != nil {
		return err
	}
	if err := writeVendorDefAndModTables(w, src, g, defs, modIDs); err != nil {
		return err
	}
	offerCount, preds, err := writeVendorValueAndAvailTables(w, g, baseIDs, invs)
	if err != nil {
		return err
	}
	links, err := writeNpcVendorLinks(w, src, defs)
	if err != nil {
		return err
	}
	rows, listings, err := writeVendorPrices(w, g, defs, invs)
	if err != nil {
		return err
	}
	reasons := make([]string, 0, len(g.skipped))
	for k := range g.skipped {
		reasons = append(reasons, k)
	}
	sort.Strings(reasons)
	skippedTotal := 0
	for _, k := range reasons {
		skippedTotal += g.skipped[k]
		opt.logf("vendor_price: %d listing(s) without a price: %s (e.g. %s)", g.skipped[k], k, g.example[k])
	}
	opt.logf("vendors: %d defs, %d modifiers, %d sell filters, %d base values, %d offers, %d predicates, %d NPC links, %d listings priced into %d rows (%d without a price)",
		len(defs), len(modIDs), len(src.rows["VendorSellFilterRecordBase"]), len(baseIDs), offerCount, preds, links, listings-skippedTotal, rows, skippedTotal)
	meta["typed.npc_vendor_link"] = strconv.Itoa(links)
	meta["typed.vendor_price"] = strconv.Itoa(rows)
	meta["typed.vendor_modifier"] = strconv.Itoa(len(modIDs))
	meta["typed.vendor_listings_unpriced"] = strconv.Itoa(skippedTotal)
	return buildVendorSaleFacts(w, g, opt, meta)
}

func buildVendorSaleFacts(w *writer, g *priceGen, opt *Options, meta map[string]string) error {
	// --- what a sale needs of an item (contentc version 19, the selling step) ------------------------------------------------------------------
	soldRows, sellable, unpriced := writeSaleFacts(w, g)
	reasons := make([]string, 0, len(unpriced))
	for k := range unpriced {
		reasons = append(reasons, k)
	}
	sort.Strings(reasons)
	unpricedTotal := 0
	for _, k := range reasons {
		unpricedTotal += unpriced[k].count
		opt.logf("item_sell_value: %d item(s) without a sale value: %s (e.g. %s)", unpriced[k].count, k, unpriced[k].example)
	}
	if soldRows.err != nil {
		return soldRows.err
	}
	opt.logf("item_vendor: %d items, %d of them with a base value (not blacklisted) into %d item_sell_value rows (%d without a value)", len(g.items), sellable, soldRows.rows, unpricedTotal)
	meta["typed.item_vendor"] = strconv.Itoa(len(g.items))
	meta["typed.item_sell_value"] = strconv.Itoa(soldRows.rows)
	return nil
}

// sold is the outcome of writeSaleFacts.
type sold struct {
	rows int
	err  error
}

type reasonCount struct {
	count   int
	example string
}

// writeSaleFacts writes item_vendor (one row per item) and item_sell_value: for every item with a base value record that is not blacklisted, and every rarity the item can have, its
// base value for a sale in each currency of the record's sale expressions (the record's own when it overrides them, else its buy expressions), rounded up.  Whether a shop may buy
// the item is item_vendor's business (Vendorable); the value is also what a repair is priced from.  An item whose expression this builder cannot run, or that gives no price, gets
// no value (it can be neither sold nor repaired); the reasons come back counted.
func writeSaleFacts(w *writer, g *priceGen) (sold, int, map[string]*reasonCount) {
	ids := make([]int64, 0, len(g.items))
	for id := range g.items {
		ids = append(ids, id)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	unpriced := map[string]*reasonCount{}
	note := func(reason, where string) {
		if rc := unpriced[reason]; rc != nil {
			rc.count++
			return
		}
		unpriced[reason] = &reasonCount{count: 1, example: where}
	}
	var out sold
	sellable := 0
	for _, id := range ids {
		it := g.items[id]
		if err := w.exec(`INSERT INTO item_vendor (item_guid, vendorable, blacklisted, rarity_min, rarity_max) VALUES (?, ?, ?, ?, ?)`,
			it.guid, b2i(it.vendorable), b2i(it.blacklisted), it.rarityMin, it.rarityMax); err != nil {
			out.err = err
			return out, sellable, unpriced
		}
		// every item with a base value gets its values, vendorable or not: a shop buys only what is vendorable (item_vendor), but a worn item is mended by its value whether or
		// not a shop would buy it (the starter tools are not vendorable)
		if it.blacklisted || it.baseValue == 0 {
			continue
		}
		b := g.bases[it.baseValue]
		if b == nil {
			note("the item has no base value record", it.name)
			continue
		}
		sellable++
		exprs := b.buy
		if b.override {
			exprs = b.sell
		}
		if len(exprs) == 0 {
			note("the base value record overrides the sale values and lists none", it.name)
			continue
		}
		n, err := writeItemSellRarities(w, g, it, exprs, note)
		out.rows += n
		if err != nil {
			out.err = err
			return out, sellable, unpriced
		}
	}
	return out, sellable, unpriced
}

func writeItemSellRarities(w *writer, g *priceGen, it *vitem, exprs []curExpr, note func(string, string)) (int, error) {
	rows := 0
	for rarity := it.rarityMin; rarity <= it.rarityMax; rarity++ {
		c := &evalCtx{item: it, quality: it.quality, rarity: rarity}
		for _, ce := range exprs {
			e, err := g.eval(ce.expr, c)
			if err != nil {
				note(reasonOf(err), fmt.Sprintf("%s at rarity %d", it.name, rarity))
				continue
			}
			if e < 0 || e > 1e15 {
				note("the expression gives no price (negative or out of range)", fmt.Sprintf("%s at rarity %d", it.name, rarity))
				continue
			}
			if err := w.exec(`INSERT OR IGNORE INTO item_sell_value (item_guid, rarity, currency_guid, price) VALUES (?, ?, ?, ?)`, it.guid, rarity, ce.currency, int64(math.Ceil(e))); err != nil {
				return rows, err
			}
			rows++
		}
	}
	return rows, nil
}
