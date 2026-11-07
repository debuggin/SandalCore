// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package go_test

import (
	"bytes"
	"compress/gzip"
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"github.com/quic-go/qpack"
	"golang.org/x/net/http2"
	"golang.org/x/net/http2/hpack"
)

const (
	streamLayerPlayers  uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_PLAYERS)
	streamLayerNpcs     uint32 = uint32(worldadminv1.StreamLayer_STREAM_LAYER_NPCS)
	streamLayerPois     uint32 = 32
	streamLayerMonsters uint32 = 64
)

var (
	ephemBinOnce        sync.Once
	ephemBinPath        string
	ephemBinErr         error
	webAdminEffSeq      uint64
	activeAdminBearerMu sync.RWMutex
	activeAdminBearer   string
	spawnedTestPlayers  sync.Map
)

var effWebAdminWords64 = [64]string{
	"Grueling", "Biscuit", "Launcher", "Amber", "Falcon", "Lantern", "Cobalt", "Badger",
	"Harbor", "Acorn", "Alder", "Anvil", "Apex", "Arch", "Aspen", "Atlas",
	"Bison", "Blade", "Blaze", "Briar", "Brook", "Cedar", "Cliff", "Cloud",
	"Comet", "Coral", "Crane", "Crest", "Crown", "Delta", "Drift", "Dune",
	"Eagle", "Ember", "Fable", "Fern", "Finch", "Fjord", "Flame", "Flint",
	"Forge", "Frost", "Glade", "Gleam", "Grove", "Haven", "Hawk", "Heron",
	"Hound", "Ivory", "Jade", "Larch", "Lotus", "Maple", "Marsh", "Mesa",
	"Mirth", "Nexus", "Noble", "Oaken", "Onyx", "Orbit", "Otter", "Pearl",
}

func nextWebAdminEffCharacterName() string {
	idx := atomic.AddUint64(&webAdminEffSeq, 1) - 1
	if idx < 3 {
		name := "TST " + effWebAdminWords64[idx*3] + effWebAdminWords64[idx*3+1] + effWebAdminWords64[idx*3+2]
		spawnedTestPlayers.Store(name, true)
		return name
	}
	var b [3]byte
	_, _ = rand.Read(b[:])
	name := fmt.Sprintf("TST %s%s%s", effWebAdminWords64[(int(b[0])+int(idx))%64], effWebAdminWords64[(int(b[1])+int(idx)*7)%64], effWebAdminWords64[(int(b[2])+int(idx)*13)%64])
	spawnedTestPlayers.Store(name, true)
	return name
}

func setActiveAdminBearer(tok string) {
	activeAdminBearerMu.Lock()
	activeAdminBearer = strings.TrimSpace(tok)
	activeAdminBearerMu.Unlock()
}

func getActiveAdminBearer(fallback string) string {
	activeAdminBearerMu.RLock()
	tok := activeAdminBearer
	activeAdminBearerMu.RUnlock()
	if tok != "" {
		return tok
	}
	return fallback
}

func webAdminRepoRoot(t *testing.T) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}
	return filepath.Clean(filepath.Join(filepath.Dir(file), "..", "..", ".."))
}

func resolveAdminPassphrase(t *testing.T) string {
	t.Helper()
	for _, k := range []string{"AOC_ADMIN_PASSPHRASE", "SANDAL_ADMIN_PASSPHRASE"} {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" {
			return v
		}
	}
	root := webAdminRepoRoot(t)
	if raw, err := os.ReadFile(filepath.Join(root, "deploy", ".admin_passphrase")); err == nil && strings.TrimSpace(string(raw)) != "" {
		return strings.TrimSpace(string(raw))
	}
	if raw, err := os.ReadFile(filepath.Join(root, "deploy", ".env")); err == nil {
		for _, line := range strings.Split(string(raw), "\n") {
			if k, v, ok := strings.Cut(strings.TrimSpace(line), "="); ok && strings.TrimSpace(k) == "AOC_ADMIN_PASSPHRASE" {
				if clean := strings.Trim(strings.TrimSpace(v), `"'`); clean != "" {
					return clean
				}
			}
		}
	}
	return "sandal-admin-dev-passphrase"
}

func ensureEphemeralAdminGateway(t *testing.T) string {
	t.Helper()
	root := webAdminRepoRoot(t)
	ephemBinOnce.Do(func() {
		ephemBinPath = filepath.Join(os.TempDir(), "sandal-test-admin-gateway.exe")
		if runtime.GOOS != "windows" {
			ephemBinPath = filepath.Join(os.TempDir(), "sandal-test-admin-gateway")
		}
		buildCmd := exec.Command("go", "build", "-o", ephemBinPath, "./cmd/admin-gateway")
		buildCmd.Dir = filepath.Join(root, "src", "server", "services")
		if out, err := buildCmd.CombinedOutput(); err != nil {
			ephemBinErr = fmt.Errorf("build admin-gateway: %v: %s", err, string(out))
		}
	})
	if ephemBinErr != nil {
		t.Fatalf("ensureEphemeralAdminGateway build failed: %v", ephemBinErr)
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen 127.0.0.1:0: %v", err)
	}
	addr := ln.Addr().String()
	_ = ln.Close()
	cmd := exec.Command(ephemBinPath)
	cmd.Dir = root
	cmd.Env = append(os.Environ(),
		"AOC_ADMIN_HTTP_BIND_ADDR="+addr,
		"AOC_ADMIN_PASSPHRASE="+resolveAdminPassphrase(t),
		"AOC_PMTILES_PATH="+filepath.Join(root, "dist", "content", "verra.pmtiles"),
		"AOC_CONTENT_DB_PATH="+filepath.Join(root, "dist", "content", "content.db"),
	)
	if err := cmd.Start(); err != nil {
		t.Fatalf("start admin-gateway: %v", err)
	}
	t.Cleanup(func() {
		if cmd.Process != nil {
			_ = cmd.Process.Kill()
			_, _ = cmd.Process.Wait()
		}
	})
	baseURL := "http://" + addr
	waitForAdminGatewayReady(baseURL)
	return baseURL
}

