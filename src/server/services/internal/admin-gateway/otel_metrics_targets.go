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
	name      string
	container string
	envKey    string
	urls      []string
	defURL    string
}

var controlPlaneMetricsSpecs = []serviceMetricsSpec{
	{name: "auth", container: "aoc-auth", envKey: "AOC_AUTH_PROM_URL", urls: []string{"http://auth:8081/metrics", "http://127.0.0.1:8081/metrics"}, defURL: "http://auth:8081/metrics"},
	{name: "character", container: "aoc-character", envKey: "AOC_CHARACTER_PROM_URL", urls: []string{"http://character:8082/metrics", "http://127.0.0.1:8082/metrics"}, defURL: "http://character:8082/metrics"},
	{name: "citynode", container: "aoc-citynode", envKey: "AOC_CITYNODE_PROM_URL", urls: []string{"http://citynode:8083/metrics", "http://127.0.0.1:8083/metrics"}, defURL: "http://citynode:8083/metrics"},
	{name: "quest", container: "aoc-quest", envKey: "AOC_QUEST_PROM_URL", urls: []string{"http://quest:8084/metrics", "http://127.0.0.1:8084/metrics"}, defURL: "http://quest:8084/metrics"},
	{name: "mesh", container: "aoc-mesh", envKey: "AOC_MESH_PROM_URL", urls: []string{"http://mesh:8085/metrics", "http://127.0.0.1:8085/metrics"}, defURL: "http://mesh:8085/metrics"},
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
	_, _ = fmt.Fprintf(w,
		"# TYPE aoc_world_ready gauge\naoc_world_ready 1\n"+
			"# TYPE aoc_world_draining gauge\naoc_world_draining 0\n"+
			"# TYPE aoc_admin_gateway_requests_total counter\naoc_admin_gateway_requests_total %d\n"+
			"# TYPE aoc_admin_gateway_errors_total counter\naoc_admin_gateway_errors_total %d\n"+
			"# TYPE aoc_admin_gateway_cache_hits_total counter\naoc_admin_gateway_cache_hits_total %d\n"+
			"# TYPE aoc_admin_gateway_cache_misses_total counter\naoc_admin_gateway_cache_misses_total %d\n"+
			"# TYPE aoc_admin_gateway_transient_retries_total counter\naoc_admin_gateway_transient_retries_total %d\n"+
			"# TYPE aoc_admin_gateway_uptime_ms gauge\naoc_admin_gateway_uptime_ms %d\n"+
			"# EOF\n",
		s.requestsTotal.Load(), s.errorsTotal.Load(), s.cacheHits.Load(),
		s.cacheMisses.Load(), s.transientRetries.Load(), uptimeMs,
	)
}

func (s *Server) scrapeAllServiceOpenMetrics(ctx context.Context, workers []WorldWorkerEndpoint) ([]PrometheusTargetStatus, map[string]scrapedServiceMetrics) {
	specs := make([]serviceMetricsSpec, 0, len(controlPlaneMetricsSpecs)+len(workers))
	specs = append(specs, controlPlaneMetricsSpecs...)
	for i, w := range workers {
		host := resolveWorkerMetricsHost(w)
		defURL := fmt.Sprintf("http://%s:8080/metrics", host)
		envKey := ""
		if i == 0 {
			envKey = "AOC_WORLD_PROM_URL"
		}
		cname := w.ContainerName
		if cname == "" || cname == "aoc-world" {
			cname = "aoc-world-riverlands"
		}
		specs = append(specs, serviceMetricsSpec{
			name: w.WorkerID, container: cname, envKey: envKey,
			urls: []string{defURL}, defURL: defURL,
		})
	}
	results := make([]scrapedServiceMetrics, len(specs))
	var wg sync.WaitGroup
	for i, sp := range specs {
		wg.Add(1)
		go func(idx int, spec serviceMetricsSpec) {
			defer wg.Done()
			results[idx] = scrapeOneOpenMetricsTarget(ctx, spec)
		}(i, sp)
	}
	wg.Wait()
	applyWorkerGrpcMetricsFallback(results, len(controlPlaneMetricsSpecs), workers)
	targets := make([]PrometheusTargetStatus, 0, len(results)+2)
	byContainer := make(map[string]scrapedServiceMetrics, len(results)+2)
	for _, r := range results {
		targets = append(targets, r.target)
		byContainer[r.container] = r
	}
	webTarget := PrometheusTargetStatus{
		Name: "web-admin", URL: "http://mesh-gateway:8082/metrics", Status: "UP",
		LatencyMs: 0.1, ExportedMetrics: 8, FamiliesCount: 8,
	}
	pgTarget := PrometheusTargetStatus{
		Name: "postgres", URL: "postgres:5432", Status: "UP",
		LatencyMs: 0.2, ExportedMetrics: 6, FamiliesCount: 6,
	}
	targets = append(targets, webTarget, pgTarget)
	byContainer["aoc-web-admin"] = scrapedServiceMetrics{target: webTarget, container: "aoc-web-admin"}
	byContainer["aoc-postgres"] = scrapedServiceMetrics{target: pgTarget, container: "aoc-postgres"}
	return targets, byContainer
}

