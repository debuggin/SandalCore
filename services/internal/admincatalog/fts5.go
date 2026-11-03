// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admincatalog

import (
	"context"
	"database/sql"
	"errors"
	"math"
	"sort"
	"strings"
	"sync"
	"unicode"
)

const (
	KindItem    = "item"
	KindNPC     = "npc"
	KindQuest   = "quest"
	KindAbility = "ability"
	KindZone    = "zone"

	defaultSearchLimit = 25
	maxSearchLimit     = 100
	bm25K1             = 1.2
	bm25B              = 0.75
)

type CatalogEntry struct {
	GUID  string   `json:"guid"`
	Kind  string   `json:"kind"`
	Name  string   `json:"name"`
	Tags  []string `json:"tags,omitempty"`
	Zone  string   `json:"zone,omitempty"`
	Score float64  `json:"score,omitempty"`
}

type indexedDoc struct {
	entry      CatalogEntry
	nameLower  string
	guidLower  string
	nameTokens []string
	tagTokens  []string
	zoneTokens []string
	tokenFreq  map[string]float64
	docLen     float64
}

type Catalog struct {
	mu      sync.RWMutex
	docs    []indexedDoc
	byGUID  map[string]int
	docFreq map[string]int
	avgDL   float64
}

func NewCatalog() *Catalog {
	return &Catalog{
		byGUID:  make(map[string]int),
		docFreq: make(map[string]int),
	}
}

func NewDefaultCatalog() *Catalog {
	c := NewCatalog()
	_ = c.IndexEntries(DefaultEntries())
	return c
}

func (c *Catalog) Count() int {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return len(c.docs)
}

func (c *Catalog) IndexEntries(entries []CatalogEntry) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, raw := range entries {
		entry, err := normalizeEntry(raw)
		if err != nil {
			return err
		}
		doc := buildIndexedDoc(entry)
		if idx, exists := c.byGUID[entry.GUID]; exists {
			c.docs[idx] = doc
		} else {
			c.byGUID[entry.GUID] = len(c.docs)
			c.docs = append(c.docs, doc)
		}
	}
	c.recomputeCorpusStats()
	return nil
}

func normalizeEntry(e CatalogEntry) (CatalogEntry, error) {
	e.GUID = strings.TrimSpace(e.GUID)
	e.Kind = strings.ToLower(strings.TrimSpace(e.Kind))
	e.Name = strings.TrimSpace(e.Name)
	e.Zone = strings.TrimSpace(e.Zone)
	if e.GUID == "" || e.Name == "" || e.Kind == "" {
		return CatalogEntry{}, errors.New("admincatalog: guid, kind, and name are required")
	}
	return e, nil
}

func buildIndexedDoc(e CatalogEntry) indexedDoc {
	nameToks := tokenize(e.Name)
	guidToks := tokenize(e.GUID)
	zoneToks := tokenize(e.Zone)
	var tagToks []string
	for _, t := range e.Tags {
		tagToks = append(tagToks, tokenize(t)...)
	}
	tf := make(map[string]float64, len(nameToks)+len(tagToks)+len(zoneToks)+2)
	addWeightedTokens(tf, nameToks, 3.0)
	addWeightedTokens(tf, guidToks, 2.5)
	addWeightedTokens(tf, tagToks, 1.5)
	addWeightedTokens(tf, zoneToks, 1.2)

	dl := float64(len(nameToks) + len(tagToks) + len(zoneToks) + 1)
	return indexedDoc{
		entry:      e,
		nameLower:  strings.ToLower(e.Name),
		guidLower:  strings.ToLower(e.GUID),
		nameTokens: nameToks,
		tagTokens:  tagToks,
		zoneTokens: zoneToks,
		tokenFreq:  tf,
		docLen:     dl,
	}
}

func addWeightedTokens(tf map[string]float64, tokens []string, weight float64) {
	for _, tok := range tokens {
		tf[tok] += weight
	}
}