func waitForAdminGatewayReady(baseURL string) {
	cli := &http.Client{Timeout: 500 * time.Millisecond}
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if resp, err := cli.Get(baseURL + "/livez"); err == nil {
			_, _ = io.Copy(io.Discard, resp.Body)
			_ = resp.Body.Close()
			if resp.StatusCode == http.StatusOK {
				return
			}
		}
		// Poll briefly until the ephemeral admin-gateway TCP listener accepts connections.
		time.Sleep(50 * time.Millisecond)
	}
}

func TestWebAdminStaticContractAndNamingHygiene(t *testing.T) {
	root := webAdminRepoRoot(t)
	for _, rel := range []string{
		"src/server/services/internal/admin-gateway/router.go",
		"src/server/services/internal/admin-catalog/store.go",
		"src/server/services/internal/world-stream/coalescer.go",
		"src/server/services/cmd/admin-gateway/main.go",
	} {
		if st, err := os.Stat(filepath.Join(root, filepath.FromSlash(rel))); err != nil || st.IsDir() || st.Size() == 0 {
			t.Fatalf("expected required kebab-case source file %s (err=%v)", rel, err)
		}
	}
	for _, rel := range []string{
		"src/server/services/internal/admingw",
		"src/server/services/internal/admingateway",
		"src/server/services/internal/admincatalog",
		"src/server/services/internal/worldstream",
	} {
		if _, err := os.Stat(filepath.Join(root, filepath.FromSlash(rel))); err == nil {
			t.Fatalf("forbidden shorthand/non-kebab directory exists: %s", rel)
		}
	}
}

func TestWebAdminOpenAPIAndPaginationSortingFieldsets(t *testing.T) {
	baseURL := ensureEphemeralAdminGateway(t)
	verifyLiveAdminAuthLoginAndSession(t, baseURL)
	code, _, body := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/gm/openapi.json", nil, nil)
	var oaDoc map[string]any
	if code != http.StatusOK || json.Unmarshal(body, &oaDoc) != nil || oaDoc["openapi"] != "3.1.0" {
		t.Fatalf("GET /api/v1/gm/openapi.json status=%d openapi=%v", code, oaDoc["openapi"])
	}
	paths, _ := oaDoc["paths"].(map[string]any)
	for _, p := range []string{
		"/api/v1/world/map-meta", "/api/v1/world/tiles/{z}/{x}/{y}", "/api/v1/world/pmtiles",
		"/api/v1/catalog/items", "/api/v1/catalog/npcs", "/api/v1/catalog/places",
		"/api/v1/gm/commands", "/api/v1/gm/containers", "/api/v1/gm/logs",
	} {
		if paths[p] == nil {
			t.Fatalf("openapi.json missing path %s", p)
		}
	}
	assertCatalogPaginationAndLinks(t, baseURL, "/api/v1/catalog/items?q=a&page=1&page_size=5&sort=-rarity,name&fields=guid,name,rarity", true)
	assertCatalogPaginationAndLinks(t, baseURL, "/api/v1/catalog/npcs?page=1&page_size=5&sort=-level,name", false)
	assertCatalogPaginationAndLinks(t, baseURL, "/api/v1/catalog/places?page=1&page_size=5&sort=name", false)
}

func assertCatalogPaginationAndLinks(t *testing.T, baseURL, path string, checkProjection bool) {
	t.Helper()
	code, hdr, body := doAdminHTTP(t, http.MethodGet, baseURL+path, nil, nil)
	var xPag, env map[string]any
	if code != http.StatusOK || json.Unmarshal([]byte(hdr.Get("X-Pagination")), &xPag) != nil || int(xPag["page"].(float64)) != 1 || int(xPag["page_size"].(float64)) != 5 {
		t.Fatalf("GET %s status=%d X-Pagination=%q", path, code, hdr.Get("X-Pagination"))
	}
	if err := json.Unmarshal(body, &env); err != nil || int(env["page"].(float64)) != 1 || int(env["page_size"].(float64)) != 5 {
		t.Fatalf("GET %s invalid pagination envelope: err=%v env=%#v", path, err, env)
	}
	links, _ := env["_links"].(map[string]any)
	if links["self"] == nil || links["first"] == nil || links["last"] == nil || (env["has_more"] == true && links["next"] == nil) {
		t.Fatalf("GET %s missing HATEOAS _links: %#v", path, links)
	}
	if checkProjection {
		if items, _ := env["items"].([]any); len(items) > 0 {
			first, _ := items[0].(map[string]any)
			if _, hasSlot := first["equip_slot"]; hasSlot || first["name"] == nil {
				t.Fatalf("sparse fieldset projection failed on %s: %#v", path, first)
			}
		}
	}
}

