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
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"aocrop/services/internal/telemetry"
	worldstream "aocrop/services/internal/world-stream"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

func TestAdminGatewayETagAndConditionalGet(t *testing.T) {
	fixedNow := time.Date(2026, 11, 5, 12, 0, 0, 0, time.UTC)
	srv := NewRouter(Config{Now: func() time.Time { return fixedNow }})

	for _, path := range []string{"/api/v1/gm/commands", "/api/v1/gm/audit", "/healthz"} {
		rec1 := serveAuthed(srv, http.MethodGet, path, "gm-observer-dev-token", nil, nil)
		if rec1.Code != http.StatusOK {
			t.Fatalf("GET %s status = %d, want 200", path, rec1.Code)
		}
		etag := rec1.Header().Get("ETag")
		lastMod := rec1.Header().Get("Last-Modified")
		if !strings.HasPrefix(etag, `"sha256-`) || lastMod == "" || rec1.Header().Get("Alt-Svc") != `h3=":8443"; ma=86400` {
			t.Fatalf("GET %s ETag=%q Last-Modified=%q Alt-Svc=%q", path, etag, lastMod, rec1.Header().Get("Alt-Svc"))
		}
		rec304 := serveAuthed(srv, http.MethodGet, path, "gm-observer-dev-token", map[string]string{"If-None-Match": etag}, nil)
		if rec304.Code != http.StatusNotModified || rec304.Body.Len() != 0 || rec304.Header().Get("ETag") != etag {
			t.Fatalf("GET %s If-None-Match status=%d bodyLen=%d etag=%q", path, rec304.Code, rec304.Body.Len(), rec304.Header().Get("ETag"))
		}
		recWeak := serveAuthed(srv, http.MethodGet, path, "gm-observer-dev-token", map[string]string{"If-None-Match": "W/" + etag}, nil)
		if recWeak.Code != http.StatusNotModified {
			t.Fatalf("GET %s weak If-None-Match status=%d, want 304", path, recWeak.Code)
		}
		recIMS := serveAuthed(srv, http.MethodGet, path, "gm-observer-dev-token", map[string]string{"If-Modified-Since": lastMod}, nil)
		if recIMS.Code != http.StatusNotModified {
			t.Fatalf("GET %s If-Modified-Since status=%d, want 304", path, recIMS.Code)
		}
		recMiss := serveAuthed(srv, http.MethodGet, path, "gm-observer-dev-token", map[string]string{"If-None-Match": `"sha256-0000000000000000"`}, nil)
		if recMiss.Code != http.StatusOK {
			t.Fatalf("GET %s If-None-Match miss status=%d, want 200", path, recMiss.Code)
		}
	}
}

func TestAdminGatewayBrotliAndGzipAndHPACKQPACK(t *testing.T) {
	srv := NewRouter(Config{})
	recBr := serveAuthed(srv, http.MethodGet, "/api/v1/catalog/places", "", map[string]string{"Accept-Encoding": "br, gzip"}, nil)
	if recBr.Code != http.StatusOK || recBr.Header().Get("Content-Encoding") != "br" || recBr.Header().Get("Vary") != "Accept-Encoding" {
		t.Fatalf("br status=%d ce=%q vary=%q", recBr.Code, recBr.Header().Get("Content-Encoding"), recBr.Header().Get("Vary"))
	}
	decodedBr, err := decodeBrotli(recBr.Body.Bytes())
	if err != nil || !strings.Contains(string(decodedBr), `"login"`) {
		t.Fatalf("decodeBrotli err=%v body=%q", err, string(decodedBr))
	}
	brETag := recBr.Header().Get("ETag")
	recBr304 := serveAuthed(srv, http.MethodGet, "/api/v1/catalog/places", "", map[string]string{"Accept-Encoding": "br", "If-None-Match": brETag}, nil)
	if recBr304.Code != http.StatusNotModified {
		t.Fatalf("br If-None-Match status=%d, want 304", recBr304.Code)
	}

	recGz := serveAuthed(srv, http.MethodGet, "/api/v1/catalog/places", "", map[string]string{"Accept-Encoding": "gzip"}, nil)
	if recGz.Code != http.StatusOK || recGz.Header().Get("Content-Encoding") != "gzip" {
		t.Fatalf("gzip status=%d ce=%q", recGz.Code, recGz.Header().Get("Content-Encoding"))
	}
	gzReader, err := gzip.NewReader(bytes.NewReader(recGz.Body.Bytes()))
	if err != nil {
		t.Fatalf("gzip.NewReader: %v", err)
	}
	decodedGz, _ := io.ReadAll(gzReader)
	_ = gzReader.Close()
	if !strings.Contains(string(decodedGz), `"garden"`) {
		t.Fatalf("decoded gzip missing garden: %s", string(decodedGz))
	}

	hpackBytes, err := telemetry.EncodeHPACKHeaders(recBr.Header())
	if err != nil || len(hpackBytes) == 0 {
		t.Fatalf("EncodeHPACKHeaders err=%v", err)
	}
	hpackHdr, err := telemetry.DecodeHPACKHeaders(hpackBytes)
	if err != nil || hpackHdr.Get("etag") != brETag {
		t.Fatalf("DecodeHPACKHeaders err=%v etag=%q", err, hpackHdr.Get("etag"))
	}
	qpackBytes, err := telemetry.EncodeQPACKHeaders(recBr.Header())
	if err != nil || len(qpackBytes) == 0 {
		t.Fatalf("EncodeQPACKHeaders err=%v", err)
	}
	qpackHdr, err := telemetry.DecodeQPACKHeaders(qpackBytes)
	if err != nil || qpackHdr.Get("etag") != brETag {
		t.Fatalf("DecodeQPACKHeaders err=%v etag=%q", err, qpackHdr.Get("etag"))
	}
}

