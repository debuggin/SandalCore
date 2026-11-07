// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admincatalog

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type ZoiRecord struct {
	Index       int     `json:"index"`
	Guid        int64   `json:"guid"`
	Name        string  `json:"name"`
	DisplayName string  `json:"display_name"`
	Region      string  `json:"region"`
	ZoneType    string  `json:"zone_type"`
	BorderImage string  `json:"border_image"`
	MinX        float64 `json:"min_x"`
	MinY        float64 `json:"min_y"`
	MaxX        float64 `json:"max_x"`
	MaxY        float64 `json:"max_y"`
}

type PlaceRecord = PlaceEntry

const (
	zoisTSVHeader   = "index\tguid\tname\tdisplay_name\tregion\tzone_type\tborder_image\tmin_x\tmin_y\tmax_x\tmax_y"
	placesTSVHeader = "guid\tname\tdisplay_name\tnode_name\tnode_type\tkind\tx\ty\tz\tyaw\tradius\tlevelable\tlevel\tstage_name"
)

func ResolveDataMode(dataType string) string {
	key := strings.ToUpper(strings.ReplaceAll(strings.TrimSpace(dataType), "-", "_"))
	if key != "" {
		if v := strings.TrimSpace(os.Getenv("AOC_DATA_MODE_" + key)); v != "" {
			if strings.EqualFold(v, "disk") {
				return "disk"
			}
			return "memory"
		}
	}
	for _, envKey := range []string{"AOC_DATA_MODE", "AOC_DATA_MODE_DEFAULT"} {
		if v := strings.TrimSpace(os.Getenv(envKey)); v != "" {
			if strings.EqualFold(v, "disk") {
				return "disk"
			}
			return "memory"
		}
	}
	return "memory"
}

func WriteZoisTSV(path string, rows []ZoiRecord) error {
	if strings.TrimSpace(path) == "" {
		return fmt.Errorf("admincatalog: empty zois.tsv path")
	}
	var b strings.Builder
	b.WriteString(zoisTSVHeader)
	b.WriteByte('\n')
	for _, r := range rows {
		fmt.Fprintf(&b, "%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			r.Index, r.Guid, cleanTSVField(r.Name), cleanTSVField(r.DisplayName),
			cleanTSVField(r.Region), cleanTSVField(r.ZoneType), cleanTSVField(r.BorderImage),
			formatTSVFloat(r.MinX), formatTSVFloat(r.MinY), formatTSVFloat(r.MaxX), formatTSVFloat(r.MaxY))
	}
	return os.WriteFile(path, []byte(b.String()), 0o600)
}

func LoadZoisTSV(path string) ([]ZoiRecord, error) {
	var out []ZoiRecord
	if err := scanTSVRows(path, func(cols []string) {
		if rec, ok := parseZoiTSVRow(cols, len(out)); ok {
			out = append(out, rec)
		}
	}); err != nil {
		return nil, fmt.Errorf("admincatalog: load zois tsv: %w", err)
	}
	return out, nil
}

func parseZoiTSVRow(cols []string, fallbackIdx int) (ZoiRecord, bool) {
	if len(cols) >= 11 {
		idx, err1 := strconv.Atoi(strings.TrimSpace(cols[0]))
		guid, err2 := strconv.ParseInt(strings.TrimSpace(cols[1]), 10, 64)
		if err1 != nil || err2 != nil {
			return ZoiRecord{}, false
		}
		return ZoiRecord{
			Index: idx, Guid: guid, Name: strings.TrimSpace(cols[2]), DisplayName: strings.TrimSpace(cols[3]),
			Region: strings.TrimSpace(cols[4]), ZoneType: strings.TrimSpace(cols[5]), BorderImage: strings.TrimSpace(cols[6]),
			MinX: parseTSVFloat(cols[7]), MinY: parseTSVFloat(cols[8]), MaxX: parseTSVFloat(cols[9]), MaxY: parseTSVFloat(cols[10]),
		}, true
	}
	if len(cols) >= 2 {
		guid, err := strconv.ParseInt(strings.TrimSpace(cols[0]), 10, 64)
		if err != nil {
			return ZoiRecord{}, false
		}
		name := strings.TrimSpace(cols[1])
		return ZoiRecord{Index: fallbackIdx, Guid: guid, Name: name, DisplayName: humanizeIdentifier(name)}, true
	}
	return ZoiRecord{}, false
}

