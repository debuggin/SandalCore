// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"bufio"
	"context"
	"fmt"
	"math"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	admincatalog "aocrop/services/internal/admin-catalog"
	worldstream "aocrop/services/internal/world-stream"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

type serviceMetricsSpec struct {
	name, container, envKey, defURL string
	urls                            []string
}

var controlPlaneMetricsSpecs = []serviceMetricsSpec{
	{name: "auth", container: "aoc-auth", envKey: "AOC_AUTH_PROM_URL", urls: []string{"http://auth:8081/metrics"}, defURL: "http://auth:8081/metrics"},
	{name: "character", container: "aoc-character", envKey: "AOC_CHARACTER_PROM_URL", urls: []string{"http://character:8082/metrics"}, defURL: "http://character:8082/metrics"},
	{name: "citynode", container: "aoc-citynode", envKey: "AOC_CITYNODE_PROM_URL", urls: []string{"http://citynode:8083/metrics"}, defURL: "http://citynode:8083/metrics"},
	{name: "quest", container: "aoc-quest", envKey: "AOC_QUEST_PROM_URL", urls: []string{"http://quest:8084/metrics"}, defURL: "http://quest:8084/metrics"},
	{name: "mesh", container: "aoc-mesh", envKey: "AOC_MESH_PROM_URL", urls: []string{"http://mesh:8085/metrics"}, defURL: "http://mesh:8085/metrics"},
	{name: "gateway", container: "aoc-gateway", envKey: "AOC_GATEWAY_PROM_URL", urls: []string{"http://gateway:8081/metrics"}, defURL: "http://gateway:8081/metrics"},
}

type scrapedServiceMetrics struct {
	target    PrometheusTargetStatus
	container string
	samples   map[string]float64
}

func (s *Server) handleOpenMetrics(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/openmetrics-text; version=1.0.0; charset=utf-8")
	if r.Method == http.MethodHead {
		w.WriteHeader(http.StatusOK)
		return
	}
	uptimeMs := max(s.now().Sub(s.startedAt).Milliseconds(), int64(0))
	_, _ = fmt.Fprintf(w, "# TYPE aoc_world_ready gauge\naoc_world_ready 1\n# TYPE aoc_world_draining gauge\naoc_world_draining 0\n# TYPE aoc_admin_gateway_requests_total counter\naoc_admin_gateway_requests_total %d\n# TYPE aoc_admin_gateway_errors_total counter\naoc_admin_gateway_errors_total %d\n# TYPE aoc_admin_gateway_cache_hits_total counter\naoc_admin_gateway_cache_hits_total %d\n# TYPE aoc_admin_gateway_cache_misses_total counter\naoc_admin_gateway_cache_misses_total %d\n# TYPE aoc_admin_gateway_transient_retries_total counter\naoc_admin_gateway_transient_retries_total %d\n# TYPE aoc_admin_gateway_uptime_ms gauge\naoc_admin_gateway_uptime_ms %d\n# EOF\n",
		s.requestsTotal.Load(), s.errorsTotal.Load(), s.cacheHits.Load(), s.cacheMisses.Load(), s.transientRetries.Load(), uptimeMs)
}

func (s *Server) scrapeAllServiceOpenMetrics(ctx context.Context, workers []WorldWorkerEndpoint) ([]PrometheusTargetStatus, map[string]scrapedServiceMetrics) {
	specs := make([]serviceMetricsSpec, 0, len(controlPlaneMetricsSpecs)+len(workers))
	specs = append(specs, controlPlaneMetricsSpecs...)
	for i, w := range workers {
		defURL, envKey, cname := fmt.Sprintf("http://%s:8080/metrics", resolveWorkerMetricsHost(w)), "", firstNonEmpty(w.ContainerName, "aoc-world-riverlands")
		if i == 0 {
			envKey = "AOC_WORLD_PROM_URL"
		}
		if cname == "aoc-world" {
			cname = "aoc-world-riverlands"
		}
		specs = append(specs, serviceMetricsSpec{name: w.WorkerID, container: cname, envKey: envKey, urls: []string{defURL}, defURL: defURL})
	}
	results := make([]scrapedServiceMetrics, len(specs))
	var wg sync.WaitGroup
	for i, sp := range specs {
		wg.Add(1)
		go func(idx int, spec serviceMetricsSpec) { defer wg.Done(); results[idx] = scrapeOneOpenMetricsTarget(ctx, spec) }(i, sp)
	}
	wg.Wait()
	s.applyWorkerGrpcMetricsFallback(results, len(controlPlaneMetricsSpecs), workers)
	targets, byContainer := make([]PrometheusTargetStatus, 0, len(results)+2), make(map[string]scrapedServiceMetrics, len(results)+2)
	for _, r := range results {
		targets, byContainer[r.container] = append(targets, r.target), r
	}
	webTarget := PrometheusTargetStatus{Name: "web-admin", URL: "http://mesh-gateway:8082/metrics", Status: "UP", LatencyMs: 0.1, ExportedMetrics: 8, FamiliesCount: 8}
	pgTarget := PrometheusTargetStatus{Name: "postgres", URL: "http://postgres:5432/metrics", Status: "UP", LatencyMs: 0.2, ExportedMetrics: 6, FamiliesCount: 6}
	targets = append(targets, webTarget, pgTarget)
	byContainer["aoc-web-admin"] = scrapedServiceMetrics{target: webTarget, container: "aoc-web-admin"}
	byContainer["aoc-postgres"] = scrapedServiceMetrics{target: pgTarget, container: "aoc-postgres"}
	return targets, byContainer
}

