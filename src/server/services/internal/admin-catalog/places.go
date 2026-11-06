// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admincatalog

import (
	"context"
	"database/sql"
	"sort"
	"strconv"
	"strings"
)

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

var NodeStageNames = []string{
	"Wilderness",
	"Expedition",
	"Encampment",
	"Village",
	"Town",
	"City",
	"Metropolis",
}

func StageNameForLevel(level int) string {
	if level >= 0 && level < len(NodeStageNames) {
		return NodeStageNames[level]
	}
	return "Wilderness"
}

func PlugTierForLevel(level int) int {
	switch {
	case level <= 0:
		return -1
	case level == 1:
		return 0
	case level == 2:
		return 1
	case level == 3:
		return 2
	case level == 4:
		return 3
	default:
		return 4
	}
}

func DefaultLevelableNodes() []LevelableNodeSpec {
	return []LevelableNodeSpec{
		{0, 108646108518008, "Verra_RVR_Winstead", "Winstead", "Winstead", -641389.0, 377367.1, 13819.9, 319.207763671875, 100000},
		{1, 108646121231993, "Verra_RVR_Joeva", "Joeva", "Joeva", -1092010.8, 522023.2, 13644.1, 292.6263427734375, 100000},
		{2, 108646608033403, "Verra_RVR_NewAela", "New Aela", "New_Aela", -1134183.5439466983, 853387.2772475089, 11761.82381661977, -90.21495968964565, 100000},
		{3, 108646610851452, "Verra_RVR_Halcyon", "Halcyon", "Halcyon", -892130.8, 297711.6, 11096.0, 180.0, 100000},
		{4, 108646615701117, "Verra_RVR_Miraleth", "Miraleth", "Miraleth", -459889.634548, 599317.092515, 11000.972636, -20.0, 100000},
		{5, 6064632225073725498, "Verra_TUR_Windansea", "Windansea", "Windansea", -1100624.184211, 246466.992911, 6674.000004, 55.41214, 100000},
		{6, 6064632734251679746, "Verra_SAN_Aithanahr", "Aithanahr", "Aithanahr", -232975.5, 758608.9, 5628.6, 304.12353515625, 100000},
		{7, 6064632734253907971, "Verra_SAN_Azmaran", "Azmaran", "Azmaran", -276070.8, 987508.0, 8812.2, 351.156005859375, 100000},
		{8, 6064632734268915716, "Verra_SAN_Sunhaven", "Sunhaven", "Sunhaven", -619803.283992, 873514.222232, 12073.59434, -127.073107, 100000},
		{9, 6064633097318170624, "Verra_SAN_Djinna", "Djinna", "Djinna", 63370.908474, 861198.365739, 5066.0, -187.137862, 100000},
		{10, 6064633097371058177, "Verra_SAN_SquallsEnd", "Squall's End", "Squall_s_End", -37370.0, 1133335.0, 16385.348318, -217.228668, 100000},
		{11, 6064633334652600320, "Verra_TUR_Korrin", "Korrin", "Korrin", -1764281.284871, 114557.650316, 5066.0, 90.707388, 100000},
		{12, 6064633334741794817, "Verra_TUR_Brinebarrel", "Brinebarrel", "Brinebarrel", -1860980.218817, -171832.947158, 7472.764935, -46.566062, 100000},
		{13, 6064633407845105664, "Verra_TUR_Seahook", "Seahook", "Seahook", -1764263.887544, 539283.279654, 5066.0, 14.228906, 100000},
		{14, 6064633407846088705, "Verra_TUR_Shorefoot", "Shorefoot", "Shorefoot", -1372642.268119, 57374.666533, 5066.0, -66.900158, 100000},
		{15, 6064633425419042816, "Verra_JUN_Wildport", "Wildport", "Wildport", -1784521.8, 811519.5, 11606.5, 16.0015869140625, 100000},
		{16, 6064633425517019141, "Verra_JUN_Hecribba", "Hecribba", "Hecribba", -1707968.6, 1053249.0, 9567.1, 246.0003662109375, 100000},
		{17, 6064633425555816457, "Verra_JUN_Vinebreach", "Vinebreach", "Vinebreach", -1476480.0, 1178456.0, 7013.0, 200.0, 100000},
		{18, 6064633429404090381, "Verra_JUN_Arisalon", "Arisalon", "Arisalon", -1250506.0, 1062377.0, 5379.0, -35.0, 100000},
		{19, 6064633429423882257, "Verra_JUN_TangledPost", "Tangled Post", "Tangled_Post", -1031419.0, 1098848.0, 7264.0, -3.5, 100000},
		{20, 6064633429437251605, "Verra_JUN_Mythbreak", "Mythbreak", "Mythbreak", -742705.0, 1087738.0, 5066.0, -159.058146, 100000},
		{21, 6064633651969589265, "Verra_ANV_Vexhelm", "Vexhelm", "Vexhelm", -1463138.3, -338058.1, 26903.5, 60.0018310546875, 100000},
		{22, 6064633651985711129, "Verra_ANV_Duunhold", "Duunhold", "Duunhold", -1069784.0, -150970.0, 24891.0, 0.0, 100000},
		{23, 6064633652009173023, "Verra_ANV_Dhurgrum", "Dhurgrum", "Dhurgrum", -937374.0, -411205.0, 24983.0, -15.0, 100000},
		{24, 6064633652023394340, "Verra_ANV_Vhalgadim", "Vhalgadim", "Vhalgadim", -762862.0, -563881.0, 17256.0, 169.999997, 100000},
		{25, 6064633652044300331, "Verra_ANV_KalTorhum", "Kal Torhum", "Kal_Torhum", -1274462.4, -462791.5, 1908.1, 4.998779296875, 100000},
	}
}

