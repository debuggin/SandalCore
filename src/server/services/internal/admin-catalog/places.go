// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admincatalog

import (
	"bytes"
	"compress/gzip"
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"sort"
	"strconv"
	"strings"
	"sync"
)

// WHY: PlaceEntry struct is declared in store.go (outside Wave 38B write_set); PlaceVisualMeta and
// PlaceEntry.MarshalJSON/UnmarshalJSON extend PlaceEntry in-package with icon/category/biome fields.
type PlaceVisualMeta struct {
	Category, IconName, IconUrl, TintColor, NodeType, Biome string
	IconSize                                                int
}
type mapIconVisual struct {
	textureName, tintHex string
	size                 int
}

var placeVisualRegistry sync.Map

func registerPlaceVisual(guid, name string, meta PlaceVisualMeta) {
	meta = fillPlaceVisualDefaults(meta, meta.Category, meta.NodeType)
	if guid != "" {
		placeVisualRegistry.Store(guid, meta)
	}
	if name != "" {
		placeVisualRegistry.Store("name:"+strings.ToLower(name), meta)
	}
}

func fillPlaceVisualDefaults(m PlaceVisualMeta, fallbackCat, fallbackNodeType string) PlaceVisualMeta {
	m.Category = firstNonEmptyStr(m.Category, fallbackCat, "poi")
	m.NodeType = firstNonEmptyStr(m.NodeType, fallbackNodeType)
	m.IconName = firstNonEmptyStr(m.IconName, defaultIconForKind(m.Category, m.NodeType))
	m.IconUrl = firstNonEmptyStr(m.IconUrl, "/api/v1/world/icons/"+m.IconName+".png")
	m.TintColor = firstNonEmptyStr(m.TintColor, defaultTintForKind(m.Category, m.NodeType))
	if m.IconSize <= 0 {
		m.IconSize = 28
	}
	return m
}

func (p PlaceEntry) VisualMeta() PlaceVisualMeta {
	for _, k := range []string{p.GUID, "name:" + strings.ToLower(p.Name)} {
		if v, ok := placeVisualRegistry.Load(k); ok && k != "" && k != "name:" {
			return fillPlaceVisualDefaults(v.(PlaceVisualMeta), p.Kind, p.NodeType)
		}
	}
	return fillPlaceVisualDefaults(PlaceVisualMeta{}, p.Kind, p.NodeType)
}

func (p PlaceEntry) IconName() string  { return p.VisualMeta().IconName }
func (p PlaceEntry) IconUrl() string   { return p.VisualMeta().IconUrl }
func (p PlaceEntry) TintColor() string { return p.VisualMeta().TintColor }
func (p PlaceEntry) IconSize() int     { return p.VisualMeta().IconSize }
func (p PlaceEntry) Biome() string     { return p.VisualMeta().Biome }
func (p PlaceEntry) Category() string  { return p.VisualMeta().Category }

func (p PlaceEntry) MarshalJSON() ([]byte, error) {
	m := p.VisualMeta()
	kind := firstNonEmptyStr(p.Kind, m.Category, "poi")
	return json.Marshal(map[string]any{
		"guid": p.GUID, "name": p.Name, "display_name": p.DisplayName, "node_name": p.NodeName,
		"node_type": firstNonEmptyStr(p.NodeType, m.NodeType), "biome": m.Biome,
		"category": firstNonEmptyStr(m.Category, kind), "kind": kind,
		"icon_name": m.IconName, "icon_url": m.IconUrl, "tint_color": m.TintColor, "icon_size": m.IconSize,
		"x": p.X, "y": p.Y, "z": p.Z, "yaw": p.Yaw, "radius": p.Radius,
		"levelable": p.Levelable, "level": p.Level, "stage_name": p.StageName,
	})
}

func (p *PlaceEntry) UnmarshalJSON(data []byte) error {
	var doc map[string]any
	if err := json.Unmarshal(data, &doc); err != nil {
		return err
	}
	*p = PlaceEntry{
		GUID: extractNestedString(doc, "guid"), Name: extractNestedString(doc, "name"),
		DisplayName: extractNestedString(doc, "display_name"), NodeName: extractNestedString(doc, "node_name"),
		NodeType: extractNestedString(doc, "node_type"), Kind: firstNonEmptyStr(extractNestedString(doc, "kind"), extractNestedString(doc, "category")),
		X: extractFloat(doc, "x"), Y: extractFloat(doc, "y"), Z: extractFloat(doc, "z"),
		Yaw: extractFloat(doc, "yaw"), Radius: extractFloat(doc, "radius"),
		Levelable: doc["levelable"] == true, Level: int(extractFloat(doc, "level")), StageName: extractNestedString(doc, "stage_name"),
	}
	registerPlaceVisual(p.GUID, p.Name, PlaceVisualMeta{
		Category: firstNonEmptyStr(extractNestedString(doc, "category"), p.Kind), IconName: extractNestedString(doc, "icon_name"),
		IconUrl: extractNestedString(doc, "icon_url"), TintColor: extractNestedString(doc, "tint_color"),
		IconSize: int(extractFloat(doc, "icon_size")), NodeType: p.NodeType, Biome: extractNestedString(doc, "biome"),
	})
	return nil
}

