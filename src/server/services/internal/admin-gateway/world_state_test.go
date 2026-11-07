// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"context"
	"encoding/json"
	"math"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	worldstream "aocrop/services/internal/world-stream"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

type capturedTimeSyncCall struct {
	npc       string
	yaw       float32
	x, y, z   float64
	hasPoint  bool
	hasZ      bool
	httpQuery string
}

type recordingTimeAdminClient struct {
	fakeWorldAdminClient
	mu    sync.Mutex
	calls []capturedTimeSyncCall
}

func (r *recordingTimeAdminClient) SpawnNpc(_ context.Context, req *worldadminv1.SpawnNpcRequest, _ ...grpc.CallOption) (*worldadminv1.SpawnNpcResponse, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	call := capturedTimeSyncCall{npc: req.GetNpc(), yaw: req.GetYawDegrees()}
	if pt := req.GetPoint(); pt != nil {
		call.hasPoint = true
		call.x, call.y, call.z, call.hasZ = pt.GetX(), pt.GetY(), pt.GetZ(), pt.GetHasZ()
	}
	r.calls = append(r.calls, call)
	return &worldadminv1.SpawnNpcResponse{Npc: req.GetNpc()}, nil
}

func (r *recordingTimeAdminClient) lastCall() (capturedTimeSyncCall, int) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.calls) == 0 {
		return capturedTimeSyncCall{}, 0
	}
	return r.calls[len(r.calls)-1], len(r.calls)
}

type toggleableStreamClient struct {
	worldadminv1.WorldStreamServiceClient
	failSubscribe atomic.Bool
	failRecv      atomic.Bool
}

type failingStreamRecv struct {
	grpc.ServerStreamingClient[worldadminv1.SubscribeResponse]
}

func (f *failingStreamRecv) Recv() (*worldadminv1.SubscribeResponse, error) {
	return nil, status.Error(codes.Unavailable, "worker stream closed on container restart")
}

func (t *toggleableStreamClient) Subscribe(_ context.Context, _ *worldadminv1.SubscribeRequest, _ ...grpc.CallOption) (grpc.ServerStreamingClient[worldadminv1.SubscribeResponse], error) {
	if t.failSubscribe.Load() {
		return nil, status.Error(codes.Unavailable, "worker unreachable")
	}
	if t.failRecv.Load() {
		return &failingStreamRecv{}, nil
	}
	return &fakeStreamRecv{entities: []*worldadminv1.Entity{{Id: 42, Kind: worldadminv1.EntityKind_ENTITY_KIND_NPC, Name: "TestGuard", HealthPct: 100}}}, nil
}

func TestVerraCalendarFourSeasonsAndNoFabricatedMonths(t *testing.T) {
	fixedNow := time.Date(2026, 11, 7, 12, 0, 0, 0, time.UTC)
	srv := NewRouter(Config{Now: func() time.Time { return fixedNow }})
	bannedMonths := []string{"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"}

	cases := []struct {
		dayOfYear     int
		wantSeason    string
		wantSeasonDay int
	}{
		{1, "Spring", 1},
		{90, "Spring", 90},
		{91, "Summer", 1},
		{180, "Summer", 90},
		{181, "Autumn", 1},
		{270, "Autumn", 90},
		{271, "Winter", 1},
		{360, "Winter", 90},
	}
	for _, tc := range cases {
		srv.timeOfDayMu.Lock()
		srv.timeOfDayHours, srv.timeOfDayUpdatedAt = 12.0, fixedNow
		srv.calendarAnchorYear, srv.calendarAnchorDay = 4, tc.dayOfYear
		srv.timeOfDayMu.Unlock()
		cal := srv.CalendarAt(fixedNow)
		if cal.Year != 4 || cal.DayOfYear != tc.dayOfYear || cal.Season != tc.wantSeason || cal.DayOfSeason != tc.wantSeasonDay {
			t.Fatalf("CalendarAt(day=%d) = %+v, want Year=4 DayOfYear=%d Season=%s DayOfSeason=%d", tc.dayOfYear, cal, tc.dayOfYear, tc.wantSeason, tc.wantSeasonDay)
		}
		lowMonth := strings.ToLower(cal.Month)
		for _, banned := range bannedMonths {
			if strings.Contains(lowMonth, banned) {
				t.Fatalf("CalendarAt(day=%d) Month=%q contains fabricated Gregorian month %q", tc.dayOfYear, cal.Month, banned)
			}
		}
	}
}