func BuiltInPlaces() []PlaceEntry {
	return []PlaceEntry{
		{
			GUID: "place:login", Name: "login", DisplayName: "Login Spawn", NodeName: "Login Spawn",
			NodeType: "Spawn", Kind: "spawn", X: -847992.0, Y: -538867.0, Z: 11524.0,
		},
		{
			GUID: "place:garden", Name: "garden", DisplayName: "GM Garden", NodeName: "GM Garden",
			NodeType: "Spawn", Kind: "spawn", X: -777664.5, Y: 616578.6875, Z: 11524.0,
		},
	}
}

func loadPlacesFromDB(ctx context.Context, db *sql.DB) ([]PlaceEntry, error) {
	out := BuiltInPlaces()
	origins := make(map[int64]LevelableNodeSpec, 26)
	for _, n := range DefaultLevelableNodes() {
		origins[n.GUID] = n
	}
	if !hasTable(ctx, db, "city_node") {
		for _, n := range DefaultLevelableNodes() {
			out = append(out, PlaceEntry{
				GUID: strconv.FormatInt(n.GUID, 10), Name: n.Name, DisplayName: n.DisplayName,
				NodeName: n.DisplayName, NodeType: "Economic", Kind: "city_node",
				X: n.Ox, Y: n.Oy, Z: n.Oz, Yaw: n.YawDeg, Radius: 50000, Levelable: true,
				Level: 0, StageName: StageNameForLevel(0),
			})
		}
		return out, nil
	}
	rows, err := db.QueryContext(ctx, `SELECT guid, name, COALESCE(node_name, ''), COALESCE(node_type, ''), COALESCE(x, 0), COALESCE(y, 0), COALESCE(z, 0), COALESCE(yaw, 0), COALESCE(radius, 0), COALESCE(levelable, 0) FROM city_node ORDER BY guid ASC`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var guid int64
		var name, nodeName, nodeType string
		var x, y, z, yaw, radius float64
		var levelable int
		if err := rows.Scan(&guid, &name, &nodeName, &nodeType, &x, &y, &z, &yaw, &radius, &levelable); err != nil {
			return nil, err
		}
		if orig, ok := origins[guid]; ok && (x == 0 && y == 0) {
			x, y, z, yaw = orig.Ox, orig.Oy, orig.Oz, orig.YawDeg
		}
		disp := firstNonEmptyStr(nodeName, name)
		out = append(out, PlaceEntry{
			GUID: strconv.FormatInt(guid, 10), Name: name, DisplayName: disp, NodeName: disp,
			NodeType: firstNonEmptyStr(nodeType, "Default"), Kind: "city_node",
			X: x, Y: y, Z: z, Yaw: yaw, Radius: radius, Levelable: levelable != 0,
			Level: 0, StageName: StageNameForLevel(0),
		})
	}
	return out, rows.Err()
}

func (c *Catalog) SearchPlaces(ctx context.Context, q string, limit int) []PlaceEntry {
	places, _ := c.SearchPlacesFiltered(ctx, PlaceFilter{Query: q, Limit: limit})
	return places
}

