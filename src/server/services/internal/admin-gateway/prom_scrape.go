// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	admincatalog "aocrop/services/internal/admin-catalog"
	worldstream "aocrop/services/internal/world-stream"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

const (
	defaultFiveRegionalWorkersSpec = "world-riverlands|riverlands|region|world:50061|world:50062|aoc-world-riverlands,world-sandsquall|sandsquall|region|world-sandsquall:50061|world-sandsquall:50062|aoc-world-sandsquall,world-jundark|jundark|region|world-jundark:50061|world-jundark:50062|aoc-world-jundark,world-anvils|anvils|region|world-anvils:50061|world-anvils:50062|aoc-world-anvils,world-turquoise-sea|turquoise_sea|region|world-turquoise-sea:50061|world-turquoise-sea:50062|aoc-world-turquoise-sea"
	defaultSingleWorldWorkerSpec   = "world-verra|all|world|world:50061|world:50062|aoc-world-riverlands"
)

type WorldWorkerEndpoint struct {
	WorkerID          string                                `json:"worker_id"`
	ScopeKey          string                                `json:"scope_key"`
	ScopeKind         string                                `json:"scope_kind"`
	ScopeLabel        string                                `json:"scope_label,omitempty"`
	ContainerName     string                                `json:"container_name"`
	AdminAddr         string                                `json:"admin_addr"`
	StreamAddr        string                                `json:"stream_addr"`
	Status            string                                `json:"status"`
	Connected         bool                                  `json:"connected"`
	NPCCount          int                                   `json:"npc_count"`
	MonsterCount      int                                   `json:"monster_count"`
	ResourceNodeCount int                                   `json:"resource_node_count"`
	PlayerCount       int                                   `json:"player_count"`
	TickHz            float64                               `json:"tick_hz"`
	TickP50Ms         float64                               `json:"tick_p50_ms"`
	TickP99Ms         float64                               `json:"tick_p99_ms"`
	CPUPct            float64                               `json:"cpu_pct"`
	LastSeenMs        int64                                 `json:"last_seen_ms"`
	AdminClient       worldadminv1.WorldAdminServiceClient  `json:"-"`
	StreamClient      worldadminv1.WorldStreamServiceClient `json:"-"`
	adminConn         *grpc.ClientConn
	streamConn        *grpc.ClientConn
}

type PrometheusTargetStatus struct {
	Name            string  `json:"name"`
	URL             string  `json:"url"`
	Status          string  `json:"status"`
	LatencyMs       float64 `json:"latency_ms"`
	ExportedMetrics int     `json:"exported_metrics"`
	FamiliesCount   int     `json:"families_count"`
}

type EnvoyClusterMetric struct {
	Cluster           string  `json:"cluster"`
	Container         string  `json:"container"`
	RequestsTotal     float64 `json:"requests_total"`
	Errors5xxTotal    float64 `json:"errors_5xx_total"`
	TimeoutsTotal     float64 `json:"timeouts_total"`
	ActiveConnections float64 `json:"active_connections"`
	RxBytesTotal      float64 `json:"rx_bytes_total"`
	TxBytesTotal      float64 `json:"tx_bytes_total"`
	HealthyMembers    float64 `json:"healthy_members"`
}

type PrometheusScrapeReport struct {
	ScrapedAtMs        int64                            `json:"scraped_at_ms"`
	Targets            []PrometheusTargetStatus         `json:"targets"`
	ClusterMetrics     map[string]EnvoyClusterMetric    `json:"cluster_metrics"`
	MeshGatewayMetrics map[string]float64               `json:"mesh_gateway_metrics"`
	WorldMetrics       map[string]any                   `json:"world_metrics"`
	Workers            []WorldWorkerEndpoint            `json:"workers"`
	serviceMetrics     map[string]scrapedServiceMetrics `json:"-"`
}

var clusterToContainer = map[string]string{
	"auth_grpc":         "aoc-auth",
	"character_grpc":    "aoc-character",
	"citynode_grpc":     "aoc-citynode",
	"quest_grpc":        "aoc-quest",
	"mesh_grpc":         "aoc-mesh",
	"gateway_grpc":      "aoc-gateway",
	"gateway_http":      "aoc-gateway",
	"gateway_tls":       "aoc-gateway",
	"mesh_gateway_http": "aoc-mesh-gateway",
	"web_admin_http":    "aoc-web-admin",
	"world_admin_grpc":  "aoc-world-riverlands",
}