func TestWebAdminConditionalCachingAndWriteThroughInvalidation(t *testing.T) {
	baseURL := ensureEphemeralAdminGateway(t)
	verifyLiveAdminAuthLoginAndSession(t, baseURL)
	futureIMS := time.Now().UTC().Add(24 * time.Hour).Format(http.TimeFormat)
	for _, path := range []string{"/api/v1/catalog/places", "/api/v1/world/map-meta"} {
		code1, hdr1, _ := doAdminHTTP(t, http.MethodGet, baseURL+path, nil, nil)
		etag1, lastMod := hdr1.Get("ETag"), hdr1.Get("Last-Modified")
		if code1 != http.StatusOK || !strings.HasPrefix(etag1, `"sha256-`) || lastMod == "" || hdr1.Get("Cache-Control") == "" || hdr1.Get("Vary") != "Accept-Encoding" {
			t.Fatalf("GET %s headers invalid: code=%d etag=%q lastMod=%q vary=%q", path, code1, etag1, lastMod, hdr1.Get("Vary"))
		}
		if c304, _, b304 := doAdminHTTP(t, http.MethodGet, baseURL+path, map[string]string{"If-None-Match": etag1}, nil); c304 != http.StatusNotModified || len(b304) != 0 {
			t.Fatalf("GET %s If-None-Match status=%d bodyLen=%d, want 304", path, c304, len(b304))
		}
		if cIMS, _, bIMS := doAdminHTTP(t, http.MethodGet, baseURL+path, map[string]string{"If-Modified-Since": futureIMS}, nil); cIMS != http.StatusNotModified || len(bIMS) != 0 {
			t.Fatalf("GET %s If-Modified-Since status=%d bodyLen=%d, want 304", path, cIMS, len(bIMS))
		}
	}
	_, hdrBefore, _ := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/map-meta", nil, nil)
	metaBefore := hdrBefore.Get("ETag")
	if putCode, _, putBody := doAdminHTTP(t, http.MethodPut, baseURL+"/api/v1/world/nodes/Winstead", map[string]string{"Content-Type": "application/json"}, []byte(`{"level":5}`)); putCode != http.StatusOK {
		t.Fatalf("PUT /api/v1/world/nodes/Winstead status=%d body=%s", putCode, string(putBody))
	}
	codeAfter, hdrAfter, _ := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/map-meta", map[string]string{"If-None-Match": metaBefore}, nil)
	if codeAfter != http.StatusOK || hdrAfter.Get("ETag") == metaBefore {
		t.Fatalf("expected write-through cache invalidation after node PUT: status=%d oldETag=%q newETag=%q", codeAfter, metaBefore, hdrAfter.Get("ETag"))
	}
}

func TestWebAdminBrotliGzipAndH2CHpackQpack(t *testing.T) {
	envoyRaw, err := os.ReadFile(filepath.Join(webAdminRepoRoot(t), "src", "deploy", "envoy", "envoy.yaml"))
	if err != nil {
		t.Fatalf("read envoy.yaml: %v", err)
	}
	for _, needle := range []string{"hpack_table_size: 65536", "brotli.compressor.v3.Brotli", "gzip.compressor.v3.Gzip"} {
		if !strings.Contains(string(envoyRaw), needle) {
			t.Fatalf("envoy.yaml missing %q", needle)
		}
	}
	baseURL := ensureEphemeralAdminGateway(t)
	verifyLiveAdminAuthLoginAndSession(t, baseURL)
	codeBr, hdrBr, _ := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/gm/openapi.json", map[string]string{"Accept-Encoding": "br"}, nil)
	brETag := hdrBr.Get("ETag")
	if codeBr != http.StatusOK || hdrBr.Get("Content-Encoding") != "br" || hdrBr.Get("Vary") != "Accept-Encoding" || !strings.HasSuffix(brETag, `-br"`) {
		t.Fatalf("Brotli openapi.json code=%d ce=%q vary=%q etag=%q", codeBr, hdrBr.Get("Content-Encoding"), hdrBr.Get("Vary"), brETag)
	}
	if c304, _, _ := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/gm/openapi.json", map[string]string{"Accept-Encoding": "br", "If-None-Match": brETag}, nil); c304 != http.StatusNotModified {
		t.Fatalf("Brotli If-None-Match status=%d, want 304", c304)
	}
	codeGz, hdrGz, bodyGz := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/gm/openapi.json", map[string]string{"Accept-Encoding": "gzip"}, nil)
	gzETag := hdrGz.Get("ETag")
	if codeGz != http.StatusOK || hdrGz.Get("Content-Encoding") != "gzip" || !strings.HasSuffix(gzETag, `-gzip"`) {
		t.Fatalf("Gzip openapi.json code=%d ce=%q etag=%q", codeGz, hdrGz.Get("Content-Encoding"), gzETag)
	}
	zr, err := gzip.NewReader(bytes.NewReader(bodyGz))
	if err != nil {
		t.Fatalf("gzip.NewReader: %v", err)
	}
	unzipped, _ := io.ReadAll(zr)
	_ = zr.Close()
	if !strings.Contains(string(unzipped), `"3.1.0"`) {
		t.Fatalf("unzipped openapi.json missing 3.1.0: %s", string(unzipped))
	}
	if c304, _, _ := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/gm/openapi.json", map[string]string{"Accept-Encoding": "gzip", "If-None-Match": gzETag}, nil); c304 != http.StatusNotModified {
		t.Fatalf("Gzip If-None-Match status=%d, want 304", c304)
	}
	verifyH2CAndHeaderCompression(t, baseURL, brETag)
}