func TestAdminGatewayHotfixIfMatchPrecondition(t *testing.T) {
	srv := NewRouter(Config{})
	auditRec := serveAuthed(srv, http.MethodGet, "/api/v1/gm/audit", "gm-observer-dev-token", nil, nil)
	auditETag := auditRec.Header().Get("ETag")
	if !strings.HasPrefix(auditETag, `"sha256-`) {
		t.Fatalf("expected strong audit ETag, got %q", auditETag)
	}

	payload := []byte(`{"sql":"UPDATE spawns SET overlay_value = '2' WHERE record_guid = 'sp-1'","reason":"tune"}`)
	staleHdrs := map[string]string{"Idempotency-Key": "idem-hotfix-1", "If-Match": `"stale-etag"`}
	rec412 := serveAuthed(srv, http.MethodPost, "/api/v1/gm/hotfix/apply", "gm-architect-dev-token", staleHdrs, payload)
	if rec412.Code != http.StatusPreconditionFailed || !strings.HasPrefix(rec412.Header().Get("Content-Type"), "application/problem+json") {
		t.Fatalf("stale If-Match status=%d ct=%q, want 412 problem+json", rec412.Code, rec412.Header().Get("Content-Type"))
	}
	if srv.SnapshotEpoch() != 1 || srv.ReloadCount() != 0 {
		t.Fatalf("expected zero state mutation on 412, got epoch=%d reloads=%d", srv.SnapshotEpoch(), srv.ReloadCount())
	}

	validHdrs := map[string]string{"Idempotency-Key": "idem-hotfix-2", "If-Match": auditETag}
	rec200 := serveAuthed(srv, http.MethodPost, "/api/v1/gm/hotfix/apply", "gm-architect-dev-token", validHdrs, payload)
	if rec200.Code != http.StatusOK || srv.SnapshotEpoch() != 2 || srv.ReloadCount() != 1 {
		t.Fatalf("matching If-Match status=%d epoch=%d reloads=%d", rec200.Code, srv.SnapshotEpoch(), srv.ReloadCount())
	}

	replayOldHdrs := map[string]string{"Idempotency-Key": "idem-hotfix-3", "If-Match": auditETag}
	recStaleAfter := serveAuthed(srv, http.MethodPost, "/api/v1/gm/hotfix/apply", "gm-architect-dev-token", replayOldHdrs, payload)
	if recStaleAfter.Code != http.StatusPreconditionFailed {
		t.Fatalf("expected 412 when reusing outdated audit ETag after mutation, got %d", recStaleAfter.Code)
	}
}