type LevelableNodeSpec struct {
	NodeIndex   int     `json:"node_index"`
	GUID        int64   `json:"guid"`
	Name        string  `json:"name"`
	DisplayName string  `json:"display_name"`
	TextureStem string  `json:"texture_stem"`
	Ox          float64 `json:"ox"`
	Oy          float64 `json:"oy"`
	Oz          float64 `json:"oz"`
	YawDeg      float64 `json:"yaw_deg"`
	SizeCm      int     `json:"size_cm"`
}

var NodeStageNames = []string{"Wilderness", "Expedition", "Encampment", "Village", "Town", "City", "Metropolis"}

func StageNameForLevel(level int) string {
	if level >= 0 && level < len(NodeStageNames) {
		return NodeStageNames[level]
	}
	return "Wilderness"
}

func PlugTierForLevel(level int) int {
	if level <= 0 {
		return -1
	}
	return min(level-1, 4)
}

func DefaultLevelableNodes() []LevelableNodeSpec {
	return []LevelableNodeSpec{
		{0, 108646108518008, "Verra_RVR_Winstead", "Winstead", "Winstead", -644519.11, 376773.24, 12012.23, 319.20846, 100000}, {1, 108646121231993, "Verra_RVR_Joeva", "Joeva", "Joeva", -1094396.04, 521336.34, 11836.45, 292.625506, 100000},
		{2, 108646608033403, "Verra_RVR_NewAela", "New Aela", "New_Aela", -1134183.5439466983, 853387.2772475089, 11761.82381661977, -90.21495968964565, 100000}, {3, 108646610851452, "Verra_RVR_Halcyon", "Halcyon", "Halcyon", -891024.50, 296919.66, 11657.05, 180.0, 100000},
		{4, 108646615701117, "Verra_RVR_Miraleth", "Miraleth", "Miraleth", -459889.634548, 599317.092515, 11000.972636, -20.0, 100000}, {5, 6064632225073725498, "Verra_TUR_Windansea", "Windansea", "Windansea", -1100624.184211, 246466.992911, 6674.000004, 55.41214, 100000},
		{6, 6064632734251679746, "Verra_SAN_Aithanahr", "Aithanahr", "Aithanahr", -232617.63, 762558.56, 6679.26, 304.121675, 100000}, {7, 6064632734253907971, "Verra_SAN_Azmaran", "Azmaran", "Azmaran", -278767.94, 987391.03, 7227.0, 351.156897, 100000},
		{8, 6064632734268915716, "Verra_SAN_Sunhaven", "Sunhaven", "Sunhaven", -619803.283992, 873514.222232, 12073.59434, -127.073107, 100000}, {9, 6064633097318170624, "Verra_SAN_Djinna", "Djinna", "Djinna", 63370.908474, 861198.365739, 5066.0, -187.137862, 100000},
		{10, 6064633097371058177, "Verra_SAN_SquallsEnd", "Squall's End", "Squall_s_End", -37370.0, 1133335.0, 16385.348318, -217.228668, 100000}, {11, 6064633334652600320, "Verra_TUR_Korrin", "Korrin", "Korrin", -1764281.284871, 114557.650316, 5066.0, 90.707388, 100000},
		{12, 6064633334741794817, "Verra_TUR_Brinebarrel", "Brinebarrel", "Brinebarrel", -1860980.218817, -171832.947158, 7472.764935, -46.566062, 100000}, {13, 6064633407845105664, "Verra_TUR_Seahook", "Seahook", "Seahook", -1764263.887544, 539283.279654, 5066.0, 14.228906, 100000},
		{14, 6064633407846088705, "Verra_TUR_Shorefoot", "Shorefoot", "Shorefoot", -1372642.268119, 57374.666533, 5066.0, -66.900158, 100000}, {15, 6064633425419042816, "Verra_JUN_Wildport", "Wildport", "Wildport", -1783982.0, 809682.0, 9777.0, 16.0, 100000},
		{16, 6064633425517019141, "Verra_JUN_Hecribba", "Hecribba", "Hecribba", -1708500.0, 1053019.0, 9620.0, 246.0, 100000}, {17, 6064633425555816457, "Verra_JUN_Vinebreach", "Vinebreach", "Vinebreach", -1476480.0, 1178456.0, 7013.0, 200.0, 100000},
		{18, 6064633429404090381, "Verra_JUN_Arisalon", "Arisalon", "Arisalon", -1250506.0, 1062377.0, 5379.0, -35.0, 100000}, {19, 6064633429423882257, "Verra_JUN_TangledPost", "Tangled Post", "Tangled_Post", -1031419.0, 1098848.0, 7264.0, -3.5, 100000},
		{20, 6064633429437251605, "Verra_JUN_Mythbreak", "Mythbreak", "Mythbreak", -742705.0, 1087738.0, 5066.0, -159.058146, 100000}, {21, 6064633651969589265, "Verra_ANV_Vexhelm", "Vexhelm", "Vexhelm", -1463243.0, -341306.0, 25074.0, 60.0, 100000},
		{22, 6064633651985711129, "Verra_ANV_Duunhold", "Duunhold", "Duunhold", -1069784.0, -150970.0, 24891.0, 0.0, 100000}, {23, 6064633652009173023, "Verra_ANV_Dhurgrum", "Dhurgrum", "Dhurgrum", -937374.0, -411205.0, 24983.0, -15.0, 100000},
		{24, 6064633652023394340, "Verra_ANV_Vhalgadim", "Vhalgadim", "Vhalgadim", -762862.0, -563881.0, 17256.0, 349.999997, 100000}, {25, 6064633652044300331, "Verra_ANV_KalTorhum", "Kal Torhum", "Kal_Torhum", -1271772.0, -462821.0, 5066.0, 5.0, 100000},
	}
}