func verifyH2CAndHeaderCompression(t *testing.T, baseURL, wantETag string) {
	t.Helper()
	var hpackBuf, qpackBuf bytes.Buffer
	_ = hpack.NewEncoder(&hpackBuf).WriteField(hpack.HeaderField{Name: "etag", Value: wantETag})
	if fields, err := hpack.NewDecoder(4096, nil).DecodeFull(hpackBuf.Bytes()); err != nil || len(fields) != 1 || fields[0].Value != wantETag {
		t.Fatalf("HPACK round-trip failed: err=%v fields=%v", err, fields)
	}
	qpEnc := qpack.NewEncoder(&qpackBuf)
	_ = qpEnc.WriteField(qpack.HeaderField{Name: "etag", Value: wantETag})
	_ = qpEnc.Close()
	if qf, err := qpack.NewDecoder().Decode(qpackBuf.Bytes())(); err != nil || qf.Value != wantETag {
		t.Fatalf("QPACK round-trip failed: err=%v field=%v", err, qf)
	}
	h2cClient := &http.Client{
		Timeout: 5 * time.Second,
		Transport: &http2.Transport{
			AllowHTTP: true,
			DialTLSContext: func(ctx context.Context, network, addr string, _ *tls.Config) (net.Conn, error) {
				var d net.Dialer
				return d.DialContext(ctx, network, addr)
			},
		},
	}
	req, _ := http.NewRequest(http.MethodGet, baseURL+"/api/v1/gm/openapi.json", nil)
	req.Header.Set("Authorization", "Bearer "+getActiveAdminBearer("gm-architect-dev-token"))
	resp, err := h2cClient.Do(req)
	if err != nil {
		t.Fatalf("h2c GET openapi.json: %v", err)
	}
	_, _ = io.Copy(io.Discard, resp.Body)
	_ = resp.Body.Close()
	if resp.StatusCode != http.StatusOK || resp.ProtoMajor != 2 {
		t.Fatalf("h2c status=%d proto=%s, want 200 HTTP/2", resp.StatusCode, resp.Proto)
	}
}

func TestWebAdminPmTilesAndStreamLayerBitmasks(t *testing.T) {
	pmPath := filepath.Join(webAdminRepoRoot(t), "dist", "content", "verra.pmtiles")
	if st, err := os.Stat(pmPath); err != nil || st.Size() <= 100<<20 {
		t.Fatalf("expected dist/content/verra.pmtiles > 100 MB, got stat=%v err=%v", st, err)
	}
	baseURL := ensureEphemeralAdminGateway(t)
	verifyLiveAdminAuthLoginAndSession(t, baseURL)
	if codeMeta, _, bodyMeta := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/map-meta", nil, nil); codeMeta != http.StatusOK || !strings.Contains(string(bodyMeta), `"pmtiles_present":true`) {
		t.Fatalf("GET /api/v1/world/map-meta code=%d body=%s", codeMeta, string(bodyMeta))
	}
	if codeRange, _, bodyRange := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/pmtiles", map[string]string{"Range": "bytes=0-126"}, nil); codeRange != http.StatusPartialContent || len(bodyRange) != 127 || string(bodyRange[:8]) != "PMTiles\x03" {
		t.Fatalf("Range /api/v1/world/pmtiles code=%d len=%d magic=%q", codeRange, len(bodyRange), bodyRange[:8])
	}
	if img0 := fetchAndDecodePNG(t, baseURL+"/api/v1/world/tiles/0/0/0"); img0.Bounds().Dx() != 256 || img0.Bounds().Dy() != 256 {
		t.Fatalf("tile 0/0/0 dim=%dx%d", img0.Bounds().Dx(), img0.Bounds().Dy())
	}
	if img8 := fetchAndDecodePNG(t, baseURL+"/api/v1/world/tiles/8/0/0"); img8.Bounds().Dx() != 1024 || img8.Bounds().Dy() != 1024 {
		t.Fatalf("node plug tile 8/0/0 dim=%dx%d", img8.Bounds().Dx(), img8.Bounds().Dy())
	}
	for _, tc := range []struct {
		query                    string
		wantMask, wantStreamMask uint32
	}{
		{"?layer_mask=1", streamLayerPlayers, streamLayerPlayers},
		{"?layer_mask=2", streamLayerNpcs, streamLayerNpcs},
		{"?layer_mask=32", streamLayerPois, 0},
		{"?layer_mask=64", streamLayerMonsters, streamLayerNpcs},
		{"?layers=none", 0, 0},
	} {
		code, _, body := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/state"+tc.query, nil, nil)
		var payload map[string]any
		_ = json.Unmarshal(body, &payload)
		if code != http.StatusOK || uint32(payload["layer_mask"].(float64)) != tc.wantMask || uint32(payload["stream_layers"].(float64)) != tc.wantStreamMask {
			t.Fatalf("%s code=%d layer_mask=%v stream_layers=%v", tc.query, code, payload["layer_mask"], payload["stream_layers"])
		}
	}
}

