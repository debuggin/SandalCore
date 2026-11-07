// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admincatalog

import (
	"bufio"
	"context"
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	_ "modernc.org/sqlite"
)

type ItemEntry struct {
	GUID        string `json:"guid"`
	Name        string `json:"name"`
	DisplayName string `json:"display_name"`
	EquipSlot   string `json:"equip_slot"`
	RarityMin   int    `json:"rarity_min"`
	RarityMax   int    `json:"rarity_max"`
	MaxStack    int    `json:"max_stack"`
	Level       int    `json:"level"`
}

type NpcEntry struct {
	GUID          string `json:"guid"`
	Name          string `json:"name"`
	DisplayName   string `json:"display_name"`
	Title         string `json:"title,omitempty"`
	NpcDefGUID    string `json:"npc_def_guid"`
	ArchetypePath string `json:"archetype_path"`
	BlueprintLeaf string `json:"blueprint_leaf"`
	MinLevel      int    `json:"min_level"`
	MaxLevel      int    `json:"max_level"`
}

type PlaceEntry struct {
	GUID        string  `json:"guid"`
	Name        string  `json:"name"`
	DisplayName string  `json:"display_name"`
	NodeName    string  `json:"node_name"`
	NodeType    string  `json:"node_type"`
	Kind        string  `json:"kind"`
	X           float64 `json:"x"`
	Y           float64 `json:"y"`
	Z           float64 `json:"z"`
	Yaw         float64 `json:"yaw"`
	Radius      float64 `json:"radius"`
	Levelable   bool    `json:"levelable"`
	Level       int     `json:"level"`
	StageName   string  `json:"stage_name,omitempty"`
}

type ItemFilter struct {
	Query  string
	Slot   string
	Rarity string
	Sort   string
	Limit  int
	Offset int
}

type NpcFilter struct {
	Query    string
	MinLevel int
	MaxLevel int
	Sort     string
	Limit    int
	Offset   int
}

type PlaceFilter struct {
	Query       string
	Kind        string
	Sort        string
	Limit       int
	Offset      int
	ExtraPlaces []PlaceEntry
	NodeLevels  map[string]int
}

type itemNameTable struct {
	byGUID map[string]string
	byKey  map[string]string
}

func (t itemNameTable) Resolve(guid, dbDisplay, recName string) string {
	if v := strings.TrimSpace(t.byGUID[guid]); v != "" {
		return v
	}
	if v := strings.TrimSpace(t.byKey[dbDisplay]); v != "" {
		return v
	}
	if strings.TrimSpace(dbDisplay) != "" {
		return strings.TrimSpace(dbDisplay)
	}
	if v := strings.TrimSpace(t.byKey[recName]); v != "" {
		return v
	}
	return humanizeIdentifier(recName)
}

type npcNameTable struct {
	byAsset     map[string]string
	byInstance  map[string]string
	byTitle     map[string]string
	byArchetype map[string]string
}

type Store = Catalog

func (c *Store) DataMode(dataType string) string {
	return ResolveDataMode(dataType)
}

func (c *Store) DataModes() map[string]string {
	return map[string]string{
		"zois":       ResolveDataMode("zois"),
		"city_nodes": ResolveDataMode("city_nodes"),
		"pois":       ResolveDataMode("pois"),
		"npcs":       ResolveDataMode("npcs"),
		"items":      ResolveDataMode("items"),
	}
}

func NewCatalogFromContentDB(dbPath string) (*Catalog, error) {
	c := NewCatalog()
	if err := c.LoadContentDB(dbPath); err != nil {
		return nil, err
	}
	return c, nil
}

func (c *Catalog) LoadContentDB(dbPath string) error {
	dbPath = strings.TrimSpace(dbPath)
	if dbPath == "" {
		return fmt.Errorf("admincatalog: empty content.db path")
	}
	if _, err := os.Stat(dbPath); err != nil {
		return fmt.Errorf("admincatalog: %w", err)
	}
	dsn := dbPath + "?_pragma=query_only(1)&_pragma=mmap_size(536870912)&_pragma=cache_size(-8192)&_pragma=temp_store(MEMORY)&_pragma=busy_timeout(5000)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return err
	}
	if err := c.AttachContentDB(context.Background(), db, filepath.Dir(dbPath)); err != nil {
		_ = db.Close()
		return err
	}
	return nil
}