func TestAdminGatewayH2CNegotiation(t *testing.T) {
	ln, err := telemetry.ListenDualStackTCP(context.Background(), "127.0.0.1:0")
	if err != nil {
		t.Fatalf("ListenDualStackTCP: %v", err)
	}
	router := NewRouter(Config{})
	httpSrv := telemetry.NewCleartextHTTPServer(ln.Addr().String(), telemetry.ETagMiddleware(router))
	defer func() { _ = httpSrv.Close() }()
	go func() { _ = httpSrv.Serve(ln) }()

	h2cClient := telemetry.NewETagCachingClient(telemetry.NewH2CClient(5 * time.Second))
	url := "http://" + ln.Addr().String() + "/api/v1/gm/commands"
	req1, _ := http.NewRequest(http.MethodGet, url, nil)
	req1.Header.Set("Authorization", "Bearer gm-observer-dev-token")
	resp1, err := h2cClient.Do(req1)
	if err != nil {
		t.Fatalf("h2c GET 1: %v", err)
	}
	body1, _ := io.ReadAll(resp1.Body)
	_ = resp1.Body.Close()
	if resp1.StatusCode != http.StatusOK || resp1.ProtoMajor != 2 || !strings.HasPrefix(resp1.Header.Get("ETag"), `"sha256-`) {
		t.Fatalf("h2c GET 1 status=%d proto=%s etag=%q", resp1.StatusCode, resp1.Proto, resp1.Header.Get("ETag"))
	}

	req2, _ := http.NewRequest(http.MethodGet, url, nil)
	req2.Header.Set("Authorization", "Bearer gm-observer-dev-token")
	resp2, err := h2cClient.Do(req2)
	if err != nil {
		t.Fatalf("h2c GET 2: %v", err)
	}
	body2, _ := io.ReadAll(resp2.Body)
	_ = resp2.Body.Close()
	if resp2.StatusCode != http.StatusOK || string(body2) != string(body1) || resp2.Header.Get(telemetry.ETagRehydratedHeader) != "true" {
		t.Fatalf("h2c rehydrated GET 2 status=%d rehydrated=%q", resp2.StatusCode, resp2.Header.Get(telemetry.ETagRehydratedHeader))
	}
}

func TestAdminGatewayCatalogPaginationSortingProjectionAndHateoas(t *testing.T) {
	srv := NewRouter(Config{})
	rec := serveAuthed(srv, http.MethodGet, "/api/v1/catalog/places?page=1&page_size=5&sort=-name&fields=name,category", "", nil, nil)
	if rec.Code != http.StatusOK || rec.Header().Get("X-Pagination") == "" || rec.Header().Get("Server-Timing") == "" {
		t.Fatalf("GET /api/v1/catalog/places status=%d xpag=%q", rec.Code, rec.Header().Get("X-Pagination"))
	}
	var env map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("unmarshal places: %v", err)
	}
	if int(env["page"].(float64)) != 1 || int(env["page_size"].(float64)) != 5 || env["has_more"] != true {
		t.Fatalf("unexpected pagination envelope: %#v", env)
	}
	links, _ := env["_links"].(map[string]any)
	if links["self"] == nil || links["next"] == nil || links["last"] == nil {
		t.Fatalf("missing HATEOAS _links: %#v", links)
	}
	items, _ := env["items"].([]any)
	if len(items) != 5 {
		t.Fatalf("expected 5 projected items, got %d", len(items))
	}
	firstItem, _ := items[0].(map[string]any)
	if _, hasX := firstItem["x_cm"]; hasX || firstItem["name"] == nil {
		t.Fatalf("field projection failed: %#v", firstItem)
	}
	rec406 := serveAuthed(srv, http.MethodGet, "/api/v1/catalog/items", "", map[string]string{"Accept": "text/xml"}, nil)
	if rec406.Code != http.StatusNotAcceptable {
		t.Fatalf("expected 406 Not Acceptable, got %d", rec406.Code)
	}
}