func TestWebAdminLiveStackAPIsE2E(t *testing.T) { TestLiveWebAdminAPIsE2E(t) }
func TestLiveWebAdminAPIsE2E(t *testing.T) {
	baseURL := resolveLiveOrEphemeralBaseURL(t)
	verifyLiveAdminAuthLoginAndSession(t, baseURL)
	if os.Getenv("SANDAL_REQUIRE_LIVE_WEB_ADMIN") != "1" {
		if c, _, _ := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", map[string]string{"Content-Type": "application/json", "Idempotency-Key": fmt.Sprintf("probe-tr-%d", time.Now().UnixNano())}, []byte(`{"command":"set_tick_rate","tick_hz":20}`)); c != http.StatusOK {
			baseURL = ensureEphemeralAdminGateway(t)
			verifyLiveAdminAuthLoginAndSession(t, baseURL)
		} else if c2, _, _ := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", map[string]string{"Content-Type": "application/json", "Idempotency-Key": fmt.Sprintf("probe-mc-%d", time.Now().UnixNano())}, []byte(`{"command":"give_currency","character":"TST Probe","copper":1}`)); c2 != http.StatusOK {
			baseURL = ensureEphemeralAdminGateway(t)
			verifyLiveAdminAuthLoginAndSession(t, baseURL)
		}
	}
	client := &http.Client{Timeout: 8 * time.Second}
	for _, ep := range []string{
		"/api/v1/gm/openapi.json", "/api/v1/world/map-meta", "/api/v1/world/tiles/0/0/0", "/api/v1/world/tiles/8/0/0",
		"/api/v1/catalog/items?page=1&page_size=5", "/api/v1/catalog/npcs?page=1&page_size=5", "/api/v1/catalog/places",
		"/api/v1/world/state?layer_mask=95", "/api/v1/gm/containers", "/api/v1/gm/logs?container=aoc-world-riverlands&tail=20",
	} {
		probeWebAdminEndpoint(t, client, baseURL+ep)
	}
	verifyLiveBaseMapOceanAndNodePlugs(t, baseURL)
	verifyLiveMetadataPoisAndIcons(t, baseURL)
	verifyLiveWorldStateAndPlayers(t, baseURL)
	verifyLiveContainersAndLogs(t, baseURL)
	verifyLiveCityNodeGovernance(t, baseURL)
	verifyLiveGmWorkItemEndpoints(t, baseURL)
	despawnAndVerifyZeroOnlineTestPlayers(t, baseURL)
}

func verifyLiveAdminAuthLoginAndSession(t *testing.T, baseURL string) {
	t.Helper()
	hdrJSON := map[string]string{"Content-Type": "application/json", "Accept": "application/json"}
	pass := resolveAdminPassphrase(t)
	code, _, raw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/admin/auth/login", hdrJSON, []byte(fmt.Sprintf(`{"username":"admin","passphrase":%q}`, pass)))
	if code == http.StatusUnauthorized && pass != "sandal-admin-dev-passphrase" {
		pass = "sandal-admin-dev-passphrase"
		code, _, raw = doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/admin/auth/login", hdrJSON, []byte(fmt.Sprintf(`{"username":"admin","passphrase":%q}`, pass)))
	}
	var loginDoc map[string]any
	_ = json.Unmarshal(raw, &loginDoc)
	tok, _ := loginDoc["token"].(string)
	bootID, _ := loginDoc["boot_id"].(string)
	if code != http.StatusOK || strings.Count(tok, ".") != 2 || bootID == "" || len(extractSliceField(loginDoc, "scopes")) == 0 {
		t.Fatalf("POST /api/v1/admin/auth/login status=%d tok=%q boot_id=%q body=%s", code, tok, bootID, string(raw))
	}
	setActiveAdminBearer(tok)
	if badLogin, _, _ := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/admin/auth/login", hdrJSON, []byte(`{"username":"admin","passphrase":"wrong-passphrase-invalid"}`)); badLogin != http.StatusUnauthorized {
		t.Fatalf("wrong passphrase login status=%d, want 401", badLogin)
	}
	sCode, _, sRaw := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/admin/auth/session", map[string]string{"Authorization": "Bearer " + tok}, nil)
	var sessDoc map[string]any
	_ = json.Unmarshal(sRaw, &sessDoc)
	if sCode != http.StatusOK || sessDoc["authenticated"] != true || fmt.Sprint(sessDoc["boot_id"]) != bootID || len(extractSliceField(sessDoc, "scopes")) == 0 {
		t.Fatalf("GET /api/v1/admin/auth/session status=%d body=%s", sCode, string(sRaw))
	}
	badHdr := map[string]string{"Authorization": "Bearer " + tok + "corrupt", "Content-Type": "application/json"}
	if c401, _, _ := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/admin/auth/session", badHdr, nil); c401 != http.StatusUnauthorized {
		t.Fatalf("corrupted Bearer token on /api/v1/admin/auth/session status=%d, want 401", c401)
	}
	_, _, obsRaw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/admin/auth/login", hdrJSON, []byte(fmt.Sprintf(`{"username":"admin","passphrase":%q,"role":"OBSERVER","scopes":["admin:read"]}`, pass)))
	var obsDoc map[string]any
	_ = json.Unmarshal(obsRaw, &obsDoc)
	if obsTok, _ := obsDoc["token"].(string); obsTok != "" {
		obsHdr := map[string]string{"Authorization": "Bearer " + obsTok, "Content-Type": "application/json"}
		if c403, _, _ := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", obsHdr, []byte(`{"command":"set_node_level","node":"Winstead","level":3}`)); c403 != http.StatusForbidden {
			t.Fatalf("read-only OBSERVER JWT on /api/v1/gm/commands status=%d, want 403", c403)
		}
	}
}