func TestWorkerTimeAndCalendarSyncBroadcastReconnectAndRegister(t *testing.T) {
	var httpMu sync.Mutex
	var healthzQueries []string
	httpSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/healthz" && r.URL.Query().Get("time_of_day") != "" {
			httpMu.Lock()
			healthzQueries = append(healthzQueries, r.URL.RawQuery)
			httpMu.Unlock()
		}
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	}))
	defer httpSrv.Close()
	t.Setenv("AOC_WORLD_METRICS_URL", httpSrv.URL+"/metrics")

	fixedNow := time.Date(2026, 11, 7, 16, 0, 0, 0, time.UTC)
	waRvr, waSnd := &recordingTimeAdminClient{}, &recordingTimeAdminClient{}
	wsRvr, wsSnd := &toggleableStreamClient{}, &toggleableStreamClient{}
	srv := NewRouter(Config{
		Now: func() time.Time { return fixedNow },
		WorldWorkers: []*WorldWorkerEndpoint{
			{WorkerID: "world-riverlands", ScopeKey: "riverlands", ScopeKind: "region", ContainerName: "aoc-world-riverlands", AdminAddr: httpSrv.Listener.Addr().String(), AdminClient: waRvr, StreamClient: wsRvr},
			{WorkerID: "world-sandsquall", ScopeKey: "sandsquall", ScopeKind: "region", ContainerName: "aoc-world-sandsquall", AdminAddr: httpSrv.Listener.Addr().String(), AdminClient: waSnd, StreamClient: wsSnd},
		},
	})

	hdrs := map[string]string{"Content-Type": "application/json", "Accept": "application/json"}
	putBody := []byte(`{"hours": 19.25, "year": 3, "day_of_year": 215}`)
	if rec := serveAuthed(srv, http.MethodPut, "/api/v1/gm/world/time-of-day", "gm-architect-dev-token", hdrs, putBody); rec.Code != http.StatusOK {
		t.Fatalf("PUT /api/v1/gm/world/time-of-day status=%d body=%s", rec.Code, rec.Body.String())
	}

	assertWorkerTimeSyncCall(t, "riverlands-broadcast", waRvr, 19.25, 3, 215)
	assertWorkerTimeSyncCall(t, "sandsquall-broadcast", waSnd, 19.25, 3, 215)
	assertHealthzTimeQuery(t, &httpMu, &healthzQueries, "time_of_day=19.2500&year=3&day_of_year=215")

	verifyStreamDisconnectResetsAndResyncs(t, srv, waRvr, wsRvr)
	verifyRegisterWorkerTriggersTimeSync(t, srv, waSnd, httpSrv.Listener.Addr().String())
}

func assertWorkerTimeSyncCall(t *testing.T, label string, cli *recordingTimeAdminClient, wantHour float64, wantYear, wantDay int) {
	t.Helper()
	call, count := cli.lastCall()
	if count == 0 || call.npc != "gm.SetTimeOfDay" || !call.hasPoint || !call.hasZ {
		t.Fatalf("%s: expected gm.SetTimeOfDay with Point(HasZ=true), got count=%d call=%+v", label, count, call)
	}
	if math.Abs(call.x-wantHour) > 0.01 || int(call.y) != wantYear || int(call.z) != wantDay {
		t.Fatalf("%s: Point=(%.4f, %.0f, %.0f), want (%.4f, %d, %d)", label, call.x, call.y, call.z, wantHour, wantYear, wantDay)
	}
}

func assertHealthzTimeQuery(t *testing.T, mu *sync.Mutex, queries *[]string, wantSubstr string) {
	t.Helper()
	mu.Lock()
	defer mu.Unlock()
	for _, q := range *queries {
		if strings.Contains(q, wantSubstr) {
			return
		}
	}
	t.Fatalf("expected /healthz query containing %q, got %v", wantSubstr, *queries)
}