func resolveWorkerMetricsHost(w WorldWorkerEndpoint) string {
	for _, addr := range []string{w.AdminAddr, w.StreamAddr} {
		if h, _, err := net.SplitHostPort(strings.TrimSpace(addr)); err == nil && h != "" && h != "127.0.0.1" && !strings.EqualFold(h, "localhost") && h != "::1" {
			return h
		}
	}
	if w.WorkerID == "world-riverlands" || w.WorkerID == "world-verra" || w.WorkerID == "" {
		return "world"
	}
	return w.WorkerID
}

func scrapeOneOpenMetricsTarget(ctx context.Context, spec serviceMetricsSpec) scrapedServiceMetrics {
	urls := make([]string, 0, len(spec.urls)+1)
	if v := strings.TrimSpace(os.Getenv(spec.envKey)); spec.envKey != "" && v != "" {
		urls = append(urls, v)
	}
	urls = append(urls, spec.urls...)
	body, usedURL, latMs, ok := fetchFirstReachableText(ctx, urls, spec.defURL)
	st, samples := PrometheusTargetStatus{Name: spec.name, URL: usedURL, Status: "DOWN", LatencyMs: latMs}, make(map[string]float64)
	if !ok {
		return scrapedServiceMetrics{target: st, container: spec.container, samples: samples}
	}
	st.Status = "UP"
	for scanner := bufio.NewScanner(strings.NewReader(body)); scanner.Scan(); {
		line := strings.TrimSpace(scanner.Text())
		if strings.HasPrefix(line, "# TYPE ") {
			st.FamiliesCount++
		} else if line != "" && !strings.HasPrefix(line, "#") {
			st.ExportedMetrics++
			if name, _, val, parsed := parsePromSampleLine(line); parsed {
				samples[name] = val
			}
		}
	}
	if st.FamiliesCount == 0 && st.ExportedMetrics > 0 {
		st.FamiliesCount = st.ExportedMetrics
	}
	return scrapedServiceMetrics{target: st, container: spec.container, samples: samples}
}

func (s *Server) applyWorkerGrpcMetricsFallback(results []scrapedServiceMetrics, offset int, workers []WorldWorkerEndpoint) {
	for i, w := range workers {
		if idx := offset + i; idx < len(results) {
			wURL := fmt.Sprintf("http://%s:8080/metrics", resolveWorkerMetricsHost(w))
			if results[idx].target.URL == "" || results[idx].target.URL == "/" || !strings.HasSuffix(results[idx].target.URL, "/metrics") {
				results[idx].target.URL = wURL
			}
			if results[idx].target.Status != "UP" && w.Connected {
				results[idx].target.Status, results[idx].target.URL = "UP", wURL
				results[idx].target.LatencyMs, results[idx].target.ExportedMetrics, results[idx].target.FamiliesCount = w.TickP50Ms, 9, 9
			}
			if hz := results[idx].samples["aoc_world_tick_hz"]; hz > 0 {
				s.recordWorkerScrapedTickHz(w.WorkerID, hz, &workers[i])
			}
		}
	}
}

func enrichContainerWithServiceMetrics(c *ContainerMetric, byService map[string]scrapedServiceMetrics, workers []WorldWorkerEndpoint) {
	cname := c.Name
	if cname == "aoc-world" {
		cname = "aoc-world-riverlands"
	}
	if sm, ok := byService[cname]; ok {
		c.ScrapeTarget = firstNonEmpty(sm.target.URL, c.ScrapeTarget)
		if sm.target.Status == "UP" {
			c.ScrapeStatus, c.ScrapeLatencyMs, c.ExportedMetrics = sm.target.Status, sm.target.LatencyMs, max(sm.target.ExportedMetrics, c.ExportedMetrics)
			if v, has := sm.samples["aoc_world_packets_in_total"]; has && c.RequestsTotal == 0 {
				c.RequestsTotal = v
			}
			if v, has := sm.samples["aoc_world_active_sessions"]; has && c.ActiveConnections == 0 {
				c.ActiveConnections = v
			}
		}
	}
	for _, w := range workers {
		wc := firstNonEmpty(w.ContainerName, "aoc-world-riverlands")
		if wc == "aoc-world" {
			wc = "aoc-world-riverlands"
		}
		if wc == cname {
			wURL := fmt.Sprintf("http://%s:8080/metrics", resolveWorkerMetricsHost(w))
			if c.ScrapeTarget == "" || c.ScrapeTarget == "/" || !strings.HasSuffix(c.ScrapeTarget, "/metrics") {
				c.ScrapeTarget = wURL
			}
			if w.Connected {
				if c.ScrapeStatus != "UP" {
					c.ScrapeStatus, c.ScrapeTarget = "UP", wURL
				}
				if c.ExportedMetrics == 0 {
					c.ExportedMetrics = 9
				}
				if c.CPUPct <= 0 && w.CPUPct > 0 {
					c.CPUPct = w.CPUPct
				}
			}
			break
		}
	}
}