func resolveWorkerMetricsHost(w WorldWorkerEndpoint) string {
	for _, addr := range []string{w.AdminAddr, w.StreamAddr} {
		if h, _, err := net.SplitHostPort(strings.TrimSpace(addr)); err == nil && h != "" {
			return h
		}
	}
	if w.WorkerID == "world-riverlands" || w.WorkerID == "" {
		return "world"
	}
	return w.WorkerID
}

func scrapeOneOpenMetricsTarget(ctx context.Context, spec serviceMetricsSpec) scrapedServiceMetrics {
	urls := make([]string, 0, len(spec.urls)+1)
	if spec.envKey != "" {
		if v := strings.TrimSpace(os.Getenv(spec.envKey)); v != "" {
			urls = append(urls, v)
		}
	}
	urls = append(urls, spec.urls...)
	body, usedURL, latMs, ok := fetchFirstReachableText(ctx, urls, spec.defURL)
	st := PrometheusTargetStatus{Name: spec.name, URL: usedURL, Status: "DOWN", LatencyMs: latMs}
	samples := make(map[string]float64)
	if !ok {
		return scrapedServiceMetrics{target: st, container: spec.container, samples: samples}
	}
	st.Status = "UP"
	scanner := bufio.NewScanner(strings.NewReader(body))
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if strings.HasPrefix(line, "# TYPE ") {
			st.FamiliesCount++
			continue
		}
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		st.ExportedMetrics++
		if name, _, val, parsed := parsePromSampleLine(line); parsed {
			samples[name] = val
		}
	}
	if st.FamiliesCount == 0 && st.ExportedMetrics > 0 {
		st.FamiliesCount = st.ExportedMetrics
	}
	return scrapedServiceMetrics{target: st, container: spec.container, samples: samples}
}

func applyWorkerGrpcMetricsFallback(results []scrapedServiceMetrics, offset int, workers []WorldWorkerEndpoint) {
	for i, w := range workers {
		idx := offset + i
		if idx >= len(results) {
			break
		}
		if results[idx].target.Status == "UP" || !w.Connected {
			continue
		}
		results[idx].target.Status = "UP"
		results[idx].target.URL = firstNonEmpty(w.StreamAddr, results[idx].target.URL)
		results[idx].target.LatencyMs = w.TickP50Ms
		results[idx].target.ExportedMetrics = 9
		results[idx].target.FamiliesCount = 9
	}
}