func verifyStreamDisconnectResetsAndResyncs(t *testing.T, srv *Server, waRvr *recordingTimeAdminClient, wsRvr *toggleableStreamClient) {
	t.Helper()
	_, beforeCount := waRvr.lastCall()
	wsRvr.failRecv.Store(true)
	_ = serveAuthed(srv, http.MethodGet, "/api/v1/world/state?fresh=1", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)

	srv.mu.Lock()
	syncedAfterErr := srv.workerMeta["world-riverlands"].SyncedTimeAt
	srv.mu.Unlock()
	if !syncedAfterErr.IsZero() {
		t.Fatalf("expected SyncedTimeAt reset to zero after stream Recv error, got %v", syncedAfterErr)
	}

	wsRvr.failRecv.Store(false)
	_ = serveAuthed(srv, http.MethodGet, "/api/v1/world/state?fresh=1", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
	_, afterCount := waRvr.lastCall()
	if afterCount <= beforeCount {
		t.Fatalf("expected reconnecting worker to re-sync time and calendar (calls before=%d, after=%d)", beforeCount, afterCount)
	}
	assertWorkerTimeSyncCall(t, "riverlands-reconnect", waRvr, 19.25, 3, 215)
}

func verifyRegisterWorkerTriggersTimeSync(t *testing.T, srv *Server, waSnd *recordingTimeAdminClient, adminAddr string) {
	t.Helper()
	_, beforeCount := waSnd.lastCall()
	regBody, _ := json.Marshal(map[string]any{
		"worker_id":      "world-sandsquall",
		"scope_key":      "sandsquall",
		"scope_kind":     "region",
		"container_name": "aoc-world-sandsquall",
		"admin_addr":     adminAddr,
	})
	rec := serveAuthed(srv, http.MethodPost, "/api/v1/mesh/workers/register", "gm-operator-dev-token", map[string]string{"Content-Type": "application/json"}, regBody)
	if rec.Code != http.StatusOK {
		t.Fatalf("POST /api/v1/mesh/workers/register status=%d body=%s", rec.Code, rec.Body.String())
	}
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if _, afterCount := waSnd.lastCall(); afterCount > beforeCount {
			assertWorkerTimeSyncCall(t, "sandsquall-register", waSnd, 19.25, 3, 215)
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("expected worker registration to trigger async time/calendar sync on world-sandsquall")
}

func TestEnvoyClusterResponseCodeClassesAndFallback2xx(t *testing.T) {
	envoySrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(strings.Join([]string{
			"# TYPE envoy_cluster_upstream_rq_total counter",
			`envoy_cluster_upstream_rq_total{envoy_cluster_name="auth_grpc"} 100`,
			`envoy_cluster_upstream_rq_xx{envoy_cluster_name="auth_grpc",envoy_response_code_class="2"} 85`,
			`envoy_cluster_upstream_rq_xx{envoy_cluster_name="auth_grpc",envoy_response_code_class="3"} 5`,
			`envoy_cluster_upstream_rq_xx{envoy_cluster_name="auth_grpc",envoy_response_code_class="4"} 7`,
			`envoy_cluster_upstream_rq_xx{envoy_cluster_name="auth_grpc",envoy_response_code_class="5"} 3`,
			`envoy_cluster_upstream_rq_total{envoy_cluster_name="world_admin_grpc"} 50`,
			`envoy_cluster_upstream_rq_xx{envoy_cluster_name="world_admin_grpc",envoy_response_code_class="4"} 4`,
			`envoy_cluster_upstream_rq_xx{envoy_cluster_name="world_admin_grpc",envoy_response_code_class="5"} 1`,
			`envoy_http_downstream_rq_total{} 200`,
		}, "\n")))
	}))
	defer envoySrv.Close()
	t.Setenv("AOC_ENVOY_PROM_URL", envoySrv.URL)

	st, clusters, downRq, _ := scrapeEnvoyPrometheus(context.Background())
	if st.Status != "UP" || downRq != 200 {
		t.Fatalf("scrapeEnvoyPrometheus status=%s downRq=%v, want UP and 200", st.Status, downRq)
	}
	authCm := clusters["auth_grpc"]
	if authCm.Responses2xxTotal != 85 || authCm.Responses3xxTotal != 5 || authCm.Errors4xxTotal != 7 || authCm.Errors5xxTotal != 3 {
		t.Fatalf("auth_grpc metrics = %+v, want 2xx=85 3xx=5 4xx=7 5xx=3", authCm)
	}
	worldCm := clusters["world_admin_grpc"]
	if worldCm.Responses2xxTotal != 45 || worldCm.Errors4xxTotal != 4 || worldCm.Errors5xxTotal != 1 {
		t.Fatalf("world_admin_grpc fallback 2xx metrics = %+v, want 2xx=45 4xx=4 5xx=1", worldCm)
	}
}