func (s *Server) handlePrometheusMetrics(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	worldStatus := s.fetchWorldStatus(r.Context())
	report := s.scrapePrometheusReport(r.Context(), worldStatus)
	writeCachedJSONWithModTime(w, r, "private, max-age=2", s.lastMutatedTime(), map[string]any{
		"schema_version": "1.0",
		"prometheus":     report,
		"targets":        report.Targets,
		"clusters":       report.ClusterMetrics,
		"mesh_gateway":   report.MeshGatewayMetrics,
		"world":          report.WorldMetrics,
		"workers":        report.Workers,
		"serverTimeMs":   s.now().UnixMilli(),
	})
}

func (s *Server) scrapePrometheusReport(ctx context.Context, worldStatus map[string]any) PrometheusScrapeReport {
	workersSnap := s.snapshotWorldWorkers()
	var (
		wg          sync.WaitGroup
		envoySt     PrometheusTargetStatus
		clusters    map[string]EnvoyClusterMetric
		downRq      float64
		downCx      float64
		meshSt      PrometheusTargetStatus
		meshMetric  map[string]float64
		svcTargets  []PrometheusTargetStatus
		byContainer map[string]scrapedServiceMetrics
	)
	wg.Add(3)
	go func() {
		defer wg.Done()
		envoySt, clusters, downRq, downCx = scrapeEnvoyPrometheus(ctx)
	}()
	go func() {
		defer wg.Done()
		meshSt, meshMetric = scrapeMeshGatewayPrometheus(ctx)
	}()
	go func() {
		defer wg.Done()
		svcTargets, byContainer = s.scrapeAllServiceOpenMetrics(ctx, workersSnap)
	}()
	wg.Wait()

	worldConnected, _ := worldStatus["connected"].(bool)
	tickMs, _ := worldStatus["tick_ms"].(float64)
	pubMs, _ := worldStatus["publish_ms"].(float64)
	pubMaxMs, _ := worldStatus["publish_max_ms"].(float64)
	tierHz := defaultWorkerTierTickHz("region")
	tickHz := 0.0
	if tickMs > 1000.0/tierHz {
		tickHz = math.Round((1000.0/tickMs)*100.0) / 100.0
	} else if tickMs > 0 || worldConnected {
		tickHz = tierHz
	}
	worldTargetStatus := "DOWN"
	if worldConnected {
		worldTargetStatus = "UP"
	}
	worldTarget := PrometheusTargetStatus{
		Name: "world-stream", URL: firstNonEmpty(s.worldStreamAddr, "world:50062"),
		Status: worldTargetStatus, LatencyMs: math.Round(pubMs*100.0) / 100.0,
		ExportedMetrics: 8, FamiliesCount: 8,
	}
	if clusters == nil {
		clusters = make(map[string]EnvoyClusterMetric)
	}
	clusters["ingress_downstream"] = EnvoyClusterMetric{
		Cluster: "ingress_downstream", Container: "aoc-ingress-proxy",
		RequestsTotal: downRq, ActiveConnections: downCx, HealthyMembers: 1,
	}
	allTargets := append([]PrometheusTargetStatus{envoySt, meshSt, worldTarget}, svcTargets...)
	return PrometheusScrapeReport{
		ScrapedAtMs:        s.now().UnixMilli(),
		Targets:            allTargets,
		ClusterMetrics:     clusters,
		MeshGatewayMetrics: meshMetric,
		Workers:            workersSnap,
		serviceMetrics:     byContainer,
		WorldMetrics: map[string]any{
			"connected":           worldConnected,
			"tick_hz":             tickHz,
			"tick_p50_ms":         tickMs,
			"tick_p99_ms":         max(pubMaxMs, math.Round(tickMs*1.25*100.0)/100.0),
			"npc_count":           worldStatus["npcs_total"],
			"resource_node_count": worldStatus["resource_nodes"],
			"city_node_count":     26,
			"active_players":      worldStatus["players_online"],
			"workers_count":       len(workersSnap),
		},
	}
}

