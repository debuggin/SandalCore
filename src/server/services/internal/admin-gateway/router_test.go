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
	"image/png"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

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

	for _, sparsePath := range []string{"/api/v1/world/tiles/6/10/10", "/api/v1/world/tiles/7/25/30"} {
		recOcean := serveAuthed(srv, http.MethodGet, sparsePath, "", nil, nil)
		if recOcean.Code != http.StatusOK || recOcean.Header().Get("Content-Type") != "image/png" {
			t.Fatalf("GET sparse base tile %s status=%d ct=%q, want 200 image/png", sparsePath, recOcean.Code, recOcean.Header().Get("Content-Type"))
		}
		img, err := png.Decode(bytes.NewReader(recOcean.Body.Bytes()))
		if err != nil || img.Bounds().Dx() != 256 || img.Bounds().Dy() != 256 {
			t.Fatalf("decode sparse black tile %s err=%v bounds=%v", sparsePath, err, img.Bounds())
		}
		r0, g0, b0, a0 := img.At(128, 128).RGBA()
		if r0 != 0 || g0 != 0 || b0 != 0 || a0 != 0xffff {
			t.Fatalf("sparse tile %s pixel=(%d,%d,%d,%d), want pure black #000000 (0,0,0,65535)", sparsePath, r0, g0, b0, a0)
		}
	}

	recMissingPlug := serveAuthed(srv, http.MethodGet, "/api/v1/world/tiles/8/15/2", "", nil, nil)
	if recMissingPlug.Code != http.StatusNotFound {
		t.Fatalf("GET missing z=8 plug tile status=%d, want 404", recMissingPlug.Code)
	}

	recIconArch := serveAuthed(srv, http.MethodGet, "/api/v1/world/icons/TUI_Icon_Map_Castle.png", "", nil, nil)
	if recIconArch.Code != http.StatusOK || recIconArch.Header().Get("Content-Type") != "image/png" {
		t.Fatalf("GET indexed z=9 icon status=%d ct=%q", recIconArch.Code, recIconArch.Header().Get("Content-Type"))
	}
	recIconFallback := serveAuthed(srv, http.MethodGet, "/api/v1/gm/world/icons/TUI_Icon_Map_StartingArea.png", "", nil, nil)
	if recIconFallback.Code != http.StatusOK || recIconFallback.Header().Get("Content-Type") != "image/png" {
		t.Fatalf("GET fallback badge icon status=%d ct=%q", recIconFallback.Code, recIconFallback.Header().Get("Content-Type"))
	}
	badgeImg, err := png.Decode(bytes.NewReader(recIconFallback.Body.Bytes()))
	if err != nil || badgeImg.Bounds().Dx() != 32 || badgeImg.Bounds().Dy() != 32 {
		t.Fatalf("decode fallback badge err=%v bounds=%v", err, badgeImg.Bounds())
	}

	recMeta := serveAuthed(srv, http.MethodGet, "/api/v1/world/map-meta", "", nil, nil)
	if recMeta.Code != http.StatusOK {
		t.Fatalf("GET map-meta status=%d", recMeta.Code)
	}
	var meta map[string]any
	_ = json.Unmarshal(recMeta.Body.Bytes(), &meta)
	nodes, _ := meta["nodes"].([]any)
	icons, _ := meta["icons"].([]any)
	if meta["pmtiles_present"] != true || len(nodes) != 26 || len(icons) < 30 {
		t.Fatalf("map-meta pmtiles_present=%v nodes=%d icons=%d", meta["pmtiles_present"], len(nodes), len(icons))
	}
	recMetaAlias := serveAuthed(srv, http.MethodGet, "/api/v1/world/tiles/metadata.json", "", nil, nil)
	if recMetaAlias.Code != http.StatusOK || !strings.Contains(recMetaAlias.Body.String(), `"node_plugs"`) {
		t.Fatalf("GET /api/v1/world/tiles/metadata.json status=%d", recMetaAlias.Code)
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
	assertCompactWorldStateResourceTypes(t, srv, coal)
}