type customStatsStreamClient struct {
	fakeWorldStreamClient
	stats *worldadminv1.StreamStats
}

func (c *customStatsStreamClient) GetStatus(_ context.Context, _ *worldadminv1.GetStatusRequest, _ ...grpc.CallOption) (*worldadminv1.GetStatusResponse, error) {
	return &worldadminv1.GetStatusResponse{Subscribers: 3, Stats: c.stats}, nil
}

func assertClusterInternalMetricsTargetURL(t *testing.T, label, u string) {
	t.Helper()
	if u == "" || u == "/" {
		t.Fatalf("%s: metric target URL is empty or '/': %q", label, u)
	}
	low := strings.ToLower(u)
	if strings.Contains(low, "127.0.0.1") || strings.Contains(low, "localhost") {
		t.Fatalf("%s: metric target URL %q contains forbidden loopback host", label, u)
	}
	if !strings.HasPrefix(low, "http://") && !strings.HasPrefix(low, "https://") && !strings.HasPrefix(low, "postgres://") {
		t.Fatalf("%s: metric target URL %q is bare host:port without scheme", label, u)
	}
	if !strings.HasSuffix(low, "/metrics") && !strings.HasSuffix(low, "/stats/prometheus") {
		t.Fatalf("%s: metric target URL %q must end with /metrics or /stats/prometheus", label, u)
	}
}