func BuiltInPlaces() []PlaceEntry {
	out := []PlaceEntry{
		{GUID: "place:login", Name: "login", DisplayName: "Login Spawn", NodeName: "Login Spawn", NodeType: "Spawn", Kind: "spawn", X: -847992.0, Y: -538867.0, Z: 11524.0},
		{GUID: "place:garden", Name: "garden", DisplayName: "GM Garden", NodeName: "GM Garden", NodeType: "Spawn", Kind: "spawn", X: -777664.5, Y: 616578.6875, Z: 11524.0},
	}
	for _, p := range out {
		registerPlaceVisual(p.GUID, p.Name, PlaceVisualMeta{Category: "spawn", IconName: "TUI_Icon_Map_StartingArea", TintColor: "#22c55e", IconSize: 28, NodeType: "Spawn"})
	}
	return out
}

func decodeRecordBlob(raw []byte) ([]byte, error) {
	if len(raw) >= 2 && raw[0] == 0x1f && raw[1] == 0x8b {
		zr, err := gzip.NewReader(bytes.NewReader(raw))
		if err != nil {
			return nil, err
		}
		defer zr.Close()
		return io.ReadAll(io.LimitReader(zr, 16<<20))
	}
	return raw, nil
}

type placeLoadCtx struct {
	ctx     context.Context
	db      *sql.DB
	out     []PlaceEntry
	seen    map[string]int
	visuals map[string]mapIconVisual
	biomes  map[string]string
	origins map[int64]LevelableNodeSpec
}

func loadPlaces(db *sql.DB) ([]PlaceEntry, error) { return loadPlacesFromDB(context.Background(), db) }

func loadPlacesFromDB(ctx context.Context, db *sql.DB) ([]PlaceEntry, error) {
	origins := make(map[int64]LevelableNodeSpec, 26)
	for _, n := range DefaultLevelableNodes() {
		origins[n.GUID] = n
	}
	st := &placeLoadCtx{ctx: ctx, db: db, out: BuiltInPlaces(), seen: map[string]int{"place:login": 0, "place:garden": 1}, origins: origins}
	if db == nil {
		st.appendDefaultLevelableNodes()
		return st.out, nil
	}
	st.visuals = loadMapIconVisuals(ctx, db)
	st.biomes = loadNodeBiomes(ctx, db)
	if !st.appendCityNodePlaces() && !hasTable(ctx, db, "city_node") {
		st.appendDefaultLevelableNodes()
	}
	st.appendPOIPlaces()
	st.appendWayshrinePlaces()
	st.appendLegacyZoneAndSpawnPlaces()
	return st.out, nil
}

func (st *placeLoadCtx) addOrUpdate(entry PlaceEntry, meta PlaceVisualMeta, overwrite bool) {
	registerPlaceVisual(entry.GUID, entry.Name, meta)
	if idx, exists := st.seen[entry.GUID]; exists {
		if overwrite {
			st.out[idx] = entry
		}
		return
	}
	st.seen[entry.GUID] = len(st.out)
	st.out = append(st.out, entry)
}

func (st *placeLoadCtx) appendDefaultLevelableNodes() {
	icon := resolveCityNodeIconName("", "", "Economic", "city_node", nil)
	for _, n := range DefaultLevelableNodes() {
		st.addOrUpdate(PlaceEntry{
			GUID: strconv.FormatInt(n.GUID, 10), Name: n.Name, DisplayName: n.DisplayName,
			NodeName: n.DisplayName, NodeType: "Economic", Kind: "city_node",
			X: n.Ox, Y: n.Oy, Z: n.Oz, Yaw: n.YawDeg, Radius: 50000, Levelable: true, Level: 0, StageName: StageNameForLevel(0),
		}, PlaceVisualMeta{Category: "city_node", IconName: icon, TintColor: "#f59e0b", IconSize: 32, NodeType: "Economic"}, false)
	}
}

func loadMapIconVisuals(ctx context.Context, db *sql.DB) map[string]mapIconVisual {
	out := make(map[string]mapIconVisual)
	forEachRecordRow(ctx, db, "MapIconVisualRecordBase", func(guid, name string, doc map[string]any) {
		tex := extractIconAssetName(doc, "MapIcon", "IconTexture", "Texture", "Icon")
		if tex == "" && strings.HasPrefix(name, "TUI_") {
			tex = extractShortAssetName(name)
		}
		out[guid] = mapIconVisual{textureName: tex, tintHex: extractColorHex(doc, "Color", "TintColor", "IconColor"), size: extractIconSizePx(doc)}
	})
	return out
}

func forEachRecordRow(ctx context.Context, db *sql.DB, recordType string, fn func(guid, name string, doc map[string]any)) {
	if !hasTable(ctx, db, "record") {
		return
	}
	rows, err := db.QueryContext(ctx, `SELECT CAST(guid AS TEXT), COALESCE(name, ''), CAST(data AS BLOB) FROM record WHERE schema = ? ORDER BY guid ASC`, recordType)
	if err != nil {
		rows, err = db.QueryContext(ctx, `SELECT CAST(guid AS TEXT), COALESCE(name, ''), CAST(data AS BLOB) FROM record WHERE type = ? ORDER BY guid ASC`, recordType)
	}
	if err != nil || rows == nil {
		return
	}
	defer rows.Close()
	for rows.Next() {
		var guid, name string
		var raw []byte
		if rows.Scan(&guid, &name, &raw) == nil {
			if decoded, err := decodeRecordBlob(raw); err == nil {
				var doc map[string]any
				if json.Unmarshal(decoded, &doc) == nil {
					fn(guid, name, doc)
				}
			}
		}
	}
}