func assertCompactWorldStateResourceTypes(t *testing.T, srv *Server, coal *worldstream.Coalescer) {
	t.Helper()
	wantTypes := map[string]string{
		"Stone_Wyrdstone_Large_1": "Mining / Ore",
		"Plant_Banana_Fruited_1":  "Herbalism / Plant",
		"Tree_Oak_1":              "Lumberjacking / Tree",
		"Stone_Basalt_1":          "Mining / Stone",
	}
	for name := range wantTypes {
		coal.UpsertEntity(worldstream.EntitySnapshot{ID: "res:" + name, Kind: "resource_node", Name: name, Zone: "verra", X: 55, Y: 65, HealthPct: 100, Level: 1, State: "active"})
	}
	recCompact := serveAuthed(srv, http.MethodGet, "/api/v1/world/state?compact=1", "", nil, nil)
	var stCompact map[string]any
	_ = json.Unmarshal(recCompact.Body.Bytes(), &stCompact)
	gotTypes := make(map[string]string)
	for _, raw := range stCompact["entities"].([]any) {
		if m, ok := raw.(map[string]any); ok && m["kind"] == "resource_node" {
			name, _ := m["name"].(string)
			rt, _ := m["resource_type"].(string)
			gotTypes[name] = rt
		}
	}
	for name, want := range wantTypes {
		if got := gotTypes[name]; got != want {
			t.Fatalf("compact=1 resource_node %q resource_type=%q, want %q", name, got, want)
		}
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
				{"Id": "1111222233334444", "Names": []string{"/aoc-world-riverlands"}, "Image": "sandal-world", "State": "running", "Status": "Up 5m"},
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
	if recCont.Code != http.StatusOK || !strings.Contains(recCont.Body.String(), `"aoc-world-riverlands"`) {
		t.Fatalf("GET /api/v1/gm/containers status=%d body=%s", recCont.Code, recCont.Body.String())
	}
	recLogs := serveAuthed(srv, http.MethodGet, "/api/v1/gm/logs?container=aoc-world-riverlands&q=healthy", "", nil, nil)
	if recLogs.Code != http.StatusOK || !strings.Contains(recLogs.Body.String(), "world tick healthy") {
		t.Fatalf("GET /api/v1/gm/logs status=%d body=%s", recLogs.Code, recLogs.Body.String())
	}
	recTail := serveAuthed(srv, http.MethodGet, "/api/v1/gm/logs?container=aoc-world&tail=1", "", nil, nil)
	var tailDoc map[string]any
	_ = json.Unmarshal(recTail.Body.Bytes(), &tailDoc)
	if recTail.Code != http.StatusOK || int(tailDoc["total"].(float64)) != 2 || len(tailDoc["entries"].([]any)) != 1 {
		t.Fatalf("GET /api/v1/gm/logs?tail=1 status=%d doc=%#v", recTail.Code, tailDoc)
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
	idIcon, _ := ZxyToTileID(9, 0, 0)
	var dirRaw bytes.Buffer
	writeUvarint(&dirRaw, 3)
	writeUvarint(&dirRaw, id0)
	writeUvarint(&dirRaw, idPlug-id0)
	writeUvarint(&dirRaw, idIcon-idPlug)
	writeUvarint(&dirRaw, 1)
	writeUvarint(&dirRaw, 1)
	writeUvarint(&dirRaw, 1)
	writeUvarint(&dirRaw, uint64(len(pngData)))
	writeUvarint(&dirRaw, uint64(len(pngData)))
	writeUvarint(&dirRaw, uint64(len(pngData)))
	writeUvarint(&dirRaw, 1)
	writeUvarint(&dirRaw, 0)
	writeUvarint(&dirRaw, 0)

	metaBytes := []byte(`{"x0":-47,"y0":-17,"cols":51,"rows":46,"width_px":26112,"height_px":23552,"icons":[{"index":0,"name":"TUI_Icon_Map_Castle","width":64,"height":64}]}`)
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
	binary.LittleEndian.PutUint64(hdr[64:72], uint64(len(pngData)*3))
	fileBuf.Write(hdr)
	fileBuf.Write(dirRaw.Bytes())
	fileBuf.Write(metaBytes)
	fileBuf.Write(pngData)
	fileBuf.Write(pngData)
	fileBuf.Write(pngData)

	p := filepath.Join(t.TempDir(), "verra.pmtiles")
	if err := os.WriteFile(p, fileBuf.Bytes(), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestAdminGatewayNodeMayorElectionsAndWorldctlActions(t *testing.T) {
	waFake := &fakeWorldAdminClient{}
	wsFake := &fakeWorldStreamClient{}
	cnFake := &fakeCityNodeClient{}
	coal := worldstream.NewCoalescer(50 * time.Millisecond)
	coal.UpsertEntity(protoEntityToSnapshot(&worldadminv1.Entity{
		Id: 777, Kind: worldadminv1.EntityKind_ENTITY_KIND_NPC, Name: "Ashlands Patrol",
		XCm: -644500, YCm: 376700, ZCm: 12000, HealthPct: 100, Level: 25, State: 4,
	}, time.Now().UnixMilli()))
	srv := NewRouter(Config{
		Coalescer: coal, WorldAdminClient: waFake, WorldStreamClient: wsFake, CityNodeClient: cnFake,
	})
	jsonHdr := map[string]string{"Content-Type": "application/json"}

	recMayor := serveAuthed(srv, http.MethodPost, "/api/v1/gm/node/mayor", "", jsonHdr, []byte(`{"node_name":"New Aela","mayor_guid":"char-101"}`))
	if recMayor.Code != http.StatusOK {
		t.Fatalf("POST /api/v1/gm/node/mayor status=%d body=%s", recMayor.Code, recMayor.Body.String())
	}
	var mayorResp map[string]any
	_ = json.Unmarshal(recMayor.Body.Bytes(), &mayorResp)
	if mayorResp["status"] != "executed" || mayorResp["mayor_name"] != "TargetHero" || mayorResp["election_status"] != "active_term" {
		t.Fatalf("unexpected mayor response: %#v", mayorResp)
	}

	recElec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/node/election", "", jsonHdr, []byte(`{"node_name":"Winstead","action":"open_voting"}`))
	if recElec.Code != http.StatusOK {
		t.Fatalf("POST /api/v1/gm/node/election status=%d body=%s", recElec.Code, recElec.Body.String())
	}

	recState := serveAuthed(srv, http.MethodGet, "/api/v1/gm/world/state", "", nil, nil)
	if recState.Code != http.StatusOK {
		t.Fatalf("GET /api/v1/gm/world/state status=%d", recState.Code)
	}
	var st map[string]any
	_ = json.Unmarshal(recState.Body.Bytes(), &st)
	if len(st["monsters"].([]any)) != 1 || st["layer_counts"] == nil || len(st["pois"].([]any)) < 26 {
		t.Fatalf("expected hostile state-bit-4 NPC classified as monster and full pois, got monsters=%d pois=%d", len(st["monsters"].([]any)), len(st["pois"].([]any)))
	}
	assertCityNodeGovernanceAndCoords(t, st["city_nodes"].([]any))

	for _, act := range [][]byte{
		[]byte(`{"action":"teleport_player","character":"Tester","target_player_guid":"char-101"}`),
		[]byte(`{"action":"kill_npc","entity_guid":"777"}`),
		[]byte(`{"action":"set_move_speed","player_guid":"char-101","multiplier":2.5}`),
		[]byte(`{"action":"set_weapon_xp","player_guid":"char-101","weapon":"greatsword","xp_amount":5000}`),
		[]byte(`{"action":"reset_tutorials","player_guid":"char-101"}`),
		[]byte(`{"action":"add_node_xp","node_name":"Winstead","xp_amount":25000}`),
		[]byte(`{"action":"advance_node_clock","node_name":"Winstead","seconds":3600}`),
	} {
		rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/command", "", jsonHdr, act)
		if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"status":"executed"`) {
			t.Fatalf("POST /api/v1/gm/command %s status=%d body=%s", string(act), rec.Code, rec.Body.String())
		}
	}
	if len(coal.Snapshot(worldstream.ViewportFilter{})) != 0 {
		t.Fatalf("expected kill_npc to remove entity 777 from coalescer")
	}
}

func assertCityNodeGovernanceAndCoords(t *testing.T, cityNodes []any) {
	t.Helper()
	var foundNewAela, foundWinstead, foundVhalgadim bool
	for _, raw := range cityNodes {
		m, _ := raw.(map[string]any)
		if int(m["plug_tier"].(float64)) < 0 {
			t.Fatalf("expected non-negative plug_tier for node %v, got %v", m["name"], m["plug_tier"])
		}
		switch m["name"] {
		case "Verra_RVR_NewAela":
			foundNewAela = m["mayor_name"] == "TargetHero" && m["mayor_guid"] == "char-101" && m["election_status"] == "active_term"
		case "Verra_RVR_Winstead":
			foundWinstead = m["election_status"] == "voting" && m["ox"] == -644519.11 && m["oy"] == 376773.24 && m["yaw_deg"] == 319.20846 && int(m["level"].(float64)) == 3
		case "Verra_ANV_Vhalgadim":
			foundVhalgadim = m["ox"] == -762862.0 && m["oy"] == -563881.0 && m["yaw_deg"] == 349.999997 && int(m["plug_tier"].(float64)) == 0
		}
	}
	if !foundNewAela || !foundWinstead || !foundVhalgadim {
		t.Fatalf("city_nodes governance/coords check failed: newAela=%v winstead=%v vhalgadim=%v", foundNewAela, foundWinstead, foundVhalgadim)
	}
}

func TestAdminGatewayPrometheusScrapeOneShotStatsAndGMFallback(t *testing.T) {
	envoySrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("# TYPE envoy_cluster_upstream_rq_total counter\nenvoy_cluster_upstream_rq_total{envoy_cluster_name=\"world_admin_grpc\"} 42\nenvoy_http_downstream_rq_total{} 128\n"))
	}))
	defer envoySrv.Close()
	meshSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("# TYPE aoc_mesh_gateway_up gauge\naoc_mesh_gateway_up 1\naoc_mesh_gateway_GrpcCalls 19\n"))
	}))
	defer meshSrv.Close()
	t.Setenv("AOC_ENVOY_PROM_URL", envoySrv.URL)
	t.Setenv("AOC_MESH_GATEWAY_PROM_URL", meshSrv.URL)
	var statsCalls atomic.Int32
	dockerSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/containers/json") {
			_ = json.NewEncoder(w).Encode([]map[string]any{{"Id": "1111222233334444", "Names": []string{"/aoc-world-riverlands"}, "Image": "sandal-world", "State": "running", "Status": "Up 5m"}})
			return
		}
		if strings.HasSuffix(r.URL.Path, "/stats") && r.URL.Query().Get("one-shot") == "true" {
			call := uint64(statsCalls.Add(1))
			_ = json.NewEncoder(w).Encode(map[string]any{
				"cpu_stats":    map[string]any{"cpu_usage": map[string]any{"total_usage": call * 15000000}, "system_cpu_usage": call * 100000000, "online_cpus": 2},
				"precpu_stats": map[string]any{"cpu_usage": map[string]any{"total_usage": 0}, "system_cpu_usage": 0},
				"memory_stats": map[string]any{"usage": 209715200, "limit": 1073741824},
				"pids_stats":   map[string]any{"current": 18},
			})
		}
	}))
	defer dockerSrv.Close()
	waFake := &fakeWorldAdminClient{notFoundMode: true}
	srv := NewRouter(Config{WorldAdminClient: waFake, WorldStreamClient: &fakeWorldStreamClient{}, DockerClient: dockerSrv.Client()})
	srv.dockerClient = &http.Client{Transport: rewriteHostTransport{base: dockerSrv.Client().Transport, targetURL: dockerSrv.URL}}
	recCont := serveAuthed(srv, http.MethodGet, "/api/v1/gm/containers", "", nil, nil)
	if recCont.Code != http.StatusOK || !strings.Contains(recCont.Body.String(), `"prometheus"`) || !strings.Contains(recCont.Body.String(), `"memory_mb":200`) {
		t.Fatalf("GET /api/v1/gm/containers status=%d body=%s", recCont.Code, recCont.Body.String())
	}
	recProm := serveAuthed(srv, http.MethodGet, "/api/v1/gm/metrics", "", nil, nil)
	if recProm.Code != http.StatusOK || !strings.Contains(recProm.Body.String(), `"world_admin_grpc"`) {
		t.Fatalf("GET /api/v1/gm/metrics status=%d body=%s", recProm.Code, recProm.Body.String())
	}
	if recOM := serveAuthed(srv, http.MethodGet, "/metrics", "", nil, nil); recOM.Code != http.StatusOK || !strings.Contains(recOM.Body.String(), "aoc_admin_gateway_requests_total") {
		t.Fatalf("GET /metrics status=%d body=%s", recOM.Code, recOM.Body.String())
	}
	assertGMWorkItemFallbacks(t, srv)
	assertMultiWorkerFanOutAndRouting(t)
}

func assertMultiWorkerFanOutAndRouting(t *testing.T) {
	t.Helper()
	parsed := parseWorldWorkersSpec(defaultFiveRegionalWorkersSpec)
	if len(parsed) != 5 || parsed[0].ContainerName != "aoc-world-riverlands" || parsed[1].ScopeKey != "sandsquall" || parsed[4].ContainerName != "aoc-world-turquoise-sea" {
		t.Fatalf("unexpected parseWorldWorkersSpec: len=%d %#v", len(parsed), parsed)
	}
	single := parseWorldWorkersSpec(defaultSingleWorldWorkerSpec)
	if len(single) != 1 || single[0].ScopeKind != "world" || single[0].ScopeLabel != "Verra (All Regions)" || single[0].ContainerName != "aoc-world-riverlands" {
		t.Fatalf("unexpected single worker spec: %#v", single)
	}
	waRvr := &fakeWorldAdminClient{notFoundMode: true}
	waSnd := &fakeWorldAdminClient{notFoundMode: false}
	wsRvr := &fakeWorldStreamClient{entities: []*worldadminv1.Entity{{Id: 100, Kind: worldadminv1.EntityKind_ENTITY_KIND_NPC, Name: "Lionhold Guard", XCm: -847000, YCm: -538000, HealthPct: 100}}}
	wsSnd := &fakeWorldStreamClient{entities: []*worldadminv1.Entity{{Id: 100, Kind: worldadminv1.EntityKind_ENTITY_KIND_NPC, Name: "Sandsquall Scorpion", XCm: -1783000, YCm: 809000, HealthPct: 100, State: 4}}}
	srv := NewRouter(Config{
		WorldWorkers: []*WorldWorkerEndpoint{
			{WorkerID: "world-riverlands", ScopeKey: "riverlands", ScopeKind: "region", ContainerName: "aoc-world-riverlands", AdminClient: waRvr, StreamClient: wsRvr},
			{WorkerID: "world-sandsquall", ScopeKey: "sandsquall", ScopeKind: "region", ContainerName: "aoc-world-sandsquall", AdminClient: waSnd, StreamClient: wsSnd},
		},
	})
	recWorkers := serveAuthed(srv, http.MethodGet, "/api/v1/gm/workers", "", nil, nil)
	if recWorkers.Code != http.StatusOK || !strings.Contains(recWorkers.Body.String(), `"world-sandsquall"`) || !strings.Contains(recWorkers.Body.String(), `"ONLINE"`) {
		t.Fatalf("GET /api/v1/gm/workers status=%d body=%s", recWorkers.Code, recWorkers.Body.String())
	}
	recState := serveAuthed(srv, http.MethodGet, "/api/v1/world/state", "", nil, nil)
	if recState.Code != http.StatusOK || !strings.Contains(recState.Body.String(), `"riverlands:npc:100"`) || !strings.Contains(recState.Body.String(), `"sandsquall:monster:100"`) {
		t.Fatalf("multi-worker collision-free state failed: status=%d body=%s", recState.Code, recState.Body.String())
	}
	recSpawn := serveAuthed(srv, http.MethodPost, "/api/v1/gm/spawn_npc", "", map[string]string{"Content-Type": "application/json"}, []byte(`{"command":"spawn_npc","npc":"Scorpion","place":"Wildport","level":20}`))
	if recSpawn.Code != http.StatusOK || !strings.Contains(recSpawn.Body.String(), `"worker_id":"world-sandsquall"`) {
		t.Fatalf("regional spawn_npc routing failed: status=%d body=%s", recSpawn.Code, recSpawn.Body.String())
	}
	wsRvr.entities = []*worldadminv1.Entity{{Id: 202, Kind: worldadminv1.EntityKind_ENTITY_KIND_NPC, Name: "Winstead Metropolis Guard", XCm: -644519, YCm: 376773, HealthPct: 100}}
	_ = serveAuthed(srv, http.MethodPut, "/api/v1/world/nodes/Winstead", "", map[string]string{"Content-Type": "application/json"}, []byte(`{"level":6}`))
	recPruned := serveAuthed(srv, http.MethodGet, "/api/v1/world/state", "", nil, nil)
	if strings.Contains(recPruned.Body.String(), `"riverlands:npc:100"`) || !strings.Contains(recPruned.Body.String(), `"riverlands:npc:202"`) {
		t.Fatalf("expected old-tier NPC riverlands:npc:100 pruned and riverlands:npc:202 present, got %s", recPruned.Body.String())
	}
	assertStepPlayerBorderRegionalHandover(t, srv)
}

func assertStepPlayerBorderRegionalHandover(t *testing.T, srv *Server) {
	t.Helper()
	hdrs := map[string]string{"Content-Type": "application/json"}
	for i, tc := range []struct {
		body, wantReg, wantWorker string
		wantCnt                   float64
		wantSuppr                 bool
	}{
		{`{"command":"step_player_border","character":"BorderHero","x_cm":-454000,"y_cm":-568000}`, "riverlands", "aoc-world-riverlands", 0, false},
		{`{"command":"step_player_border","character":"BorderHero","x_cm":-454000,"y_cm":2400,"border_distance_cm":2400}`, "riverlands", "aoc-world-riverlands", 0, true},
		{`{"command":"step_player_border","character":"BorderHero","x_cm":-619803,"y_cm":873514}`, "riverlands", "aoc-world-riverlands", 0, true},
		{`{"command":"step_player_border","character":"BorderHero","x_cm":-619803,"y_cm":873514}`, "riverlands", "aoc-world-riverlands", 0, true},
		{`{"command":"step_player_border","character":"BorderHero","x_cm":-619803,"y_cm":873514}`, "sandsquall", "aoc-world-sandsquall", 1, false},
		{`{"command":"step_player_border","character":"BorderHero","x_cm":-1174491,"y_cm":-694106}`, "sandsquall", "aoc-world-sandsquall", 1, true},
	} {
		rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "", hdrs, []byte(tc.body))
		var doc map[string]any
		_ = json.Unmarshal(rec.Body.Bytes(), &doc)
		if rec.Code != http.StatusOK || doc["authoritative_region"] != tc.wantReg || doc["authoritative_worker"] != tc.wantWorker || doc["handover_count"] != tc.wantCnt || doc["hysteresis_suppressed"] != tc.wantSuppr {
			t.Fatalf("step_player_border[%d] status=%d doc=%#v", i, rec.Code, doc)
		}
	}
}

func assertGMWorkItemFallbacks(t *testing.T, srv *Server) {
	t.Helper()
	hdrs := map[string]string{"Content-Type": "application/json"}
	for _, tc := range []struct{ path, body string }{
		{"/api/v1/gm/give_currency", `{"command":"give_currency","character":"OfflineArchon","gold":5,"silver":10,"copper":50}`},
		{"/api/v1/gm/give_item", `{"command":"give_item","character":"OfflineArchon","item":"6064629100001","count":2,"rarity":"Epic"}`},
		{"/api/v1/gm/teleport", `{"command":"teleport","character":"OfflineArchon","target_player":"OfflineTarget","place":"Winstead","x_cm":-644519.11,"y_cm":376773.24,"z_cm":12012.23}`},
		{"/api/v1/gm/spawn_npc", `{"command":"spawn_npc","npc":"DesignData_NPC_Goblin","place":"Winstead","level":10}`},
	} {
		if rec := serveAuthed(srv, http.MethodPost, tc.path, "", hdrs, []byte(tc.body)); rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"status":"executed"`) {
			t.Fatalf("POST %s status=%d body=%s", tc.path, rec.Code, rec.Body.String())
		}
	}
}
func writeUvarint(buf *bytes.Buffer, v uint64) {
	var tmp [10]byte
	buf.Write(tmp[:binary.PutUvarint(tmp[:], v)])
}

type fakeWorldAdminClient struct {
	worldadminv1.WorldAdminServiceClient
	listPlayersCalls atomic.Int32
	failNextTeleport int32
	notFoundMode     bool
}

func (f *fakeWorldAdminClient) ListPlayers(context.Context, *worldadminv1.ListPlayersRequest, ...grpc.CallOption) (*worldadminv1.ListPlayersResponse, error) {
	f.listPlayersCalls.Add(1)
	return &worldadminv1.ListPlayersResponse{Players: []*worldadminv1.Player{{Name: "TargetHero", CharacterId: "char-101", Position: &worldadminv1.Position{X: -847000, Y: -538000, Z: 11524}}}}, nil
}
func (f *fakeWorldAdminClient) ListPlaces(context.Context, *worldadminv1.ListPlacesRequest, ...grpc.CallOption) (*worldadminv1.ListPlacesResponse, error) {
	return &worldadminv1.ListPlacesResponse{Places: []*worldadminv1.Place{{Name: "lionhold", DisplayName: "Lionhold", Position: &worldadminv1.Position{X: -847992, Y: -538867, Z: 11524}, Kind: "spawn"}}}, nil
}
func (f *fakeWorldAdminClient) Teleport(_ context.Context, req *worldadminv1.TeleportRequest, _ ...grpc.CallOption) (*worldadminv1.TeleportResponse, error) {
	if f.notFoundMode {
		return nil, status.Error(codes.NotFound, "character not online")
	}
	if atomic.AddInt32(&f.failNextTeleport, -1) >= 0 {
		return nil, status.Error(codes.Unavailable, "transient world admin unavailability")
	}
	return &worldadminv1.TeleportResponse{Character: req.GetCharacter(), To: &worldadminv1.Position{X: -847000, Y: -538000, Z: 11524}}, nil
}
func (f *fakeWorldAdminClient) GiveMoney(_ context.Context, req *worldadminv1.GiveMoneyRequest, _ ...grpc.CallOption) (*worldadminv1.GiveMoneyResponse, error) {
	if f.notFoundMode {
		return nil, status.Error(codes.NotFound, "character not online")
	}
	return &worldadminv1.GiveMoneyResponse{Character: req.GetCharacter(), Balance: req.GetCopper(), Added: req.GetCopper()}, nil
}
func (f *fakeWorldAdminClient) GiveItem(_ context.Context, req *worldadminv1.GiveItemRequest, _ ...grpc.CallOption) (*worldadminv1.GiveItemResponse, error) {
	if f.notFoundMode {
		return nil, status.Error(codes.InvalidArgument, "item or character not in world")
	}
	return &worldadminv1.GiveItemResponse{Character: req.GetCharacter(), Item: req.GetItem(), ItemGuid: 609166844926386, Container: "Backpack", Count: req.GetCount(), Rarity: req.GetRarity()}, nil
}
func (f *fakeWorldAdminClient) SpawnNpc(_ context.Context, req *worldadminv1.SpawnNpcRequest, _ ...grpc.CallOption) (*worldadminv1.SpawnNpcResponse, error) {
	if f.notFoundMode {
		return nil, status.Error(codes.NotFound, "npc asset not in world")
	}
	return &worldadminv1.SpawnNpcResponse{Id: 9001, Npc: req.GetNpc(), DisplayName: req.GetNpc(), AssetGuid: 1090001, Level: req.GetLevel(), Position: &worldadminv1.Position{X: -847992, Y: -538867, Z: 11524}}, nil
}
func TestTargetTickHzVsCurrentTickHzAndUDPTelemetry(t *testing.T) {
	srv := NewRouter(Config{WorldAdminAddr: "world:50061", WorldStreamAddr: "world:50062"})
	hdrs := map[string]string{"Content-Type": "application/json", "Accept": "application/json"}

	dynBody := []byte(`{"command":"set_worker_tick_rate","targetId":"aoc-world-riverlands","params":{"worker_id":"aoc-world-riverlands","tick_hz":-1,"tick_mode":"dynamic"}}`)
	if rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "gm-architect-dev-token", hdrs, dynBody); rec.Code != http.StatusOK {
		t.Fatalf("set dynamic tick_rate status = %d; body = %s", rec.Code, rec.Body.String())
	}
	assertWorkerTickFields(t, srv, "dynamic", -1, 30, true)

	manBody := []byte(`{"command":"set_worker_tick_rate","targetId":"aoc-world-riverlands","params":{"worker_id":"aoc-world-riverlands","tick_hz":20}}`)
	if rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "gm-architect-dev-token", hdrs, manBody); rec.Code != http.StatusOK {
		t.Fatalf("set manual 20 Hz status = %d; body = %s", rec.Code, rec.Body.String())
	}
	assertWorkerTickFields(t, srv, "manual", 20, 20, false)
}