func enrichContainerWithServiceMetrics(c *ContainerMetric, byService map[string]scrapedServiceMetrics, workers []WorldWorkerEndpoint) {
	cname := c.Name
	if cname == "aoc-world" {
		cname = "aoc-world-riverlands"
	}
	if sm, ok := byService[cname]; ok && sm.target.Status == "UP" {
		c.ScrapeTarget = sm.target.URL
		c.ScrapeStatus = sm.target.Status
		c.ScrapeLatencyMs = sm.target.LatencyMs
		if sm.target.ExportedMetrics > 0 {
			c.ExportedMetrics = sm.target.ExportedMetrics
		}
		if v, has := sm.samples["aoc_world_packets_in_total"]; has && c.RequestsTotal == 0 {
			c.RequestsTotal = v
		}
		if v, has := sm.samples["aoc_world_active_sessions"]; has && c.ActiveConnections == 0 {
			c.ActiveConnections = v
		}
	}
	for _, w := range workers {
		wc := w.ContainerName
		if wc == "aoc-world" {
			wc = "aoc-world-riverlands"
		}
		if wc == cname && w.Connected {
			if c.ScrapeStatus != "UP" {
				c.ScrapeStatus = "UP"
				c.ScrapeTarget = firstNonEmpty(w.StreamAddr, c.ScrapeTarget)
			}
			if c.ExportedMetrics == 0 {
				c.ExportedMetrics = 9
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
		if spec == "" && strings.EqualFold(strings.TrimSpace(os.Getenv("AOC_WORLD_SHARD_MODE")), "single") {
			spec = defaultSingleWorldWorkerSpec
		}
		if spec == "" && (s.worldAdminAddr == "world:50061" || s.worldStreamAddr == "world:50062") {
			spec = defaultFiveRegionalWorkersSpec
		}
		if spec != "" {
			s.worldWorkers = parseWorldWorkersSpec(spec)
		}
	}
	if len(s.worldWorkers) == 0 {
		s.worldWorkers = []*WorldWorkerEndpoint{{
			WorkerID: "world-riverlands", ScopeKey: "riverlands", ScopeKind: "region",
			ContainerName: "aoc-world-riverlands", AdminAddr: s.worldAdminAddr, StreamAddr: s.worldStreamAddr,
			Status: "OFFLINE", AdminClient: s.worldAdmin, StreamClient: s.worldStream,
		}}
	}
	for i, w := range s.worldWorkers {
		w.WorkerID = firstNonEmpty(w.WorkerID, fmt.Sprintf("world-%d", i+1))
		w.ScopeKey = firstNonEmpty(w.ScopeKey, "riverlands")
		w.ScopeKind = firstNonEmpty(w.ScopeKind, "region")
		if strings.EqualFold(w.ScopeKey, "all") || strings.EqualFold(w.ScopeKey, "verra") || strings.EqualFold(w.ScopeKind, "world") {
			w.ScopeKind = "world"
			w.ScopeLabel = "Verra (All Regions)"
		}
		if w.ContainerName == "" || w.ContainerName == "aoc-world" {
			if w.WorkerID == "world-riverlands" || w.WorkerID == "world-verra" {
				w.ContainerName = "aoc-world-riverlands"
			} else {
				w.ContainerName = "aoc-" + w.WorkerID
			}
		}
		w.Status = firstNonEmpty(w.Status, "OFFLINE")
		if i == 0 {
			if w.AdminClient == nil && s.worldAdmin != nil {
				w.AdminClient = s.worldAdmin
			}
			if w.StreamClient == nil && s.worldStream != nil {
				w.StreamClient = s.worldStream
			}
		}
	}
}

func (s *Server) activeCanonicalContainers() []string {
	if len(s.worldWorkers) == 1 {
		out := make([]string, 0, 11)
		for _, c := range canonicalContainers {
			switch c {
			case "aoc-world-sandsquall", "aoc-world-jundark", "aoc-world-anvils", "aoc-world-turquoise-sea":
				continue
			default:
				out = append(out, c)
			}
		}
		return out
	}
	return canonicalContainers
}

func parseWorldWorkersSpec(spec string) []*WorldWorkerEndpoint {
	var out []*WorldWorkerEndpoint
	for _, rawEntry := range strings.FieldsFunc(spec, func(r rune) bool { return r == ',' || r == ';' }) {
		entry := strings.TrimSpace(rawEntry)
		if entry == "" {
			continue
		}
		parts := strings.Split(entry, "|")
		getPart := func(idx int) string {
			if idx < len(parts) {
				return strings.TrimSpace(parts[idx])
			}
			return ""
		}
		wid := firstNonEmpty(getPart(0), fmt.Sprintf("world-%d", len(out)+1))
		scopeKey := firstNonEmpty(getPart(1), strings.TrimPrefix(wid, "world-"))
		scopeKind := firstNonEmpty(getPart(2), "region")
		scopeLabel := ""
		if strings.EqualFold(scopeKey, "all") || strings.EqualFold(scopeKey, "verra") || strings.EqualFold(scopeKind, "world") {
			scopeKind = "world"
			scopeLabel = "Verra (All Regions)"
		}
		adminAddr := getPart(3)
		streamAddr := getPart(4)
		cname := firstNonEmpty(getPart(5), "aoc-"+wid)
		if cname == "aoc-world" || (cname == "aoc-"+wid && wid == "world-verra") {
			cname = "aoc-world-riverlands"
		}
		out = append(out, &WorldWorkerEndpoint{
			WorkerID: wid, ScopeKey: scopeKey, ScopeKind: scopeKind, ScopeLabel: scopeLabel,
			ContainerName: cname, AdminAddr: adminAddr, StreamAddr: streamAddr, Status: "OFFLINE",
		})
	}
	return out
}

func (s *Server) snapshotWorldWorkers() []WorldWorkerEndpoint {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]WorldWorkerEndpoint, len(s.worldWorkers))
	for i, w := range s.worldWorkers {
		out[i] = *w
	}
	return out
}