func resolveLiveOrEphemeralBaseURL(t *testing.T) string {
	t.Helper()
	baseURL := strings.TrimRight(os.Getenv("SANDAL_WEB_ADMIN_BASE_URL"), "/")
	if baseURL == "" {
		baseURL = "http://127.0.0.1:8080"
	}
	cli := &http.Client{Timeout: 2 * time.Second}
	if r1, e1 := cli.Get(baseURL + "/livez"); e1 == nil {
		_ = r1.Body.Close()
		if r1.StatusCode == http.StatusOK {
			if r2, e2 := cli.Get(baseURL + "/api/v1/gm/openapi.json"); e2 == nil {
				_ = r2.Body.Close()
				if r2.StatusCode == http.StatusOK || r2.StatusCode == http.StatusUnauthorized {
					return baseURL
				}
			}
		}
	}
	if os.Getenv("SANDAL_REQUIRE_LIVE_WEB_ADMIN") == "1" {
		t.Fatalf("live Web Admin stack at %s is unreachable while SANDAL_REQUIRE_LIVE_WEB_ADMIN=1", baseURL)
	}
	return ensureEphemeralAdminGateway(t)
}

func verifyLiveBaseMapOceanAndNodePlugs(t *testing.T, baseURL string) {
	t.Helper()
	for _, landPath := range []string{"/api/v1/world/tiles/0/0/0.png", "/api/v1/world/tiles/6/34/24.png", "/api/v1/world/tiles/7/68/49.png"} {
		if img := fetchAndDecodePNG(t, baseURL+landPath); img.Bounds().Dx() != 256 || img.Bounds().Dy() != 256 {
			t.Fatalf("GET %s dim=%dx%d, want 256x256", landPath, img.Bounds().Dx(), img.Bounds().Dy())
		}
	}
	for _, oceanPath := range []string{"/api/v1/world/tiles/6/0/0.png", "/api/v1/world/tiles/7/0/0.png"} {
		img := fetchAndDecodePNG(t, baseURL+oceanPath)
		px := color.NRGBAModel.Convert(img.At(img.Bounds().Dx()/2, img.Bounds().Dy()/2)).(color.NRGBA)
		if px.R != 0 || px.G != 0 || px.B != 0 || px.A != 255 {
			t.Fatalf("GET %s sparse ocean center pixel=%+v, want pure black RGBA(0,0,0,255) (#000000)", oceanPath, px)
		}
	}
	for _, plugPath := range []string{"/api/v1/world/tiles/8/0/0.png", "/api/v1/world/tiles/8/1/1.png", "/api/v1/world/tiles/8/2/2.png", "/api/v1/world/tiles/8/12/3.png", "/api/v1/world/tiles/8/25/4.png"} {
		if img := fetchAndDecodePNG(t, baseURL+plugPath); img.Bounds().Dx() != 1024 || img.Bounds().Dy() != 1024 {
			t.Fatalf("GET %s plug dim=%dx%d, want 1024x1024", plugPath, img.Bounds().Dx(), img.Bounds().Dy())
		}
	}
}

func fetchAndDecodePNG(t *testing.T, fullURL string) image.Image {
	t.Helper()
	code, hdr, body := doAdminHTTP(t, http.MethodGet, fullURL, nil, nil)
	if code != http.StatusOK || !strings.HasPrefix(hdr.Get("Content-Type"), "image/png") {
		t.Fatalf("GET %s status=%d content-type=%q, want 200 image/png", fullURL, code, hdr.Get("Content-Type"))
	}
	img, err := png.Decode(bytes.NewReader(body))
	if err != nil || img.Bounds().Dx() <= 0 || img.Bounds().Dy() <= 0 {
		t.Fatalf("GET %s PNG decode failed: err=%v", fullURL, err)
	}
	return img
}

func verifyLiveMetadataPoisAndIcons(t *testing.T, baseURL string) {
	t.Helper()
	codeMeta, _, bodyMeta := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/tiles/metadata.json", nil, nil)
	var metaDoc map[string]any
	if codeMeta != http.StatusOK || json.Unmarshal(bodyMeta, &metaDoc) != nil {
		t.Fatalf("GET /api/v1/world/tiles/metadata.json status=%d", codeMeta)
	}
	if nodes, icons := extractSliceField(metaDoc, "nodes"), extractSliceField(metaDoc, "icons"); len(nodes) != 26 || len(icons) < 30 {
		t.Fatalf("metadata.json nodes=%d (want 26) icons=%d (want >=30)", len(nodes), len(icons))
	}
	codePl, _, bodyPl := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/catalog/places?limit=300", nil, nil)
	var env map[string]any
	if codePl != http.StatusOK || json.Unmarshal(bodyPl, &env) != nil {
		t.Fatalf("GET /api/v1/catalog/places?limit=300 status=%d", codePl)
	}
	places := extractSliceField(env, "places", "items")
	minPlaces, minIcons := 28, 2
	if os.Getenv("SANDAL_REQUIRE_LIVE_WEB_ADMIN") == "1" {
		minPlaces, minIcons = 150, 3
	}
	if len(places) < minPlaces {
		t.Fatalf("GET /api/v1/catalog/places?limit=300 returned %d places, want >=%d", len(places), minPlaces)
	}
	cats, seenIcon := make(map[string]bool), make(map[string]bool)
	var iconURLs []string
	for _, raw := range places {
		m, _ := raw.(map[string]any)
		iconName, iconURL, tint, cat := fmt.Sprint(m["icon_name"]), fmt.Sprint(m["icon_url"]), fmt.Sprint(m["tint_color"]), fmt.Sprint(m["category"])
		if iconName == "" || iconURL == "" || tint == "" {
			t.Fatalf("place missing icon metadata: %#v", m)
		}
		cats[cat] = true
		if !seenIcon[iconURL] {
			seenIcon[iconURL], iconURLs = true, append(iconURLs, iconURL)
		}
	}
	if len(cats) < 2 || len(iconURLs) < minIcons {
		t.Fatalf("places categories=%d distinctIconURLs=%d", len(cats), len(iconURLs))
	}
	_ = fetchAndDecodePNG(t, baseURL+"/api/v1/world/icons/TUI_Icon_Map_MinorPOI.png")
	for _, u := range iconURLs[:min(len(iconURLs), 3)] {
		_ = fetchAndDecodePNG(t, baseURL+u)
	}
}

