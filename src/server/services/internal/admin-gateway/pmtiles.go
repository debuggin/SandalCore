// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"bytes"
	"compress/gzip"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	admincatalog "aocrop/services/internal/admin-catalog"
)

const pmtilesHeaderBytes = 127

var tzBase = [...]uint64{
	0, 1, 5, 21, 85, 341, 1365, 5461, 21845, 87381, 349525, 1398101, 5592405,
	22369621, 89478485, 357913941, 1431655765,
}

type PmTilesEntry struct {
	TileID    uint64
	Offset    uint64
	Length    uint32
	RunLength uint32
}

type PmTilesNodeMeta struct {
	NodeIndex   int     `json:"node_index"`
	GUID        string  `json:"guid"`
	Name        string  `json:"name"`
	DisplayName string  `json:"display_name"`
	TextureStem string  `json:"texture_stem,omitempty"`
	Ox          float64 `json:"ox"`
	Oy          float64 `json:"oy"`
	Oz          float64 `json:"oz"`
	YawDeg      float64 `json:"yaw_deg"`
	SizeCm      int     `json:"size_cm"`
	Level       int     `json:"level"`
	PlugTier    int     `json:"plug_tier"`
	StageName   string  `json:"stage_name"`
}

type rawPmTilesMeta struct {
	X0       int `json:"x0"`
	Y0       int `json:"y0"`
	Cols     int `json:"cols"`
	Rows     int `json:"rows"`
	SourceCm int `json:"source_cm"`
	SourcePx int `json:"source_px"`
	TilePx   int `json:"tile_px"`
	PlugPx   int `json:"plug_px"`
	WidthPx  int `json:"width_px"`
	HeightPx int `json:"height_px"`
	MaxZoom  int `json:"max_zoom"`
	PlugZoom int `json:"plug_zoom"`
	Nodes    []struct {
		NodeIndex   int         `json:"node_index"`
		GUID        json.Number `json:"guid"`
		Name        string      `json:"name"`
		DisplayName string      `json:"display_name"`
		TextureStem string      `json:"texture_stem"`
		Ox          float64     `json:"ox"`
		Oy          float64     `json:"oy"`
		Oz          float64     `json:"oz"`
		YawDeg      float64     `json:"yaw_deg"`
		SizeCm      int         `json:"size_cm"`
	} `json:"nodes"`
}

type PmTilesArchive struct {
	path           string
	file           *os.File
	modTime        time.Time
	size           int64
	rootOffset     uint64
	rootBytes      uint64
	metadataOffset uint64
	metadataBytes  uint64
	leafOffset     uint64
	leafBytes      uint64
	dataOffset     uint64
	dataBytes      uint64
	rootEntries    []PmTilesEntry
	meta           *rawPmTilesMeta
	leafMu         sync.RWMutex
	leafCache      map[uint64][]PmTilesEntry
}