func loadNodeBiomes(ctx context.Context, db *sql.DB) map[string]string {
	out := make(map[string]string)
	if hasTable(ctx, db, "city_zoi") {
		if rows, err := db.QueryContext(ctx, `SELECT CAST(node_guid AS TEXT), COALESCE(biome, '') FROM city_zoi WHERE node_guid != 0`); err == nil {
			defer rows.Close()
			for rows.Next() {
				var guid, biome string
				if rows.Scan(&guid, &biome) == nil && biome != "" {
					out[guid] = trimEnumPrefix(biome)
				}
			}
		}
	}
	return out
}

type parsedPlaceWithMeta struct {
	entry PlaceEntry
	meta  PlaceVisualMeta
}

func (st *placeLoadCtx) appendCityNodePlaces() bool {
	var recNodes []parsedPlaceWithMeta
	forEachRecordRow(st.ctx, st.db, "CityNodeRecordBase", func(guid, name string, doc map[string]any) {
		recNodes = append(recNodes, st.buildCityNodeFromRecord(guid, name, doc))
	})
	tblNodes := st.parseTableCityNodes()
	totalRows := max(len(recNodes), len(tblNodes))
	merged := mergeCityNodeSources(recNodes, tblNodes)
	for _, item := range merged {
		if item.entry.X != 0 || item.entry.Y != 0 || totalRows <= 10 {
			st.addOrUpdate(item.entry, item.meta, true)
		}
	}
	return len(merged) > 0
}

func (st *placeLoadCtx) buildCityNodeFromRecord(guid, name string, doc map[string]any) parsedPlaceWithMeta {
	nodeName := extractLocalizedText(doc, "nodeName", "NodeName", "DisplayName")
	desc := extractLocalizedText(doc, "description", "Description", "DisplayName", "nodeName", "NodeName")
	disp := firstNonEmptyStr(desc, nodeName, humanizeIdentifier(name), name)
	nodeType := trimEnumPrefix(extractNestedString(doc, "nodeType", "NodeType"))
	biome := trimEnumPrefix(firstNonEmptyStr(extractNestedString(doc, "biomeType", "BiomeType", "Biome"), st.biomes[guid]))
	x, y, z := extractVec3(doc, "location", "Location", "MapLocation", "OriginLocation")
	yaw, radius := extractFloat(doc, "yaw", "Yaw"), extractFloat(doc, "radius", "Radius")
	guidInt, _ := strconv.ParseInt(guid, 10, 64)
	orig, isLevelableOrigin := st.origins[guidInt]
	if isLevelableOrigin && x == 0 && y == 0 {
		x, y, z, yaw = orig.Ox, orig.Oy, orig.Oz, orig.YawDeg
	}
	cat := classifyCityNodeCategory(nodeType, name, disp)
	icon, tint, sz := resolveCityNodeVisual(extractRefGUID(doc, "mapIconVisualId", "MapIconVisualId"), extractIconAssetName(doc, "nodeIcon", "NodeIcon"), nodeType, cat, st.visuals)
	return parsedPlaceWithMeta{
		entry: PlaceEntry{GUID: guid, Name: firstNonEmptyStr(name, disp), DisplayName: disp, NodeName: firstNonEmptyStr(nodeName, disp), NodeType: firstNonEmptyStr(nodeType, "Default"), Kind: cat, X: x, Y: y, Z: z, Yaw: yaw, Radius: radius, Levelable: isLevelableOrigin, Level: 0, StageName: StageNameForLevel(0)},
		meta:  PlaceVisualMeta{Category: cat, IconName: icon, IconUrl: "/api/v1/world/icons/" + icon + ".png", TintColor: tint, IconSize: sz, NodeType: nodeType, Biome: biome},
	}
}

func (st *placeLoadCtx) parseTableCityNodes() []parsedPlaceWithMeta {
	if !hasTable(st.ctx, st.db, "city_node") {
		return nil
	}
	rows, err := st.db.QueryContext(st.ctx, `SELECT guid, name, COALESCE(node_name, ''), COALESCE(node_type, ''), COALESCE(x, 0), COALESCE(y, 0), COALESCE(z, 0), COALESCE(yaw, 0), COALESCE(radius, 0), COALESCE(levelable, 0) FROM city_node ORDER BY guid ASC`)
	if err != nil {
		return nil
	}
	defer rows.Close()
	var list []parsedPlaceWithMeta
	for rows.Next() {
		var guid int64
		var name, nodeName, nodeType string
		var x, y, z, yaw, radius float64
		var levelable int
		if rows.Scan(&guid, &name, &nodeName, &nodeType, &x, &y, &z, &yaw, &radius, &levelable) != nil {
			continue
		}
		if orig, ok := st.origins[guid]; ok && x == 0 && y == 0 {
			x, y, z, yaw = orig.Ox, orig.Oy, orig.Oz, orig.YawDeg
		}
		guidStr, disp, cleanType := strconv.FormatInt(guid, 10), firstNonEmptyStr(nodeName, name), trimEnumPrefix(firstNonEmptyStr(nodeType, "Default"))
		cat := classifyCityNodeCategory(cleanType, name, disp)
		icon, tint, sz := resolveCityNodeVisual("", "", cleanType, cat, st.visuals)
		list = append(list, parsedPlaceWithMeta{
			entry: PlaceEntry{GUID: guidStr, Name: name, DisplayName: disp, NodeName: disp, NodeType: cleanType, Kind: cat, X: x, Y: y, Z: z, Yaw: yaw, Radius: radius, Levelable: levelable != 0, Level: 0, StageName: StageNameForLevel(0)},
			meta:  PlaceVisualMeta{Category: cat, IconName: icon, IconUrl: "/api/v1/world/icons/" + icon + ".png", TintColor: tint, IconSize: sz, NodeType: cleanType, Biome: st.biomes[guidStr]},
		})
	}
	return list
}