func (c *Catalog) AttachContentDB(ctx context.Context, db *sql.DB, contentDir string) error {
	itemNames := loadItemNamesTSV(filepath.Join(contentDir, "item_names.tsv"))
	npcNames := loadNpcNamesTSV(filepath.Join(contentDir, "npc_names.tsv"), filepath.Join(contentDir, "npc_looks.tsv"))

	items, err := loadItemsFromDB(ctx, db, itemNames)
	if err != nil {
		return fmt.Errorf("admincatalog: load items: %w", err)
	}
	npcs, err := loadNpcsFromDB(ctx, db, npcNames)
	if err != nil {
		return fmt.Errorf("admincatalog: load npcs: %w", err)
	}
	places, err := loadPlacesFromDB(ctx, db)
	if err != nil {
		return fmt.Errorf("admincatalog: load places: %w", err)
	}
	places = mergeDiskTSVPlaces(places, contentDir)

	c.mu.Lock()
	if c.db != nil && c.db != db {
		_ = c.db.Close()
	}
	c.db = db
	c.contentDir = contentDir
	c.items = items
	c.npcs = npcs
	c.places = places
	c.loadedDB = true
	c.docs = nil
	clear(c.byGUID)
	c.mu.Unlock()

	return c.indexLoadedCatalog(items, npcs, places)
}

func (c *Catalog) HasContentDB() bool {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.loadedDB
}

func (c *Catalog) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.db != nil {
		err := c.db.Close()
		c.db = nil
		return err
	}
	return nil
}

func (c *Catalog) indexLoadedCatalog(items []ItemEntry, npcs []NpcEntry, places []PlaceEntry) error {
	entries := make([]CatalogEntry, 0, len(items)+len(npcs)+len(places))
	for _, it := range items {
		tags := []string{it.Name}
		if it.EquipSlot != "" {
			tags = append(tags, it.EquipSlot)
		}
		entries = append(entries, CatalogEntry{GUID: it.GUID, Kind: KindItem, Name: firstNonEmptyStr(it.DisplayName, it.Name), Tags: tags})
	}
	for _, n := range npcs {
		tags := []string{n.Name}
		if n.BlueprintLeaf != "" {
			tags = append(tags, n.BlueprintLeaf)
		}
		entries = append(entries, CatalogEntry{GUID: n.GUID, Kind: KindNPC, Name: firstNonEmptyStr(n.DisplayName, n.Name), Tags: tags})
	}
	for _, p := range places {
		entries = append(entries, CatalogEntry{GUID: p.GUID, Kind: KindZone, Name: firstNonEmptyStr(p.DisplayName, p.Name), Tags: []string{p.Name, p.NodeType}})
	}
	return c.IndexEntries(entries)
}

func loadItemNamesTSV(path string) itemNameTable {
	t := itemNameTable{byGUID: make(map[string]string), byKey: make(map[string]string)}
	f, err := os.Open(path)
	if err != nil {
		return t
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64*1024), 1024*1024)
	for sc.Scan() {
		cols := strings.Split(strings.TrimRight(sc.Text(), "\r\n"), "\t")
		if len(cols) < 2 {
			continue
		}
		c0, c1 := strings.TrimSpace(cols[0]), strings.TrimSpace(cols[1])
		if strings.EqualFold(c0, "table") || strings.EqualFold(c0, "guid") || strings.EqualFold(c0, "id") || strings.HasPrefix(c0, "#") {
			continue
		}
		if len(cols) >= 3 && !isDecimalGUID(c0) {
			if txt := strings.TrimSpace(cols[2]); txt != "" && !strings.HasSuffix(c1, "Desc") {
				t.byKey[c1] = txt
			}
			continue
		}
		if c1 != "" {
			t.byGUID[c0] = c1
			t.byKey[c0] = c1
		}
	}
	return t
}