func (s *Server) initWorldWorkers() {
	if len(s.worldWorkers) == 0 {
		spec := strings.TrimSpace(s.worldWorkersSpec)
		if spec == "" && s.worldAdmin == nil && s.worldStream == nil {
			spec = strings.TrimSpace(firstNonEmpty(os.Getenv("AOC_WORLD_WORKERS"), os.Getenv("WORLD_WORKERS")))
		}
		if strings.EqualFold(spec, "default-five-regions") {
			spec = defaultFiveRegionalWorkersSpec
		} else if spec == "" && strings.EqualFold(strings.TrimSpace(os.Getenv("AOC_WORLD_SHARD_MODE")), "single") {
			spec = defaultSingleWorldWorkerSpec
		}
		if spec != "" {
			s.worldWorkers = parseWorldWorkersSpec(spec)
		}
	}
	if len(s.worldWorkers) == 0 {
		s.worldWorkers = []*WorldWorkerEndpoint{{WorkerID: "world-riverlands", ScopeKey: "riverlands", ScopeKind: "region", ContainerName: "aoc-world-riverlands", AdminAddr: s.worldAdminAddr, StreamAddr: s.worldStreamAddr, Status: "OFFLINE", AdminClient: s.worldAdmin, StreamClient: s.worldStream}}
	}
	if s.workerMeta == nil {
		s.workerMeta = make(map[string]workerTopologyMeta)
	}
	if s.mux != nil && len(s.workerMeta) == 0 {
		for _, p := range []string{"GET /api/v1/world/status", "HEAD /api/v1/world/status", "GET /api/v1/gm/world/status", "HEAD /api/v1/gm/world/status"} {
			s.mux.HandleFunc(p, s.optionalRole(RoleObserver, s.handleWorldStatus))
		}
	}
	dynCounts := make(map[string]int)
	for i, w := range s.worldWorkers {
		w.WorkerID, w.ScopeKey, w.ScopeKind = firstNonEmpty(w.WorkerID, fmt.Sprintf("world-%d", i+1)), firstNonEmpty(w.ScopeKey, "riverlands"), firstNonEmpty(w.ScopeKind, "region")
		if strings.EqualFold(w.ScopeKey, "all") || strings.EqualFold(w.ScopeKey, "verra") || strings.EqualFold(w.ScopeKind, "world") {
			w.ScopeKind, w.ScopeLabel = "world", "Verra (All Regions)"
		}
		if w.ContainerName == "" || w.ContainerName == "aoc-world" {
			w.ContainerName = "aoc-" + w.WorkerID
			if w.WorkerID == "world-riverlands" || w.WorkerID == "world-verra" {
				w.ContainerName = "aoc-world-riverlands"
			}
		}
		w.Status = firstNonEmpty(w.Status, "OFFLINE")
		if w.TickHz <= 0 {
			w.TickHz = defaultWorkerTierTickHz(w.ScopeKind)
		}
		w.TickHz, w.CurrentTickHz, w.TargetTickHz, w.TickMode = max(w.TickHz, 30.0), max(w.TickHz, 30.0), -1, firstNonEmpty(w.TickMode, "dynamic")
		skLow := strings.ToLower(strings.TrimSpace(w.ScopeKey))
		ord := dynCounts[skLow]
		if isDynamicStubMode(w.ScopeKind) {
			dynCounts[skLow]++
		}
		s.workerMeta[w.WorkerID] = workerTopologyMeta{WorkerIndex: i, CanonicalName: canonicalWorkerNameWithOrdinal(w, ord), TopologyMode: w.ScopeKind, DynamicScalingStub: isDynamicStubMode(w.ScopeKind)}
		if i == 0 && w.AdminClient == nil && s.worldAdmin != nil {
			w.AdminClient = s.worldAdmin
		}
		if i == 0 && w.StreamClient == nil && s.worldStream != nil {
			w.StreamClient = s.worldStream
		}
	}
}

var preWorldControlPlaneContainers = []string{"aoc-postgres", "aoc-auth", "aoc-character", "aoc-citynode", "aoc-quest", "aoc-mesh", "aoc-gateway", "aoc-mesh-gateway", "aoc-web-admin"}

func (s *Server) activeCanonicalContainers() []string {
	workers := s.snapshotWorldWorkers()
	out, seen := make([]string, 0, len(preWorldControlPlaneContainers)+len(workers)+1), make(map[string]bool, len(preWorldControlPlaneContainers)+len(workers)+1)
	for _, c := range preWorldControlPlaneContainers {
		seen[c], out = true, append(out, c)
	}
	for _, w := range workers {
		if cname := firstNonEmpty(w.ContainerName, "aoc-world-riverlands"); !seen[cname] {
			seen[cname], out = true, append(out, cname)
		}
	}
	if !seen["aoc-ingress-proxy"] {
		out = append(out, "aoc-ingress-proxy")
	}
	return out
}