func TestClusterInternalMetricsTargetURLsOnly(t *testing.T) {
	for _, spec := range controlPlaneMetricsSpecs {
		assertClusterInternalMetricsTargetURL(t, "spec:"+spec.name+":defURL", spec.defURL)
		for _, u := range spec.urls {
			assertClusterInternalMetricsTargetURL(t, "spec:"+spec.name+":url", u)
		}
	}
	meshSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("# TYPE aoc_mesh_gateway_up gauge\naoc_mesh_gateway_up 1\naoc_mesh_gateway_GrpcCalls 7\n"))
	}))
	defer meshSrv.Close()
	t.Setenv("AOC_MESH_GATEWAY_LOCAL_ADDR", meshSrv.Listener.Addr().String())

	meshSt, meshSamples := scrapeMeshGatewayPrometheus(context.Background())
	if meshSt.URL != "http://mesh-gateway:8082/metrics" {
		t.Fatalf("scrapeMeshGatewayPrometheus URL=%q, want http://mesh-gateway:8082/metrics", meshSt.URL)
	}
	if meshSt.Status != "UP" || meshSamples["aoc_mesh_gateway_GrpcCalls"] != 7 {
		t.Fatalf("expected dial fallback to reach local listener without mutating URL, got status=%s samples=%v", meshSt.Status, meshSamples)
	}

	ws := &customStatsStreamClient{stats: &worldadminv1.StreamStats{TickMs: 12, PublishMs: 2, PublishMaxMs: 4, Npcs: 100}}
	srv := NewRouter(Config{
		WorldStreamAddr: "world:50062",
		WorldWorkers: []*WorldWorkerEndpoint{
			{WorkerID: "world-riverlands", ScopeKey: "riverlands", ScopeKind: "region", ContainerName: "aoc-world-riverlands", AdminAddr: "world:50061", StreamAddr: "world:50062", Connected: true, StreamClient: ws},
			{WorkerID: "world-sandsquall", ScopeKey: "sandsquall", ScopeKind: "region", ContainerName: "aoc-world-sandsquall", AdminAddr: "world-sandsquall:50061", StreamAddr: "world-sandsquall:50062", Connected: true, StreamClient: ws},
		},
	})
	recMetrics := serveAuthed(srv, http.MethodGet, "/api/v1/gm/metrics", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
	if recMetrics.Code != http.StatusOK {
		t.Fatalf("GET /api/v1/gm/metrics status=%d body=%s", recMetrics.Code, recMetrics.Body.String())
	}
	var mBody struct {
		Targets []PrometheusTargetStatus `json:"targets"`
	}
	if err := json.Unmarshal(recMetrics.Body.Bytes(), &mBody); err != nil || len(mBody.Targets) == 0 {
		t.Fatalf("decode /api/v1/gm/metrics err=%v targets=%d", err, len(mBody.Targets))
	}
	for _, tgt := range mBody.Targets {
		assertClusterInternalMetricsTargetURL(t, "target:"+tgt.Name, tgt.URL)
	}
	recCont := serveAuthed(srv, http.MethodGet, "/api/v1/gm/containers", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
	var cBody struct {
		Containers []ContainerMetric `json:"containers"`
	}
	if err := json.Unmarshal(recCont.Body.Bytes(), &cBody); err != nil || len(cBody.Containers) == 0 {
		t.Fatalf("decode /api/v1/gm/containers err=%v count=%d", err, len(cBody.Containers))
	}
	for _, c := range cBody.Containers {
		assertClusterInternalMetricsTargetURL(t, "container:"+c.Name, c.ScrapeTarget)
	}
}

func TestWorldStatusNineCategoryMutuallyExclusivePopulation(t *testing.T) {
	coal := worldstream.NewCoalescer(10 * time.Millisecond)
	seedEntities := []worldstream.EntitySnapshot{
		{ID: "p1", Kind: "player", Name: "HeroOne", Zone: "riverlands"},
		{ID: "p2", Kind: "player", Name: "HeroTwo", Zone: "riverlands"},
		{ID: "n1", Kind: "npc", Name: "Lionhold Guard", Zone: "riverlands"},
		{ID: "n2", Kind: "npc", Name: "Vaelune Merchant", Zone: "riverlands"},
		{ID: "n3", Kind: "npc", Name: "Samias Hope Innkeeper", Zone: "riverlands"},
		{ID: "m1", Kind: "monster", Name: "Risen Skeleton", Zone: "riverlands"},
		{ID: "m2", Kind: "npc", Name: "Ashen Goblin Raider", Zone: "riverlands"},
		{ID: "m3", Kind: "monster", Name: "Corrupted Wolf", Zone: "riverlands"},
		{ID: "m4", Kind: "monster", Name: "Highwayman Bandit", Zone: "riverlands"},
		{ID: "h1", Kind: "huntable", Name: "Riverlands Stag", Zone: "riverlands"},
		{ID: "h2", Kind: "npc", Name: "Hunting_Deer", Zone: "riverlands"},
		{ID: "h3", Kind: "monster", Name: "Wild Buck [Hunting|Common]", NpcSubkind: "huntable", Zone: "riverlands"},
		{ID: "t1", Kind: "resource_node", Name: "Weeping Willow [Lumberjacking|Common]", Zone: "riverlands"},
		{ID: "t2", Kind: "resource_node", Name: "Oak Tree", ResourceType: "lumberjacking", Zone: "riverlands"},
		{ID: "r1", Kind: "resource_node", Name: "Granite Outcrop [Stonecutting|Uncommon]", Zone: "riverlands"},
		{ID: "r2", Kind: "resource_node", Name: "Basalt Deposit", ResourceType: "stonecutting", Zone: "riverlands"},
		{ID: "r3", Kind: "resource_node", Name: "Limestone Boulder", ResourceType: "rock", Zone: "riverlands"},
		{ID: "o1", Kind: "resource_node", Name: "Copper Vein [Mining|Rare]", Zone: "riverlands"},
		{ID: "o2", Kind: "resource_node", Name: "Ruby Geode", ResourceType: "mining", Zone: "riverlands"},
		{ID: "pl1", Kind: "resource_node", Name: "Spiritbloom [Herbalism|Heroic]", Zone: "riverlands"},
		{ID: "pl2", Kind: "resource_node", Name: "Moonbell Herb", ResourceType: "herbalism", Zone: "riverlands"},
		{ID: "pl3", Kind: "resource_node", Name: "Wild Daffodil", ResourceType: "plant", Zone: "riverlands"},
		{ID: "pl4", Kind: "resource_node", Name: "Yarrow Flower", ResourceType: "herbalism", Zone: "riverlands"},
		{ID: "mt1", Kind: "mount", Name: "Royal Steed", Zone: "riverlands"},
		{ID: "lp1", Kind: "loot_pile", Name: "Dropped Chest", Zone: "riverlands"},
	}
	for _, e := range seedEntities {
		coal.UpsertEntity(e)
	}
	// gRPC stats reports Npcs=100 (which includes 4 monsters + 3 huntables + 93 pure NPCs) and ResourceNodes=15 (4 more trees than the 11 snapshot nodes).
	ws := &customStatsStreamClient{stats: &worldadminv1.StreamStats{TickMs: 10, PublishMs: 1, PublishMaxMs: 2, Players: 2, Npcs: 100, ResourceNodes: 15, Mounts: 1}}
	srv := NewRouter(Config{
		Coalescer: coal,
		WorldWorkers: []*WorldWorkerEndpoint{
			{WorkerID: "world-riverlands", ScopeKey: "riverlands", ScopeKind: "region", ContainerName: "aoc-world-riverlands", AdminAddr: "world:50061", StreamAddr: "world:50062", StreamClient: ws},
		},
	})
	rec := serveAuthed(srv, http.MethodGet, "/api/v1/world/status", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /api/v1/world/status status=%d body=%s", rec.Code, rec.Body.String())
	}
	var status map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &status); err != nil {
		t.Fatalf("decode /api/v1/world/status: %v", err)
	}
	want := worldPopulationCounts{Players: 2, Npcs: 93, Monsters: 4, Huntables: 3, Trees: 6, Rocks: 3, OreGems: 2, Plants: 4, Mounts: 1}
	assertWorldPopulationInvariants(t, "top-level", status, want)
	workers, _ := status["workers"].([]any)
	if len(workers) != 1 {
		t.Fatalf("expected 1 worker in status.workers, got %d", len(workers))
	}
	wMap, _ := workers[0].(map[string]any)
	assertWorldPopulationInvariants(t, "worker[0]", wMap, want)
}

