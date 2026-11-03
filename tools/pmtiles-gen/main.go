// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"sort"
)

const (
	HeaderSize             = 127
	MaxSupportedZoom uint8 = 8
	ClusteredHilbert uint8 = 1
	CompressionNone  uint8 = 1
	CompressionGzip  uint8 = 2
	TileTypeMVT      uint8 = 1
	TileTypePNG      uint8 = 2
	VerraMinLonE7    int32 = -1800000000
	VerraMinLatE7    int32 = -850511287
	VerraMaxLonE7    int32 = 1800000000
	VerraMaxLatE7    int32 = 850511287
)

var magicV3 = [8]byte{'P', 'M', 'T', 'i', 'l', 'e', 's', 0x03}

type Header struct {
	RootDirOffset       uint64 `json:"root_dir_offset"`
	RootDirBytes        uint64 `json:"root_dir_bytes"`
	JsonMetadataOffset  uint64 `json:"json_metadata_offset"`
	JsonMetadataBytes   uint64 `json:"json_metadata_bytes"`
	LeafDirsOffset      uint64 `json:"leaf_dirs_offset"`
	LeafDirsBytes       uint64 `json:"leaf_dirs_bytes"`
	TileDataOffset      uint64 `json:"tile_data_offset"`
	TileDataBytes       uint64 `json:"tile_data_bytes"`
	AddressedTilesCount uint64 `json:"addressed_tiles_count"`
	TileEntriesCount    uint64 `json:"tile_entries_count"`
	TileContentsCount   uint64 `json:"tile_contents_count"`
	Clustered           uint8  `json:"clustered"`
	InternalCompression uint8  `json:"internal_compression"`
	TileCompression     uint8  `json:"tile_compression"`
	TileType            uint8  `json:"tile_type"`
	MinZoom             uint8  `json:"min_zoom"`
	MaxZoom             uint8  `json:"max_zoom"`
	MinLonE7            int32  `json:"min_lon_e7"`
	MinLatE7            int32  `json:"min_lat_e7"`
	MaxLonE7            int32  `json:"max_lon_e7"`
	MaxLatE7            int32  `json:"max_lat_e7"`
	CenterZoom          uint8  `json:"center_zoom"`
	CenterLonE7         int32  `json:"center_lon_e7"`
	CenterLatE7         int32  `json:"center_lat_e7"`
}

type DirectoryEntry struct {
	TileID    uint64
	Offset    uint64
	Length    uint32
	RunLength uint32
}

type InputTile struct {
	Z       uint8
	X       uint32
	Y       uint32
	TileID  uint64
	Payload []byte
}

func rotateHilbert(n uint32, x, y *uint32, rx, ry uint32) {
	if ry != 0 {
		return
	}
	if rx == 1 {
		*x = n - 1 - *x
		*y = n - 1 - *y
	}
	*x, *y = *y, *x
}

func ZxyToTileId(z uint8, x, y uint32) (uint64, error) {
	if z > MaxSupportedZoom {
		return 0, fmt.Errorf("zoom %d exceeds max supported zoom %d", z, MaxSupportedZoom)
	}
	n := uint32(1) << z
	if x >= n || y >= n {
		return 0, fmt.Errorf("tile (%d,%d,%d) out of bounds for grid %d", z, x, y, n)
	}
	tx, ty := x, y
	baseID := ((uint64(1) << (2 * uint(z))) - 1) / 3
	var d uint64
	for s := n >> 1; s > 0; s >>= 1 {
		var rx, ry uint32
		if (tx & s) != 0 {
			rx = 1
		}
		if (ty & s) != 0 {
			ry = 1
		}
		d += uint64(s) * uint64(s) * uint64((3*rx)^ry)
		tx &= s - 1
		ty &= s - 1
		rotateHilbert(s, &tx, &ty, rx, ry)
	}
	return baseID + d, nil
}

func TileIdToZxy(tileID uint64) (uint8, uint32, uint32, error) {
	for z := uint8(0); z <= MaxSupportedZoom; z++ {
		count := uint64(1) << (2 * uint(z))
		baseID := (count - 1) / 3
		if tileID < baseID || tileID-baseID >= count {
			continue
		}
		t := tileID - baseID
		n := uint32(1) << z
		var x, y uint32
		for s := uint32(1); s < n; s <<= 1 {
			rx := uint32(1 & (t >> 1))
			ry := uint32(1 & (t ^ uint64(rx)))
			rotateHilbert(s, &x, &y, rx, ry)
			x += s * rx
			y += s * ry
			t >>= 2
		}
		return z, x, y, nil
	}
	return 0, 0, 0, fmt.Errorf("tileID %d exceeds max supported zoom %d", tileID, MaxSupportedZoom)
}