func TestAdminGatewayPmTilesRangeAndTilesAndMapMeta(t *testing.T) {
	pmPath := buildTestPmTilesFile(t)
	srv := NewRouter(Config{PmTilesPath: pmPath})
	defer func() { _ = srv.Close() }()

	recRange := serveAuthed(srv, http.MethodGet, "/api/v1/world/pmtiles", "", map[string]string{"Range": "bytes=0-126"}, nil)
	if recRange.Code != http.StatusPartialContent || recRange.Body.Len() != 127 {
		t.Fatalf("Range GET /api/v1/world/pmtiles status=%d len=%d", recRange.Code, recRange.Body.Len())
	}

	recTile0 := serveAuthed(srv, http.MethodGet, "/api/v1/world/tiles/0/0/0", "", nil, nil)
	if recTile0.Code != http.StatusOK || recTile0.Header().Get("Content-Type") != "image/png" {
		t.Fatalf("GET tile 0/0/0 status=%d ct=%q", recTile0.Code, recTile0.Header().Get("Content-Type"))
	}
	tileETag := recTile0.Header().Get("ETag")
	tileMod := recTile0.Header().Get("Last-Modified")
	recTile304 := serveAuthed(srv, http.MethodGet, "/api/v1/world/tiles/0/0/0", "", map[string]string{"If-None-Match": tileETag}, nil)
	if recTile304.Code != http.StatusNotModified {
		t.Fatalf("tile If-None-Match status=%d, want 304", recTile304.Code)
	}
	recTileIMS := serveAuthed(srv, http.MethodGet, "/api/v1/world/tiles/0/0/0", "", map[string]string{"If-Modified-Since": tileMod}, nil)
	if recTileIMS.Code != http.StatusNotModified {
		t.Fatalf("tile If-Modified-Since status=%d, want 304", recTileIMS.Code)
	}

	recPlug := serveAuthed(srv, http.MethodGet, "/api/v1/world/tiles/8/0/0", "", nil, nil)
	if recPlug.Code != http.StatusOK || recPlug.Header().Get("Content-Type") != "image/png" {
		t.Fatalf("GET node plug tile 8/0/0 status=%d ct=%q", recPlug.Code, recPlug.Header().Get("Content-Type"))
	}

	recMeta := serveAuthed(srv, http.MethodGet, "/api/v1/world/map-meta", "", nil, nil)
	if recMeta.Code != http.StatusOK {
		t.Fatalf("GET map-meta status=%d", recMeta.Code)
	}
	var meta map[string]any
	_ = json.Unmarshal(recMeta.Body.Bytes(), &meta)
	nodes, _ := meta["nodes"].([]any)
	if meta["pmtiles_present"] != true || len(nodes) != 26 {
		t.Fatalf("map-meta pmtiles_present=%v nodes=%d", meta["pmtiles_present"], len(nodes))
	}
}