func assertWorldPopulationInvariants(t *testing.T, label string, m map[string]any, want worldPopulationCounts) {
	t.Helper()
	getInt := func(k string) int { v, _ := extractIntParam(m, k); return v }
	p, n, mo, h := getInt("players"), getInt("npcs"), getInt("monsters"), getInt("huntables")
	tr, ro, og, pl, mt := getInt("trees"), getInt("rocks"), getInt("ore_gems"), getInt("plants"), getInt("mounts")
	res, tot := getInt("resource_nodes"), getInt("total_entities")
	if p != want.Players || n != want.Npcs || mo != want.Monsters || h != want.Huntables || tr != want.Trees || ro != want.Rocks || og != want.OreGems || pl != want.Plants || mt != want.Mounts {
		t.Fatalf("%s: got (players=%d npcs=%d monsters=%d huntables=%d trees=%d rocks=%d ore_gems=%d plants=%d mounts=%d), want %+v",
			label, p, n, mo, h, tr, ro, og, pl, mt, want)
	}
	if res != tr+ro+og+pl {
		t.Fatalf("%s: resource_nodes=%d != trees+rocks+ore_gems+plants (%d)", label, res, tr+ro+og+pl)
	}
	wantTot := p + n + mo + h + tr + ro + og + pl + mt
	if tot != wantTot {
		t.Fatalf("%s: total_entities=%d != sum of 9 categories (%d)", label, tot, wantTot)
	}
}