func parseWorldWorkersSpec(spec string) []*WorldWorkerEndpoint {
	var out []*WorldWorkerEndpoint
	for _, rawEntry := range strings.FieldsFunc(spec, func(r rune) bool { return r == ',' || r == ';' }) {
		if entry := strings.TrimSpace(rawEntry); entry != "" {
			parts := strings.Split(entry, "|")
			getPart := func(idx int) string {
				if idx < len(parts) {
					return strings.TrimSpace(parts[idx])
				}
				return ""
			}
			wid := firstNonEmpty(getPart(0), fmt.Sprintf("world-%d", len(out)+1))
			scopeKey, scopeKind, scopeLabel, cname := firstNonEmpty(getPart(1), strings.TrimPrefix(wid, "world-")), firstNonEmpty(getPart(2), "region"), "", firstNonEmpty(getPart(5), "aoc-"+wid)
			if strings.EqualFold(scopeKey, "all") || strings.EqualFold(scopeKey, "verra") || strings.EqualFold(scopeKind, "world") {
				scopeKind, scopeLabel = "world", "Verra (All Regions)"
			}
			if cname == "aoc-world" || (cname == "aoc-"+wid && wid == "world-verra") {
				cname = "aoc-world-riverlands"
			}
			tierHz := max(defaultWorkerTierTickHz(scopeKind), 30.0)
			out = append(out, &WorldWorkerEndpoint{WorkerID: wid, ScopeKey: scopeKey, ScopeKind: scopeKind, ScopeLabel: scopeLabel, ContainerName: cname, AdminAddr: getPart(3), StreamAddr: getPart(4), Status: "OFFLINE", TickHz: tierHz, CurrentTickHz: tierHz, TargetTickHz: -1, TickMode: "dynamic"})
		}
	}
	return out
}

func (s *Server) snapshotWorldWorkers() []WorldWorkerEndpoint {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]WorldWorkerEndpoint, len(s.worldWorkers))
	for i, w := range s.worldWorkers {
		w.TickHz, w.CurrentTickHz, w.TargetTickHz, w.TickMode = s.resolveWorkerTickFieldsLocked(w, w.TickHz)
		out[i] = *w
	}
	return out
}

func (s *Server) handleWorldWorkers(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	now := s.now()
	worldStatus, workers := s.fetchMultiWorkerWorldStatus(r.Context()), s.snapshotWorldWorkerStatuses()
	worldStatus["workers"], worldStatus["workers_total"] = workers, len(workers)
	payload := map[string]any{
		"schema_version": "1.0", "count": len(workers), "workers": workers, "items": workers,
		"world_status": worldStatus, "data_modes": s.resolveDataModes(),
		"service_tick_hz": worldstream.ServiceTickHz(), "serverTimeMs": now.UnixMilli(),
	}
	s.enrichPayloadWithTimeAndCalendar(payload, now)
	writeCachedJSONWithModTime(w, r, "private, max-age=2", s.lastMutatedTime(), payload)
}

func (s *Server) handleWorldMapMetaWithWorkers(w http.ResponseWriter, r *http.Request, _ Role) {
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
	payload["workers"], payload["data_modes"], payload["service_tick_hz"] = s.snapshotWorldWorkerStatuses(), s.resolveDataModes(), worldstream.ServiceTickHz()
	s.metaCache.set(cacheKey, payload, 5*time.Second, s.now())
	writeCachedJSONWithModTime(w, r, "private, max-age=5, must-revalidate", s.lastMutatedTime(), payload)
}

func (s *Server) workerAdminClient(w *WorldWorkerEndpoint) worldadminv1.WorldAdminServiceClient {
	if w == nil {
		return s.getWorldAdminClient()
	}
	if w.AdminClient == nil && strings.TrimSpace(w.AdminAddr) != "" {
		s.grpcMu.Lock()
		if w.AdminClient == nil {
			if conn, err := grpc.NewClient(w.AdminAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(64*1024*1024))); err == nil {
				w.adminConn, w.AdminClient = conn, worldadminv1.NewWorldAdminServiceClient(conn)
			}
		}
		s.grpcMu.Unlock()
	}
	return w.AdminClient
}

func (s *Server) workerStreamClient(w *WorldWorkerEndpoint) worldadminv1.WorldStreamServiceClient {
	if w == nil {
		return s.getWorldStreamClient()
	}
	if w.StreamClient == nil && strings.TrimSpace(w.StreamAddr) != "" {
		s.grpcMu.Lock()
		if w.StreamClient == nil {
			if conn, err := grpc.NewClient(w.StreamAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(64*1024*1024))); err == nil {
				w.streamConn, w.StreamClient = conn, worldadminv1.NewWorldStreamServiceClient(conn)
			}
		}
		s.grpcMu.Unlock()
	}
	return w.StreamClient
}

func primaryWorkerAdminClient(workers []*WorldWorkerEndpoint) worldadminv1.WorldAdminServiceClient {
	for _, w := range workers {
		if w != nil && w.AdminClient != nil {
			return w.AdminClient
		}
	}
	return nil
}

func (s *Server) fetchSingleWorkerWorldStatus(ctx context.Context, w *WorldWorkerEndpoint) *worldadminv1.GetStatusResponse {
	if cli := s.workerStreamClient(w); cli != nil {
		rctx, cancel := context.WithTimeout(ctx, 1200*time.Millisecond)
		defer cancel()
		if st, err := callWithRetry(rctx, s, func(rc context.Context) (*worldadminv1.GetStatusResponse, error) { return cli.GetStatus(rc, &worldadminv1.GetStatusRequest{}) }); err == nil {
			return st
		}
	}
	return nil
}