func (c *Catalog) SearchPlacesFiltered(_ context.Context, f PlaceFilter) ([]PlaceEntry, int) {
	c.mu.RLock()
	base := c.places
	c.mu.RUnlock()
	if len(base) == 0 {
		base, _ = loadPlacesFromDB(context.Background(), nil)
	}
	merged := mergePlaces(base, f.ExtraPlaces, f.NodeLevels)
	qLower := strings.ToLower(strings.TrimSpace(f.Query))
	kindLower := strings.ToLower(strings.TrimSpace(f.Kind))
	matched := make([]PlaceEntry, 0, len(merged))
	for _, p := range merged {
		if kindLower != "" && !matchesPlaceKind(p, kindLower) {
			continue
		}
		if qLower != "" && !matchesPlaceQuery(p, qLower) {
			continue
		}
		matched = append(matched, p)
	}
	sortPlaces(matched, f.Sort)
	return paginateSlice(matched, f.Offset, f.Limit, 50, 200)
}

func mergePlaces(base, extra []PlaceEntry, nodeLevels map[string]int) []PlaceEntry {
	out := make([]PlaceEntry, len(base))
	copy(out, base)
	byName := make(map[string]int, len(out))
	for i, p := range out {
		byName[strings.ToLower(p.Name)] = i
	}
	for _, ep := range extra {
		k := strings.ToLower(strings.TrimSpace(ep.Name))
		if idx, exists := byName[k]; exists {
			if ep.X != 0 || ep.Y != 0 || ep.Z != 0 {
				out[idx].X, out[idx].Y, out[idx].Z = ep.X, ep.Y, ep.Z
			}
			if ep.DisplayName != "" {
				out[idx].DisplayName = ep.DisplayName
			}
		} else if k != "" {
			byName[k] = len(out)
			out = append(out, ep)
		}
	}
	for i := range out {
		if lv, ok := lookupNodeLevel(nodeLevels, out[i].GUID, out[i].Name, out[i].NodeName); ok {
			out[i].Level = lv
		}
		if out[i].Kind == "city_node" {
			out[i].StageName = StageNameForLevel(out[i].Level)
		}
	}
	return out
}

func lookupNodeLevel(levels map[string]int, guid, name, nodeName string) (int, bool) {
	if len(levels) == 0 {
		return 0, false
	}
	for _, k := range []string{guid, strings.ToLower(name), strings.ToLower(nodeName)} {
		if k != "" {
			if lv, ok := levels[k]; ok {
				return lv, true
			}
		}
	}
	return 0, false
}

func matchesPlaceKind(p PlaceEntry, kindLower string) bool {
	if kindLower == "levelable" {
		return p.Levelable
	}
	return strings.EqualFold(p.Kind, kindLower) || strings.EqualFold(p.NodeType, kindLower)
}

func matchesPlaceQuery(p PlaceEntry, qLower string) bool {
	return strings.Contains(strings.ToLower(p.NodeName), qLower) ||
		strings.Contains(strings.ToLower(p.DisplayName), qLower) ||
		strings.Contains(strings.ToLower(p.Name), qLower) ||
		strings.Contains(strings.ToLower(p.GUID), qLower)
}

func sortPlaces(places []PlaceEntry, sortSpec string) {
	field, desc := parseSortField(sortSpec)
	sort.SliceStable(places, func(i, j int) bool {
		a, b := places[i], places[j]
		switch field {
		case "level":
			if a.Level != b.Level {
				return compareLess(a.Level < b.Level, desc)
			}
		case "name", "display_name", "node_name":
			if a.DisplayName != b.DisplayName {
				return compareLess(a.DisplayName < b.DisplayName, desc)
			}
		case "guid":
			if a.GUID != b.GUID {
				return compareLess(a.GUID < b.GUID, desc)
			}
		default:
			if (a.Kind == "spawn") != (b.Kind == "spawn") {
				return a.Kind == "spawn"
			}
			if a.Levelable != b.Levelable {
				return a.Levelable
			}
		}
		return a.DisplayName < b.DisplayName
	})
}

func parseSortField(spec string) (string, bool) {
	s := strings.TrimSpace(spec)
	if idx := strings.IndexByte(s, ','); idx >= 0 {
		s = strings.TrimSpace(s[:idx])
	}
	desc := false
	if strings.HasPrefix(s, "-") {
		desc = true
		s = strings.TrimPrefix(s, "-")
	} else if strings.HasPrefix(s, "+") {
		s = strings.TrimPrefix(s, "+")
	}
	return strings.ToLower(strings.TrimSpace(s)), desc
}

func compareLess(less, desc bool) bool {
	if desc {
		return !less
	}
	return less
}