func loadNpcNamesTSV(namesPath, looksPath string) npcNameTable {
	t := npcNameTable{
		byAsset:     make(map[string]string),
		byInstance:  make(map[string]string),
		byTitle:     make(map[string]string),
		byArchetype: make(map[string]string),
	}
	parseNpcNamesFile(namesPath, &t)
	parseNpcLooksFile(looksPath, &t)
	return t
}

func parseNpcNamesFile(path string, t *npcNameTable) {
	f, err := os.Open(path)
	if err != nil {
		return
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64*1024), 1024*1024)
	for sc.Scan() {
		cols := strings.Split(strings.TrimRight(sc.Text(), "\r\n"), "\t")
		if len(cols) < 2 {
			continue
		}
		c0, c1 := strings.TrimSpace(cols[0]), strings.TrimSpace(cols[1])
		if strings.EqualFold(c0, "kind") || strings.EqualFold(c0, "def_guid") || strings.EqualFold(c0, "guid") || strings.HasPrefix(c0, "#") {
			continue
		}
		if len(cols) >= 4 && (c0 == "asset" || c0 == "instance" || c0 == "set") {
			txt := strings.TrimSpace(cols[3])
			if txt == "" {
				continue
			}
			if c0 == "asset" && t.byAsset[c1] == "" {
				t.byAsset[c1] = txt
			} else if c0 == "instance" && t.byInstance[c1] == "" {
				t.byInstance[c1] = txt
			} else if c0 == "set" {
				if m := strings.TrimSpace(cols[2]); m != "" && t.byInstance[m] == "" {
					t.byInstance[m] = txt
				}
			}
			continue
		}
		if c1 != "" {
			t.byAsset[c0] = c1
			t.byInstance[c0] = c1
			if len(cols) >= 3 && strings.TrimSpace(cols[2]) != "" {
				t.byTitle[c0] = strings.TrimSpace(cols[2])
			}
		}
	}
}

func parseNpcLooksFile(path string, t *npcNameTable) {
	f, err := os.Open(path)
	if err != nil {
		return
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64*1024), 1024*1024)
	for sc.Scan() {
		cols := strings.Split(strings.TrimRight(sc.Text(), "\r\n"), "\t")
		if len(cols) < 3 || strings.EqualFold(strings.TrimSpace(cols[0]), "archetype") {
			continue
		}
		arch := strings.TrimSpace(cols[0])
		name := strings.TrimSpace(cols[2])
		if name == "" && len(cols) >= 6 {
			name = strings.TrimSpace(cols[5])
		}
		if arch != "" && name != "" {
			t.byArchetype[arch] = name
		}
	}
}

func loadItemsFromDB(ctx context.Context, db *sql.DB, names itemNameTable) ([]ItemEntry, error) {
	if !hasTable(ctx, db, "item") {
		return nil, nil
	}
	query := `SELECT guid, name, '', COALESCE(equip_slot, ''), COALESCE(rarity_min, 0), COALESCE(rarity_max, 6), COALESCE(max_stack, 1), COALESCE(level, 1) FROM item ORDER BY guid ASC`
	if hasColumn(ctx, db, "item", "max_stack_size") {
		slotExpr := `''`
		if hasTable(ctx, db, "item_equip_slot") && hasTable(ctx, db, "equip_slot") {
			slotExpr = `COALESCE((SELECT es.name FROM item_equip_slot ies JOIN equip_slot es ON es.slot = ies.slot WHERE ies.item_guid = i.guid ORDER BY ies.ord ASC LIMIT 1), '')`
		}
		rMinExpr, rMaxExpr := `0`, `6`
		if hasTable(ctx, db, "quality_curve") {
			rMinExpr = `COALESCE((SELECT MIN(qc.ord) FROM quality_curve qc WHERE qc.guid = i.quality_curve_guid), 0)`
			rMaxExpr = `COALESCE((SELECT MAX(qc.ord) FROM quality_curve qc WHERE qc.guid = i.quality_curve_guid), 6)`
		}
		dispExpr := `''`
		if hasColumn(ctx, db, "item", "display_name") {
			dispExpr = `COALESCE(i.display_name, '')`
		}
		query = fmt.Sprintf(`SELECT i.guid, i.name, %s, %s, %s, %s, i.max_stack_size, i.level FROM item i ORDER BY i.guid ASC`, dispExpr, slotExpr, rMinExpr, rMaxExpr)
	}
	rows, err := db.QueryContext(ctx, query)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ItemEntry
	for rows.Next() {
		var guid int64
		var name, dbDisp, slot string
		var rMin, rMax, maxStack, level int
		if err := rows.Scan(&guid, &name, &dbDisp, &slot, &rMin, &rMax, &maxStack, &level); err != nil {
			return nil, err
		}
		guidStr := strconv.FormatInt(guid, 10)
		out = append(out, ItemEntry{
			GUID:        guidStr,
			Name:        name,
			DisplayName: names.Resolve(guidStr, dbDisp, name),
			EquipSlot:   slot,
			RarityMin:   rMin,
			RarityMax:   rMax,
			MaxStack:    maxStack,
			Level:       level,
		})
	}
	return out, rows.Err()
}