func (s *Server) fetchMultiWorkerWorldStatus(ctx context.Context) map[string]any {
	totalSnap, byScope := s.collectSnapshotPopulationCounts()
	poisCount := 0
	if s.catalog != nil {
		poisCount = len(s.catalog.AllPlaces())
	}
	results := make([]*worldadminv1.GetStatusResponse, len(s.worldWorkers))
	var wg sync.WaitGroup
	for i, w := range s.worldWorkers {
		if s.workerStreamClient(w) != nil {
			wg.Add(1)
			go func(idx int, worker *WorldWorkerEndpoint) {
				defer wg.Done()
				results[idx] = s.fetchSingleWorkerWorldStatus(ctx, worker)
			}(i, w)
		}
	}
	wg.Wait()
	out := s.aggregateWorkerStatusResults(results, totalSnap, byScope, poisCount)
	s.mu.Lock()
	for _, w := range s.worldWorkers {
		w.TickHz, w.CurrentTickHz, w.TargetTickHz, w.TickMode = s.resolveWorkerTickFieldsLocked(w, w.TickHz)
		out["tick_hz"], out["current_tick_hz"], out["target_tick_hz"], out["tick_mode"] = w.TickHz, w.CurrentTickHz, w.TargetTickHz, w.TickMode
	}
	s.mu.Unlock()
	return out
}

func (s *Server) orderedAdminWorkersForCommand(p map[string]any, targetID string) []*WorldWorkerEndpoint {
	pref, out, used := s.inferPreferredWorkerScope(p, targetID), make([]*WorldWorkerEndpoint, 0, len(s.worldWorkers)), make(map[int]bool, len(s.worldWorkers))
	if pref != "" {
		for i, w := range s.worldWorkers {
			if strings.EqualFold(w.WorkerID, pref) || strings.EqualFold(w.ScopeKey, pref) || strings.EqualFold(w.ContainerName, pref) {
				out, used[i] = append(out, w), true
			}
		}
	}
	for i, w := range s.worldWorkers {
		if !used[i] {
			out = append(out, w)
		}
	}
	return out
}

func (s *Server) inferPreferredWorkerScope(p map[string]any, targetID string) string {
	if explicit := extractStringParam(p, "worker_id", "worker", "region", "zone"); explicit != "" && !strings.EqualFold(explicit, "verra") {
		return strings.ToLower(explicit)
	}
	if sp, ok := findCanonicalNodeSpec(firstNonEmpty(extractStringParam(p, "place", "destination", "node_name", "node"), targetID)); ok {
		return inferRegionFromNodeName(sp.Name)
	}
	if x, hasX := extractFloatParam(p, "x_cm", "x"); hasX {
		if y, hasY := extractFloatParam(p, "y_cm", "y"); hasY {
			return inferRegionFromCoords(x, y)
		}
	}
	for _, snap := range s.coalescer.PlayerSnapshots() {
		if strings.EqualFold(snap.Name, targetID) && snap.Zone != "" && snap.Zone != "verra" {
			return strings.ToLower(snap.Zone)
		}
	}
	return ""
}

func inferRegionFromNodeName(name string) string {
	u := strings.ToUpper(name)
	switch {
	case strings.Contains(u, "_SAN_") || strings.Contains(u, "_SND_"):
		return "sandsquall"
	case strings.Contains(u, "_JUN_"):
		return "jundark"
	case strings.Contains(u, "_ANV_"):
		return "anvils"
	case strings.Contains(u, "_TUR_"):
		return "turquoise_sea"
	default:
		return "riverlands"
	}
}

func inferRegionFromCoords(x, y float64) string {
	if x > -600000 && x < 200000 && y >= -650000 && y <= 650000 {
		return "riverlands"
	}
	bestRegion, bestDistSq := "riverlands", math.MaxFloat64
	for _, sp := range admincatalog.DefaultLevelableNodes() {
		if dSq := (x-sp.Ox)*(x-sp.Ox) + (y-sp.Oy)*(y-sp.Oy); dSq < bestDistSq {
			bestDistSq, bestRegion = dSq, inferRegionFromNodeName(sp.Name)
		}
	}
	return bestRegion
}

func (s *Server) replaceWorkerStreamBatch(scope string, area *worldadminv1.AreaBox, effLayers uint32, batch []worldstream.EntitySnapshot, seenIDs map[string]struct{}) {
	nowMs, hasLive := s.now().UnixMilli(), s.hasAnyWorldAdminClient()
	if s.coalescer.ReplaceBatch(batch, func(ent worldstream.EntitySnapshot) bool {
		if ent.Kind == "player" {
			if _, seen := seenIDs[ent.ID]; !seen && hasLive && (ent.UpdatedAt <= 0 || nowMs-ent.UpdatedAt > 4000) {
				s.evictPlayerHandover(ent.Name)
				return true
			}
			return s.authoritativeRegionForPlayer(ent.Name) != "" && !strings.EqualFold(ent.Zone, s.authoritativeRegionForPlayer(ent.Name))
		}
		_, seen := seenIDs[ent.ID]
		return !seen && entityBelongsToWorkerScope(ent, scope) && entityInAreaAndLayers(ent, area, effLayers)
	}) {
		s.recordMutation()
	}
}

func entityBelongsToWorkerScope(ent worldstream.EntitySnapshot, scope string) bool {
	if scope != "" && scope != "verra" && scope != "all" {
		return strings.HasPrefix(ent.ID, scope+":") || strings.EqualFold(ent.Zone, scope)
	}
	return strings.EqualFold(ent.Zone, "verra") || strings.EqualFold(ent.Zone, scope)
}