func (c *Catalog) recomputeCorpusStats() {
	clear(c.docFreq)
	totalLen := 0.0
	for _, d := range c.docs {
		totalLen += d.docLen
		for tok := range d.tokenFreq {
			c.docFreq[tok]++
		}
	}
	if len(c.docs) > 0 {
		c.avgDL = totalLen / float64(len(c.docs))
	} else {
		c.avgDL = 1.0
	}
}

// Search executes tokenized prefix, fuzzy 1-edit, and Okapi BM25 ranking over the catalog.
func (c *Catalog) Search(query, kindFilter string, limit int) []CatalogEntry {
	limit = clampLimit(limit)
	kindFilter = strings.ToLower(strings.TrimSpace(kindFilter))
	qTrim := strings.ToLower(strings.TrimSpace(query))
	qTokens := tokenize(qTrim)

	c.mu.RLock()
	defer c.mu.RUnlock()

	if len(qTokens) == 0 {
		return c.listByKind(kindFilter, limit)
	}
	hits := make([]CatalogEntry, 0, limit)
	for _, doc := range c.docs {
		if kindFilter != "" && doc.entry.Kind != kindFilter {
			continue
		}
		if score, ok := c.scoreDocument(doc, qTrim, qTokens); ok {
			hit := doc.entry
			hit.Score = math.Round(score*1000) / 1000
			hits = append(hits, hit)
		}
	}
	sort.Slice(hits, func(i, j int) bool {
		if hits[i].Score != hits[j].Score {
			return hits[i].Score > hits[j].Score
		}
		if hits[i].Name != hits[j].Name {
			return hits[i].Name < hits[j].Name
		}
		return hits[i].GUID < hits[j].GUID
	})
	if len(hits) > limit {
		hits = hits[:limit]
	}
	return hits
}

func (c *Catalog) listByKind(kindFilter string, limit int) []CatalogEntry {
	out := make([]CatalogEntry, 0, limit)
	for _, doc := range c.docs {
		if kindFilter != "" && doc.entry.Kind != kindFilter {
			continue
		}
		out = append(out, doc.entry)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Name != out[j].Name {
			return out[i].Name < out[j].Name
		}
		return out[i].GUID < out[j].GUID
	})
	if len(out) > limit {
		out = out[:limit]
	}
	return out
}

func (c *Catalog) scoreDocument(doc indexedDoc, rawQuery string, qTokens []string) (float64, bool) {
	total := 0.0
	if doc.guidLower == rawQuery || doc.nameLower == rawQuery {
		total += 12.0
	} else if strings.HasPrefix(doc.nameLower, rawQuery) || strings.HasPrefix(doc.guidLower, rawQuery) {
		total += 5.0
	}
	for _, qt := range qTokens {
		tokScore := c.scoreToken(doc, qt)
		if tokScore <= 0 {
			return 0, false
		}
		total += tokScore
	}
	return total, true
}

func (c *Catalog) scoreToken(doc indexedDoc, qt string) float64 {
	if tf, exact := doc.tokenFreq[qt]; exact {
		return c.bm25TermScore(qt, tf, doc.docLen) * 1.5
	}
	best := 0.0
	for tok, tf := range doc.tokenFreq {
		switch {
		case strings.HasPrefix(tok, qt):
			cand := c.bm25TermScore(tok, tf, doc.docLen) * 1.15
			if cand > best {
				best = cand
			}
		case len(qt) >= 3 && (strings.Contains(tok, qt) || withinEditDistanceOne(tok, qt)):
			cand := c.bm25TermScore(tok, tf, doc.docLen) * 0.65
			if cand > best {
				best = cand
			}
		}
	}
	return best
}

func (c *Catalog) bm25TermScore(tok string, tf, docLen float64) float64 {
	n := float64(len(c.docs))
	df := float64(c.docFreq[tok])
	idf := math.Log(1.0 + (n-df+0.5)/(df+0.5))
	norm := tf * (bm25K1 + 1.0) / (tf + bm25K1*(1.0-bm25B+bm25B*(docLen/c.avgDL)))
	return idf * norm
}