func loadNpcsFromDB(ctx context.Context, db *sql.DB, names npcNameTable) ([]NpcEntry, error) {
	if !hasTable(ctx, db, "npc_asset") {
		return nil, nil
	}
	instByAsset, minLvByAsset, maxLvByAsset := loadNpcInstanceLinks(ctx, db)
	defCol := "a.guid"
	if hasColumn(ctx, db, "npc_asset", "npc_def_guid") {
		defCol = "COALESCE(a.npc_def_guid, a.guid)"
	}
	rows, err := db.QueryContext(ctx, fmt.Sprintf(`SELECT a.guid, a.name, %s, COALESCE(a.archetype_path, '') FROM npc_asset a ORDER BY a.guid ASC`, defCol))
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []NpcEntry
	for rows.Next() {
		var guid, defGUID int64
		var name, arch string
		if err := rows.Scan(&guid, &name, &defGUID, &arch); err != nil {
			return nil, err
		}
		gStr, defStr := strconv.FormatInt(guid, 10), strconv.FormatInt(defGUID, 10)
		leaf := extractBlueprintLeaf(arch)
		disp := resolveNpcDisplayName(gStr, defStr, arch, leaf, name, instByAsset[guid], names)
		minLv, maxLv := minLvByAsset[guid], maxLvByAsset[guid]
		if minLv <= 0 {
			minLv = 1
		}
		if maxLv < minLv {
			maxLv = minLv
		}
		out = append(out, NpcEntry{
			GUID:          gStr,
			Name:          name,
			DisplayName:   disp,
			Title:         firstNonEmptyStr(names.byTitle[defStr], names.byTitle[gStr]),
			NpcDefGUID:    defStr,
			ArchetypePath: arch,
			BlueprintLeaf: leaf,
			MinLevel:      minLv,
			MaxLevel:      maxLv,
		})
	}
	return out, rows.Err()
}

func loadNpcInstanceLinks(ctx context.Context, db *sql.DB) (map[int64][]string, map[int64]int, map[int64]int) {
	instByAsset := make(map[int64][]string)
	minLv := make(map[int64]int)
	maxLv := make(map[int64]int)
	if !hasTable(ctx, db, "npc_instance") {
		return instByAsset, minLv, maxLv
	}
	if hasTable(ctx, db, "npc_asset_set_member") && hasTable(ctx, db, "npc_instance_asset_set") {
		rows, err := db.QueryContext(ctx, `SELECT asm.asset_guid, ni.guid, ni.level_min, ni.level_max FROM npc_asset_set_member asm JOIN npc_instance_asset_set ias ON ias.set_guid = asm.set_guid JOIN npc_instance ni ON ni.guid = ias.instance_guid`)
		if err == nil {
			defer rows.Close()
			for rows.Next() {
				var aGUID, iGUID int64
				var lMin, lMax int
				if rows.Scan(&aGUID, &iGUID, &lMin, &lMax) == nil {
					instByAsset[aGUID] = append(instByAsset[aGUID], strconv.FormatInt(iGUID, 10))
					updateLevelBounds(minLv, maxLv, aGUID, lMin, lMax)
				}
			}
		}
		return instByAsset, minLv, maxLv
	}
	return loadSyntheticNpcInstanceLevels(ctx, db, instByAsset, minLv, maxLv)
}