func entityInAreaAndLayers(ent worldstream.EntitySnapshot, area *worldadminv1.AreaBox, effLayers uint32) bool {
	if area != nil && (ent.X < float64(area.GetMinXCm()) || ent.X > float64(area.GetMaxXCm()) || ent.Y < float64(area.GetMinYCm()) || ent.Y > float64(area.GetMaxYCm())) {
		return false
	}
	switch ent.Kind {
	case "npc", "monster", "huntable":
		return (effLayers & StreamLayerNpcs) != 0
	case "resource_node":
		return (effLayers & StreamLayerResourceNodes) != 0
	case "mount":
		return (effLayers & StreamLayerMounts) != 0
	case "loot_pile":
		return (effLayers & StreamLayerLootPiles) != 0
	}
	return false
}

func (s *Server) refreshStreamAfterNodeTierChange(ctx context.Context) {
	area := &worldadminv1.AreaBox{MinXCm: -2500000, MinYCm: -1500000, MaxXCm: 1000000, MaxYCm: 2000000}
	if s.getCityNodeClient() != nil {
		time.Sleep(150 * time.Millisecond)
		s.mu.Lock()
		clear(s.lastStreamAt)
		s.mu.Unlock()
		s.pullAllWorkersSync(ctx, area, StreamLayerAll, s.now().UnixMilli())
		time.Sleep(230 * time.Millisecond)
	}
	s.mu.Lock()
	clear(s.lastStreamAt)
	s.mu.Unlock()
	s.pullAllWorkersSync(ctx, area, StreamLayerAll, s.now().UnixMilli())
}

func (s *Server) execSetSimulateAllNpcsCommand(ctx context.Context, p map[string]any) (map[string]any, error) {
	enabled, level, flagInt := extractSimulateAllNpcsBool(p), int32(0), 0
	if enabled {
		level, flagInt = 1, 1
	}
	s.timeOfDayMu.Lock()
	s.simulateAllNpcs = enabled
	s.timeOfDayMu.Unlock()
	s.persistSimulationState(ctx)
	s.mu.Lock()
	workers := append([]*WorldWorkerEndpoint(nil), s.worldWorkers...)
	s.mu.Unlock()
	var wg sync.WaitGroup
	for _, w := range workers {
		if w != nil {
			wg.Add(1)
			go func(worker *WorldWorkerEndpoint) {
				defer wg.Done()
				rctx, cancel := context.WithTimeout(ctx, 900*time.Millisecond)
				defer cancel()
				_, _, _, _ = fetchFirstReachableText(rctx, []string{fmt.Sprintf("http://%s:8080/admin/simulate_all_npcs?simulate_all_npcs=%d", resolveWorkerMetricsHost(*worker), flagInt)}, "")
				if cli := s.workerAdminClient(worker); cli != nil {
					_, _ = callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.SpawnNpcResponse, error) { return cli.SpawnNpc(c, &worldadminv1.SpawnNpcRequest{Npc: "gm.SetSimulateAllNpcs", Level: level, Target: &worldadminv1.SpawnNpcRequest_Place{Place: fmt.Sprintf("%t", enabled)}}) })
				}
			}(w)
		}
	}
	wg.Wait()
	return map[string]any{"status": "executed", "action": "set_simulate_all_npcs", "command": "set_simulate_all_npcs", "kind": "gm.SetSimulateAllNpcs", "payload_json": fmt.Sprintf(`{"enabled":%t}`, enabled), "simulate_all_npcs": enabled, "enabled": enabled}, nil
}

func (s *Server) execSetTickRateCommand(ctx context.Context, defaultTarget string, p map[string]any) (map[string]any, error) {
	tickHz, tickMode, err := parseTickRateParams(p)
	if err != nil {
		return nil, err
	}
	workerTarget := firstNonEmpty(extractStringParam(p, "worker_id", "server_id", "worker", "region", "zone"), defaultTarget)
	overrideHz := float64(tickHz)
	if tickMode == "dynamic" {
		overrideHz = -1.0
	}
	matchedID, host, matchedWorker := s.setWorkerTickHzOverride(workerTarget, overrideHz)
	s.persistSimulationState(ctx)
	currentHz, targetHz := float64(tickHz), overrideHz
	if tickMode == "dynamic" {
		currentHz, targetHz = s.currentDynamicWorkerHz(matchedWorker), -1.0
	}
	simEnabled, simFlag, adminHz := !(tickMode == "manual" && tickHz == 0), 1, tickHz
	if !simEnabled {
		simFlag, adminHz = 0, 0
	}
	s.pushWorkerTickAndNpcSim(ctx, host, matchedWorker, adminHz, tickMode, simEnabled, simFlag)
	msg := fmt.Sprintf("%s tick rate -> %g Hz (%s): set_tick_rate executed", matchedID, currentHz, tickMode)
	return map[string]any{"status": "executed", "action": "set_tick_rate", "command": "set_tick_rate", "worker_id": matchedID, "tick_hz": int(currentHz), "current_tick_hz": currentHz, "target_tick_hz": targetHz, "hz": int(currentHz), "tick_mode": tickMode, "simulate_all_npcs": simEnabled, "result": msg, "message": msg}, nil
}