func WritePlacesTSV(path string, rows []PlaceRecord) error {
	if strings.TrimSpace(path) == "" {
		return fmt.Errorf("admincatalog: empty places.tsv path")
	}
	var b strings.Builder
	b.WriteString(placesTSVHeader)
	b.WriteByte('\n')
	for _, r := range rows {
		lv := 0
		if r.Levelable {
			lv = 1
		}
		fmt.Fprintf(&b, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%s\n",
			cleanTSVField(r.GUID), cleanTSVField(r.Name), cleanTSVField(r.DisplayName),
			cleanTSVField(r.NodeName), cleanTSVField(r.NodeType), cleanTSVField(r.Kind),
			formatTSVFloat(r.X), formatTSVFloat(r.Y), formatTSVFloat(r.Z),
			formatTSVFloat(r.Yaw), formatTSVFloat(r.Radius), lv, r.Level, cleanTSVField(r.StageName))
	}
	return os.WriteFile(path, []byte(b.String()), 0o600)
}

func LoadPlacesTSV(path string) ([]PlaceRecord, error) {
	var out []PlaceRecord
	if err := scanTSVRows(path, func(cols []string) {
		if rec, ok := parsePlaceTSVRow(cols); ok {
			out = append(out, rec)
		}
	}); err != nil {
		return nil, fmt.Errorf("admincatalog: load places tsv: %w", err)
	}
	return out, nil
}

func scanTSVRows(path string, onRow func(cols []string)) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64*1024), 1024*1024)
	for sc.Scan() {
		line := strings.TrimRight(sc.Text(), "\r\n")
		if strings.TrimSpace(line) == "" || strings.HasPrefix(strings.TrimSpace(line), "#") {
			continue
		}
		cols := strings.Split(line, "\t")
		if c0 := strings.TrimSpace(cols[0]); !strings.EqualFold(c0, "guid") && !strings.EqualFold(c0, "index") {
			onRow(cols)
		}
	}
	return sc.Err()
}

func parsePlaceTSVRow(cols []string) (PlaceRecord, bool) {
	if len(cols) < 6 {
		return PlaceRecord{}, false
	}
	guid, name := strings.TrimSpace(cols[0]), strings.TrimSpace(cols[1])
	if guid == "" && name == "" {
		return PlaceRecord{}, false
	}
	lvRaw := strings.ToLower(colAt(cols, 11))
	level, _ := strconv.Atoi(colAt(cols, 12))
	rec := PlaceRecord{
		GUID: guid, Name: name, DisplayName: colAt(cols, 2), NodeName: colAt(cols, 3),
		NodeType: colAt(cols, 4), Kind: firstNonEmptyStr(colAt(cols, 5), "poi"),
		X: parseTSVFloat(colAt(cols, 6)), Y: parseTSVFloat(colAt(cols, 7)), Z: parseTSVFloat(colAt(cols, 8)),
		Yaw: parseTSVFloat(colAt(cols, 9)), Radius: parseTSVFloat(colAt(cols, 10)),
		Levelable: lvRaw == "1" || lvRaw == "true", Level: level, StageName: colAt(cols, 13),
	}
	registerPlaceVisual(rec.GUID, rec.Name, PlaceVisualMeta{Category: rec.Kind, NodeType: rec.NodeType})
	return rec, true
}

func mergeDiskTSVPlaces(base []PlaceEntry, contentDir string) []PlaceEntry {
	out := base
	for _, s := range []struct{ mode, file, kind string }{{"city_nodes", "city_nodes.tsv", "city_node"}, {"pois", "pois.tsv", "poi"}} {
		if ResolveDataMode(s.mode) == "disk" {
			if rows, err := LoadPlacesTSV(resolveContentTSVPath(contentDir, s.file)); err == nil && len(rows) > 0 {
				out = mergePlaces(out, rows, nil)
			}
		}
	}
	return out
}

func resolveContentTSVPath(contentDir, fileName string) string {
	for _, dir := range []string{strings.TrimSpace(os.Getenv("AOC_CONTENT_TSV_DIR")), strings.TrimSpace(contentDir)} {
		if cand := filepath.Join(dir, fileName); dir != "" && fileExists(cand) {
			return cand
		}
	}
	return ""
}

func fileExists(path string) bool { info, err := os.Stat(path); return err == nil && !info.IsDir() }
func cleanTSVField(s string) string {
	return strings.NewReplacer("\t", " ", "\r", " ", "\n", " ").Replace(strings.TrimSpace(s))
}
func formatTSVFloat(v float64) string { return strconv.FormatFloat(v, 'f', -1, 64) }
func parseTSVFloat(s string) float64  { v, _ := strconv.ParseFloat(strings.TrimSpace(s), 64); return v }
func colAt(cols []string, idx int) string {
	if idx >= 0 && idx < len(cols) {
		return strings.TrimSpace(cols[idx])
	}
	return ""
}