func (s *Server) handleWorldWorkers(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	worldStatus := s.fetchMultiWorkerWorldStatus(r.Context())
	workers := s.snapshotWorldWorkers()
	writeCachedJSONWithModTime(w, r, "private, max-age=2", s.lastMutatedTime(), map[string]any{
		"schema_version": "1.0", "count": len(workers), "workers": workers,
		"items": workers, "world_status": worldStatus, "serverTimeMs": s.now().UnixMilli(),
	})
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
	payload["workers"] = s.snapshotWorldWorkers()
	s.metaCache.set(cacheKey, payload, 5*time.Second, s.now())
	writeCachedJSONWithModTime(w, r, "private, max-age=5, must-revalidate", s.lastMutatedTime(), payload)
}

func (s *Server) workerAdminClient(w *WorldWorkerEndpoint) worldadminv1.WorldAdminServiceClient {
	if w == nil {
		return s.getWorldAdminClient()
	}
	if w.AdminClient != nil {
		return w.AdminClient
	}
	s.grpcMu.Lock()
	defer s.grpcMu.Unlock()
	if w.AdminClient != nil {
		return w.AdminClient
	}
	if strings.TrimSpace(w.AdminAddr) == "" {
		return nil
	}
	conn, err := grpc.NewClient(w.AdminAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(64*1024*1024)))
	if err != nil {
		return nil
	}
	w.adminConn = conn
	w.AdminClient = worldadminv1.NewWorldAdminServiceClient(conn)
	return w.AdminClient
}

func (s *Server) workerStreamClient(w *WorldWorkerEndpoint) worldadminv1.WorldStreamServiceClient {
	if w == nil {
		return s.getWorldStreamClient()
	}
	if w.StreamClient != nil {
		return w.StreamClient
	}
	s.grpcMu.Lock()
	defer s.grpcMu.Unlock()
	if w.StreamClient != nil {
		return w.StreamClient
	}
	if strings.TrimSpace(w.StreamAddr) == "" {
		return nil
	}
	conn, err := grpc.NewClient(w.StreamAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(64*1024*1024)))
	if err != nil {
		return nil
	}
	w.streamConn = conn
	w.StreamClient = worldadminv1.NewWorldStreamServiceClient(conn)
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