func verifyLiveWorldStateAndPlayers(t *testing.T, baseURL string) {
	t.Helper()
	verifyHostileMonsterClassificationAndZois(t, baseURL)
	for _, statePath := range []string{"/api/v1/world/state", "/api/v1/gm/world/state"} {
		code, _, body := doAdminHTTP(t, http.MethodGet, baseURL+statePath, nil, nil)
		var doc map[string]any
		if code != http.StatusOK || json.Unmarshal(body, &doc) != nil {
			t.Fatalf("GET %s status=%d", statePath, code)
		}
		assertWorldStatePayload(t, statePath, doc)
	}
	for _, plPath := range []string{"/api/v1/world/players", "/api/v1/gm/world/players"} {
		code, _, body := doAdminHTTP(t, http.MethodGet, baseURL+plPath, nil, nil)
		lower := strings.ToLower(string(body))
		if code != http.StatusOK || strings.Contains(lower, `"kaelen"`) || strings.Contains(lower, `"lyra"`) {
			t.Fatalf("GET %s status=%d or contains forbidden synthetic fake player Kaelen/Lyra: %s", plPath, code, string(body))
		}
	}
}

func assertWorldStatePayload(t *testing.T, statePath string, doc map[string]any) {
	t.Helper()
	entities := extractSliceField(doc, "entities")
	resNodes := extractSliceField(doc, "resource_nodes")
	counts, _ := doc["layer_counts"].(map[string]any)
	monstersCnt, npcsCnt, poisCnt := intField(counts, "monsters"), intField(counts, "npcs"), intField(counts, "pois")
	resCnt := max(len(resNodes), max(intField(counts, "resources"), intField(counts, "resource_nodes")))
	if os.Getenv("SANDAL_REQUIRE_LIVE_WEB_ADMIN") == "1" {
		if len(entities) < 1000 || monstersCnt <= 0 || npcsCnt <= 0 || len(resNodes) == 0 || resCnt <= 0 || poisCnt < 150 {
			t.Fatalf("GET %s live counts invalid: entities=%d monsters=%d npcs=%d resource_nodes=%d pois=%d", statePath, len(entities), monstersCnt, npcsCnt, len(resNodes), poisCnt)
		}
	}
	for _, raw := range entities {
		ent, _ := raw.(map[string]any)
		if kind, _ := ent["kind"].(string); (kind == "npc" || kind == "monster") && strings.TrimSpace(fmt.Sprint(ent["name"])) == "" {
			t.Fatalf("GET %s contains unnamed blank %s entity: %#v", statePath, kind, ent)
		}
	}
}

func verifyLiveContainersAndLogs(t *testing.T, baseURL string) {
	t.Helper()
	start := time.Now()
	code, _, body := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/gm/containers", nil, nil)
	if elapsed := time.Since(start); code != http.StatusOK || elapsed >= 3*time.Second {
		t.Fatalf("GET /api/v1/gm/containers status=%d elapsed=%v (want 200 in <3.0s)", code, elapsed)
	}
	var doc map[string]any
	if err := json.Unmarshal(body, &doc); err != nil {
		t.Fatalf("unmarshal /api/v1/gm/containers: %v", err)
	}
	containers := extractSliceField(doc, "containers", "items")
	if len(containers) < 11 {
		t.Fatalf("GET /api/v1/gm/containers len=%d, want >=11", len(containers))
	}
	if os.Getenv("SANDAL_REQUIRE_LIVE_WEB_ADMIN") == "1" {
		for _, raw := range containers {
			c, _ := raw.(map[string]any)
			if name, state := fmt.Sprint(c["name"]), fmt.Sprint(c["state"]); strings.HasPrefix(name, "aoc-") && state != "running" {
				t.Fatalf("container %s state=%q, want \"running\"", name, state)
			}
		}
		if ws, _ := doc["world_status"].(map[string]any); ws["connected"] != true || intField(ws, "npcs_total", "npcs") <= 0 {
			t.Fatalf("world_status invalid: %#v", ws)
		}
	}
	codeLogs, _, bodyLogs := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/gm/logs?container=aoc-world-riverlands&tail=30", nil, nil)
	var logsDoc map[string]any
	_ = json.Unmarshal(bodyLogs, &logsDoc)
	if codeLogs != http.StatusOK || (os.Getenv("SANDAL_REQUIRE_LIVE_WEB_ADMIN") == "1" && len(extractSliceField(logsDoc, "items", "lines")) == 0) {
		t.Fatalf("GET /api/v1/gm/logs?container=aoc-world-riverlands&tail=30 code=%d items=%d", codeLogs, len(extractSliceField(logsDoc, "items", "lines")))
	}
}

