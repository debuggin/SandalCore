// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package go_test

import (
	"bytes"
	"compress/gzip"
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
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
	ephemBinOnce sync.Once
	ephemBinPath string
	ephemBinErr  error
)

func webAdminRepoRoot(t *testing.T) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}
	return filepath.Clean(filepath.Join(filepath.Dir(file), "..", "..", ".."))
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
		resp, err := cli.Get(baseURL + "/livez")
		if err == nil {
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
	requiredFiles := []string{
		"src/server/services/internal/admin-gateway/router.go",
		"src/server/services/internal/admin-catalog/store.go",
		"src/server/services/internal/world-stream/coalescer.go",
		"src/server/services/cmd/admin-gateway/main.go",
	}
	for _, rel := range requiredFiles {
		st, err := os.Stat(filepath.Join(root, filepath.FromSlash(rel)))
		if err != nil || st.IsDir() || st.Size() == 0 {
			t.Fatalf("expected required kebab-case source file %s (err=%v)", rel, err)
		}
	}
	forbiddenDirs := []string{
		"src/server/services/internal/admingw",
		"src/server/services/internal/admingateway",
		"src/server/services/internal/admincatalog",
		"src/server/services/internal/worldstream",
	}
	for _, rel := range forbiddenDirs {
		if _, err := os.Stat(filepath.Join(root, filepath.FromSlash(rel))); err == nil {
			t.Fatalf("forbidden shorthand/non-kebab directory exists: %s", rel)
		}
	}
}