func mergeCityNodeSources(recNodes, tblNodes []parsedPlaceWithMeta) []parsedPlaceWithMeta {
	if len(recNodes) == 0 {
		return tblNodes
	}
	out := append([]parsedPlaceWithMeta(nil), recNodes...)
	byGUID := make(map[string]int, len(out))
	for i, r := range out {
		byGUID[r.entry.GUID] = i
	}
	for _, t := range tblNodes {
		if idx, ok := byGUID[t.entry.GUID]; ok {
			if out[idx].entry.X == 0 && out[idx].entry.Y == 0 {
				out[idx].entry.X, out[idx].entry.Y, out[idx].entry.Z, out[idx].entry.Yaw = t.entry.X, t.entry.Y, t.entry.Z, t.entry.Yaw
			}
			if out[idx].entry.Radius == 0 {
				out[idx].entry.Radius = t.entry.Radius
			}
			out[idx].entry.Levelable = out[idx].entry.Levelable || t.entry.Levelable
			out[idx].meta.Biome = firstNonEmptyStr(out[idx].meta.Biome, t.meta.Biome)
		} else {
			byGUID[t.entry.GUID] = len(out)
			out = append(out, t)
		}
	}
	return out
}

func classifyCityNodeCategory(nodeType, name, disp string) string {
	c := strings.ToLower(nodeType + " " + name + " " + disp)
	if strings.Contains(c, "harbor") {
		return "harbor"
	} else if strings.Contains(c, "castle") {
		return "castle"
	} else if strings.Contains(c, "startingarea") || strings.Contains(c, "starting_area") || strings.Contains(c, "portal") {
		return "starting_area"
	} else if strings.Contains(c, "outpost") {
		return "outpost"
	}
	return "city_node"
}

func resolveCityNodeVisual(visGUID, nodeIcon, nodeType, cat string, visuals map[string]mapIconVisual) (string, string, int) {
	icon, tint, sz := resolveCityNodeIconName(visGUID, nodeIcon, nodeType, cat, visuals), defaultTintForKind(cat, nodeType), 32
	if v, ok := visuals[visGUID]; ok {
		tint = firstNonEmptyStr(v.tintHex, tint)
		if v.size > 0 {
			sz = v.size
		}
	}
	return icon, tint, sz
}

func resolveCityNodeIconName(visGUID, nodeIcon, nodeType, cat string, visuals map[string]mapIconVisual) string {
	if v, ok := visuals[visGUID]; ok && visGUID != "" && visGUID != "0" && v.textureName != "" {
		return v.textureName
	} else if nodeIcon != "" {
		return nodeIcon
	}
	nt := strings.ToLower(nodeType)
	for _, m := range []struct{ key, icon string }{
		{"economic", "TUI_Icon_NodeSummary_NodeEmblem_Economic"}, {"divine", "TUI_Icon_NodeSummary_NodeEmblem_Divine"},
		{"military", "TUI_Icon_NodeSummary_NodeEmblem_Military"}, {"martial", "TUI_Icon_NodeSummary_NodeEmblem_Military"},
		{"scientific", "TUI_Icon_NodeSummary_NodeEmblem_Scientific"}, {"harbor", "TUI_Icon_Map_Harbor"},
		{"castle", "TUI_Icon_Map_Castle"}, {"startingarea", "TUI_Icon_Map_StartingArea"},
	} {
		if strings.Contains(nt, m.key) || (m.key == cat) || (m.key == "startingarea" && cat == "starting_area") {
			return m.icon
		}
	}
	return "TUI_Icon_Map_Node_LVL3"
}

func (st *placeLoadCtx) appendPOIPlaces() {
	var parsed []parsedPlaceWithMeta
	forEachRecordRow(st.ctx, st.db, "POIRecordBase", func(guid, name string, doc map[string]any) {
		disp := firstNonEmptyStr(extractLocalizedText(doc, "PlayerFacingName", "DisplayName", "Name"), humanizeIdentifier(name), name)
		x, y, z := extractVec3(doc, "MapLocation", "OriginLocation", "Location", "location")
		icon, tint, sz := "TUI_Icon_Map_MinorPOI", "#a855f7", 24
		if v, ok := st.visuals[extractRefGUID(doc, "MapIconVisualId", "mapIconVisualId")]; ok {
			icon, tint = firstNonEmptyStr(v.textureName, icon), firstNonEmptyStr(v.tintHex, tint)
			if v.size > 0 {
				sz = v.size
			}
		}
		parsed = append(parsed, parsedPlaceWithMeta{
			entry: PlaceEntry{GUID: guid, Name: firstNonEmptyStr(name, disp), DisplayName: disp, NodeName: disp, Kind: "poi", X: x, Y: y, Z: z, Radius: extractFloat(doc, "Radius", "radius")},
			meta:  PlaceVisualMeta{Category: "poi", IconName: icon, IconUrl: "/api/v1/world/icons/" + icon + ".png", TintColor: tint, IconSize: sz},
		})
	})
	for _, p := range parsed {
		if p.entry.X != 0 || p.entry.Y != 0 || p.entry.Z != 0 || len(parsed) <= 10 {
			st.addOrUpdate(p.entry, p.meta, false)
		}
	}
}