func loadSyntheticNpcInstanceLevels(ctx context.Context, db *sql.DB, instByAsset map[int64][]string, minLv, maxLv map[int64]int) (map[int64][]string, map[int64]int, map[int64]int) {
	colMin, colMax := "level_min", "level_max"
	if hasColumn(ctx, db, "npc_instance", "min_level") {
		colMin, colMax = "min_level", "max_level"
	}
	joinCol := "guid"
	if hasColumn(ctx, db, "npc_instance", "npc_def_guid") {
		joinCol = "npc_def_guid"
	} else if hasColumn(ctx, db, "npc_instance", "asset_guid") {
		joinCol = "asset_guid"
	}
	q := fmt.Sprintf(`SELECT a.guid, COALESCE(MIN(i.%s), 1), COALESCE(MAX(i.%s), 1) FROM npc_asset a LEFT JOIN npc_instance i ON i.%s = a.guid OR (a. rowid IS NOT NULL AND i.%s = COALESCE(a.npc_def_guid, a.guid)) GROUP BY a.guid`, colMin, colMax, joinCol, joinCol)
	if !hasColumn(ctx, db, "npc_asset", "npc_def_guid") {
		q = fmt.Sprintf(`SELECT a.guid, COALESCE(MIN(i.%s), 1), COALESCE(MAX(i.%s), 1) FROM npc_asset a LEFT JOIN npc_instance i ON i.%s = a.guid GROUP BY a.guid`, colMin, colMax, joinCol)
	}
	rows, err := db.QueryContext(ctx, q)
	if err == nil {
		defer rows.Close()
		for rows.Next() {
			var aGUID int64
			var lMin, lMax int
			if rows.Scan(&aGUID, &lMin, &lMax) == nil {
				updateLevelBounds(minLv, maxLv, aGUID, lMin, lMax)
			}
		}
	}
	return instByAsset, minLv, maxLv
}

func updateLevelBounds(minLv, maxLv map[int64]int, guid int64, lMin, lMax int) {
	if cur, ok := minLv[guid]; !ok || (lMin > 0 && lMin < cur) {
		minLv[guid] = lMin
	}
	if cur, ok := maxLv[guid]; !ok || lMax > cur {
		maxLv[guid] = lMax
	}
}

func resolveNpcDisplayName(guidStr, defStr, arch, leaf, recName string, instGUIDs []string, names npcNameTable) string {
	for _, k := range []string{defStr, guidStr} {
		if v := strings.TrimSpace(names.byAsset[k]); v != "" {
			return v
		}
		if v := strings.TrimSpace(names.byInstance[k]); v != "" {
			return v
		}
	}
	for _, ig := range instGUIDs {
		if v := strings.TrimSpace(names.byInstance[ig]); v != "" {
			return v
		}
	}
	if v := strings.TrimSpace(names.byArchetype[arch]); v != "" {
		return v
	}
	if leaf != "" {
		return humanizeIdentifier(strings.TrimSuffix(strings.TrimPrefix(leaf, "Default__"), "_C"))
	}
	return humanizeIdentifier(recName)
}

func extractBlueprintLeaf(archetypePath string) string {
	s := strings.TrimSpace(archetypePath)
	if idx := strings.LastIndexByte(s, '.'); idx >= 0 {
		s = s[idx+1:]
	} else if idx := strings.LastIndexByte(s, '/'); idx >= 0 {
		s = s[idx+1:]
	}
	return strings.TrimPrefix(s, "Default__")
}

func humanizeIdentifier(raw string) string {
	s := strings.TrimSpace(raw)
	for _, p := range []string{"Item_", "NPC_", "PopAsset_", "PopulationAsset_", "Verra_"} {
		s = strings.TrimPrefix(s, p)
	}
	s = strings.TrimSuffix(s, "_C")
	s = strings.ReplaceAll(s, "_", " ")
	return strings.TrimSpace(s)
}