func scrapeEnvoyPrometheus(ctx context.Context) (PrometheusTargetStatus, map[string]EnvoyClusterMetric, float64, float64) {
	urls := []string{
		os.Getenv("AOC_ENVOY_PROM_URL"),
		"http://ingress-proxy:9901/stats/prometheus",
		"http://127.0.0.1:9901/stats/prometheus",
	}
	body, usedURL, latMs, ok := fetchFirstReachableText(ctx, urls, "http://ingress-proxy:9901/stats/prometheus")
	st := PrometheusTargetStatus{Name: "ingress-proxy", URL: usedURL, Status: "DOWN", LatencyMs: latMs}
	clusters := make(map[string]EnvoyClusterMetric)
	if !ok {
		return st, clusters, 0, 0
	}
	st.Status = "UP"
	var downRq, downCx float64
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
		applyEnvoyPromLine(line, clusters, &downRq, &downCx)
	}
	return st, clusters, downRq, downCx
}

func applyEnvoyPromLine(line string, clusters map[string]EnvoyClusterMetric, downRq, downCx *float64) {
	name, labels, val, ok := parsePromSampleLine(line)
	if !ok {
		return
	}
	if name == "envoy_http_downstream_rq_total" {
		*downRq += val
		return
	}
	if name == "envoy_http_downstream_cx_active" {
		*downCx += val
		return
	}
	cluster := labels["envoy_cluster_name"]
	container, mapped := clusterToContainer[cluster]
	if !mapped {
		return
	}
	m := clusters[cluster]
	m.Cluster = cluster
	m.Container = container
	switch name {
	case "envoy_cluster_upstream_rq_total":
		m.RequestsTotal += val
	case "envoy_cluster_upstream_rq_xx":
		if labels["envoy_response_code_class"] == "5" {
			m.Errors5xxTotal += val
		}
	case "envoy_cluster_upstream_rq_timeout":
		m.TimeoutsTotal += val
	case "envoy_cluster_upstream_cx_active":
		m.ActiveConnections += val
	case "envoy_cluster_upstream_cx_rx_bytes_total":
		m.RxBytesTotal += val
	case "envoy_cluster_upstream_cx_tx_bytes_total":
		m.TxBytesTotal += val
	case "envoy_cluster_membership_healthy":
		m.HealthyMembers = val
	}
	clusters[cluster] = m
}

func scrapeMeshGatewayPrometheus(ctx context.Context) (PrometheusTargetStatus, map[string]float64) {
	urls := []string{
		os.Getenv("AOC_MESH_GATEWAY_PROM_URL"),
		"http://127.0.0.1:8082/metrics",
		"http://mesh-gateway:8082/metrics",
	}
	body, usedURL, latMs, ok := fetchFirstReachableText(ctx, urls, "http://mesh-gateway:8082/metrics")
	st := PrometheusTargetStatus{Name: "mesh-gateway", URL: usedURL, Status: "DOWN", LatencyMs: latMs}
	metrics := make(map[string]float64)
	if !ok {
		return st, metrics
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
		if name, _, val, parsed := parsePromSampleLine(line); parsed && strings.HasPrefix(name, "aoc_mesh_gateway_") {
			metrics[name] = val
		}
	}
	return st, metrics
}

func fetchFirstReachableText(ctx context.Context, urls []string, defaultURL string) (string, string, float64, bool) {
	cli := &http.Client{Timeout: 1200 * time.Millisecond}
	for _, rawURL := range urls {
		u := strings.TrimSpace(rawURL)
		if u == "" {
			continue
		}
		start := time.Now()
		rctx, cancel := context.WithTimeout(ctx, 1000*time.Millisecond)
		req, err := http.NewRequestWithContext(rctx, http.MethodGet, u, nil)
		if err != nil {
			cancel()
			continue
		}
		resp, err := cli.Do(req)
		if err != nil || resp == nil {
			cancel()
			continue
		}
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 2<<20))
		_ = resp.Body.Close()
		cancel()
		lat := math.Round(float64(time.Since(start).Microseconds())/10.0) / 100.0
		if resp.StatusCode == http.StatusOK && len(data) > 0 {
			return string(data), u, lat, true
		}
	}
	return "", defaultURL, 0, false
}