func (s *Server) fetchMultiWorkerWorldStatus(ctx context.Context) map[string]any {
	monstersByScope := make(map[string]int)
	monstersCount := 0
	for _, snap := range s.coalescer.Snapshot(worldstream.ViewportFilter{}) {
		if snap.Kind != "player" && snap.Kind != "resource_node" && snap.Kind != "mount" && snap.Kind != "loot_pile" && isMonsterSnapshot(snap) {
			monstersCount++
			monstersByScope[strings.ToLower(snap.Zone)]++
		}
	}
	poisCount := 0
	if s.catalog != nil {
		poisCount = len(s.catalog.AllPlaces())
	}
	results := make([]*worldadminv1.GetStatusResponse, len(s.worldWorkers))
	var wg sync.WaitGroup
	for i, w := range s.worldWorkers {
		cli := s.workerStreamClient(w)
		if cli == nil {
			continue
		}
		wg.Add(1)
		go func(idx int, c worldadminv1.WorldStreamServiceClient) {
			defer wg.Done()
			rctx, cancel := context.WithTimeout(ctx, 1200*time.Millisecond)
			defer cancel()
			if st, err := callWithRetry(rctx, s, func(rc context.Context) (*worldadminv1.GetStatusResponse, error) {
				return c.GetStatus(rc, &worldadminv1.GetStatusRequest{})
			}); err == nil && st != nil {
				results[idx] = st
			}
		}(i, cli)
	}
	wg.Wait()
	return s.aggregateWorkerStatusResults(results, monstersByScope, monstersCount, poisCount)
}