func (st *placeLoadCtx) appendWayshrinePlaces() {
	if hasTable(st.ctx, st.db, "attune_point") {
		if rows, err := st.db.QueryContext(st.ctx, `SELECT CAST(guid AS TEXT), COALESCE(name, ''), COALESCE(descriptor, ''), COALESCE(x, 0), COALESCE(y, 0), COALESCE(z, 0), COALESCE(radius, 0) FROM attune_point ORDER BY guid ASC`); err == nil {
			defer rows.Close()
			for rows.Next() {
				var guid, name, desc string
				var x, y, z, radius float64
				if rows.Scan(&guid, &name, &desc, &x, &y, &z, &radius) == nil {
					disp := firstNonEmptyStr(desc, humanizeIdentifier(name), name)
					st.addOrUpdate(PlaceEntry{GUID: guid, Name: firstNonEmptyStr(name, disp), DisplayName: disp, NodeName: disp, Kind: "wayshrine", X: x, Y: y, Z: z, Radius: radius},
						PlaceVisualMeta{Category: "wayshrine", IconName: "TUI_Icon_Map_ReviveShrine_Active", IconUrl: "/api/v1/world/icons/TUI_Icon_Map_ReviveShrine_Active.png", TintColor: "#38bdf8", IconSize: 24}, false)
				}
			}
		}
	}
	forEachRecordRow(st.ctx, st.db, "AttuneRecordBase", func(guid, name string, doc map[string]any) {
		disp := firstNonEmptyStr(extractLocalizedText(doc, "PlayerFacingDescriptor", "DisplayName", "Name"), humanizeIdentifier(name), name)
		x, y, z := extractVec3(doc, "WorldLocation", "Location", "MapLocation", "location")
		icon := "TUI_Icon_Map_ReviveShrine_Active"
		if v, ok := st.visuals[extractRefGUID(doc, "MapIconVisualId", "mapIconVisualId")]; ok && v.textureName != "" {
			icon = v.textureName
		}
		st.addOrUpdate(PlaceEntry{GUID: guid, Name: firstNonEmptyStr(name, disp), DisplayName: disp, NodeName: disp, Kind: "wayshrine", X: x, Y: y, Z: z, Radius: extractFloat(doc, "AutoAttuneRadius", "Radius")},
			PlaceVisualMeta{Category: "wayshrine", IconName: icon, IconUrl: "/api/v1/world/icons/" + icon + ".png", TintColor: "#38bdf8", IconSize: 24}, false)
	})
}

func (st *placeLoadCtx) appendLegacyZoneAndSpawnPlaces() {
	for _, tbl := range []struct{ table, kind, icon, tint string }{
		{"zone_volume", "zone", "TUI_Icon_Map_MinorPOI", "#10b981"},
		{"spawn_location", "spawn", "TUI_Icon_Map_StartingArea", "#22c55e"},
	} {
		if !hasTable(st.ctx, st.db, tbl.table) {
			continue
		}
		if rows, err := st.db.QueryContext(st.ctx, fmt.Sprintf(`SELECT CAST(guid AS TEXT), COALESCE(name, ''), COALESCE(x, 0), COALESCE(y, 0), COALESCE(z, 0) FROM %s ORDER BY guid ASC`, tbl.table)); err == nil {
			for rows.Next() {
				var guid, name string
				var x, y, z float64
				if rows.Scan(&guid, &name, &x, &y, &z) == nil {
					disp := firstNonEmptyStr(humanizeIdentifier(name), name)
					st.addOrUpdate(PlaceEntry{GUID: guid, Name: name, DisplayName: disp, NodeName: disp, Kind: tbl.kind, X: x, Y: y, Z: z},
						PlaceVisualMeta{Category: tbl.kind, IconName: tbl.icon, IconUrl: "/api/v1/world/icons/" + tbl.icon + ".png", TintColor: tbl.tint, IconSize: 24}, false)
				}
			}
			_ = rows.Close()
		}
	}
}

func extractIconAssetName(doc map[string]any, keys ...string) string {
	for _, k := range keys {
		if v, ok := findKeyCI(doc, k); ok && v != nil {
			if str, isStr := v.(string); isStr {
				if s := extractShortAssetName(str); s != "" {
					return s
				}
			} else if m, isMap := v.(map[string]any); isMap {
				if s := extractShortAssetName(extractNestedString(m, "AssetPathName", "AssetPath", "ObjectName", "Name")); s != "" {
					return s
				}
			}
		}
	}
	return ""
}