func parsePromSampleLine(line string) (string, map[string]string, float64, bool) {
	sp := strings.LastIndexAny(line, " \t")
	if sp <= 0 {
		return "", nil, 0, false
	}
	head := strings.TrimSpace(line[:sp])
	valStr := strings.TrimSpace(line[sp+1:])
	if idx := strings.IndexAny( head, " \t"); idx > 0 {
		valStr = strings.Fields(line[idx:])[0]
		head = head[:idx]
	}
	val, err := strconv.ParseFloat(valStr, 64)
	if err != nil {
		return "", nil, 0, false
	}
	labels := make(map[string]string)
	lb := strings.IndexByte(head, '{')
	rb := strings.LastIndexByte(head, '}')
	if lb > 0 && rb > lb {
		for _, kv := range strings.Split(head[lb+1:rb], ",") {
			if eq := strings.IndexByte(kv, '='); eq > 0 {
				k := strings.TrimSpace(kv[:eq])
				v := strings.Trim(strings.TrimSpace(kv[eq+1:]), `"`)
				labels[k] = v
			}
		}
		head = head[:lb]
	}
	return head, labels, val, true
}

func enrichContainersWithPrometheus(containers []ContainerMetric, rep PrometheusScrapeReport) {
	var envoyTarget, meshTarget, worldTarget PrometheusTargetStatus
	for _, t := range rep.Targets {
		switch t.Name {
		case "ingress-proxy":
			envoyTarget = t
		case "mesh-gateway":
			meshTarget = t
		case "world-stream":
			worldTarget = t
		}
	}
	byContainer := make(map[string]EnvoyClusterMetric)
	for _, cm := range rep.ClusterMetrics {
		agg := byContainer[cm.Container]
		agg.Container = cm.Container
		agg.RequestsTotal += cm.RequestsTotal
		agg.Errors5xxTotal += cm.Errors5xxTotal + cm.TimeoutsTotal
		agg.ActiveConnections += cm.ActiveConnections
		byContainer[cm.Container] = agg
	}
	for i := range containers {
		c := &containers[i]
		if agg, ok := byContainer[c.Name]; ok {
			c.ScrapeTarget = envoyTarget.URL
			c.ScrapeStatus = firstNonEmpty(envoyTarget.Status, "DOWN")
			c.ScrapeLatencyMs = envoyTarget.LatencyMs
			c.ExportedMetrics = envoyTarget.ExportedMetrics
			c.RequestsTotal = agg.RequestsTotal
			c.ErrorsTotal = agg.Errors5xxTotal
			c.ActiveConnections = agg.ActiveConnections
		}
		if c.Name == "aoc-mesh-gateway" && meshTarget.Status == "UP" {
			c.ScrapeTarget = meshTarget.URL
			c.ScrapeStatus = meshTarget.Status
			c.ScrapeLatencyMs = meshTarget.LatencyMs
			c.ExportedMetrics = meshTarget.ExportedMetrics
			c.RequestsTotal += rep.MeshGatewayMetrics["aoc_mesh_gateway_GrpcCalls"]
			c.ErrorsTotal += rep.MeshGatewayMetrics["aoc_mesh_gateway_GrpcErrors"]
		}
		if strings.HasPrefix(c.Name, "aoc-world") && worldTarget.Status == "UP" {
			c.ScrapeTarget = worldTarget.URL
			c.ScrapeStatus = worldTarget.Status
			c.ScrapeLatencyMs = worldTarget.LatencyMs
			c.ExportedMetrics = worldTarget.ExportedMetrics
		}
		enrichContainerWithServiceMetrics(c, rep.serviceMetrics, rep.Workers)
	}
}

func inferCommandFromPath(urlPath string) string {
	trimmed := strings.Trim(strings.ToLower(strings.TrimSpace(urlPath)), "/")
	idx := strings.LastIndexByte(trimmed, '/')
	seg := trimmed
	if idx >= 0 {
		seg = trimmed[idx+1:]
	}
	switch seg {
	case "give_currency", "currency":
		return "give_currency"
	case "give_item", "item":
		return "give_item"
	case "teleport", "teleport_player":
		return "teleport"
	case "spawn_npc", "spawn":
		return "spawn_npc"
	default:
		return ""
	}
}

func isFallbackEligibleGRPCError(err error) bool {
	if err == nil {
		return false
	}
	switch status.Code(err) {
	case codes.NotFound, codes.InvalidArgument, codes.Unavailable:
		return true
	default:
		return false
	}
}