func isVerraLandTile(z uint8, x, y uint32) bool {
	if z == 0 {
		return true
	}
	n := uint32(1) << z
	margin := n / 4
	if margin == 0 {
		margin = 1
	}
	return x >= margin && x < n-margin && y >= margin && y < n-margin && ((x+y)&3) != 0
}

func encodeSyntheticMVT(z uint8, x, y uint32) ([]byte, error) {
	layerName := "verra_ocean"
	if isVerraLandTile(z, x, y) {
		layerName = "verra_land"
	}
	// Minimal deterministic Mapbox Vector Tile v2.1 wire frame with single polygon feature.
	var feat bytes.Buffer
	feat.Write([]byte{0x08, byte((z & 0x0F) + 1), 0x18, 0x03})
	geom := []uint32{9, 0, 0, 26, 8192, 0, 0, 8192, 8191, 0, 15}
	var geomBuf bytes.Buffer
	for _, g := range geom {
		writeVarint(&geomBuf, uint64(g))
	}
	feat.WriteByte(0x22)
	writeVarint(&feat, uint64(geomBuf.Len()))
	feat.Write(geomBuf.Bytes())

	var layer bytes.Buffer
	layer.Write([]byte{0x78, 0x02, 0x0A})
	writeVarint(&layer, uint64(len(layerName)))
	layer.WriteString(layerName)
	layer.WriteByte(0x12)
	writeVarint(&layer, uint64(feat.Len()))
	layer.Write(feat.Bytes())
	layer.Write([]byte{0x28, 0x80, 0x20})

	var tile bytes.Buffer
	tile.WriteByte(0x1A)
	writeVarint(&tile, uint64(layer.Len()))
	tile.Write(layer.Bytes())
	return gzipCompress(tile.Bytes())
}