func OpenPmTilesArchive(path string) (*PmTilesArchive, error) {
	path = strings.TrimSpace(path)
	if path == "" {
		return nil, errors.New("pmtiles: empty path")
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	st, err := f.Stat()
	if err != nil || st.IsDir() || st.Size() < pmtilesHeaderBytes {
		_ = f.Close()
		return nil, fmt.Errorf("pmtiles: invalid file %s", path)
	}
	var hdr [pmtilesHeaderBytes]byte
	if _, err := io.ReadFull(f, hdr[:]); err != nil {
		_ = f.Close()
		return nil, err
	}
	if string(hdr[0:7]) != "PMTiles" || hdr[7] != 3 {
		_ = f.Close()
		return nil, fmt.Errorf("pmtiles: invalid magic or version in %s", path)
	}
	arch := &PmTilesArchive{
		path:           path,
		file:           f,
		modTime:        st.ModTime(),
		size:           st.Size(),
		rootOffset:     binary.LittleEndian.Uint64(hdr[8:16]),
		rootBytes:      binary.LittleEndian.Uint64(hdr[16:24]),
		metadataOffset: binary.LittleEndian.Uint64(hdr[24:32]),
		metadataBytes:  binary.LittleEndian.Uint64(hdr[32:40]),
		leafOffset:     binary.LittleEndian.Uint64(hdr[40:48]),
		leafBytes:      binary.LittleEndian.Uint64(hdr[48:56]),
		dataOffset:     binary.LittleEndian.Uint64(hdr[56:64]),
		dataBytes:      binary.LittleEndian.Uint64(hdr[64:72]),
		leafCache:      make(map[uint64][]PmTilesEntry),
	}
	if err := arch.loadRootAndMetadata(); err != nil {
		_ = f.Close()
		return nil, err
	}
	return arch, nil
}

func (a *PmTilesArchive) Close() error {
	if a == nil || a.file == nil {
		return nil
	}
	return a.file.Close()
}

func (a *PmTilesArchive) loadRootAndMetadata() error {
	rootBuf := make([]byte, a.rootBytes)
	if _, err := a.file.ReadAt(rootBuf, int64(a.rootOffset)); err != nil {
		return fmt.Errorf("pmtiles: read root dir: %w", err)
	}
	entries, err := decodePmTilesDirectory(rootBuf)
	if err != nil {
		return fmt.Errorf("pmtiles: decode root dir: %w", err)
	}
	a.rootEntries = entries
	if a.metadataBytes > 0 && a.metadataBytes <= 4<<20 {
		metaBuf := make([]byte, a.metadataBytes)
		if _, err := a.file.ReadAt(metaBuf, int64(a.metadataOffset)); err == nil {
			rawJSON, err := maybeGunzip(metaBuf)
			if err == nil {
				var parsed rawPmTilesMeta
				if json.Unmarshal(rawJSON, &parsed) == nil {
					a.meta = &parsed
				}
			}
		}
	}
	return nil
}

func maybeGunzip(data []byte) ([]byte, error) {
	if len(data) >= 2 && data[0] == 0x1f && data[1] == 0x8b {
		zr, err := gzip.NewReader(bytes.NewReader(data))
		if err != nil {
			return nil, err
		}
		defer zr.Close()
		return io.ReadAll(io.LimitReader(zr, 16<<20))
	}
	return data, nil
}

func decodePmTilesDirectory(compressed []byte) ([]PmTilesEntry, error) {
	raw, err := maybeGunzip(compressed)
	if err != nil {
		return nil, err
	}
	r := bytes.NewReader(raw)
	nEntries, err := binary.ReadUvarint(r)
	if err != nil || nEntries > 2_000_000 {
		return nil, fmt.Errorf("pmtiles: invalid directory entry count %d", nEntries)
	}
	n := int(nEntries)
	entries := make([]PmTilesEntry, n)
	var lastID uint64
	for i := 0; i < n; i++ {
		delta, err := binary.ReadUvarint(r)
		if err != nil {
			return nil, err
		}
		lastID += delta
		entries[i].TileID = lastID
	}
	for i := 0; i < n; i++ {
		rl, err := binary.ReadUvarint(r)
		if err != nil {
			return nil, err
		}
		entries[i].RunLength = uint32(rl)
	}
	for i := 0; i < n; i++ {
		ln, err := binary.ReadUvarint(r)
		if err != nil {
			return nil, err
		}
		entries[i].Length = uint32(ln)
	}
	for i := 0; i < n; i++ {
		off, err := binary.ReadUvarint(r)
		if err != nil {
			return nil, err
		}
		if off == 0 && i > 0 {
			entries[i].Offset = entries[i-1].Offset + uint64(entries[i-1].Length)
		} else if off > 0 {
			entries[i].Offset = off - 1
		}
	}
	return entries, nil
}

func ZxyToTileID(z, x, y int) (uint64, error) {
	if z < 0 || z >= len(tzBase) {
		return 0, fmt.Errorf("invalid zoom level %d", z)
	}
	maxCoord := 1 << z
	if x < 0 || y < 0 || x >= maxCoord || y >= maxCoord {
		return 0, fmt.Errorf("tile coordinates (%d,%d) out of range for z=%d", x, y, z)
	}
	if z == 0 {
		return 0, nil
	}
	n := int64(1) << z
	tx, ty, d := int64(x), int64(y), int64(0)
	for s := n / 2; s > 0; s /= 2 {
		var rx, ry int64
		if (tx & s) > 0 {
			rx = 1
		}
		if (ty & s) > 0 {
			ry = 1
		}
		d += s * s * ((3 * rx) ^ ry)
		if ry == 0 {
			if rx == 1 {
				tx = s - 1 - tx
				ty = s - 1 - ty
			}
			tx, ty = ty, tx
		}
	}
	return tzBase[z] + uint64(d), nil
}

func (a *PmTilesArchive) ReadTile(z, x, y int) ([]byte, PmTilesEntry, bool, error) {
	if a == nil || a.file == nil {
		return nil, PmTilesEntry{}, false, errors.New("pmtiles archive not loaded")
	}
	tileID, err := ZxyToTileID(z, x, y)
	if err != nil {
		return nil, PmTilesEntry{}, false, err
	}
	entry, found, err := a.findTileEntry(tileID)
	if err != nil || !found || entry.Length == 0 {
		return nil, PmTilesEntry{}, false, err
	}
	buf := make([]byte, entry.Length)
	if _, err := a.file.ReadAt(buf, int64(a.dataOffset+entry.Offset)); err != nil {
		return nil, PmTilesEntry{}, false, err
	}
	return buf, entry, true, nil
}

func (a *PmTilesArchive) findTileEntry(tileID uint64) (PmTilesEntry, bool, error) {
	if e, ok := lookupInDirectory(a.rootEntries, tileID); ok {
		return e, true, nil
	}
	leafPtr, hasLeaf := findLeafDirectory(a.rootEntries, tileID)
	if !hasLeaf {
		return PmTilesEntry{}, false, nil
	}
	leafEntries, err := a.loadLeafDirectory(leafPtr)
	if err != nil {
		return PmTilesEntry{}, false, err
	}
	e, ok := lookupInDirectory(leafEntries, tileID)
	return e, ok, nil
}

func lookupInDirectory(entries []PmTilesEntry, tileID uint64) (PmTilesEntry, bool) {
	lo, hi := 0, len(entries)-1
	for lo <= hi {
		mid := int(uint(lo+hi) >> 1)
		e := entries[mid]
		if e.RunLength == 0 {
			if e.TileID <= tileID {
				lo = mid + 1
			} else {
				hi = mid - 1
			}
			continue
		}
		if tileID < e.TileID {
			hi = mid - 1
		} else if tileID >= e.TileID+uint64(e.RunLength) {
			lo = mid + 1
		} else {
			return e, true
		}
	}
	for _, e := range entries {
		if e.RunLength > 0 && tileID >= e.TileID && tileID < e.TileID+uint64(e.RunLength) {
			return e, true
		}
	}
	return PmTilesEntry{}, false
}

func findLeafDirectory(entries []PmTilesEntry, tileID uint64) (PmTilesEntry, bool) {
	var matched PmTilesEntry
	found := false
	for _, e := range entries {
		if e.RunLength == 0 && e.TileID <= tileID {
			matched, found = e, true
		}
	}
	return matched, found
}

func (a *PmTilesArchive) loadLeafDirectory(ptr PmTilesEntry) ([]PmTilesEntry, error) {
	a.leafMu.RLock()
	cached, ok := a.leafCache[ptr.Offset]
	a.leafMu.RUnlock()
	if ok {
		return cached, nil
	}
	buf := make([]byte, ptr.Length)
	if _, err := a.file.ReadAt(buf, int64(a.leafOffset+ptr.Offset)); err != nil {
		return nil, err
	}
	parsed, err := decodePmTilesDirectory(buf)
	if err != nil {
		return nil, err
	}
	a.leafMu.Lock()
	a.leafCache[ptr.Offset] = parsed
	a.leafMu.Unlock()
	return parsed, nil
}

func (s *Server) handleWorldPmTiles(w http.ResponseWriter, r *http.Request, _ Role) {
	arch := s.getPmTilesArchive()
	if arch == nil || arch.file == nil {
		writeProblem(w, r, http.StatusNotFound, "pmtiles-not-found", "verra.pmtiles archive is not present on disk.")
		return
	}
	w.Header().Set("Content-Type", "application/vnd.pmtiles")
	w.Header().Set("Accept-Ranges", "bytes")
	w.Header().Set("Cache-Control", "public, max-age=3600")
	http.ServeContent(w, r, "verra.pmtiles", arch.modTime, arch.file)
}

func (s *Server) handleWorldTile(w http.ResponseWriter, r *http.Request, _ Role) {
	zStr := r.PathValue("z")
	xStr := r.PathValue("x")
	yRaw := strings.TrimSuffix(strings.TrimSuffix(r.PathValue("y"), ".png"), ".webp")
	z, errZ := strconv.Atoi(zStr)
	x, errX := strconv.Atoi(xStr)
	y, errY := strconv.Atoi(yRaw)
	if errZ != nil || errX != nil || errY != nil || !isValidTileZxy(z, x, y) {
		writeProblem(w, r, http.StatusBadRequest, "invalid-tile-coords", "Invalid tile coordinates {z}/{x}/{y}.")
		return
	}
	arch := s.getPmTilesArchive()
	if arch == nil {
		writeProblem(w, r, http.StatusNotFound, "pmtiles-not-found", "verra.pmtiles archive is not present.")
		return
	}
	data, entry, found, err := arch.ReadTile(z, x, y)
	if err != nil || !found {
		writeProblem(w, r, http.StatusNotFound, "tile-not-found", fmt.Sprintf("Tile %d/%d/%d not found in archive.", z, x, y))
		return
	}
	etag := fmt.Sprintf(`"tile-%d-%d-%d-%d-%d"`, z, x, y, entry.Offset, entry.Length)
	w.Header().Set("ETag", etag)
	w.Header().Set("Vary", "Accept-Encoding")
	if !arch.modTime.IsZero() {
		w.Header().Set("Last-Modified", arch.modTime.UTC().Format(http.TimeFormat))
	}
	w.Header().Set("Cache-Control", "public, max-age=86400, immutable")
	if matchesIfNoneMatch(r.Header.Get("If-None-Match"), etag) || notModifiedSince(r, arch.modTime) {
		w.WriteHeader(http.StatusNotModified)
		return
	}
	w.Header().Set("Content-Type", detectTileContentType(data))
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	w.WriteHeader(http.StatusOK)
	if r.Method != http.MethodHead {
		_, _ = w.Write(data)
	}
}

func isValidTileZxy(z, x, y int) bool {
	if z == 8 {
		return x >= 0 && x < 26 && y >= 0 && y < 5
	}
	if z < 0 || z > 7 {
		return false
	}
	limit := 1 << z
	return x >= 0 && x < limit && y >= 0 && y < limit
}

func detectTileContentType(data []byte) string {
	if len(data) >= 8 && string(data[:4]) == "\x89PNG" {
		return "image/png"
	}
	if len(data) >= 12 && string(data[:4]) == "RIFF" && string(data[8:12]) == "WEBP" {
		return "image/webp"
	}
	return "application/octet-stream"
}

func (s *Server) handleWorldMapMeta(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	const cacheKey = "world:map-meta"
	if cached, ok := s.metaCache.get(cacheKey, s.now()); ok {
		s.cacheHits.Add(1)
		writeCachedJSONWithModTime(w, r, "private, max-age=5, must-revalidate", s.lastMutatedTime(), cached)
		return
	}
	s.cacheMisses.Add(1)
	payload := s.buildMapMetaPayload(r.Context())
	s.metaCache.set(cacheKey, payload, 5*time.Second, s.now())
	writeCachedJSONWithModTime(w, r, "private, max-age=5, must-revalidate", s.lastMutatedTime(), payload)
}

func (s *Server) buildMapMetaPayload(ctx context.Context) map[string]any {
	arch := s.getPmTilesArchive()
	liveLevels := s.fetchCityNodeLevels(ctx)
	nodes := buildMapMetaNodes(arch, liveLevels)
	meta := map[string]any{
		"schema_version":  "1.0",
		"x0":              -47,
		"y0":              -17,
		"cols":            51,
		"rows":            46,
		"source_cm":       50000,
		"source_px":       512,
		"tile_px":         256,
		"plug_px":         1024,
		"width_px":        26112,
		"height_px":       23552,
		"min_zoom":        0,
		"max_zoom":        7,
		"plug_zoom":       8,
		"pmtiles_present": arch != nil,
		"nodes":           nodes,
	}
	if arch != nil && arch.meta != nil && arch.meta.Cols > 0 {
		meta["x0"] = arch.meta.X0
		meta["y0"] = arch.meta.Y0
		meta["cols"] = arch.meta.Cols
		meta["rows"] = arch.meta.Rows
		meta["width_px"] = arch.meta.WidthPx
		meta["height_px"] = arch.meta.HeightPx
	}
	return meta
}

func buildMapMetaNodes(arch *PmTilesArchive, liveLevels map[string]int) []PmTilesNodeMeta {
	specs := admincatalog.DefaultLevelableNodes()
	out := make([]PmTilesNodeMeta, 0, len(specs))
	if arch != nil && arch.meta != nil && len(arch.meta.Nodes) > 0 {
		for _, n := range arch.meta.Nodes {
			guidStr := n.GUID.String()
			lv := resolveLiveLevel(liveLevels, guidStr, n.Name, n.DisplayName)
			sizeCm := n.SizeCm
			if sizeCm <= 0 {
				sizeCm = 100000
			}
			out = append(out, PmTilesNodeMeta{
				NodeIndex: n.NodeIndex, GUID: guidStr, Name: n.Name, DisplayName: n.DisplayName,
				TextureStem: n.TextureStem, Ox: n.Ox, Oy: n.Oy, Oz: n.Oz, YawDeg: n.YawDeg,
				SizeCm: sizeCm, Level: lv, PlugTier: admincatalog.PlugTierForLevel(lv),
				StageName: admincatalog.StageNameForLevel(lv),
			})
		}
		return out
	}
	for _, n := range specs {
		guidStr := strconv.FormatInt(n.GUID, 10)
		lv := resolveLiveLevel(liveLevels, guidStr, n.Name, n.DisplayName)
		out = append(out, PmTilesNodeMeta{
			NodeIndex: n.NodeIndex, GUID: guidStr, Name: n.Name, DisplayName: n.DisplayName,
			TextureStem: n.TextureStem, Ox: n.Ox, Oy: n.Oy, Oz: n.Oz, YawDeg: n.YawDeg,
			SizeCm: n.SizeCm, Level: lv, PlugTier: admincatalog.PlugTierForLevel(lv),
			StageName: admincatalog.StageNameForLevel(lv),
		})
	}
	return out
}

func resolveLiveLevel(levels map[string]int, guidStr, name, disp string) int {
	for _, k := range []string{guidStr, strings.ToLower(name), strings.ToLower(disp)} {
		if k != "" {
			if lv, ok := levels[k]; ok {
				return lv
			}
		}
	}
	return 0
}