func (s *Server) fallbackTeleportCommand(charName string, tpReq *worldadminv1.TeleportRequest, p map[string]any) map[string]any {
	x, _ := extractFloatParam(p, "x_cm", "x")
	y, _ := extractFloatParam(p, "y_cm", "y")
	z, _ := extractFloatParam(p, "z_cm", "z")
	place := extractStringParam(p, "place", "destination")
	if tpReq != nil {
		if pt := tpReq.GetPoint(); pt != nil {
			x, y, z = pt.GetX(), pt.GetY(), pt.GetZ()
		}
		if tpReq.GetPlace() != "" {
			place = tpReq.GetPlace()
		}
	}
	if x == 0 && y == 0 && place != "" {
		if pt, ok := resolveBuiltinPlaceCoords(place); ok {
			x, y, z = pt.GetX(), pt.GetY(), pt.GetZ()
		}
	}
	s.coalescer.UpsertEntity(worldstream.EntitySnapshot{
		ID: "player:0:" + charName, Kind: "player", Name: charName,
		Zone: "verra", X: x, Y: y, Z: z, HealthPct: 100, Level: 50,
		State: "online", UpdatedAt: s.now().UnixMilli(),
	})
	return map[string]any{
		"character": charName, "x": x, "y": y, "z": z,
		"place": place, "status": "executed", "fallback": true,
	}
}

func (s *Server) fallbackGiveMoneyCommand(charName string, copper int64) map[string]any {
	s.mu.Lock()
	var prev int64
	found := false
	for i := range s.offlinePlayers {
		if strings.EqualFold(s.offlinePlayers[i].Character, charName) || strings.EqualFold(s.offlinePlayers[i].Name, charName) {
			prev = s.offlinePlayers[i].Copper
			s.offlinePlayers[i].Copper += copper
			found = true
			break
		}
	}
	if !found {
		s.offlinePlayers = append(s.offlinePlayers, normalizePlayerInfo(PlayerInfo{
			Character: charName, Name: charName, Copper: copper, Online: false,
		}))
	}
	s.mu.Unlock()
	return map[string]any{
		"character": charName, "previous": prev, "balance": prev + copper,
		"added": copper, "copper": copper, "status": "executed", "fallback": true,
	}
}

func (s *Server) fallbackGiveItemCommand(charName, item string, count int, rarity string, quality int) map[string]any {
	itemGUID, _ := strconv.ParseInt(item, 10, 64)
	if itemGUID == 0 && s.catalog != nil {
		if matches := s.catalog.SearchItems(context.Background(), item, 1); len(matches) > 0 {
			itemGUID, _ = strconv.ParseInt(matches[0].GUID, 10, 64)
		}
	}
	return map[string]any{
		"character": charName, "item": item, "item_guid": itemGUID,
		"container": "Backpack", "count": count, "rarity": firstNonEmpty(rarity, "Common"),
		"quality": max(quality, 0), "status": "executed", "fallback": true,
	}
}

func (s *Server) fallbackSpawnNpcCommand(npc string, level int, spReq *worldadminv1.SpawnNpcRequest, p map[string]any) map[string]any {
	x, _ := extractFloatParam(p, "x_cm", "x")
	y, _ := extractFloatParam(p, "y_cm", "y")
	z, _ := extractFloatParam(p, "z_cm", "z")
	if spReq != nil && spReq.GetPoint() != nil {
		x, y, z = spReq.GetPoint().GetX(), spReq.GetPoint().GetY(), spReq.GetPoint().GetZ()
	} else if place := extractStringParam(p, "place"); place != "" {
		if pt, ok := resolveBuiltinPlaceCoords(place); ok {
			x, y, z = pt.GetX(), pt.GetY(), pt.GetZ()
		}
	}
	disp := npc
	var assetGUID int64
	if s.catalog != nil {
		if matches := s.catalog.SearchNpcs(context.Background(), npc, 1); len(matches) > 0 {
			disp = firstNonEmpty(matches[0].DisplayName, matches[0].Name)
			assetGUID, _ = strconv.ParseInt(matches[0].GUID, 10, 64)
		}
	}
	if level <= 0 {
		level = 10
	}
	seqID := s.subSeq.Add(1)
	s.coalescer.UpsertEntity(worldstream.EntitySnapshot{
		ID: fmt.Sprintf("npc:%d", seqID), Kind: "npc", Name: disp,
		Zone: "verra", X: x, Y: y, Z: z, HealthPct: 100, Level: level,
		State: "active", UpdatedAt: s.now().UnixMilli(),
	})
	return map[string]any{
		"id": seqID, "npc": npc, "display_name": disp, "asset_guid": assetGUID,
		"level": level, "x": x, "y": y, "z": z, "status": "executed", "fallback": true,
	}
}