func TestAdminGatewayWorldStateLayersAndBackendQuerySuppression(t *testing.T) {
	waFake := &fakeWorldAdminClient{}
	wsFake := &fakeWorldStreamClient{}
	cnFake := &fakeCityNodeClient{}
	coal := worldstream.NewCoalescer(50 * time.Millisecond)
	coal.UpsertEntity(worldstream.EntitySnapshot{ID: "npc-1", Kind: "npc", Name: "Village Vendor", Zone: "verra", X: 10, Y: 20, HealthPct: 100, Level: 10, State: "friendly"})
	coal.UpsertEntity(worldstream.EntitySnapshot{ID: "mon-1", Kind: "npc", Name: "Goblin Raider", Zone: "verra", X: 30, Y: 40, HealthPct: 100, Level: 15, State: "hostile"})
	coal.UpsertEntity(worldstream.EntitySnapshot{ID: "res-1", Kind: "resource_node", Name: "Oak Tree", Zone: "verra", X: 50, Y: 60, HealthPct: 100, Level: 1, State: "active"})
	coal.UpsertEntity(worldstream.EntitySnapshot{ID: "mnt-1", Kind: "mount", Name: "Swift Warhorse", Zone: "verra", X: 70, Y: 80, HealthPct: 100, Level: 1, State: "active"})
	coal.UpsertEntity(worldstream.EntitySnapshot{ID: "loot-1", Kind: "loot_pile", Name: "Goblin Corpse", Zone: "verra", X: 90, Y: 100, HealthPct: 0, Level: 15, State: "dead"})

	srv := NewRouter(Config{
		Coalescer:         coal,
		WorldAdminClient:  waFake,
		WorldStreamClient: wsFake,
		CityNodeClient:    cnFake,
	})

	recPlayers := serveAuthed(srv, http.MethodGet, "/api/v1/world/state?layers=players", "", nil, nil)
	var stPlayers map[string]any
	_ = json.Unmarshal(recPlayers.Body.Bytes(), &stPlayers)
	if uint32(stPlayers["layer_mask"].(float64)) != StreamLayerPlayers || uint32(stPlayers["stream_layers"].(float64)) != StreamLayerPlayers {
		t.Fatalf("players layer_mask=%v stream_layers=%v", stPlayers["layer_mask"], stPlayers["stream_layers"])
	}
	if waFake.listPlayersCalls.Load() != 1 || cnFake.listNodesCalls.Load() != 0 || wsFake.lastLayers.Load() != StreamLayerPlayers {
		t.Fatalf("players query suppression failed: listPlayers=%d listNodes=%d subLayers=%d", waFake.listPlayersCalls.Load(), cnFake.listNodesCalls.Load(), wsFake.lastLayers.Load())
	}
	if len(stPlayers["players"].([]any)) != 1 || len(stPlayers["npcs"].([]any)) != 0 || len(stPlayers["monsters"].([]any)) != 0 || len(stPlayers["pois"].([]any)) != 0 {
		t.Fatalf("unexpected slices for ?layers=players: %#v", stPlayers)
	}

	recMonsters := serveAuthed(srv, http.MethodGet, "/api/v1/world/state?layers=monsters", "", nil, nil)
	var stMonsters map[string]any
	_ = json.Unmarshal(recMonsters.Body.Bytes(), &stMonsters)
	if uint32(stMonsters["layer_mask"].(float64)) != LayerMonsters || uint32(stMonsters["stream_layers"].(float64)) != StreamLayerNpcs {
		t.Fatalf("monsters layer_mask=%v stream_layers=%v", stMonsters["layer_mask"], stMonsters["stream_layers"])
	}
	if waFake.listPlayersCalls.Load() != 1 || cnFake.listNodesCalls.Load() != 0 || wsFake.lastLayers.Load() != StreamLayerNpcs {
		t.Fatalf("monsters query suppression failed: listPlayers=%d listNodes=%d subLayers=%d", waFake.listPlayersCalls.Load(), cnFake.listNodesCalls.Load(), wsFake.lastLayers.Load())
	}
	if len(stMonsters["monsters"].([]any)) != 1 || len(stMonsters["npcs"].([]any)) != 0 {
		t.Fatalf("expected 1 monster and 0 npcs, got monsters=%d npcs=%d", len(stMonsters["monsters"].([]any)), len(stMonsters["npcs"].([]any)))
	}

	recNpcs := serveAuthed(srv, http.MethodGet, "/api/v1/world/state?layers=npcs", "", nil, nil)
	var stNpcs map[string]any
	_ = json.Unmarshal(recNpcs.Body.Bytes(), &stNpcs)
	if len(stNpcs["npcs"].([]any)) != 1 || len(stNpcs["monsters"].([]any)) != 0 {
		t.Fatalf("expected 1 friendly npc and 0 monsters, got npcs=%d monsters=%d", len(stNpcs["npcs"].([]any)), len(stNpcs["monsters"].([]any)))
	}

	recPois := serveAuthed(srv, http.MethodGet, "/api/v1/world/state?layers=pois", "", nil, nil)
	var stPois map[string]any
	_ = json.Unmarshal(recPois.Body.Bytes(), &stPois)
	if cnFake.listNodesCalls.Load() != 1 || len(stPois["nodes"].([]any)) != 26 || len(stPois["players"].([]any)) != 0 {
		t.Fatalf("pois query failed: listNodes=%d nodes=%d", cnFake.listNodesCalls.Load(), len(stPois["nodes"].([]any)))
	}
}