func TestDynamicTickModeDefaultScrapedHzAndAliasClearing(t *testing.T) {
	var tickQueriesMu sync.Mutex
	var tickQueries []string
	promSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/metrics" {
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte("# TYPE aoc_world_tick_hz gauge\naoc_world_tick_hz 52.5\n"))
			return
		}
		if r.URL.Path == "/admin/tick_hz" {
			tickQueriesMu.Lock()
			tickQueries = append(tickQueries, r.URL.RawQuery)
			tickQueriesMu.Unlock()
		}
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	}))
	defer promSrv.Close()
	t.Setenv("AOC_WORLD_PROM_URL", promSrv.URL+"/metrics")
	t.Setenv("AOC_WORLD_METRICS_URL", promSrv.URL+"/metrics")

	srv := NewRouter(Config{
		Now: func() time.Time { return time.Date(2026, 11, 7, 18, 0, 0, 0, time.UTC) },
		WorldWorkers: []*WorldWorkerEndpoint{
			{WorkerID: "world-riverlands", ScopeKey: "riverlands", ScopeKind: "region", ContainerName: "aoc-world-riverlands", AdminAddr: "world:50061", StreamAddr: "world:50062", Connected: true},
		},
	})

	assertWorldStateWorkerTick(t, srv, "startup-default", "dynamic", [2]float64{-1, 45.0})

	srv.applyHydratedTickState("dynamic", []byte(`{"aoc-world":40,"world-riverlands":45,"aoc-world-riverlands":40}`))
	assertWorldStateWorkerTick(t, srv, "after-dynamic-db-hydrate", "dynamic", [2]float64{-1, 45.0})

	srv.scrapeAllServiceOpenMetrics(context.Background(), srv.snapshotWorldWorkers())
	assertWorldStateWorkerTick(t, srv, "after-openmetrics-scrape", "dynamic", [2]float64{-1, 52.5})

	hdrs := map[string]string{"Content-Type": "application/json", "Accept": "application/json"}
	manBody := []byte(`{"command":"set_worker_tick_rate","targetId":"world-riverlands","params":{"worker_id":"world-riverlands","tick_hz":40}}`)
	if rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "gm-architect-dev-token", hdrs, manBody); rec.Code != http.StatusOK {
		t.Fatalf("POST manual 40 Hz status=%d body=%s", rec.Code, rec.Body.String())
	}
	assertWorldStateWorkerTick(t, srv, "after-manual-40", "manual", [2]float64{40.0, 40.0})

	dynBody := []byte(`{"command":"set_worker_tick_rate","targetId":"aoc-world","params":{"worker_id":"aoc-world","tick_hz":-1,"tick_mode":"dynamic"}}`)
	if rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "gm-architect-dev-token", hdrs, dynBody); rec.Code != http.StatusOK {
		t.Fatalf("POST dynamic -1 Hz status=%d body=%s", rec.Code, rec.Body.String())
	}
	assertWorldStateWorkerTick(t, srv, "after-dynamic-restore-across-aliases", "dynamic", [2]float64{-1, 52.5})
	assertNoStaleWorkerTickAliasesAndQuery(t, srv, &tickQueriesMu, &tickQueries)
}

func assertWorldStateWorkerTick(t *testing.T, srv *Server, step, wantMode string, wantTargetAndCur [2]float64) {
	t.Helper()
	rec := serveAuthed(srv, http.MethodGet, "/api/v1/world/state", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("%s: GET /api/v1/world/state status=%d body=%s", step, rec.Code, rec.Body.String())
	}
	var payload map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &payload); err != nil {
		t.Fatalf("%s: decode /api/v1/world/state: %v", step, err)
	}
	workers, _ := payload["workers"].([]any)
	if len(workers) != 1 {
		t.Fatalf("%s: expected 1 worker, got %d", step, len(workers))
	}
	w0, _ := workers[0].(map[string]any)
	gotMode := extractStringParam(w0, "tick_mode")
	gotTarget, okT := extractFloatParam(w0, "target_tick_hz")
	gotCurrent, okC := extractFloatParam(w0, "current_tick_hz")
	if gotMode != wantMode || !okT || gotTarget != wantTargetAndCur[0] || !okC || math.Abs(gotCurrent-wantTargetAndCur[1]) > 0.01 {
		t.Fatalf("%s: worker[0] = (mode=%q target=%v current=%v), want (mode=%q target=%v current=%v)",
			step, gotMode, gotTarget, gotCurrent, wantMode, wantTargetAndCur[0], wantTargetAndCur[1])
	}
}

func assertNoStaleWorkerTickAliasesAndQuery(t *testing.T, srv *Server, mu *sync.Mutex, queries *[]string) {
	t.Helper()
	playerHandoversMu.Lock()
	for _, alias := range []string{"world-riverlands", "riverlands", "aoc-world-riverlands", "aoc-riverlands", "aoc-world"} {
		if hz, exists := workerTickHzOverrides[serverPlayerKey{srv: srv, player: alias}]; exists && hz >= 0 {
			playerHandoversMu.Unlock()
			t.Fatalf("stale positive override left for alias %q: %v", alias, hz)
		}
	}
	playerHandoversMu.Unlock()
	mu.Lock()
	gotQueries := append([]string(nil), *queries...)
	mu.Unlock()
	for _, q := range gotQueries {
		if strings.Contains(q, "tick_hz=-1") && strings.Contains(q, "tick_mode=dynamic") {
			return
		}
	}
	t.Fatalf("expected /admin/tick_hz query with tick_hz=-1&tick_mode=dynamic, got %v", gotQueries)
}