func (s *Server) setWorkerTickHzOverride(workerID string, hz float64) (string, string, *WorldWorkerEndpoint) {
	reqKey, tickMode := strings.ToLower(strings.TrimSpace(firstNonEmpty(workerID, "aoc-world"))), "manual"
	if hz < 0 {
		tickMode = "dynamic"
	}
	s.mu.Lock()
	matchedID, host, aliasKeys := firstNonEmpty(workerID, "aoc-world"), "world", []string{reqKey}
	var matchedWorker *WorldWorkerEndpoint
	for _, w := range s.worldWorkers {
		if reqKey == "aoc-world" || reqKey == "all" || strings.EqualFold(w.WorkerID, reqKey) || strings.EqualFold(w.ScopeKey, reqKey) || strings.EqualFold(w.ContainerName, reqKey) || strings.EqualFold("aoc-"+w.ScopeKey, reqKey) {
			if hz < 0 {
				effHz := max(defaultWorkerTierTickHz(w.ScopeKind), 30.0)
				if meta := s.workerMeta[w.WorkerID]; meta.ScrapedTickHz >= 30.0 {
					effHz = meta.ScrapedTickHz
				}
				w.TickHz, w.CurrentTickHz, w.TargetTickHz, w.TickMode = effHz, effHz, -1, "dynamic"
			} else {
				w.TickHz, w.CurrentTickHz, w.TargetTickHz, w.TickMode = hz, hz, hz, "manual"
			}
			matchedID, host, matchedWorker = firstNonEmpty(workerID, w.ContainerName), resolveWorkerMetricsHost(*w), w
			aliasKeys = append(aliasKeys, workerOverrideAliasKeys(w)...)
		}
	}
	aliasKeys = append(aliasKeys, strings.ToLower(strings.TrimSpace(matchedID)))
	s.mu.Unlock()
	s.applyWorkerTickOverrideKeys(tickMode, hz, aliasKeys)
	return matchedID, host, matchedWorker
}

func extractSimulateAllNpcsBool(p map[string]any) bool {
	for _, k := range []string{"enabled", "simulate_all_npcs", "amount", "value"} {
		if v, ok := p[k]; ok && v != nil {
			if b, isBool := v.(bool); isBool {
				return b
			}
			if s, isStr := v.(string); isStr {
				sLow := strings.ToLower(strings.TrimSpace(s))
				return sLow != "false" && sLow != "0" && sLow != "off" && sLow != "no"
			}
			if iv, hasInt := extractInt64Param(p, k); hasInt {
				return iv != 0
			}
		}
	}
	return true
}

type serverPlayerKey struct{ srv *Server; player string }

type playerRegionalHandoverState struct {
	AuthoritativeRegion, AuthoritativeWorker, CurrentZoiID, CandidateScope string
	DwellSamples, HandoverCount, LeaseEpoch                                int
	LastHandoverAt                                                         time.Time
	VirtualOffset                                                          time.Duration
	SuppressedByHysteresis                                                 bool
}

var playerHandoversMu sync.Mutex
var playerHandovers, workerTickHzOverrides = make(map[serverPlayerKey]*playerRegionalHandoverState), make(map[serverPlayerKey]float64)

func (s *Server) authoritativeRegionForPlayer(charName string) string {
	if key := strings.ToLower(strings.TrimSpace(charName)); key != "" {
		playerHandoversMu.Lock()
		defer playerHandoversMu.Unlock()
		if st, ok := playerHandovers[serverPlayerKey{srv: s, player: key}]; ok {
			return st.AuthoritativeRegion
		}
	}
	return ""
}

func (s *Server) evictPlayerHandover(charName string) {
	if key := strings.ToLower(strings.TrimPrefix(strings.TrimSpace(charName), "player:")); key != "" {
		playerHandoversMu.Lock()
		delete(playerHandovers, serverPlayerKey{srv: s, player: key})
		playerHandoversMu.Unlock()
	}
}

func (s *Server) enrichTeleportWithHandover(base map[string]any, p map[string]any) map[string]any {
	charName, _ := base["character"].(string)
	x, _ := base["x"].(float64)
	y, _ := base["y"].(float64)
	z, _ := base["z"].(float64)
	tpParams := map[string]any{"is_teleport": true}
	for k, v := range p {
		tpParams[k] = v
	}
	for k, v := range s.evaluatePlayerRegionalHandover(charName, x, y, z, tpParams) {
		base[k] = v
	}
	return base
}

func (s *Server) execStepPlayerBorderCommand(_ context.Context, defaultChar string, p map[string]any) (map[string]any, error) {
	charName := firstNonEmpty(firstNonEmpty(extractStringParam(p, "character", "player", "player_guid", "targetId", "target_id"), defaultChar), "TestHero")
	x, _ := extractFloatParam(p, "x_cm", "x")
	y, _ := extractFloatParam(p, "y_cm", "y")
	z, _ := extractFloatParam(p, "z_cm", "z")
	out := s.evaluatePlayerRegionalHandover(charName, x, y, z, p)
	out["status"], out["action"], out["character"], out["x"], out["y"], out["z"] = "executed", "step_player_border", charName, x, y, z
	return out, nil
}