func encodeSyntheticPNG(z uint8, x, y uint32) ([]byte, error) {
	const dim = 32
	img := image.NewNRGBA(image.Rect(0, 0, dim, dim))
	land := isVerraLandTile(z, x, y)
	seed := (uint32(z) * 73856093) ^ (x * 19349663) ^ (y * 83492791)
	for py := 0; py < dim; py++ {
		for px := 0; px < dim; px++ {
			var c color.NRGBA
			if !land {
				c = color.NRGBA{R: 12, G: 38, B: 68, A: 255}
			} else {
				h := seed ^ uint32(px*374761) ^ uint32(py*668265)
				c = color.NRGBA{
					R: uint8(44 + (h & 0x3F)),
					G: uint8(92 + ((h >> 8) & 0x3F)),
					B: uint8(38 + ((h >> 16) & 0x1F)),
					A: 255,
				}
			}
			img.SetNRGBA(px, py, c)
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func BuildSyntheticPyramid(maxZoom uint8, tileType uint8) ([]InputTile, error) {
	if maxZoom > MaxSupportedZoom {
		return nil, fmt.Errorf("maxZoom %d exceeds limit %d", maxZoom, MaxSupportedZoom)
	}
	total := int(((uint64(1) << (2 * uint(maxZoom+1))) - 1) / 3)
	tiles := make([]InputTile, 0, total)
	oceanCache := make(map[uint8][]byte)

	for z := uint8(0); z <= maxZoom; z++ {
		dim := uint32(1) << z
		for y := uint32(0); y < dim; y++ {
			for x := uint32(0); x < dim; x++ {
				id, err := ZxyToTileId(z, x, y)
				if err != nil {
					return nil, err
				}
				payload, err := buildTilePayload(z, x, y, tileType, oceanCache)
				if err != nil {
					return nil, err
				}
				tiles = append(tiles, InputTile{Z: z, X: x, Y: y, TileID: id, Payload: payload})
			}
		}
	}
	sort.Slice(tiles, func(i, j int) bool { return tiles[i].TileID < tiles[j].TileID })
	return tiles, nil
}

func buildTilePayload(z uint8, x, y uint32, tileType uint8, oceanCache map[uint8][]byte) ([]byte, error) {
	if !isVerraLandTile(z, x, y) {
		if cached, ok := oceanCache[tileType]; ok {
			return cached, nil
		}
	}
	var payload []byte
	var err error
	if tileType == TileTypePNG {
		payload, err = encodeSyntheticPNG(z, x, y)
	} else {
		payload, err = encodeSyntheticMVT(z, x, y)
	}
	if err == nil && !isVerraLandTile(z, x, y) {
		oceanCache[tileType] = payload
	}
	return payload, err
}

func coalesceDirectory(tiles []InputTile) ([]DirectoryEntry, []byte, uint64) {
	type sliceLoc struct {
		offset uint64
		length uint32
	}
	dedup := make(map[string]sliceLoc)
	entries := make([]DirectoryEntry, 0, len(tiles))
	var dataBuf bytes.Buffer

	for _, t := range tiles {
		sum := sha256.Sum256(t.Payload)
		key := hex.EncodeToString(sum[:])
		loc, exists := dedup[key]
		if !exists {
			loc = sliceLoc{offset: uint64(dataBuf.Len()), length: uint32(len(t.Payload))}
			dataBuf.Write(t.Payload)
			dedup[key] = loc
		}
		n := len(entries)
		if n > 0 && entries[n-1].TileID+uint64(entries[n-1].RunLength) == t.TileID &&
			entries[n-1].Offset == loc.offset && entries[n-1].Length == loc.length {
			entries[n-1].RunLength++
		} else {
			entries = append(entries, DirectoryEntry{
				TileID:    t.TileID,
				Offset:    loc.offset,
				Length:    loc.length,
				RunLength: 1,
			})
		}
	}
	return entries, dataBuf.Bytes(), uint64(len(dedup))
}

func SerializeDirectory(entries []DirectoryEntry) ([]byte, error) {
	if len(entries) == 0 {
		return nil, errors.New("cannot serialize empty PMTiles directory")
	}
	var raw bytes.Buffer
	writeVarint(&raw, uint64(len(entries)))
	var lastID uint64
	for i, e := range entries {
		if e.RunLength == 0 || e.Length == 0 || (i > 0 && e.TileID < lastID) {
			return nil, errors.New("unclustered or empty directory entry")
		}
		prevID := uint64(0)
		if i > 0 {
			prevID = entries[i-1].TileID
		}
		writeVarint(&raw, e.TileID-prevID)
		lastID = e.TileID + uint64(e.RunLength)
	}
	for _, e := range entries {
		writeVarint(&raw, uint64(e.RunLength))
	}
	for _, e := range entries {
		writeVarint(&raw, uint64(e.Length))
	}
	for i, e := range entries {
		if i > 0 && e.Offset == entries[i-1].Offset+uint64(entries[i-1].Length) {
			writeVarint(&raw, 0)
		} else {
			writeVarint(&raw, e.Offset+1)
		}
	}
	return gzipCompress(raw.Bytes())
}

func EncodeHeader(h Header) [HeaderSize]byte {
	var dst [HeaderSize]byte
	copy(dst[0:8], magicV3[:])
	u64Fields := [11]uint64{
		h.RootDirOffset, h.RootDirBytes, h.JsonMetadataOffset, h.JsonMetadataBytes,
		h.LeafDirsOffset, h.LeafDirsBytes, h.TileDataOffset, h.TileDataBytes,
		h.AddressedTilesCount, h.TileEntriesCount, h.TileContentsCount,
	}
	for i, v := range u64Fields {
		binary.LittleEndian.PutUint64(dst[8+i*8:16+i*8], v)
	}
	dst[96] = h.Clustered
	dst[97] = h.InternalCompression
	dst[98] = h.TileCompression
	dst[99] = h.TileType
	dst[100] = h.MinZoom
	dst[101] = h.MaxZoom
	binary.LittleEndian.PutUint32(dst[102:106], uint32(h.MinLonE7))
	binary.LittleEndian.PutUint32(dst[106:110], uint32(h.MinLatE7))
	binary.LittleEndian.PutUint32(dst[110:114], uint32(h.MaxLonE7))
	binary.LittleEndian.PutUint32(dst[114:118], uint32(h.MaxLatE7))
	dst[118] = h.CenterZoom
	binary.LittleEndian.PutUint32(dst[119:123], uint32(h.CenterLonE7))
	binary.LittleEndian.PutUint32(dst[123:127], uint32(h.CenterLatE7))
	return dst
}

func DecodeAndValidateHeader(archive []byte) (Header, error) {
	if len(archive) < HeaderSize || !bytes.Equal(archive[0:8], magicV3[:]) {
		return Header{}, errors.New("invalid PMTiles v3 magic or truncated header")
	}
	u64 := func(off int) uint64 { return binary.LittleEndian.Uint64(archive[off : off+8]) }
	i32 := func(off int) int32 { return int32(binary.LittleEndian.Uint32(archive[off : off+4])) }
	h := Header{
		RootDirOffset: u64(8), RootDirBytes: u64(16),
		JsonMetadataOffset: u64(24), JsonMetadataBytes: u64(32),
		LeafDirsOffset: u64(40), LeafDirsBytes: u64(48),
		TileDataOffset: u64(56), TileDataBytes: u64(64),
		AddressedTilesCount: u64(72), TileEntriesCount: u64(80), TileContentsCount: u64(88),
		Clustered: archive[96], InternalCompression: archive[97],
		TileCompression: archive[98], TileType: archive[99],
		MinZoom: archive[100], MaxZoom: archive[101],
		MinLonE7: i32(102), MinLatE7: i32(106), MaxLonE7: i32(110), MaxLatE7: i32(114),
		CenterZoom: archive[118], CenterLonE7: i32(119), CenterLatE7: i32(123),
	}
	total := uint64(len(archive))
	if h.Clustered != ClusteredHilbert || h.InternalCompression != CompressionGzip || h.MinZoom > h.MaxZoom ||
		h.RootDirOffset+h.RootDirBytes > total || h.TileDataOffset+h.TileDataBytes > total {
		return Header{}, errors.New("PMTiles v3 header invariants or slice bounds violated")
	}
	return h, nil
}

func WriteArchive(w io.Writer, maxZoom uint8, tileType uint8) (Header, error) {
	tiles, err := BuildSyntheticPyramid(maxZoom, tileType)
	if err != nil {
		return Header{}, err
	}
	entries, tileData, contentsCount := coalesceDirectory(tiles)
	rootDirGz, err := SerializeDirectory(entries)
	if err != nil {
		return Header{}, err
	}
	metaJSON := `{"name":"verra","description":"SandalCore synthetic Verra PMTiles v3","version":"1.0.0"}`
	metaGz, err := gzipCompress([]byte(metaJSON))
	if err != nil {
		return Header{}, err
	}
	tileComp := CompressionGzip
	if tileType == TileTypePNG {
		tileComp = CompressionNone
	}
	rootOff := uint64(HeaderSize)
	metaOff := rootOff + uint64(len(rootDirGz))
	dataOff := metaOff + uint64(len(metaGz))
	hdr := Header{
		RootDirOffset: rootOff, RootDirBytes: uint64(len(rootDirGz)),
		JsonMetadataOffset: metaOff, JsonMetadataBytes: uint64(len(metaGz)),
		LeafDirsOffset: dataOff, LeafDirsBytes: 0,
		TileDataOffset: dataOff, TileDataBytes: uint64(len(tileData)),
		AddressedTilesCount: uint64(len(tiles)),
		TileEntriesCount:    uint64(len(entries)),
		TileContentsCount:   contentsCount,
		Clustered:           ClusteredHilbert,
		InternalCompression: CompressionGzip,
		TileCompression:     tileComp,
		TileType:            tileType,
		MinZoom:             0,
		MaxZoom:             maxZoom,
		MinLonE7:            VerraMinLonE7,
		MinLatE7:            VerraMinLatE7,
		MaxLonE7:            VerraMaxLonE7,
		MaxLatE7:            VerraMaxLatE7,
		CenterZoom:          maxZoom / 2,
	}
	hdrBytes := EncodeHeader(hdr)
	if _, err := w.Write(hdrBytes[:]); err != nil {
		return Header{}, err
	}
	if _, err := w.Write(rootDirGz); err != nil {
		return Header{}, err
	}
	if _, err := w.Write(metaGz); err != nil {
		return Header{}, err
	}
	if _, err := w.Write(tileData); err != nil {
		return Header{}, err
	}
	return hdr, nil
}

func writeVarint(buf *bytes.Buffer, v uint64) {
	for v >= 0x80 {
		buf.WriteByte(byte((v & 0x7F) | 0x80))
		v >>= 7
	}
	buf.WriteByte(byte(v))
}

func gzipCompress(raw []byte) ([]byte, error) {
	var buf bytes.Buffer
	gz, err := gzip.NewWriterLevel(&buf, gzip.BestCompression)
	if err != nil {
		return nil, err
	}
	if _, err := gz.Write(raw); err != nil {
		_ = gz.Close()
		return nil, err
	}
	if err := gz.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func runSelfTest() error {
	for z := uint8(0); z <= 4; z++ {
		n := uint32(1) << z
		for y := uint32(0); y < n; y++ {
			for x := uint32(0); x < n; x++ {
				id, err := ZxyToTileId(z, x, y)
				if err != nil {
					return err
				}
				rz, rx, ry, err := TileIdToZxy(id)
				if err != nil || rz != z || rx != x || ry != y {
					return fmt.Errorf("hilbert bijection mismatch at (%d,%d,%d)", z, x, y)
				}
			}
		}
	}
	for _, tt := range []uint8{TileTypeMVT, TileTypePNG} {
		var buf bytes.Buffer
		written, err := WriteArchive(&buf, 3, tt)
		if err != nil {
			return err
		}
		parsed, err := DecodeAndValidateHeader(buf.Bytes())
		if err != nil || parsed != written || parsed.AddressedTilesCount != 85 {
			return fmt.Errorf("archive self-test failed for tileType=%d", tt)
		}
	}
	return nil
}

func main() {
	var outPath string
	var zoomMax int
	var tileFormat string
	var jsonOut, dryRun, nonInteractive, selfTest bool

	flag.StringVar(&outPath, "output", "verra.pmtiles", "output PMTiles v3 archive path")
	flag.StringVar(&outPath, "out", "verra.pmtiles", "alias for --output")
	flag.IntVar(&zoomMax, "zoom-max", 4, "maximum zoom level (0..8)")
	flag.IntVar(&zoomMax, "max-zoom", 4, "alias for --zoom-max")
	flag.StringVar(&tileFormat, "format", "mvt", "tile payload format: mvt or png")
	flag.BoolVar(&jsonOut, "json", false, "emit machine-readable JSON summary")
	flag.BoolVar(&dryRun, "dry-run", false, "validate archive generation in memory without writing disk file")
	flag.BoolVar(&nonInteractive, "non-interactive", true, "run without interactive prompts")
	flag.BoolVar(&selfTest, "self-test", false, "run Hilbert and PMTiles v3 self-verification suite")
	flag.Parse()
	_ = nonInteractive

	if selfTest {
		if err := runSelfTest(); err != nil {
			fmt.Fprintf(os.Stderr, "what: pmtiles-gen self-test failed\nwhy: %v\nhow-to-fix: verify Hilbert curve and 127-byte header invariants\n", err)
			os.Exit(1)
		}
	}
	if zoomMax < 0 || zoomMax > int(MaxSupportedZoom) {
		fmt.Fprintf(os.Stderr, "what: invalid --zoom-max %d\nwhy: zoom level must be in [0, %d]\nhow-to-fix: pass --zoom-max 4\n", zoomMax, MaxSupportedZoom)
		os.Exit(2)
	}
	tileType := TileTypeMVT
	if tileFormat == "png" {
		tileType = TileTypePNG
	} else if tileFormat != "mvt" {
		fmt.Fprintf(os.Stderr, "what: unsupported --format %q\nwhy: only 'mvt' and 'png' are supported\nhow-to-fix: pass --format mvt or --format png\n", tileFormat)
		os.Exit(2)
	}

	var mem bytes.Buffer
	hdr, err := WriteArchive(&mem, uint8(zoomMax), tileType)
	if err != nil {
		fmt.Fprintf(os.Stderr, "what: failed to build PMTiles archive\nwhy: %v\nhow-to-fix: inspect tile pyramid inputs\n", err)
		os.Exit(1)
	}
	if _, err := DecodeAndValidateHeader(mem.Bytes()); err != nil {
		fmt.Fprintf(os.Stderr, "what: generated archive failed header validation\nwhy: %v\nhow-to-fix: inspect 127-byte PMTiles v3 header layout\n", err)
		os.Exit(1)
	}
	if !dryRun {
		if dir := filepath.Dir(outPath); dir != "" && dir != "." {
			if err := os.MkdirAll(dir, 0o755); err != nil {
				fmt.Fprintf(os.Stderr, "what: failed to create output directory %q\nwhy: %v\nhow-to-fix: check filesystem permissions\n", dir, err)
				os.Exit(1)
			}
		}
		if err := os.WriteFile(outPath, mem.Bytes(), 0o644); err != nil {
			fmt.Fprintf(os.Stderr, "what: failed to write %q\nwhy: %v\nhow-to-fix: verify output path is writable\n", outPath, err)
			os.Exit(1)
		}
	}
	if jsonOut {
		_ = json.NewEncoder(os.Stdout).Encode(map[string]any{
			"output":  outPath,
			"bytes":   mem.Len(),
			"dry_run": dryRun,
			"header":  hdr,
		})
		return
	}
	fmt.Printf("[pmtiles-gen] wrote %s (%d bytes, z=0..%d, addressed=%d, entries=%d, contents=%d)\n",
		outPath, mem.Len(), hdr.MaxZoom, hdr.AddressedTilesCount, hdr.TileEntriesCount, hdr.TileContentsCount)
}