func TestAdminGatewayGMCommandsAndTransientRetry(t *testing.T) {
	waFake := &fakeWorldAdminClient{failNextTeleport: 1}
	cnFake := &fakeCityNodeClient{}
	srv := NewRouter(Config{WorldAdminClient: waFake, CityNodeClient: cnFake, ServiceToken: "svc-secret"})

	tpBody := []byte(`{"command":"teleport_player","character":"Tester","target_player":"TargetHero"}`)
	recTP := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "", map[string]string{"Content-Type": "application/json"}, tpBody)
	if recTP.Code != http.StatusOK || srv.transientRetries.Load() != 1 {
		t.Fatalf("teleport_player status=%d retries=%d body=%s", recTP.Code, srv.transientRetries.Load(), recTP.Body.String())
	}

	for _, tc := range [][]byte{
		[]byte(`{"command":"give_money","character":"Tester","gold":2,"silver":5,"copper":10}`),
		[]byte(`{"command":"grant_item","character":"Tester","item":"609166844926386","count":3,"rarity":2}`),
		[]byte(`{"command":"spawn_npc","npc":"Goblin","place":"login","level":12}`),
		[]byte(`{"command":"set_node_level","node":"Winstead","level":4}`),
	} {
		rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "", map[string]string{"Content-Type": "application/json"}, tc)
		if rec.Code != http.StatusOK {
			t.Fatalf("POST /api/v1/gm/commands %s status=%d body=%s", string(tc), rec.Code, rec.Body.String())
		}
	}

	recPut := serveAuthed(srv, http.MethodPut, "/api/v1/world/nodes/Halcyon", "", map[string]string{"Content-Type": "application/json"}, []byte(`{"level":6}`))
	if recPut.Code != http.StatusOK {
		t.Fatalf("PUT /api/v1/world/nodes/Halcyon status=%d body=%s", recPut.Code, recPut.Body.String())
	}
	rec415 := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "", map[string]string{"Content-Type": "text/plain"}, []byte(`{}`))
	if rec415.Code != http.StatusUnsupportedMediaType {
		t.Fatalf("expected 415 Unsupported Media Type, got %d", rec415.Code)
	}
	rec405 := serveAuthed(srv, http.MethodDelete, "/api/v1/catalog/items", "", nil, nil)
	if rec405.Code != http.StatusMethodNotAllowed || rec405.Header().Get("Allow") == "" {
		t.Fatalf("expected 405 Method Not Allowed with Allow header, got %d allow=%q", rec405.Code, rec405.Header().Get("Allow"))
	}
}

func TestAdminGatewayDockerContainersAndLogsDemux(t *testing.T) {
	dockerSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case strings.HasPrefix(r.URL.Path, "/containers/json"):
			_ = json.NewEncoder(w).Encode([]map[string]any{
				{"Id": "1111222233334444", "Names": []string{"/aoc-world"}, "Image": "sandal-world", "State": "running", "Status": "Up 5m"},
			})
		case strings.HasSuffix(r.URL.Path, "/stats"):
			_ = json.NewEncoder(w).Encode(map[string]any{
				"cpu_stats":    map[string]any{"cpu_usage": map[string]any{"total_usage": 20000000}, "system_cpu_usage": 100000000, "online_cpus": 4},
				"precpu_stats": map[string]any{"cpu_usage": map[string]any{"total_usage": 10000000}, "system_cpu_usage": 50000000},
				"memory_stats": map[string]any{"usage": 104857600, "limit": 1073741824},
			})
		case strings.HasSuffix(r.URL.Path, "/logs"):
			var buf bytes.Buffer
			writeDockerLogFrame(&buf, 1, "2026-11-06T12:00:00Z world tick healthy\n")
			writeDockerLogFrame(&buf, 2, "2026-11-06T12:00:01Z stderr warning check\n")
			_, _ = w.Write(buf.Bytes())
		}
	}))
	defer dockerSrv.Close()

	srv := NewRouter(Config{DockerClient: dockerSrv.Client()})
	srv.dockerClient = &http.Client{Transport: rewriteHostTransport{base: dockerSrv.Client().Transport, targetURL: dockerSrv.URL}}

	recCont := serveAuthed(srv, http.MethodGet, "/api/v1/gm/containers", "", nil, nil)
	if recCont.Code != http.StatusOK || !strings.Contains(recCont.Body.String(), `"aoc-world"`) {
		t.Fatalf("GET /api/v1/gm/containers status=%d body=%s", recCont.Code, recCont.Body.String())
	}
	recLogs := serveAuthed(srv, http.MethodGet, "/api/v1/gm/logs?container=aoc-world&q=healthy", "", nil, nil)
	if recLogs.Code != http.StatusOK || !strings.Contains(recLogs.Body.String(), "world tick healthy") {
		t.Fatalf("GET /api/v1/gm/logs status=%d body=%s", recLogs.Code, recLogs.Body.String())
	}
	recBad := serveAuthed(srv, http.MethodGet, "/api/v1/gm/logs?container=evil-container", "", nil, nil)
	if recBad.Code != http.StatusBadRequest {
		t.Fatalf("expected 400 for disallowed container, got %d", recBad.Code)
	}
}