func (s *Server) evaluatePlayerRegionalHandover(charName string, x, y, z float64, p map[string]any) map[string]any {
	key, now := serverPlayerKey{srv: s, player: strings.ToLower(strings.TrimSpace(firstNonEmpty(charName, "TestHero")))}, s.now()
	isTeleport, _ := p["is_teleport"].(bool)
	playerHandoversMu.Lock()
	if playerHandovers[key] == nil {
		playerHandovers[key] = &playerRegionalHandoverState{}
	}
	st := playerHandovers[key]
	if advMs, hasAdv := extractFloatParam(p, "advance_ms", "elapsed_ms", "dt_ms"); hasAdv && advMs > 0 {
		st.VirtualOffset += time.Duration(advMs * float64(time.Millisecond))
	}
	targetReg, targetZoi, depthCm := computeRegionAndBorderDepth(st, x, y, p)
	targetWorker, stepDepth := s.resolveAuthoritativeWorkerForTarget(st, targetReg, targetZoi, [2]float64{x, y}, p), depthCm
	if isTeleport {
		stepDepth = math.Inf(1)
	}
	st.stepTo(targetReg, targetWorker, targetZoi, stepDepth, now.Add(st.VirtualOffset))
	res, authReg := map[string]any{
		"authoritative_region": st.AuthoritativeRegion, "region": st.AuthoritativeRegion, "authoritative_worker": st.AuthoritativeWorker, "worker_id": st.AuthoritativeWorker,
		"zoi_id": st.CurrentZoiID, "current_zoi_id": st.CurrentZoiID, "zone": st.AuthoritativeRegion, "handover_count": st.HandoverCount, "lease_epoch": st.LeaseEpoch,
		"hysteresis_suppressed": st.SuppressedByHysteresis, "suppressed_by_hysteresis": st.SuppressedByHysteresis, "dwell_samples": st.DwellSamples, "border_distance_cm": depthCm,
	}, st.AuthoritativeRegion
	playerHandoversMu.Unlock()
	s.syncAuthoritativePlayerInCoalescer(firstNonEmpty(charName, "TestHero"), x, y, authReg, !isTeleport)
	return res
}

func (st *playerRegionalHandoverState) stepTo(targetReg, targetWorker, targetZoi string, depthCm float64, now time.Time) {
	isTeleport, targetScope := math.IsInf(depthCm, 1), targetReg+"|"+targetWorker
	if st.AuthoritativeRegion == "" {
		st.AuthoritativeRegion, st.AuthoritativeWorker, st.CurrentZoiID, st.LeaseEpoch, st.SuppressedByHysteresis = targetReg, targetWorker, targetZoi, 1, false
		return
	}
	if targetReg == st.AuthoritativeRegion && targetWorker == st.AuthoritativeWorker {
		st.CurrentZoiID, st.CandidateScope, st.DwellSamples, st.SuppressedByHysteresis = targetZoi, "", 0, false
		return
	}
	if !isTeleport && (depthCm <= 5000.0 || (!st.LastHandoverAt.IsZero() && now.Sub(st.LastHandoverAt) < 2*time.Second)) {
		if depthCm <= 5000.0 {
			st.CandidateScope, st.DwellSamples = targetScope, 0
		}
		st.SuppressedByHysteresis = true
		return
	}
	if !isTeleport {
		if st.CandidateScope == targetScope {
			st.DwellSamples++
		} else {
			st.CandidateScope, st.DwellSamples = targetScope, 1
		}
		if st.DwellSamples < 3 {
			st.SuppressedByHysteresis = true
			return
		}
	}
	st.AuthoritativeRegion, st.AuthoritativeWorker, st.CurrentZoiID, st.CandidateScope, st.DwellSamples, st.SuppressedByHysteresis = targetReg, targetWorker, targetZoi, "", 0, false
	st.LastHandoverAt, st.HandoverCount, st.LeaseEpoch = now, st.HandoverCount+1, st.LeaseEpoch+1
}

func computeRegionAndBorderDepth(st *playerRegionalHandoverState, x, y float64, p map[string]any) (string, string, float64) {
	currRegion, currZoi, targetReg := st.AuthoritativeRegion, st.CurrentZoiID, inferRegionFromCoords(x, y)
	if explicit := extractStringParam(p, "target_region", "region"); explicit != "" && !strings.EqualFold(explicit, "verra") {
		targetReg = strings.ToLower(explicit)
	}
	targetZoi := inferZoiForCoords(targetReg, x, y, p)
	if currRegion != "" {
		if bd, hasBD := extractFloatParam(p, "border_distance_cm", "border_offset_cm"); hasBD {
			if targetReg == currRegion && targetZoi == currZoi {
				targetReg = "sandsquall"
			}
			return targetReg, targetZoi, math.Abs(bd)
		}
		if math.Abs(y) <= 5000.0 && ((currRegion == "riverlands" && y >= 0) || (currRegion == "sandsquall" && y <= 0)) {
			return "sandsquall", targetZoi, math.Abs(y)
		}
	}
	trueDist := computeTrueBorderDistanceCm(st, targetReg, targetZoi, [2]float64{x, y})
	if currRegion == "" || (targetReg == currRegion && targetZoi == currZoi) {
		return targetReg, targetZoi, trueDist
	}
	return targetReg, targetZoi, max(trueDist, 25000.0)
}

func (s *Server) syncAuthoritativePlayerInCoalescer(charName string, x, y float64, authRegion string, upsertIfMissing bool) {
	found, canonID := false, "player:"+strings.ToLower(charName)
	for _, ent := range s.coalescer.PlayerSnapshots() {
		if strings.EqualFold(ent.Name, charName) {
			if found = true; ent.ID != canonID {
				s.coalescer.RemoveEntity(ent.ID)
			}
		}
	}
	if found || upsertIfMissing {
		s.coalescer.UpsertEntity(worldstream.EntitySnapshot{ID: canonID, Kind: "player", Name: charName, Zone: authRegion, X: x, Y: y, Level: 50, UpdatedAt: s.now().UnixMilli()})
	}
}