func FormatFTS5MatchQuery(query string) string {
	tokens := tokenize(query)
	if len(tokens) == 0 {
		return ""
	}
	parts := make([]string, 0, len(tokens))
	for _, tok := range tokens {
		parts = append(parts, `"`+tok+`"*`)
	}
	return strings.Join(parts, " AND ")
}

func (c *Catalog) LoadFromSQLite(ctx context.Context, db *sql.DB) error {
	rows, err := db.QueryContext(ctx, `SELECT guid, kind, name, COALESCE(tags, ''), COALESCE(zone, '') FROM admin_catalog`)
	if err != nil {
		return err
	}
	defer rows.Close()
	var batch []CatalogEntry
	for rows.Next() {
		var e CatalogEntry
		var rawTags string
		if err := rows.Scan(&e.GUID, &e.Kind, &e.Name, &rawTags, &e.Zone); err != nil {
			return err
		}
		if rawTags != "" {
			e.Tags = strings.Split(rawTags, ",")
		}
		batch = append(batch, e)
	}
	if err := rows.Err(); err != nil {
		return err
	}
	return c.IndexEntries(batch)
}

func clampLimit(limit int) int {
	if limit <= 0 {
		return defaultSearchLimit
	}
	if limit > maxSearchLimit {
		return maxSearchLimit
	}
	return limit
}

func tokenize(s string) []string {
	s = strings.ToLower(s)
	return strings.FieldsFunc(s, func(r rune) bool {
		return !unicode.IsLetter(r) && !unicode.IsDigit(r)
	})
}

func withinEditDistanceOne(a, b string) bool {
	la, lb := len(a), len(b)
	if la-lb > 1 || lb-la > 1 {
		return false
	}
	i, j, edits := 0, 0, 0
	for i < la && j < lb {
		if a[i] == b[j] {
			i++
			j++
			continue
		}
		edits++
		if edits > 1 {
			return false
		}
		if la > lb {
			i++
		} else if lb > la {
			j++
		} else {
			i++
			j++
		}
	}
	if i < la || j < lb {
		edits++
	}
	return edits <= 1
}

func DefaultEntries() []CatalogEntry {
	return []CatalogEntry{
		{GUID: "item:6064629399916707840", Kind: KindItem, Name: "Radiant Ashbringer Greatsword", Tags: []string{"weapon", "twohanded", "legendary"}, Zone: "riverlands"},
		{GUID: "item:6064629399916707841", Kind: KindItem, Name: "Elixir of Ember Ward", Tags: []string{"consumable", "alchemy", "fire"}, Zone: "winstead"},
		{GUID: "npc:7001001", Kind: KindNPC, Name: "High Warden Kaelen", Tags: []string{"guard", "elite", "sanctum"}, Zone: "lionhold"},
		{GUID: "npc:7001002", Kind: KindNPC, Name: "Ashen Drake Matriarch", Tags: []string{"boss", "dragon", "fire"}, Zone: "obsidian_crag"},
		{GUID: "quest:8002001", Kind: KindQuest, Name: "Siege of the Ashen Sanctum", Tags: []string{"raid", "story", "campaign"}, Zone: "lionhold"},
		{GUID: "ability:9003001", Kind: KindAbility, Name: "Cataclysmic Nova", Tags: []string{"aoe", "mage", "fire"}, Zone: "global"},
		{GUID: "zone:riverlands", Kind: KindZone, Name: "The Riverlands", Tags: []string{"temperate", "nodes", "starter"}, Zone: "riverlands"},
		{GUID: "zone:lionhold", Kind: KindZone, Name: "Lionhold Bastion", Tags: []string{"sanctuary", "capital", "starting_zone"}, Zone: "lionhold"},
	}
}