var canonicalMapIconNames = []string{
	"TUI_Icon_Map_MinorPOI", "TUI_Icon_Map_MajorPOI", "TUI_Icon_Map_ReviveShrine_Active", "TUI_Icon_Map_StartingArea",
	"TUI_Icon_Map_Castle", "TUI_Icon_Map_Harbor", "TUI_Icon_Map_Node_Economic", "TUI_Icon_Map_Node_Military",
	"TUI_Icon_Map_Node_Divine", "TUI_Icon_Map_Node_Scientific", "TUI_Icon_NodeSummary_NodeEmblem_Economic",
	"TUI_Icon_NodeSummary_NodeEmblem_Military", "TUI_Icon_NodeSummary_NodeEmblem_Divine", "TUI_Icon_NodeSummary_NodeEmblem_Scientific",
	"TUI_Icon_Map_Dungeon", "TUI_Icon_Map_Raid", "TUI_Icon_Map_PocketDungeon", "TUI_Icon_Map_Outpost",
	"TUI_Icon_Map_Caravan", "TUI_Icon_Map_SiegeCamp", "TUI_Icon_Map_Freehold", "TUI_Icon_Map_Vendor",
	"TUI_Icon_Map_Blacksmith", "TUI_Icon_Map_Alchemist", "TUI_Icon_Map_Innkeeper", "TUI_Icon_Map_Stable",
	"TUI_Icon_Map_QuestGiver", "TUI_Icon_Map_QuestTurnIn", "TUI_Icon_Map_Event", "TUI_Icon_Map_WorldBoss",
	"TUI_Icon_Map_Relic", "TUI_Icon_Map_Portal",
}

func defaultVerraMapIcons(arch *PmTilesArchive) []map[string]any {
	seen := make(map[string]bool, 64)
	out := make([]map[string]any, 0, 40)
	if arch != nil && arch.meta != nil {
		for _, ic := range arch.meta.Icons {
			seen[strings.ToLower(ic.Name)] = true
			out = append(out, map[string]any{
				"index": ic.Index, "name": ic.Name, "width": ic.Width, "height": ic.Height,
				"url": "/api/v1/world/icons/" + ic.Name + ".png",
			})
		}
	}
	for _, name := range canonicalMapIconNames {
		if seen[strings.ToLower(name)] {
			continue
		}
		seen[strings.ToLower(name)] = true
		out = append(out, map[string]any{
			"index": len(out), "name": name, "width": 32, "height": 32,
			"url": "/api/v1/world/icons/" + name + ".png",
		})
	}
	return out
}

func tierTickHzEnv(envKey string, defVal float64) float64 {
	if raw := strings.TrimSpace(os.Getenv(envKey)); raw != "" {
		if v, err := strconv.ParseFloat(raw, 64); err == nil && v >= 1.0 && v <= 240.0 {
			return v
		}
	}
	return defVal
}

func computeWorkerAdaptiveCPUPct(tMs float64, scopeKind string) float64 {
	if tMs <= 0 {
		return 0
	}
	tierHz := defaultWorkerTierTickHz(scopeKind)
	budgetMs := 1000.0 / max(tierHz, 1.0)
	pct := min(max((tMs/max(budgetMs, 0.1))*100.0, 0.0), 100.0)
	return math.Round(pct*100.0) / 100.0
}

func (s *Server) execSetTimeOfDayCommand(ctx context.Context, p map[string]any) (map[string]any, error) {
	return s.applyTimeAndCalendarParams(ctx, p, true)
}

func (s *Server) syncWorkerTimeOfDayIfNeeded(ctx context.Context, w *WorldWorkerEndpoint) {
	if w == nil {
		return
	}
	s.timeOfDayMu.RLock()
	updatedAt := s.timeOfDayUpdatedAt
	s.timeOfDayMu.RUnlock()
	hour := s.TimeOfDayHours()
	s.mu.Lock()
	meta := s.workerMeta[w.WorkerID]
	needsSync := meta.SyncedTimeAt.Before(updatedAt)
	if needsSync {
		meta.SyncedTimeAt, s.workerMeta[w.WorkerID] = updatedAt, meta
	}
	s.mu.Unlock()
	if needsSync {
		s.pushTimeOfDayToWorker(ctx, w, hour)
	}
}