func verifyLiveCityNodeGovernance(t *testing.T, baseURL string) {
	t.Helper()
	hdrs := map[string]string{"Content-Type": "application/json", "Accept": "application/json"}
	resetNewAela := func() {
		_, _, _ = doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/node/mayor", hdrs, []byte(`{"node_name":"NewAela","mayor_guid":"","mayor_name":""}`))
		_, _, _ = doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/node/election", hdrs, []byte(`{"node_name":"NewAela","action":"cancel"}`))
		_, _, _ = doAdminHTTP(t, http.MethodPut, baseURL+"/api/v1/world/nodes/NewAela", hdrs, []byte(`{"level":3}`))
	}
	t.Cleanup(resetNewAela)
	defer resetNewAela()
	mayorName := nextWebAdminEffCharacterName()
	mayorBody := []byte(fmt.Sprintf(`{"node_name":"NewAela","mayor_guid":"char-9001","mayor_name":%q}`, mayorName))
	if code, _, raw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/node/mayor", hdrs, mayorBody); code != http.StatusOK {
		t.Fatalf("POST /api/v1/gm/node/mayor status=%d body=%s", code, string(raw))
	}
	assertNewAelaGovernanceState(t, baseURL, mayorName, "active_term")
	for _, tr := range []struct{ action, want string }{{"start", "registration"}, {"open_voting", "voting"}, {"conclude", "active_term"}, {"cancel", "none"}} {
		if code, _, raw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/node/election", hdrs, []byte(fmt.Sprintf(`{"node_name":"NewAela","action":%q}`, tr.action))); code != http.StatusOK {
			t.Fatalf("POST /api/v1/gm/node/election (%s) status=%d body=%s", tr.action, code, string(raw))
		}
		assertNewAelaGovernanceState(t, baseURL, "", tr.want)
	}
}

func assertNewAelaGovernanceState(t *testing.T, baseURL, wantMayor, wantElection string) {
	t.Helper()
	code, _, body := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/state", nil, nil)
	var doc map[string]any
	if code != http.StatusOK || json.Unmarshal(body, &doc) != nil {
		t.Fatalf("GET /api/v1/world/state status=%d", code)
	}
	for _, raw := range extractSliceField(doc, "nodes") {
		n, _ := raw.(map[string]any)
		if strings.Contains(strings.ToLower(fmt.Sprint(n["name"])), "newaela") || strings.EqualFold(fmt.Sprint(n["display_name"]), "New Aela") {
			mayor, el := fmt.Sprint(n["mayor_name"]), fmt.Sprint(n["election_status"])
			if (wantMayor != "" && mayor != wantMayor) || el != wantElection {
				t.Fatalf("NewAela governance mayor=%q (want %q) election_status=%q (want %q)", mayor, wantMayor, el, wantElection)
			}
			return
		}
	}
	t.Fatal("NewAela not found in GET /api/v1/world/state nodes")
}

type gmWorkItemSpec struct {
	name   string
	routes []string
	body   string
}

func verifyLiveGmWorkItemEndpoints(t *testing.T, baseURL string) {
	t.Helper()
	charA, charB := nextWebAdminEffCharacterName(), nextWebAdminEffCharacterName()
	workItems := []gmWorkItemSpec{
		{"give_currency", []string{"/api/v1/gm/give_currency", "/api/v1/gm/currency", "/api/v1/gm/commands"}, fmt.Sprintf(`{"command":"give_currency","character":%q,"player_guid":"char-9001","target_player_guid":"char-9001","gold":5,"silver":10,"copper":50,"amount":51050}`, charA)},
		{"give_item", []string{"/api/v1/gm/give_item", "/api/v1/gm/item", "/api/v1/gm/commands"}, fmt.Sprintf(`{"command":"give_item","character":%q,"player_guid":"char-9001","target_player_guid":"char-9001","item":"6064629100001","item_guid":"6064629100001","quantity":1,"count":1,"rarity":"Epic"}`, charA)},
		{"teleport", []string{"/api/v1/gm/teleport", "/api/v1/gm/commands"}, fmt.Sprintf(`{"command":"teleport","character":%q,"player_guid":"char-9001","target_player_guid":"char-9002","target_player":%q,"place":"Winstead","x_cm":-641389,"y_cm":377367.1,"z_cm":12500}`, charA, charB)},
		{"spawn_npc", []string{"/api/v1/gm/spawn_npc", "/api/v1/gm/spawn", "/api/v1/gm/commands"}, `{"command":"spawn_npc","npc":"DesignData_NPC_Goblin","npc_guid":"6064630100001","place":"Winstead","x_cm":-641389,"y_cm":377367.1,"z_cm":12500,"level":10}`},
		{"set_simulate_all_npcs", []string{"/api/v1/gm/commands"}, `{"command":"set_simulate_all_npcs","enabled":true}`},
	}
	for i, wi := range workItems {
		doc := postWorkItemAndAssert(t, baseURL, wi, i+1)
		if wi.name == "spawn_npc" {
			if guid, _ := doc["entity_guid"].(string); guid != "" {
				hdr := map[string]string{"Content-Type": "application/json", "Idempotency-Key": fmt.Sprintf("wi-rm-%d", time.Now().UnixNano())}
				_, _, _ = doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", hdr, []byte(fmt.Sprintf(`{"command":"despawn_entity","entity_guid":%q}`, guid)))
			}
		}
	}
	verifyLiveBorderHandoverHysteresis(t, baseURL)
	verifyLiveSpawnKillDespawnLifecycle(t, baseURL)
	codeAud, _, bodyAud := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/gm/audit", nil, nil)
	var audDoc map[string]any
	_ = json.Unmarshal(bodyAud, &audDoc)
	if codeAud != http.StatusOK || len(extractSliceField(audDoc, "entries", "items")) < len(workItems) {
		t.Fatalf("GET /api/v1/gm/audit code=%d entries=%d, want >=%d", codeAud, len(extractSliceField(audDoc, "entries", "items")), len(workItems))
	}
}