func TestWebAdminOpenAPIAndPaginationSortingFieldsets(t *testing.T) {
	baseURL := ensureEphemeralAdminGateway(t)
	code, _, body := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/gm/openapi.json", nil, nil)
	if code != http.StatusOK {
		t.Fatalf("GET /api/v1/gm/openapi.json status=%d, want 200", code)
	}
	var oaDoc map[string]any
	if err := json.Unmarshal(body, &oaDoc); err != nil || oaDoc["openapi"] != "3.1.0" {
		t.Fatalf("invalid openapi.json: err=%v openapi=%v", err, oaDoc["openapi"])
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
	if code != http.StatusOK {
		t.Fatalf("GET %s status=%d, want 200", path, code)
	}
	xPagRaw := hdr.Get("X-Pagination")
	var xPag map[string]any
	if xPagRaw == "" || json.Unmarshal([]byte(xPagRaw), &xPag) != nil || int(xPag["page"].(float64)) != 1 || int(xPag["page_size"].(float64)) != 5 {
		t.Fatalf("GET %s invalid X-Pagination header: %q", path, xPagRaw)
	}
	var env map[string]any
	if err := json.Unmarshal(body, &env); err != nil {
		t.Fatalf("GET %s invalid JSON: %v", path, err)
	}
	if int(env["page"].(float64)) != 1 || int(env["page_size"].(float64)) != 5 {
		t.Fatalf("GET %s unexpected pagination envelope: page=%v page_size=%v", path, env["page"], env["page_size"])
	}
	links, _ := env["_links"].(map[string]any)
	if links["self"] == nil || links["first"] == nil || links["last"] == nil || (env["has_more"] == true && links["next"] == nil) {
		t.Fatalf("GET %s missing HATEOAS _links: %#v", path, links)
	}
	if checkProjection {
		items, _ := env["items"].([]any)
		if len(items) > 0 {
			first, _ := items[0].(map[string]any)
			if _, hasSlot := first["equip_slot"]; hasSlot || first["name"] == nil {
				t.Fatalf("sparse fieldset projection failed on %s: %#v", path, first)
			}
		}
	}
}

func TestWebAdminConditionalCachingAndWriteThroughInvalidation(t *testing.T) {
	baseURL := ensureEphemeralAdminGateway(t)
	futureIMS := time.Now().UTC().Add(24 * time.Hour).Format(http.TimeFormat)

	for _, path := range []string{"/api/v1/catalog/places", "/api/v1/world/map-meta"} {
		code1, hdr1, _ := doAdminHTTP(t, http.MethodGet, baseURL+path, nil, nil)
		etag1 := hdr1.Get("ETag")
		lastMod := hdr1.Get("Last-Modified")
		if code1 != http.StatusOK || !strings.HasPrefix(etag1, `"sha256-`) || lastMod == "" || hdr1.Get("Cache-Control") == "" || hdr1.Get("Vary") != "Accept-Encoding" {
			t.Fatalf("GET %s headers invalid: code=%d etag=%q lastMod=%q vary=%q", path, code1, etag1, lastMod, hdr1.Get("Vary"))
		}
		code304, _, body304 := doAdminHTTP(t, http.MethodGet, baseURL+path, map[string]string{"If-None-Match": etag1}, nil)
		if code304 != http.StatusNotModified || len(body304) != 0 {
			t.Fatalf("GET %s If-None-Match status=%d bodyLen=%d, want 304", path, code304, len(body304))
		}
		codeIMS, _, bodyIMS := doAdminHTTP(t, http.MethodGet, baseURL+path, map[string]string{"If-Modified-Since": futureIMS}, nil)
		if codeIMS != http.StatusNotModified || len(bodyIMS) != 0 {
			t.Fatalf("GET %s If-Modified-Since status=%d bodyLen=%d, want 304", path, codeIMS, len(bodyIMS))
		}
	}

	_, hdrBefore, _ := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/map-meta", nil, nil)
	metaBefore := hdrBefore.Get("ETag")
	putCode, _, putBody := doAdminHTTP(t, http.MethodPut, baseURL+"/api/v1/world/nodes/Winstead", map[string]string{"Content-Type": "application/json"}, []byte(`{"level":5}`))
	if putCode != http.StatusOK {
		t.Fatalf("PUT /api/v1/world/nodes/Winstead status=%d body=%s", putCode, string(putBody))
	}
	codeAfter, hdrAfter, _ := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/map-meta", map[string]string{"If-None-Match": metaBefore}, nil)
	if codeAfter != http.StatusOK || hdrAfter.Get("ETag") == metaBefore {
		t.Fatalf("expected write-through cache invalidation after node PUT: status=%d oldETag=%q newETag=%q", codeAfter, metaBefore, hdrAfter.Get("ETag"))
	}
}

func TestWebAdminBrotliGzipAndH2CHpackQpack(t *testing.T) {
	root := webAdminRepoRoot(t)
	envoyRaw, err := os.ReadFile(filepath.Join(root, "src", "deploy", "envoy", "envoy.yaml"))
	if err != nil {
		t.Fatalf("read envoy.yaml: %v", err)
	}
	envoyStr := string(envoyRaw)
	for _, needle := range []string{"hpack_table_size: 65536", "brotli.compressor.v3.Brotli", "gzip.compressor.v3.Gzip"} {
		if !strings.Contains(envoyStr, needle) {
			t.Fatalf("envoy.yaml missing %q", needle)
		}
	}

	baseURL := ensureEphemeralAdminGateway(t)
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
	var hpackBuf bytes.Buffer
	hpEnc := hpack.NewEncoder(&hpackBuf)
	_ = hpEnc.WriteField(hpack.HeaderField{Name: "etag", Value: wantETag})
	fields, err := hpack.NewDecoder(4096, nil).DecodeFull(hpackBuf.Bytes())
	if err != nil || len(fields) != 1 || fields[0].Value != wantETag {
		t.Fatalf("HPACK round-trip failed: err=%v fields=%v", err, fields)
	}

	var qpackBuf bytes.Buffer
	qpEnc := qpack.NewEncoder(&qpackBuf)
	_ = qpEnc.WriteField(qpack.HeaderField{Name: "etag", Value: wantETag})
	_ = qpEnc.Close()
	qf, err := qpack.NewDecoder().Decode(qpackBuf.Bytes())()
	if err != nil || qf.Value != wantETag {
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
	resp, err := h2cClient.Get(baseURL + "/api/v1/gm/openapi.json")
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
	st, err := os.Stat(pmPath)
	if err != nil || st.Size() <= 100<<20 {
		t.Fatalf("expected dist/content/verra.pmtiles > 100 MB, got stat=%v err=%v", st, err)
	}
	baseURL := ensureEphemeralAdminGateway(t)
	verifyPmTilesEndpoints(t, baseURL)
	verifyStreamLayerBitmasks(t, baseURL)
}

func verifyPmTilesEndpoints(t *testing.T, baseURL string) {
	t.Helper()
	codeMeta, _, bodyMeta := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/map-meta", nil, nil)
	if codeMeta != http.StatusOK || !strings.Contains(string(bodyMeta), `"pmtiles_present":true`) {
		t.Fatalf("GET /api/v1/world/map-meta code=%d body=%s", codeMeta, string(bodyMeta))
	}
	codeRange, _, bodyRange := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/pmtiles", map[string]string{"Range": "bytes=0-126"}, nil)
	if codeRange != http.StatusPartialContent || len(bodyRange) != 127 || string(bodyRange[:8]) != "PMTiles\x03" {
		t.Fatalf("Range /api/v1/world/pmtiles code=%d len=%d magic=%q", codeRange, len(bodyRange), bodyRange[:8])
	}
	codeT0, hdrT0, bodyT0 := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/tiles/0/0/0", nil, nil)
	cfg0, err0 := png.DecodeConfig(bytes.NewReader(bodyT0))
	if codeT0 != http.StatusOK || hdrT0.Get("Content-Type") != "image/png" || err0 != nil || cfg0.Width != 256 || cfg0.Height != 256 {
		t.Fatalf("tile 0/0/0 code=%d ct=%q err=%v dim=%dx%d", codeT0, hdrT0.Get("Content-Type"), err0, cfg0.Width, cfg0.Height)
	}
	codeT8, hdrT8, bodyT8 := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/tiles/8/0/0", nil, nil)
	cfg8, err8 := png.DecodeConfig(bytes.NewReader(bodyT8))
	if codeT8 != http.StatusOK || hdrT8.Get("Content-Type") != "image/png" || err8 != nil || cfg8.Width != 1024 || cfg8.Height != 1024 {
		t.Fatalf("node plug tile 8/0/0 code=%d ct=%q err=%v dim=%dx%d", codeT8, hdrT8.Get("Content-Type"), err8, cfg8.Width, cfg8.Height)
	}
}

func verifyStreamLayerBitmasks(t *testing.T, baseURL string) {
	t.Helper()
	cases := []struct {
		query          string
		wantMask       uint32
		wantStreamMask uint32
	}{
		{"?layer_mask=1", streamLayerPlayers, streamLayerPlayers},
		{"?layer_mask=2", streamLayerNpcs, streamLayerNpcs},
		{"?layer_mask=32", streamLayerPois, 0},
		{"?layer_mask=64", streamLayerMonsters, streamLayerNpcs},
		{"?layers=none", 0, 0},
	}
	for _, tc := range cases {
		code, _, body := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/state"+tc.query, nil, nil)
		if code != http.StatusOK {
			t.Fatalf("GET /api/v1/world/state%s status=%d", tc.query, code)
		}
		var payload map[string]any
		_ = json.Unmarshal(body, &payload)
		if uint32(payload["layer_mask"].(float64)) != tc.wantMask || uint32(payload["stream_layers"].(float64)) != tc.wantStreamMask {
			t.Fatalf("%s layer_mask=%v stream_layers=%v, want %d/%d", tc.query, payload["layer_mask"], payload["stream_layers"], tc.wantMask, tc.wantStreamMask)
		}
	}
}

func TestWebAdminLiveStackAPIsE2E(t *testing.T) { TestLiveWebAdminAPIsE2E(t) }

func TestLiveWebAdminAPIsE2E(t *testing.T) {
	baseURL := "http://127.0.0.1:8080"
	probeCli := &http.Client{Timeout: 2 * time.Second}
	if resp, err := probeCli.Get(baseURL + "/api/v1/gm/openapi.json"); err != nil || resp.StatusCode != http.StatusOK {
		if resp != nil {
			_ = resp.Body.Close()
		}
		baseURL = ensureEphemeralAdminGateway(t)
	} else {
		_ = resp.Body.Close()
	}

	client := &http.Client{Timeout: 8 * time.Second}
	for _, ep := range []string{
		"/api/v1/gm/openapi.json", "/api/v1/world/map-meta", "/api/v1/world/tiles/0/0/0", "/api/v1/world/tiles/8/0/0",
		"/api/v1/catalog/items?page=1&page_size=5", "/api/v1/catalog/npcs?page=1&page_size=5", "/api/v1/catalog/places",
		"/api/v1/world/state?layer_mask=95", "/api/v1/gm/containers", "/api/v1/gm/logs?container=aoc-world&tail=20",
	} {
		probeWebAdminEndpoint(t, client, baseURL+ep)
	}
}

func probeWebAdminEndpoint(t *testing.T, client *http.Client, fullURL string) {
	t.Helper()
	req, _ := http.NewRequest(http.MethodGet, fullURL, nil)
	req.Header.Set("Authorization", "Bearer gm-observer-dev-token")
	req.Header.Set("Accept-Encoding", "br, gzip")
	resp, err := client.Do(req)
	if err != nil || resp.StatusCode != http.StatusOK {
		t.Fatalf("GET %s failed: resp=%v err=%v", fullURL, resp, err)
	}
	_, _ = io.Copy(io.Discard, resp.Body)
	_ = resp.Body.Close()
	etag := resp.Header.Get("ETag")
	if etag == "" {
		t.Fatalf("GET %s missing ETag header", fullURL)
	}
	req304, _ := http.NewRequest(http.MethodGet, fullURL, nil)
	req304.Header.Set("Authorization", "Bearer gm-observer-dev-token")
	req304.Header.Set("Accept-Encoding", "br, gzip")
	if strings.Contains(fullURL, "/api/v1/world/state") || strings.Contains(fullURL, "/api/v1/gm/logs") {
		req304.Header.Set("If-Modified-Since", time.Now().UTC().Add(24*time.Hour).Format(http.TimeFormat))
	} else {
		req304.Header.Set("If-None-Match", etag)
	}
	resp304, err := client.Do(req304)
	if err != nil || resp304.StatusCode != http.StatusNotModified {
		t.Fatalf("GET %s conditional 304 check failed (etag=%s): got resp=%v err=%v", fullURL, etag, resp304, err)
	}
	_ = resp304.Body.Close()
}

func doAdminHTTP(t *testing.T, method, fullURL string, hdrs map[string]string, body []byte) (int, http.Header, []byte) {
	t.Helper()
	cli := &http.Client{Timeout: 5 * time.Second, Transport: &http.Transport{DisableCompression: true}}
	req, err := http.NewRequest(method, fullURL, bytes.NewReader(body))
	if err != nil {
		t.Fatalf("NewRequest %s %s: %v", method, fullURL, err)
	}
	req.Header.Set("Authorization", "Bearer gm-architect-dev-token")
	for k, v := range hdrs {
		req.Header.Set(k, v)
	}
	resp, err := cli.Do(req)
	if err != nil {
		t.Fatalf("%s %s failed: %v", method, fullURL, err)
	}
	raw, _ := io.ReadAll(resp.Body)
	_ = resp.Body.Close()
	return resp.StatusCode, resp.Header, raw
}