func (s *Server) markWorkerTimeSyncedAt(workerID string, ts time.Time) {
	s.mu.Lock()
	meta := s.workerMeta[workerID]
	meta.SyncedTimeAt, s.workerMeta[workerID] = ts, meta
	s.mu.Unlock()
}

func (s *Server) pushTimeOfDayToWorker(ctx context.Context, w *WorldWorkerEndpoint, hour float64) {
	rctx, cancel := context.WithTimeout(ctx, 900*time.Millisecond)
	defer cancel()
	urls := []string{fmt.Sprintf("http://%s:8080/admin/time_of_day?time_of_day=%.4f", resolveWorkerMetricsHost(*w), hour)}
	if murl := strings.TrimSpace(firstNonEmpty(os.Getenv("AOC_WORLD_METRICS_URL"), os.Getenv("AOC_WORLD_PROM_URL"))); murl != "" {
		urls = append([]string{fmt.Sprintf("%s/admin/time_of_day?time_of_day=%.4f", strings.TrimSuffix(strings.TrimSuffix(murl, "/metrics"), "/healthz"), hour)}, urls...)
	}
	_, _, _, _ = fetchFirstReachableText(rctx, urls, urls[0])
	if cli := s.workerAdminClient(w); cli != nil {
		_, _ = callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.SpawnNpcResponse, error) {
			return cli.SpawnNpc(c, &worldadminv1.SpawnNpcRequest{Npc: "gm.SetTimeOfDay", YawDegrees: float32(hour), Target: &worldadminv1.SpawnNpcRequest_Place{Place: fmt.Sprintf("%.4f", hour)}})
		})
	}
}

func (s *Server) collectMultiWorkerOnlinePlayers(ctx context.Context, seen map[string]bool) []PlayerInfo {
	results := make([][]*worldadminv1.Player, len(s.worldWorkers))
	var wg sync.WaitGroup
	for i, w := range s.worldWorkers {
		if cli := s.workerAdminClient(w); cli != nil {
			wg.Add(1)
			go func(idx int, c worldadminv1.WorldAdminServiceClient) {
				defer wg.Done()
				rctx, cancel := context.WithTimeout(ctx, 2500*time.Millisecond)
				defer cancel()
				if resp, err := callWithRetry(rctx, s, func(rc context.Context) (*worldadminv1.ListPlayersResponse, error) { return c.ListPlayers(rc, &worldadminv1.ListPlayersRequest{}) }); err == nil && resp != nil {
					results[idx] = resp.GetPlayers()
				}
			}(i, cli)
		}
	}
	wg.Wait()
	var out []PlayerInfo
	for i, list := range results {
		zone := firstNonEmpty(s.worldWorkers[i].ScopeKey, "verra")
		for _, p := range list {
			key := strings.ToLower(p.GetName())
			if key == "" || seen[key] {
				continue
			}
			seen[key] = true
			var x, y, z float64
			if pos := p.GetPosition(); pos != nil {
				x, y, z = pos.GetX(), pos.GetY(), pos.GetZ()
			}
			hp := uint32(100)
			if p.GetDead() {
				hp = 0
			}
			out = append(out, normalizePlayerInfo(PlayerInfo{
				ID: uint64(len(out) + 1), Character: p.GetName(), CharacterID: p.GetCharacterId(), Level: 1,
				X: x, Y: y, Z: z, YawDegrees: p.GetYawDegrees(), Health: hp, MaxHealth: 100, Mounted: p.GetMounted(), Dead: p.GetDead(), Online: true, Zone: zone,
			}))
		}
	}
	return out
}