func writeDockerLogFrame(buf *bytes.Buffer, streamType byte, line string) {
	var hdr [8]byte
	hdr[0] = streamType
	binary.BigEndian.PutUint32(hdr[4:8], uint32(len(line)))
	buf.Write(hdr[:])
	buf.WriteString(line)
}

type rewriteHostTransport struct {
	base      http.RoundTripper
	targetURL string
}

func (t rewriteHostTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	cloned := req.Clone(req.Context())
	cloned.URL.Scheme = "http"
	cloned.URL.Host = strings.TrimPrefix(t.targetURL, "http://")
	return t.base.RoundTrip(cloned)
}

func buildTestPmTilesFile(t *testing.T) string {
	t.Helper()
	pngData := []byte("\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR")
	id0, _ := ZxyToTileID(0, 0, 0)
	idPlug, _ := ZxyToTileID(8, 0, 0)
	var dirRaw bytes.Buffer
	writeUvarint(&dirRaw, 2)
	writeUvarint(&dirRaw, id0)
	writeUvarint(&dirRaw, idPlug-id0)
	writeUvarint(&dirRaw, 1)
	writeUvarint(&dirRaw, 1)
	writeUvarint(&dirRaw, uint64(len(pngData)))
	writeUvarint(&dirRaw, uint64(len(pngData)))
	writeUvarint(&dirRaw, 1)
	writeUvarint(&dirRaw, 0)

	metaBytes := []byte(`{"x0":-47,"y0":-17,"cols":51,"rows":46,"width_px":26112,"height_px":23552}`)
	var fileBuf bytes.Buffer
	hdr := make([]byte, pmtilesHeaderBytes)
	copy(hdr[0:7], "PMTiles")
	hdr[7] = 3
	rootOff := uint64(pmtilesHeaderBytes)
	metaOff := rootOff + uint64(dirRaw.Len())
	dataOff := metaOff + uint64(len(metaBytes))
	binary.LittleEndian.PutUint64(hdr[8:16], rootOff)
	binary.LittleEndian.PutUint64(hdr[16:24], uint64(dirRaw.Len()))
	binary.LittleEndian.PutUint64(hdr[24:32], metaOff)
	binary.LittleEndian.PutUint64(hdr[32:40], uint64(len(metaBytes)))
	binary.LittleEndian.PutUint64(hdr[56:64], dataOff)
	binary.LittleEndian.PutUint64(hdr[64:72], uint64(len(pngData)*2))
	fileBuf.Write(hdr)
	fileBuf.Write(dirRaw.Bytes())
	fileBuf.Write(metaBytes)
	fileBuf.Write(pngData)
	fileBuf.Write(pngData)

	p := filepath.Join(t.TempDir(), "verra.pmtiles")
	if err := os.WriteFile(p, fileBuf.Bytes(), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func writeUvarint(buf *bytes.Buffer, v uint64) {
	var tmp [10]byte
	n := binary.PutUvarint(tmp[:], v)
	buf.Write(tmp[:n])
}

type fakeWorldAdminClient struct {
	worldadminv1.WorldAdminServiceClient
	listPlayersCalls atomic.Int32
	failNextTeleport int32
}

func (f *fakeWorldAdminClient) ListPlayers(context.Context, *worldadminv1.ListPlayersRequest, ...grpc.CallOption) (*worldadminv1.ListPlayersResponse, error) {
	f.listPlayersCalls.Add(1)
	return &worldadminv1.ListPlayersResponse{
		Players: []*worldadminv1.Player{
			{Name: "TargetHero", CharacterId: "char-101", Position: &worldadminv1.Position{X: -847000, Y: -538000, Z: 11524}},
		},
	}, nil
}

func (f *fakeWorldAdminClient) ListPlaces(context.Context, *worldadminv1.ListPlacesRequest, ...grpc.CallOption) (*worldadminv1.ListPlacesResponse, error) {
	return &worldadminv1.ListPlacesResponse{
		Places: []*worldadminv1.Place{{Name: "lionhold", DisplayName: "Lionhold", Position: &worldadminv1.Position{X: -847992, Y: -538867, Z: 11524}, Kind: "spawn"}},
	}, nil
}

func (f *fakeWorldAdminClient) Teleport(_ context.Context, req *worldadminv1.TeleportRequest, _ ...grpc.CallOption) (*worldadminv1.TeleportResponse, error) {
	if atomic.AddInt32(&f.failNextTeleport, -1) >= 0 {
		return nil, status.Error(codes.Unavailable, "transient world admin unavailability")
	}
	return &worldadminv1.TeleportResponse{Character: req.GetCharacter(), To: &worldadminv1.Position{X: -847000, Y: -538000, Z: 11524}}, nil
}

func (f *fakeWorldAdminClient) GiveMoney(_ context.Context, req *worldadminv1.GiveMoneyRequest, _ ...grpc.CallOption) (*worldadminv1.GiveMoneyResponse, error) {
	return &worldadminv1.GiveMoneyResponse{Character: req.GetCharacter(), Balance: req.GetCopper(), Added: req.GetCopper()}, nil
}

func (f *fakeWorldAdminClient) GiveItem(_ context.Context, req *worldadminv1.GiveItemRequest, _ ...grpc.CallOption) (*worldadminv1.GiveItemResponse, error) {
	return &worldadminv1.GiveItemResponse{Character: req.GetCharacter(), Item: req.GetItem(), ItemGuid: 609166844926386, Container: "Backpack", Count: req.GetCount(), Rarity: req.GetRarity()}, nil
}

func (f *fakeWorldAdminClient) SpawnNpc(_ context.Context, req *worldadminv1.SpawnNpcRequest, _ ...grpc.CallOption) (*worldadminv1.SpawnNpcResponse, error) {
	return &worldadminv1.SpawnNpcResponse{Id: 9001, Npc: req.GetNpc(), DisplayName: req.GetNpc(), AssetGuid: 1090001, Level: req.GetLevel(), Position: &worldadminv1.Position{X: -847992, Y: -538867, Z: 11524}}, nil
}

type fakeWorldStreamClient struct {
	worldadminv1.WorldStreamServiceClient
	lastLayers atomic.Uint32
}

func (f *fakeWorldStreamClient) Subscribe(_ context.Context, req *worldadminv1.SubscribeRequest, _ ...grpc.CallOption) (grpc.ServerStreamingClient[worldadminv1.SubscribeResponse], error) {
	f.lastLayers.Store(req.GetLayers())
	return nil, status.Error(codes.Canceled, "snapshot complete")
}

func (f *fakeWorldStreamClient) GetStatus(context.Context, *worldadminv1.GetStatusRequest, ...grpc.CallOption) (*worldadminv1.GetStatusResponse, error) {
	return &worldadminv1.GetStatusResponse{
		Stats:       &worldadminv1.StreamStats{TickMs: 2.5, PublishMs: 0.2, Players: 1, Npcs: 14000},
		Subscribers: 1,
	}, nil
}

type fakeCityNodeClient struct {
	citynodev1.CityNodeServiceClient
	listNodesCalls atomic.Int32
}

func (f *fakeCityNodeClient) ListNodes(context.Context, *citynodev1.ListNodesRequest, ...grpc.CallOption) (*citynodev1.ListNodesResponse, error) {
	f.listNodesCalls.Add(1)
	return &citynodev1.ListNodesResponse{
		Nodes: []*citynodev1.Node{{Guid: 108646108518008, Name: "Verra_RVR_Winstead", DisplayName: "Winstead", Level: 3}},
	}, nil
}

func (f *fakeCityNodeClient) SetNodeLevel(_ context.Context, req *citynodev1.SetNodeLevelRequest, _ ...grpc.CallOption) (*citynodev1.SetNodeLevelResponse, error) {
	name := req.GetNode().GetName()
	return &citynodev1.SetNodeLevelResponse{
		Node: &citynodev1.Node{Guid: 108646108518008, Name: name, DisplayName: name, Level: uint32(req.GetLevel())},
	}, nil
}

func serveAuthed(h http.Handler, method, path, token string, hdrs map[string]string, body []byte) *httptest.ResponseRecorder {
	req := httptest.NewRequest(method, path, bytes.NewReader(body))
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	for k, v := range hdrs {
		req.Header.Set(k, v)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}