func extractShortAssetName(path string) string {
	s := strings.TrimSpace(path)
	if s == "" || s == "None" || s == "null" {
		return ""
	}
	if idx := strings.LastIndexAny(s, `/\`); idx >= 0 {
		s = s[idx+1:]
	}
	if dot := strings.IndexByte(s, '.'); dot > 0 {
		s = s[:dot]
	}
	return strings.TrimSuffix(strings.TrimSpace(s), ".png")
}

func extractColorHex(doc map[string]any, keys ...string) string {
	for _, k := range keys {
		if v, ok := findKeyCI(doc, k); ok && v != nil {
			if str, isStr := v.(string); isStr && strings.HasPrefix(strings.TrimSpace(str), "#") {
				return strings.TrimSpace(str)
			} else if m, isMap := v.(map[string]any); isMap {
				if r, g, b := extractFloat(m, "R", "r"), extractFloat(m, "G", "g"), extractFloat(m, "B", "b"); r != 0 || g != 0 || b != 0 {
					if r <= 1.0 && g <= 1.0 && b <= 1.0 {
						r, g, b = math.Round(r*255), math.Round(g*255), math.Round(b*255)
					}
					return fmt.Sprintf("#%02x%02x%02x", max(0, min(255, int(r))), max(0, min(255, int(g))), max(0, min(255, int(b))))
				}
			}
		}
	}
	return ""
}

func extractIconSizePx(doc map[string]any) int {
	for _, k := range []string{"IconSize", "Size", "icon_size"} {
		if v, ok := findKeyCI(doc, k); ok && v != nil {
			if f, isF := v.(float64); isF && f > 0 {
				return int(math.Round(f))
			} else if m, isM := v.(map[string]any); isM {
				if x := extractFloat(m, "X", "x", "Width", "width"); x > 0 {
					return int(math.Round(x))
				}
			}
		}
	}
	return 28
}

func extractLocalizedText(doc map[string]any, keys ...string) string {
	for _, k := range keys {
		if v, ok := findKeyCI(doc, k); ok && v != nil {
			if str, isStr := v.(string); isStr && strings.TrimSpace(str) != "" && str != "None" {
				return strings.TrimSpace(str)
			} else if m, isMap := v.(map[string]any); isMap {
				if s := extractNestedString(m, "Source", "LocalizedString", "SourceString", "Text", "Name"); s != "" {
					return s
				}
			}
		}
	}
	return ""
}

func extractNestedString(doc map[string]any, keys ...string) string {
	for _, k := range keys {
		if v, ok := findKeyCI(doc, k); ok {
			if s, isStr := v.(string); isStr && strings.TrimSpace(s) != "" {
				return strings.TrimSpace(s)
			}
		}
	}
	return ""
}

func extractRefGUID(doc map[string]any, keys ...string) string {
	for _, k := range keys {
		if v, ok := findKeyCI(doc, k); ok && v != nil {
			if str, isStr := v.(string); isStr && strings.TrimSpace(str) != "" && strings.TrimSpace(str) != "0" {
				return strings.TrimSpace(str)
			} else if f, isF := v.(float64); isF && int64(f) != 0 {
				return strconv.FormatInt(int64(f), 10)
			} else if m, isM := v.(map[string]any); isM {
				if g := extractRefGUID(m, "Guid", "guid"); g != "" {
					return g
				}
			}
		}
	}
	return ""
}

func extractVec3(doc map[string]any, keys ...string) (float64, float64, float64) {
	for _, k := range keys {
		if v, ok := findKeyCI(doc, k); ok {
			if m, isMap := v.(map[string]any); isMap {
				if x, y, z := extractFloat(m, "X", "x"), extractFloat(m, "Y", "y"), extractFloat(m, "Z", "z"); x != 0 || y != 0 || z != 0 {
					return x, y, z
				}
			}
		}
	}
	return extractFloat(doc, "X", "x"), extractFloat(doc, "Y", "y"), extractFloat(doc, "Z", "z")
}

func extractFloat(doc map[string]any, keys ...string) float64 {
	for _, k := range keys {
		if v, ok := findKeyCI(doc, k); ok && v != nil {
			switch val := v.(type) {
			case float64:
				return val
			case int:
				return float64(val)
			case int64:
				return float64(val)
			case string:
				if f, err := strconv.ParseFloat(strings.TrimSpace(val), 64); err == nil {
					return f
				}
			}
		}
	}
	return 0
}

func findKeyCI(m map[string]any, target string) (any, bool) {
	if v, ok := m[target]; ok {
		return v, true
	}
	for k, v := range m {
		if strings.EqualFold(k, target) {
			return v, true
		}
	}
	return nil, false
}

func trimEnumPrefix(raw string) string {
	if s := strings.TrimSpace(raw); strings.Contains(s, "::") {
		return strings.TrimSpace(s[strings.LastIndex(s, "::")+2:])
	}
	return strings.TrimSpace(raw)
}

var (
	defaultKindIcons = map[string]string{"wayshrine": "TUI_Icon_Map_ReviveShrine_Active", "attune": "TUI_Icon_Map_ReviveShrine_Active", "harbor": "TUI_Icon_Map_Harbor", "castle": "TUI_Icon_Map_Castle", "starting_area": "TUI_Icon_Map_StartingArea", "spawn": "TUI_Icon_Map_StartingArea"}
	defaultKindTints = map[string]string{"wayshrine": "#38bdf8", "attune": "#38bdf8", "harbor": "#0ea5e9", "castle": "#ef4444", "starting_area": "#22c55e", "spawn": "#22c55e", "city_node": "#f59e0b", "outpost": "#f59e0b"}
)

func defaultIconForKind(kind, nodeType string) string {
	if ic, ok := defaultKindIcons[strings.ToLower(kind)]; ok {
		return ic
	} else if strings.EqualFold(kind, "city_node") || strings.EqualFold(kind, "outpost") {
		return resolveCityNodeIconName("", "", nodeType, strings.ToLower(kind), nil)
	}
	return "TUI_Icon_Map_MinorPOI"
}

func defaultTintForKind(kind, _ string) string {
	if t, ok := defaultKindTints[strings.ToLower(kind)]; ok {
		return t
	}
	return "#a855f7"
}

func (c *Catalog) AllPlaces() []PlaceEntry {
	var contentDir string
	if c != nil {
		c.mu.RLock()
		base, dir := c.places, c.contentDir
		c.mu.RUnlock()
		contentDir = dir
		if len(base) > 0 {
			return mergeDiskTSVPlaces(append([]PlaceEntry(nil), base...), contentDir)
		}
	}
	places, _ := loadPlacesFromDB(context.Background(), nil)
	return mergeDiskTSVPlaces(places, contentDir)
}

func (c *Catalog) SearchPlaces(ctx context.Context, q string, limit int) []PlaceEntry {
	places, _ := c.SearchPlacesFiltered(ctx, PlaceFilter{Query: q, Limit: limit})
	return places
}

func (c *Catalog) SearchPlacesFiltered(_ context.Context, f PlaceFilter) ([]PlaceEntry, int) {
	merged := mergePlaces(c.AllPlaces(), f.ExtraPlaces, f.NodeLevels)
	qLower, kindLower := strings.ToLower(strings.TrimSpace(f.Query)), strings.ToLower(strings.TrimSpace(f.Kind))
	matched := make([]PlaceEntry, 0, len(merged))
	for _, p := range merged {
		if (kindLower == "" || matchesPlaceKind(p, kindLower)) && (qLower == "" || matchesPlaceQuery(p, qLower)) {
			matched = append(matched, p)
		}
	}
	sortPlaces(matched, f.Sort)
	return paginateSlice(matched, f.Offset, f.Limit, 50, max(200, f.Limit))
}

func mergePlaces(base, extra []PlaceEntry, nodeLevels map[string]int) []PlaceEntry {
	out, byName := append([]PlaceEntry(nil), base...), make(map[string]int, len(base))
	for i, p := range out {
		byName[strings.ToLower(p.Name)] = i
	}
	for _, ep := range extra {
		if k := strings.ToLower(strings.TrimSpace(ep.Name)); k != "" {
			if idx, exists := byName[k]; exists {
				if ep.X != 0 || ep.Y != 0 || ep.Z != 0 {
					out[idx].X, out[idx].Y, out[idx].Z = ep.X, ep.Y, ep.Z
				}
				out[idx].DisplayName = firstNonEmptyStr(ep.DisplayName, out[idx].DisplayName)
			} else {
				byName[k], out = len(out), append(out, ep)
			}
		}
	}
	for i := range out {
		for _, k := range []string{out[i].GUID, strings.ToLower(out[i].Name), strings.ToLower(out[i].NodeName)} {
			if lv, ok := nodeLevels[k]; ok && k != "" {
				out[i].Level = lv
				break
			}
		}
		if out[i].Kind == "city_node" {
			out[i].StageName = StageNameForLevel(out[i].Level)
		}
	}
	return out
}

func matchesPlaceKind(p PlaceEntry, kindLower string) bool {
	return (kindLower == "levelable" && p.Levelable) || strings.EqualFold(p.Kind, kindLower) || strings.EqualFold(p.Category(), kindLower) || strings.EqualFold(p.NodeType, kindLower)
}

func matchesPlaceQuery(p PlaceEntry, qLower string) bool {
	return strings.Contains(strings.ToLower(p.NodeName+" "+p.DisplayName+" "+p.Name+" "+p.GUID), qLower)
}

func sortPlaces(places []PlaceEntry, sortSpec string) {
	field, desc := parseSortField(sortSpec)
	sort.SliceStable(places, func(i, j int) bool {
		a, b := places[i], places[j]
		if field == "level" && a.Level != b.Level {
			return compareLess(a.Level < b.Level, desc)
		} else if (field == "name" || field == "display_name" || field == "node_name") && a.DisplayName != b.DisplayName {
			return compareLess(a.DisplayName < b.DisplayName, desc)
		} else if field == "guid" && a.GUID != b.GUID {
			return compareLess(a.GUID < b.GUID, desc)
		} else if field == "" && ((a.Kind == "spawn") != (b.Kind == "spawn") || a.Levelable != b.Levelable) {
			if (a.Kind == "spawn") != (b.Kind == "spawn") {
				return a.Kind == "spawn"
			}
			return a.Levelable
		}
		return a.DisplayName < b.DisplayName
	})
}

func parseSortField(spec string) (string, bool) {
	s := strings.TrimSpace(spec)
	if idx := strings.IndexByte(s, ','); idx >= 0 {
		s = strings.TrimSpace(s[:idx])
	}
	return strings.ToLower(strings.TrimSpace(strings.TrimPrefix(strings.TrimPrefix(s, "-"), "+"))), strings.HasPrefix(s, "-")
}

func compareLess(less, desc bool) bool { return less != desc }