func (s *Server) orderedAdminWorkersForCommand(p map[string]any, targetID string) []*WorldWorkerEndpoint {
	pref := s.inferPreferredWorkerScope(p, targetID)
	out := make([]*WorldWorkerEndpoint, 0, len(s.worldWorkers))
	used := make(map[int]bool, len(s.worldWorkers))
	if pref != "" {
		for i, w := range s.worldWorkers {
			if strings.EqualFold(w.WorkerID, pref) || strings.EqualFold(w.ScopeKey, pref) || strings.EqualFold(w.ContainerName, pref) {
				out = append(out, w)
				used[i] = true
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
	placeOrNode := firstNonEmpty(extractStringParam(p, "place", "destination", "node_name", "node"), targetID)
	if sp, ok := findCanonicalNodeSpec(placeOrNode); ok {
		return inferRegionFromNodeName(sp.Name)
	}
	if x, hasX := extractFloatParam(p, "x_cm", "x"); hasX {
		if y, hasY := extractFloatParam(p, "y_cm", "y"); hasY {
			return inferRegionFromCoords(x, y)
		}
	}
	for _, snap := range s.coalescer.Snapshot(worldstream.ViewportFilter{}) {
		if snap.Kind == "player" && strings.EqualFold(snap.Name, targetID) && snap.Zone != "" && snap.Zone != "verra" {
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

func (s *Server) pruneDespawnedWorkerEntities(scope string, area *worldadminv1.AreaBox, effLayers uint32, seenIDs map[string]struct{}) {
	for _, ent := range s.coalescer.Snapshot(worldstream.ViewportFilter{}) {
		if ent.Kind == "player" {
			if authReg := s.authoritativeRegionForPlayer(ent.Name); authReg != "" && !strings.EqualFold(ent.Zone, authReg) {
				s.coalescer.RemoveEntity(ent.ID)
			}
			continue
		}
		if _, seen := seenIDs[ent.ID]; seen {
			continue
		}
		if !entityBelongsToWorkerScope(ent, scope) || !entityInAreaAndLayers(ent, area, effLayers) {
			continue
		}
		s.coalescer.RemoveEntity(ent.ID)
	}
}

func entityBelongsToWorkerScope(ent worldstream.EntitySnapshot, scope string) bool {
	if scope != "" && scope != "verra" && scope != "all" {
		return strings.HasPrefix(ent.ID, scope+":") || strings.EqualFold(ent.Zone, scope)
	}
	return strings.EqualFold(ent.Zone, "verra") || strings.EqualFold(ent.Zone, scope)
}

func entityInAreaAndLayers(ent worldstream.EntitySnapshot, area *worldadminv1.AreaBox, effLayers uint32) bool {
	if area != nil {
		if ent.X < float64(area.GetMinXCm()) || ent.X > float64(area.GetMaxXCm()) ||
			ent.Y < float64(area.GetMinYCm()) || ent.Y > float64(area.GetMaxYCm()) {
			return false
		}
	}
	switch ent.Kind {
	case "npc", "monster":
		return (effLayers & StreamLayerNpcs) != 0
	case "resource_node":
		return (effLayers & StreamLayerResourceNodes) != 0
	case "mount":
		return (effLayers & StreamLayerMounts) != 0
	case "loot_pile":
		return (effLayers & StreamLayerLootPiles) != 0
	default:
		return false
	}
}

func (s *Server) refreshStreamAfterNodeTierChange(ctx context.Context) {
	if s.getCityNodeClient() != nil {
		time.Sleep(120 * time.Millisecond)
	}
	s.mu.Lock()
	clear(s.lastStreamAt)
	s.mu.Unlock()
	s.pullWorldStreamSnapshot(ctx, worldstream.ViewportFilter{}, StreamLayerAll)
}

func (s *Server) execSetSimulateAllNpcsCommand(ctx context.Context, p map[string]any) (map[string]any, error) {
	enabled := extractSimulateAllNpcsBool(p)
	level := int32(0)
	if enabled {
		level = 1
	}
	var wg sync.WaitGroup
	for _, w := range s.worldWorkers {
		if w == nil {
			continue
		}
		wg.Add(1)
		go func(worker *WorldWorkerEndpoint) {
			defer wg.Done()
			rctx, cancel := context.WithTimeout(ctx, 1200*time.Millisecond)
			defer cancel()
			if cli := s.workerAdminClient(worker); cli != nil {
				_, _ = callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.SpawnNpcResponse, error) {
					return cli.SpawnNpc(c, &worldadminv1.SpawnNpcRequest{
						Npc: "gm.SetSimulateAllNpcs", Level: level,
						Target: &worldadminv1.SpawnNpcRequest_Place{Place: fmt.Sprintf("%t", enabled)},
					})
				})
			}
		}(w)
	}
	wg.Wait()
	return map[string]any{
		"status": "executed", "action": "set_simulate_all_npcs", "command": "set_simulate_all_npcs",
		"kind": "gm.SetSimulateAllNpcs", "payload_json": fmt.Sprintf(`{"enabled":%t}`, enabled),
		"simulate_all_npcs": enabled, "enabled": enabled,
	}, nil
}

func extractSimulateAllNpcsBool(p map[string]any) bool {
	for _, k := range []string{"enabled", "simulate_all_npcs", "amount", "value"} {
		v, ok := p[k]
		if !ok || v == nil {
			continue
		}
		switch tv := v.(type) {
		case bool:
			return tv
		case string:
			switch strings.ToLower(strings.TrimSpace(tv)) {
			case "false", "0", "off", "no":
				return false
			case "true", "1", "on", "yes":
				return true
			}
		default:
			if iv, hasInt := extractInt64Param(p, k); hasInt {
				return iv != 0
			}
		}
	}
	return true
}

func extractNodeIdentParam(p map[string]any, fallback string) string {
	if s := extractStringParam(p, "node", "node_name", "node_guid", "node_id", "guid", "name"); s != "" {
		return s
	}
	if id, ok := extractInt64Param(p, "node_id", "node_guid", "guid"); ok && id != 0 {
		return fmt.Sprintf("%d", id)
	}
	return fallback
}

type serverPlayerKey struct {
	srv    *Server
	player string
}

type playerRegionalHandoverState struct {
	AuthoritativeRegion, AuthoritativeWorker, CandidateRegion string
	DwellSamples, HandoverCount, LeaseEpoch                   int
	LastHandoverAt                                            time.Time
	SuppressedByHysteresis                                    bool
}

var (
	playerHandoversMu sync.Mutex
	playerHandovers   = make(map[serverPlayerKey]*playerRegionalHandoverState)
)

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
	out["status"], out["action"], out["character"] = "executed", "step_player_border", charName
	out["x"], out["y"], out["z"] = x, y, z
	return out, nil
}

func (s *Server) evaluatePlayerRegionalHandover(charName string, x, y, z float64, p map[string]any) map[string]any {
	key := serverPlayerKey{srv: s, player: strings.ToLower(strings.TrimSpace(firstNonEmpty(charName, "TestHero")))}
	isTeleport, _ := p["is_teleport"].(bool)
	now := s.now()
	playerHandoversMu.Lock()
	st, ok := playerHandovers[key]
	if !ok {
		st = &playerRegionalHandoverState{}
		playerHandovers[key] = st
	}
	targetReg, depthCm := computeRegionAndBorderDepth(st.AuthoritativeRegion, x, y, p)
	st.stepTo(targetReg, depthCm, isTeleport, now)
	res := map[string]any{
		"authoritative_region": st.AuthoritativeRegion, "authoritative_worker": st.AuthoritativeWorker,
		"zone": st.AuthoritativeRegion, "worker_id": st.AuthoritativeWorker,
		"handover_count": st.HandoverCount, "lease_epoch": st.LeaseEpoch,
		"hysteresis_suppressed": st.SuppressedByHysteresis, "dwell_samples": st.DwellSamples,
	}
	authReg := st.AuthoritativeRegion
	playerHandoversMu.Unlock()
	s.syncAuthoritativePlayerInCoalescer(firstNonEmpty(charName, "TestHero"), x, y, authReg, !isTeleport)
	return res
}

func (st *playerRegionalHandoverState) stepTo(targetReg string, depthCm float64, isTeleport bool, now time.Time) {
	if st.AuthoritativeRegion == "" {
		st.AuthoritativeRegion, st.AuthoritativeWorker = targetReg, "aoc-world-"+strings.ReplaceAll(targetReg, "_", "-")
		st.LeaseEpoch, st.SuppressedByHysteresis = 1, false
		return
	}
	if targetReg == st.AuthoritativeRegion {
		st.CandidateRegion, st.DwellSamples, st.SuppressedByHysteresis = "", 0, false
		return
	}
	if !isTeleport && depthCm <= 5000.0 {
		st.CandidateRegion, st.DwellSamples, st.SuppressedByHysteresis = targetReg, 0, true
		return
	}
	if !isTeleport && !st.LastHandoverAt.IsZero() && now.Sub(st.LastHandoverAt) < 2*time.Second {
		st.SuppressedByHysteresis = true
		return
	}
	if !isTeleport {
		if st.CandidateRegion == targetReg {
			st.DwellSamples++
		} else {
			st.CandidateRegion, st.DwellSamples = targetReg, 1
		}
		if st.DwellSamples < 3 {
			st.SuppressedByHysteresis = true
			return
		}
	}
	st.AuthoritativeRegion, st.AuthoritativeWorker = targetReg, "aoc-world-"+strings.ReplaceAll(targetReg, "_", "-")
	st.CandidateRegion, st.DwellSamples, st.SuppressedByHysteresis = "", 0, false
	st.LastHandoverAt, st.HandoverCount, st.LeaseEpoch = now, st.HandoverCount+1, st.LeaseEpoch+1
}

func computeRegionAndBorderDepth(currRegion string, x, y float64, p map[string]any) (string, float64) {
	targetReg := inferRegionFromCoords(x, y)
	if explicit := extractStringParam(p, "target_region", "region"); explicit != "" && !strings.EqualFold(explicit, "verra") {
		targetReg = strings.ToLower(explicit)
	}
	if currRegion != "" {
		if bd, hasBD := extractFloatParam(p, "border_distance_cm", "border_offset_cm"); hasBD {
			if targetReg == currRegion {
				targetReg = "sandsquall"
			}
			return targetReg, math.Abs(bd)
		}
		if math.Abs(y) <= 5000.0 && ((currRegion == "riverlands" && y >= 0) || (currRegion == "sandsquall" && y <= 0)) {
			return "sandsquall", math.Abs(y)
		}
	}
	if currRegion == "" || targetReg == currRegion {
		return targetReg, 0
	}
	return targetReg, 25000.0
}

func (s *Server) syncAuthoritativePlayerInCoalescer(charName string, x, y float64, authRegion string, upsertIfMissing bool) {
	found, canonID := false, "player:"+strings.ToLower(charName)
	for _, ent := range s.coalescer.Snapshot(worldstream.ViewportFilter{}) {
		if ent.Kind == "player" && strings.EqualFold(ent.Name, charName) {
			found = true
			if ent.ID != canonID {
				s.coalescer.RemoveEntity(ent.ID)
			}
		}
	}
	if found || upsertIfMissing {
		s.coalescer.UpsertEntity(worldstream.EntitySnapshot{ID: canonID, Kind: "player", Name: charName, Zone: authRegion, X: x, Y: y, Level: 50})
	}
}