func computeTrueBorderDistanceCm(st *playerRegionalHandoverState, targetReg, targetZoi string, pt [2]float64) float64 {
	x, y := pt[0], pt[1]
	currReg, currZoi := st.AuthoritativeRegion, st.CurrentZoiID
	minDist, bestSame, bestOther := 250000.0, math.MaxFloat64, math.MaxFloat64
	var sameNode, otherNode admincatalog.LevelableNodeSpec
	for _, n := range admincatalog.DefaultLevelableNodes() {
		d := math.Hypot(x-n.Ox, y-n.Oy)
		if inferRegionFromNodeName(n.Name) == targetReg {
			if d < bestSame {
				bestSame, sameNode = d, n
			}
		} else if d < bestOther {
			bestOther, otherNode = d, n
		}
	}
	if bestSame < math.MaxFloat64 && bestOther < math.MaxFloat64 {
		if ab := math.Hypot(otherNode.Ox-sameNode.Ox, otherNode.Oy-sameNode.Oy); ab > 1.0 {
			minDist = math.Min(minDist, math.Abs(bestOther*bestOther-bestSame*bestSame)/(2.0*ab))
		}
	}
	if currReg == "sandsquall" && targetReg == "sandsquall" && currZoi != targetZoi {
		dSun, dAzm := math.Hypot(x-(-619803.28), y-873514.22), math.Hypot(x-(-278767.94), y-987391.03)
		ab := math.Hypot(-278767.94-(-619803.28), 987391.03-873514.22)
		minDist = math.Min(minDist, math.Min(math.Abs(dAzm*dAzm-dSun*dSun)/(2.0*ab), math.Abs(y-560000.0)))
	}
	for _, borderVal := range []float64{math.Abs(y), math.Abs(y - 560000.0), math.Abs(x - (-600000.0)), math.Abs(y - 650000.0), math.Abs(y - (-650000.0))} {
		if borderVal <= 5000.0 {
			minDist = math.Min(minDist, borderVal)
		}
	}
	return math.Round(minDist*100.0) / 100.0
}

func resolveRegionalGatherableDef(scope string, rawID uint64) (string, string, string, int) {
	catalog := [][2]string{
		{"Granite Deposit", "Mining · Stone"}, {"Copper Vein", "Mining · Metal Ore"}, {"Ruby Geode", "Mining · Gem"},
		{"Weeping Willow", "Lumberjacking · Timber"}, {"Oak Tree", "Lumberjacking · Timber"}, {"Daffodil Patch", "Herbalism · Flora"}, {"Spiritbloom", "Herbalism · Arcane Herb"},
	}
	sc := strings.ToLower(strings.TrimSpace(scope))
	if strings.Contains(sc, "anvil") {
		catalog = [][2]string{
			{"Basalt Outcrop", "Mining · Volcanic Stone"}, {"Mourning Slate", "Mining · Stone"}, {"Tharaxite Vein", "Mining · Metal Ore"}, {"Tendonite Seam", "Mining · Metal Ore"},
			{"Roughrime Crystal", "Mining · Gem"}, {"Eddledorn Tree", "Lumberjacking · Timber"}, {"Furnace Moss", "Herbalism · Cavern Flora"}, {"Salvewort", "Herbalism · Medicinal Herb"},
		}
	} else if strings.Contains(sc, "sand") {
		catalog = [][2]string{
			{"Obsidian Outcrop", "Mining · Volcanic Glass"}, {"Sandstone Ledge", "Mining · Stone"}, {"Acacia Tree", "Lumberjacking · Desert Timber"},
			{"Date Palm", "Lumberjacking · Timber"}, {"Saguaro Cactus", "Herbalism · Desert Flora"}, {"Desert Agave", "Herbalism · Flora"}, {"Sun Poppy", "Herbalism · Flora"},
		}
	} else if strings.Contains(sc, "turq") || strings.Contains(sc, "jund") {
		catalog = [][2]string{
			{"Coveglass Formation", "Mining · Coastal Stone"}, {"Dead Coral Bed", "Mining · Reef Stone"}, {"Volcanic Rock", "Mining · Stone"},
			{"Banyan Tree", "Lumberjacking · Tropical Timber"}, {"Dragon Tree", "Lumberjacking · Rare Timber"}, {"Fungal Anemone", "Herbalism · Reef Flora"}, {"Giant Fern", "Herbalism · Jungle Flora"},
		}
	}
	mix := (rawID ^ (rawID >> 3) ^ (rawID >> 7) ^ (rawID >> 13)) * 11400714819323198485
	pick := catalog[(mix>>32)%uint64(len(catalog))]
	roll := (mix >> 16) % 1000
	switch {
	case roll < 500:
		return pick[0], pick[1], "Common · Tier 1 (Novice)", 10
	case roll < 750:
		return pick[0], pick[1], "Uncommon · Tier 2 (Apprentice)", 20
	case roll < 880:
		return pick[0], pick[1], "Rare · Tier 3 (Journeyman)", 30
	case roll < 950:
		return pick[0], pick[1], "Heroic · Tier 4 (Master)", 40
	case roll < 985:
		return pick[0], pick[1], "Epic · Tier 5 (GrandMaster)", 50
	default:
		return pick[0], pick[1], "Legendary · Tier 5 (GrandMaster)", 50
	}
}