func hasTable(ctx context.Context, db *sql.DB, table string) bool {
	if db == nil {
		return false
	}
	var name string
	err := db.QueryRowContext(ctx, `SELECT name FROM sqlite_master WHERE type='table' AND name=?`, table).Scan(&name)
	return err == nil && name == table
}

func hasColumn(ctx context.Context, db *sql.DB, table, col string) bool {
	if db == nil {
		return false
	}
	rows, err := db.QueryContext(ctx, fmt.Sprintf(`PRAGMA table_info(%s)`, table))
	if err != nil {
		return false
	}
	defer rows.Close()
	for rows.Next() {
		var cid, notNull, pk int
		var name, ctype string
		var dflt sql.NullString
		if rows.Scan(&cid, &name, &ctype, &notNull, &dflt, &pk) == nil && strings.EqualFold(name, col) {
			return true
		}
	}
	return false
}

func isDecimalGUID(s string) bool {
	if len(s) == 0 {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
}

func firstNonEmptyStr(vals ...string) string {
	for _, v := range vals {
		if t := strings.TrimSpace(v); t != "" {
			return t
		}
	}
	return ""
}

var rarityOrdByName = map[string]int{
	"common": 0, "uncommon": 1, "rare": 2, "heroic": 3, "epic": 4, "legendary": 5, "artifact": 6,
}

func (c *Catalog) SearchItems(ctx context.Context, q string, limit int) []ItemEntry {
	items, _ := c.SearchItemsFiltered(ctx, ItemFilter{Query: q, Limit: limit})
	return items
}

func (c *Catalog) SearchItemsFiltered(_ context.Context, f ItemFilter) ([]ItemEntry, int) {
	c.mu.RLock()
	source := c.items
	if !c.loadedDB && len(source) == 0 {
		for _, d := range c.docs {
			if d.entry.Kind == KindItem {
				source = append(source, ItemEntry{
					GUID: strings.TrimPrefix(d.entry.GUID, "item:"), Name: d.entry.Name, DisplayName: d.entry.Name,
					RarityMin: 0, RarityMax: 6, MaxStack: 1, Level: 1,
				})
			}
		}
	}
	c.mu.RUnlock()

	qLower := strings.ToLower(strings.TrimSpace(f.Query))
	slotLower := strings.ToLower(strings.TrimSpace(f.Slot))
	rarityFilter, hasRarity := parseRarityFilter(f.Rarity)
	matched := make([]ItemEntry, 0, 64)
	for _, it := range source {
		if slotLower != "" && !strings.Contains(strings.ToLower(it.EquipSlot), slotLower) {
			continue
		}
		if hasRarity && (rarityFilter < it.RarityMin || rarityFilter > it.RarityMax) {
			continue
		}
		if qLower != "" && !matchesItemQuery(it, qLower) {
			continue
		}
		matched = append(matched, it)
	}
	sortItems(matched, f.Sort)
	return paginateSlice(matched, f.Offset, f.Limit, 50, 200)
}

func sortItems(items []ItemEntry, sortSpec string) {
	field, desc := parseSortField(sortSpec)
	sort.SliceStable(items, func(i, j int) bool {
		a, b := items[i], items[j]
		switch field {
		case "level":
			if a.Level != b.Level {
				return compareLess(a.Level < b.Level, desc)
			}
		case "max_stack", "stack":
			if a.MaxStack != b.MaxStack {
				return compareLess(a.MaxStack < b.MaxStack, desc)
			}
		case "guid":
			if a.GUID != b.GUID {
				return compareLess(a.GUID < b.GUID, desc)
			}
		case "name", "display_name":
			if a.DisplayName != b.DisplayName {
				return compareLess(a.DisplayName < b.DisplayName, desc)
			}
		}
		if a.DisplayName != b.DisplayName {
			return a.DisplayName < b.DisplayName
		}
		return a.GUID < b.GUID
	})
}

func matchesItemQuery(it ItemEntry, qLower string) bool {
	return strings.Contains(strings.ToLower(it.DisplayName), qLower) ||
		strings.Contains(strings.ToLower(it.Name), qLower) ||
		strings.Contains(strings.ToLower(it.GUID), qLower)
}

func parseRarityFilter(raw string) (int, bool) {
	s := strings.ToLower(strings.TrimSpace(raw))
	if s == "" {
		return 0, false
	}
	if ord, ok := rarityOrdByName[s]; ok {
		return ord, true
	}
	if n, err := strconv.Atoi(s); err == nil && n >= 0 && n <= 6 {
		return n, true
	}
	return -1, true
}

func (c *Catalog) SearchNpcs(ctx context.Context, q string, limit int) []NpcEntry {
	npcs, _ := c.SearchNpcsFiltered(ctx, NpcFilter{Query: q, Limit: limit})
	return npcs
}

func (c *Catalog) SearchNpcsFiltered(_ context.Context, f NpcFilter) ([]NpcEntry, int) {
	c.mu.RLock()
	source := c.npcs
	if !c.loadedDB && len(source) == 0 {
		for _, d := range c.docs {
			if d.entry.Kind == KindNPC {
				g := strings.TrimPrefix(d.entry.GUID, "npc:")
				source = append(source, NpcEntry{
					GUID: g, Name: d.entry.Name, DisplayName: d.entry.Name, NpcDefGUID: g, MinLevel: 1, MaxLevel: 50,
				})
			}
		}
	}
	c.mu.RUnlock()

	qLower := strings.ToLower(strings.TrimSpace(f.Query))
	matched := make([]NpcEntry, 0, 64)
	for _, n := range source {
		if f.MinLevel > 0 && n.MaxLevel < f.MinLevel {
			continue
		}
		if f.MaxLevel > 0 && n.MinLevel > f.MaxLevel {
			continue
		}
		if qLower != "" && !matchesNpcQuery(n, qLower) {
			continue
		}
		matched = append(matched, n)
	}
	sortNpcs(matched, f.Sort)
	return paginateSlice(matched, f.Offset, f.Limit, 50, 200)
}

func sortNpcs(npcs []NpcEntry, sortSpec string) {
	field, desc := parseSortField(sortSpec)
	sort.SliceStable(npcs, func(i, j int) bool {
		a, b := npcs[i], npcs[j]
		switch field {
		case "level", "min_level":
			if a.MinLevel != b.MinLevel {
				return compareLess(a.MinLevel < b.MinLevel, desc)
			}
		case "max_level":
			if a.MaxLevel != b.MaxLevel {
				return compareLess(a.MaxLevel < b.MaxLevel, desc)
			}
		case "guid":
			if a.GUID != b.GUID {
				return compareLess(a.GUID < b.GUID, desc)
			}
		case "name", "display_name":
			if a.DisplayName != b.DisplayName {
				return compareLess(a.DisplayName < b.DisplayName, desc)
			}
		}
		if a.DisplayName != b.DisplayName {
			return a.DisplayName < b.DisplayName
		}
		return a.GUID < b.GUID
	})
}

func matchesNpcQuery(n NpcEntry, qLower string) bool {
	return strings.Contains(strings.ToLower(n.DisplayName), qLower) ||
		strings.Contains(strings.ToLower(n.Name), qLower) ||
		strings.Contains(strings.ToLower(n.BlueprintLeaf), qLower) ||
		strings.Contains(strings.ToLower(n.ArchetypePath), qLower) ||
		strings.Contains(strings.ToLower(n.GUID), qLower) ||
		strings.Contains(strings.ToLower(n.NpcDefGUID), qLower)
}

func paginateSlice[T any](items []T, offset, limit, defLimit, maxLimit int) ([]T, int) {
	total := len(items)
	if limit <= 0 {
		limit = defLimit
	}
	if limit > maxLimit {
		limit = maxLimit
	}
	if offset < 0 {
		offset = 0
	}
	if offset >= total {
		return []T{}, total
	}
	end := offset + limit
	if end > total {
		end = total
	}
	out := make([]T, end-offset)
	copy(out, items[offset:end])
	return out, total
}
