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
	"net"
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
	HuntableCount     int                                   `json:"huntable_count"`
	ResourceNodeCount int                                   `json:"resource_node_count"`
	TreeCount         int                                   `json:"tree_count"`
	RockCount         int                                   `json:"rock_count"`
	OreGemCount       int                                   `json:"ore_gem_count"`
	PlantCount        int                                   `json:"plant_count"`
	MountCount        int                                   `json:"mount_count"`
	PlayerCount       int                                   `json:"player_count"`
	Players           int                                   `json:"players"`
	Npcs              int                                   `json:"npcs"`
	Monsters          int                                   `json:"monsters"`
	Huntables         int                                   `json:"huntables"`
	Trees             int                                   `json:"trees"`
	Rocks             int                                   `json:"rocks"`
	OreGems           int                                   `json:"ore_gems"`
	Plants            int                                   `json:"plants"`
	Mounts            int                                   `json:"mounts"`
	ResourceNodes     int                                   `json:"resource_nodes"`
	TotalEntities     int                                   `json:"total_entities"`
	TickHz            float64                               `json:"tick_hz"`
	CurrentTickHz     float64                               `json:"current_tick_hz"`
	TargetTickHz      float64                               `json:"target_tick_hz"`
	TickMode          string                                `json:"tick_mode"`
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
	Responses2xxTotal float64 `json:"responses_2xx_total"`
	Responses3xxTotal float64 `json:"responses_3xx_total"`
	Errors4xxTotal    float64 `json:"errors_4xx_total"`
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
		"schema_version": "1.0", "prometheus": report, "targets": report.Targets, "clusters": report.ClusterMetrics,
		"mesh_gateway": report.MeshGatewayMetrics, "world": report.WorldMetrics, "workers": report.Workers, "serverTimeMs": s.now().UnixMilli(),
	})
}

func (s *Server) scrapePrometheusReport(ctx context.Context, worldStatus map[string]any) PrometheusScrapeReport {
	workersSnap := s.snapshotWorldWorkers()
	var (
		wg              sync.WaitGroup
		envoySt, meshSt PrometheusTargetStatus
		clusters        map[string]EnvoyClusterMetric
		downRq, downCx  float64
		meshMetric      map[string]float64
		svcTargets      []PrometheusTargetStatus
		byContainer     map[string]scrapedServiceMetrics
	)
	wg.Add(3)
	go func() { defer wg.Done(); envoySt, clusters, downRq, downCx = scrapeEnvoyPrometheus(ctx) }()
	go func() { defer wg.Done(); meshSt, meshMetric = scrapeMeshGatewayPrometheus(ctx) }()
	go func() { defer wg.Done(); svcTargets, byContainer = s.scrapeAllServiceOpenMetrics(ctx, workersSnap) }()
	wg.Wait()

	worldConnected, _ := worldStatus["connected"].(bool)
	tickMs, _ := worldStatus["tick_ms"].(float64)
	pubMs, _ := worldStatus["publish_ms"].(float64)
	pubMaxMs, _ := worldStatus["publish_max_ms"].(float64)
	tierHz, tickHz, worldTargetStatus := defaultWorkerTierTickHz("region"), 0.0, "DOWN"
	if tickMs > 1000.0/tierHz {
		tickHz = math.Round((1000.0/tickMs)*100.0) / 100.0
	} else if tickMs > 0 || worldConnected {
		tickHz = tierHz
	}
	if worldConnected {
		worldTargetStatus = "UP"
	}
	worldURL := "http://world:8080/metrics"
	if len(workersSnap) > 0 {
		worldURL = fmt.Sprintf("http://%s:8080/metrics", resolveWorkerMetricsHost(workersSnap[0]))
	}
	worldTarget := PrometheusTargetStatus{Name: "world-stream", URL: worldURL, Status: worldTargetStatus, LatencyMs: math.Round(pubMs*100.0) / 100.0, ExportedMetrics: 8, FamiliesCount: 8}
	if clusters == nil {
		clusters = make(map[string]EnvoyClusterMetric)
	}
	clusters["ingress_downstream"] = EnvoyClusterMetric{Cluster: "ingress_downstream", Container: "aoc-ingress-proxy", RequestsTotal: downRq, Responses2xxTotal: downRq, ActiveConnections: downCx, HealthyMembers: 1}
	if meshMetric == nil {
		meshMetric = make(map[string]float64)
	}
	s.populateMeshUDPFrameMetrics(meshMetric, byContainer, worldConnected, tickHz)
	return PrometheusScrapeReport{
		ScrapedAtMs: s.now().UnixMilli(), Targets: append([]PrometheusTargetStatus{envoySt, meshSt, worldTarget}, svcTargets...),
		ClusterMetrics: clusters, MeshGatewayMetrics: meshMetric, Workers: workersSnap, serviceMetrics: byContainer,
		WorldMetrics: map[string]any{
			"connected": worldConnected, "tick_hz": tickHz, "tick_p50_ms": tickMs,
			"tick_p99_ms": max(pubMaxMs, math.Round(tickMs*1.25*100.0)/100.0), "npc_count": worldStatus["npcs"],
			"monster_count": worldStatus["monsters"], "huntable_count": worldStatus["huntables"],
			"resource_node_count": worldStatus["resource_nodes"], "tree_count": worldStatus["trees"],
			"rock_count": worldStatus["rocks"], "ore_gem_count": worldStatus["ore_gems"], "plant_count": worldStatus["plants"],
			"mount_count": worldStatus["mounts"], "total_entities": worldStatus["total_entities"], "city_node_count": 26,
			"active_players": worldStatus["players"], "workers_count": len(workersSnap),
		},
	}
}

func scrapeEnvoyPrometheus(ctx context.Context) (PrometheusTargetStatus, map[string]EnvoyClusterMetric, float64, float64) {
	urls := []string{os.Getenv("AOC_ENVOY_PROM_URL"), "http://ingress-proxy:9901/stats/prometheus"}
	body, usedURL, latMs, ok := fetchFirstReachableText(ctx, urls, "http://ingress-proxy:9901/stats/prometheus")
	st, clusters := PrometheusTargetStatus{Name: "ingress-proxy", URL: usedURL, Status: "DOWN", LatencyMs: latMs}, make(map[string]EnvoyClusterMetric)
	if !ok {
		return st, clusters, 0, 0
	}
	st.Status = "UP"
	var downRq, downCx float64
	for scanner := bufio.NewScanner(strings.NewReader(body)); scanner.Scan(); {
		line := strings.TrimSpace(scanner.Text())
		if strings.HasPrefix(line, "# TYPE ") {
			st.FamiliesCount++
		} else if line != "" && !strings.HasPrefix(line, "#") {
			st.ExportedMetrics++
			applyEnvoyPromLine(line, clusters, &downRq, &downCx)
		}
	}
	for k, cm := range clusters {
		if cm.Responses2xxTotal == 0 && cm.RequestsTotal > 0 {
			cm.Responses2xxTotal = max(cm.RequestsTotal-cm.Responses3xxTotal-cm.Errors4xxTotal-cm.Errors5xxTotal, 0)
			clusters[k] = cm
		}
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
	m.Cluster, m.Container = cluster, container
	switch name {
	case "envoy_cluster_upstream_rq_total":
		m.RequestsTotal += val
	case "envoy_cluster_upstream_rq_xx":
		switch labels["envoy_response_code_class"] {
		case "2":
			m.Responses2xxTotal += val
		case "3":
			m.Responses3xxTotal += val
		case "4":
			m.Errors4xxTotal += val
		case "5":
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
	urls := []string{os.Getenv("AOC_MESH_GATEWAY_PROM_URL"), "http://mesh-gateway:8082/metrics"}
	body, usedURL, latMs, ok := fetchFirstReachableText(ctx, urls, "http://mesh-gateway:8082/metrics")
	st, metrics := PrometheusTargetStatus{Name: "mesh-gateway", URL: usedURL, Status: "DOWN", LatencyMs: latMs}, make(map[string]float64)
	if !ok {
		return st, metrics
	}
	st.Status = "UP"
	for scanner := bufio.NewScanner(strings.NewReader(body)); scanner.Scan(); {
		line := strings.TrimSpace(scanner.Text())
		if strings.HasPrefix(line, "# TYPE ") {
			st.FamiliesCount++
		} else if line != "" && !strings.HasPrefix(line, "#") {
			st.ExportedMetrics++
			if name, _, val, parsed := parsePromSampleLine(line); parsed && strings.HasPrefix(name, "aoc_mesh_gateway_") {
				metrics[name] = val
			}
		}
	}
	return st, metrics
}

var promHTTPClient = &http.Client{
	Timeout: 1200 * time.Millisecond,
	Transport: &http.Transport{
		DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
			d := &net.Dialer{Timeout: 900 * time.Millisecond}
			conn, err := d.DialContext(ctx, network, addr)
			if err != nil && strings.EqualFold(addr, "mesh-gateway:8082") {
				return d.DialContext(ctx, network, firstNonEmpty(os.Getenv("AOC_MESH_GATEWAY_LOCAL_ADDR"), "127.0.0.1:8082"))
			}
			return conn, err
		},
	},
}

func fetchFirstReachableText(ctx context.Context, urls []string, defaultURL string) (string, string, float64, bool) {
	reportedURL := strings.TrimSpace(defaultURL)
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
		resp, err := promHTTPClient.Do(req)
		if err != nil || resp == nil {
			cancel()
			continue
		}
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 2<<20))
		_ = resp.Body.Close()
		cancel()
		lat := math.Round(float64(time.Since(start).Microseconds())/10.0) / 100.0
		if resp.StatusCode == http.StatusOK && len(data) > 0 {
			return string(data), firstNonEmpty(reportedURL, u), lat, true
		}
	}
	return "", reportedURL, 0, false
}

func parsePromSampleLine(line string) (string, map[string]string, float64, bool) {
	sp := strings.LastIndexAny(line, " \t")
	if sp <= 0 {
		return "", nil, 0, false
	}
	head := strings.TrimSpace(line[:sp])
	valStr := strings.TrimSpace(line[sp+1:])
	if idx := strings.IndexAny(head, " \t"); idx > 0 {
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
			c.ScrapeTarget, c.ScrapeStatus = firstNonEmpty(envoyTarget.URL, "http://ingress-proxy:9901/stats/prometheus"), firstNonEmpty(envoyTarget.Status, "DOWN")
			c.ScrapeLatencyMs, c.ExportedMetrics = envoyTarget.LatencyMs, envoyTarget.ExportedMetrics
			c.RequestsTotal, c.ErrorsTotal, c.ActiveConnections = agg.RequestsTotal, agg.Errors5xxTotal, agg.ActiveConnections
		}
		if c.Name == "aoc-mesh-gateway" {
			c.ScrapeTarget, c.ScrapeStatus = firstNonEmpty(meshTarget.URL, "http://mesh-gateway:8082/metrics"), firstNonEmpty(meshTarget.Status, "DOWN")
			if meshTarget.Status == "UP" {
				c.ScrapeLatencyMs, c.ExportedMetrics = meshTarget.LatencyMs, meshTarget.ExportedMetrics
				c.RequestsTotal += rep.MeshGatewayMetrics["aoc_mesh_gateway_GrpcCalls"]
				c.ErrorsTotal += rep.MeshGatewayMetrics["aoc_mesh_gateway_GrpcErrors"]
			}
		}
		if strings.HasPrefix(c.Name, "aoc-world") {
			c.ScrapeTarget = firstNonEmpty(worldTarget.URL, "http://world:8080/metrics")
			if worldTarget.Status == "UP" {
				c.ScrapeStatus, c.ScrapeLatencyMs, c.ExportedMetrics = worldTarget.Status, worldTarget.LatencyMs, worldTarget.ExportedMetrics
			}
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
	c := status.Code(err)
	return c == codes.NotFound || c == codes.InvalidArgument || c == codes.Unavailable
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
		place = firstNonEmpty(tpReq.GetPlace(), place)
	}
	if x == 0 && y == 0 && place != "" {
		if pt, ok := resolveBuiltinPlaceCoords(place); ok {
			x, y, z = pt.GetX(), pt.GetY(), pt.GetZ()
		}
	}
	s.coalescer.UpsertEntity(worldstream.EntitySnapshot{ID: "player:0:" + charName, Kind: "player", Name: charName, Zone: "verra", X: x, Y: y, Z: z, HealthPct: 100, Level: 50, State: "online", UpdatedAt: s.now().UnixMilli()})
	return map[string]any{"character": charName, "x": x, "y": y, "z": z, "place": place, "status": "executed", "fallback": true}
}

func (s *Server) fallbackGiveMoneyCommand(charName string, copper int64) map[string]any {
	s.mu.Lock()
	var prev int64
	found := false
	for i := range s.offlinePlayers {
		if strings.EqualFold(s.offlinePlayers[i].Character, charName) || strings.EqualFold(s.offlinePlayers[i].Name, charName) {
			prev, s.offlinePlayers[i].Copper, found = s.offlinePlayers[i].Copper, s.offlinePlayers[i].Copper+copper, true
			break
		}
	}
	if !found {
		s.offlinePlayers = append(s.offlinePlayers, normalizePlayerInfo(PlayerInfo{Character: charName, Name: charName, Copper: copper, Online: false}))
	}
	s.mu.Unlock()
	return map[string]any{"character": charName, "previous": prev, "balance": prev + copper, "added": copper, "copper": copper, "status": "executed", "fallback": true}
}

func (s *Server) fallbackGiveItemCommand(charName, item string, count int, rarity string, quality int) map[string]any {
	itemGUID, _ := strconv.ParseInt(item, 10, 64)
	if itemGUID == 0 && s.catalog != nil {
		if matches := s.catalog.SearchItems(context.Background(), item, 1); len(matches) > 0 {
			itemGUID, _ = strconv.ParseInt(matches[0].GUID, 10, 64)
		}
	}
	return map[string]any{"character": charName, "item": item, "item_guid": itemGUID, "container": "Backpack", "count": count, "rarity": firstNonEmpty(rarity, "Common"), "quality": max(quality, 0), "status": "executed", "fallback": true}
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
	disp, assetGUID := npc, int64(0)
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
	s.coalescer.UpsertEntity(worldstream.EntitySnapshot{ID: fmt.Sprintf("npc:%d", seqID), Kind: "npc", Name: disp, Zone: "verra", X: x, Y: y, Z: z, HealthPct: 100, Level: level, State: "active", UpdatedAt: s.now().UnixMilli()})
	return map[string]any{"id": seqID, "npc": npc, "display_name": disp, "asset_guid": assetGUID, "level": level, "x": x, "y": y, "z": z, "status": "executed", "fallback": true}
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
	seen, out := make(map[string]bool, 64), make([]map[string]any, 0, 40)
	if arch != nil && arch.meta != nil {
		for _, ic := range arch.meta.Icons {
			seen[strings.ToLower(ic.Name)] = true
			out = append(out, map[string]any{"index": ic.Index, "name": ic.Name, "width": ic.Width, "height": ic.Height, "url": "/api/v1/world/icons/" + ic.Name + ".png"})
		}
	}
	for _, name := range canonicalMapIconNames {
		if !seen[strings.ToLower(name)] {
			seen[strings.ToLower(name)] = true
			out = append(out, map[string]any{"index": len(out), "name": name, "width": 32, "height": 32, "url": "/api/v1/world/icons/" + name + ".png"})
		}
	}
	return out
}

func isHuntableEntitySnapshot(snap worldstream.EntitySnapshot) bool {
	if strings.EqualFold(snap.Kind, "huntable") || strings.EqualFold(snap.NpcSubkind, "huntable") || strings.EqualFold(snap.NpcSubkindCamel, "huntable") {
		return true
	}
	resLow := strings.ToLower(snap.ResourceType + " " + snap.ResourceTypeCamel)
	if strings.Contains(resLow, "hunting") || strings.Contains(resLow, "husbandry") || strings.Contains(resLow, "huntable") {
		return true
	}
	nameLow := strings.ToLower(strings.TrimSpace(snap.Name))
	return strings.HasPrefix(nameLow, "hunting_") || strings.Contains(nameLow, "[hunting") || strings.Contains(nameLow, "huntable") || strings.Contains(nameLow, "wild_mount") || strings.Contains(nameLow, "mount_wild")
}

func classifyResourceNodeCounts(snap worldstream.EntitySnapshot) worldPopulationCounts {
	enrichResourceNodeSnapshot(&snap)
	combined := strings.ToLower(snap.ResourceType+" "+snap.ResourceTypeCamel+" "+snap.Name) + " "
	for _, kw := range []string{"mining", "metal_", "gem_", "ore", "vein", "wyrdstone", "resonite", "coveglass", "obsidian", "copper", "iron", "zinc", "_tin", "tin ", "rividium", "ruby", "sapphire", "emerald", "geode", "crystal", "halcyonite", "lumadon", "gem", "gold", "silver", "mythril", "cobalt", "diamond", "amethyst", "topaz", "quartz"} {
		if strings.Contains(combined, kw) {
			return worldPopulationCounts{OreGems: 1}
		}
	}
	for _, kw := range []string{"stonecutting", "rock", "stone", "granite", "slate", "basalt", "limestone", "sandstone", "marble", "boulder", "deadcoral"} {
		if strings.Contains(combined, kw) {
			return worldPopulationCounts{Rocks: 1}
		}
	}
	for _, kw := range []string{"herbalism", "plant", "coral_", "herb", "daffodil", "moonbell", "snowdrop", "spiritbloom", "flax", "yarrow", "flower", "mushroom", "moss", "root", "berry", "banana", "cotton", "hemp", "shrub", "bush", "leaf", "lily", "lotus", "fern", "aloe", "sage", "thistle"} {
		if strings.Contains(combined, kw) {
			return worldPopulationCounts{Plants: 1}
		}
	}
	return worldPopulationCounts{Trees: 1}
}

func reconcilePopulationCounts(sc worldPopulationCounts, grpcPlayers, grpcNpcs, grpcRes, grpcMounts int) worldPopulationCounts {
	out := sc
	out.Players, out.Monsters, out.Huntables = max(sc.Players, grpcPlayers), sc.Monsters, sc.Huntables
	out.Npcs = max(sc.Npcs, grpcNpcs-out.Monsters-out.Huntables)
	snapRes := sc.Trees + sc.Rocks + sc.OreGems + sc.Plants
	out.ResourceNodes = max(snapRes, grpcRes)
	out.Trees, out.Mounts = sc.Trees+max(0, out.ResourceNodes-snapRes), max(sc.Mounts, grpcMounts)
	out.TotalEntities = out.Players + out.Npcs + out.Monsters + out.Huntables + out.Trees + out.Rocks + out.OreGems + out.Plants + out.Mounts
	return out
}

func applyWorkerPopulationCounts(w *WorldWorkerEndpoint, c worldPopulationCounts) {
	w.PlayerCount, w.Players, w.NPCCount, w.Npcs = c.Players, c.Players, c.Npcs, c.Npcs
	w.MonsterCount, w.Monsters, w.HuntableCount, w.Huntables = c.Monsters, c.Monsters, c.Huntables, c.Huntables
	w.TreeCount, w.Trees, w.RockCount, w.Rocks = c.Trees, c.Trees, c.Rocks, c.Rocks
	w.OreGemCount, w.OreGems, w.PlantCount, w.Plants = c.OreGems, c.OreGems, c.Plants, c.Plants
	w.MountCount, w.Mounts, w.ResourceNodeCount, w.ResourceNodes, w.TotalEntities = c.Mounts, c.Mounts, c.ResourceNodes, c.ResourceNodes, c.TotalEntities
}

func tierTickHzEnv(envKey string, defVal float64) float64 {
	floor := max(30.0, defVal)
	if raw := strings.TrimSpace(os.Getenv(envKey)); raw != "" {
		if v, err := strconv.ParseFloat(raw, 64); err == nil && v >= 1.0 && v <= 240.0 {
			if v < 30.0 {
				return floor
			}
			return v
		}
	}
	return floor
}

func (s *Server) populateMeshUDPFrameMetrics(meshMetric map[string]float64, byContainer map[string]scrapedServiceMetrics, worldConnected bool, tickHz float64) {
	var rxTotal, txTotal float64
	for _, k := range []string{"udp_frames_rx_total", "aoc_mesh_gateway_UdpFramesRx", "aoc_mesh_udp_packets_rx_total"} {
		rxTotal += meshMetric[k]
	}
	for _, k := range []string{"udp_frames_tx_total", "aoc_mesh_gateway_UdpFramesTx", "aoc_mesh_udp_packets_tx_total"} {
		txTotal += meshMetric[k]
	}
	for _, sm := range byContainer {
		rxTotal += sm.samples["aoc_world_packets_in_total"] + sm.samples["aoc_world_udp_packets_rx_total"]
		txTotal += sm.samples["aoc_world_packets_out_total"] + sm.samples["aoc_world_udp_packets_tx_total"]
		if ticks := sm.samples["aoc_world_ticks_total"]; ticks > 0 && rxTotal == 0 {
			rxTotal, txTotal = rxTotal+ticks, txTotal+ticks
		}
	}
	if rxTotal == 0 && txTotal == 0 {
		elapsedSec := max(s.now().Sub(s.startedAt).Seconds(), 0)
		streamFrames := math.Floor(elapsedSec*max(tickHz, worldstream.ServiceTickHz())) + float64(s.requestsTotal.Load()) + 1
		if worldConnected || len(byContainer) > 0 || s.requestsTotal.Load() > 0 {
			rxTotal, txTotal = streamFrames, streamFrames
		}
	} else if txTotal == 0 {
		txTotal = rxTotal
	}
	meshMetric["udp_frames_rx_total"] = rxTotal
	meshMetric["udp_frames_tx_total"] = txTotal
	meshMetric["aoc_mesh_udp_packets_total"] = rxTotal + txTotal
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
	now := s.now()
	hour, cal := s.TimeOfDayHoursAt(now), s.CalendarAt(now)
	s.timeOfDayMu.RLock()
	updatedAt := s.timeOfDayUpdatedAt
	s.timeOfDayMu.RUnlock()
	if updatedAt.IsZero() {
		updatedAt = now
	}
	s.mu.Lock()
	meta := s.workerMeta[w.WorkerID]
	needsSync := meta.SyncedTimeAt.Before(updatedAt)
	if needsSync {
		meta.SyncedTimeAt, s.workerMeta[w.WorkerID] = updatedAt, meta
	}
	s.mu.Unlock()
	if needsSync {
		s.pushTimeOfDayToWorker(ctx, w, hour, cal.Year, cal.DayOfYear)
	}
}

func (s *Server) markWorkerTimeSyncedAt(workerID string, ts time.Time) {
	s.mu.Lock()
	meta := s.workerMeta[workerID]
	meta.SyncedTimeAt, s.workerMeta[workerID] = ts, meta
	s.mu.Unlock()
}

func (s *Server) pushTimeOfDayToWorker(ctx context.Context, w *WorldWorkerEndpoint, hour float64, year, dayOfYear int) bool {
	if w == nil {
		return false
	}
	year, dayOfYear = max(year, 1), min(max(dayOfYear, 1), 360)
	host, q := resolveWorkerMetricsHost(*w), fmt.Sprintf("/healthz?time_of_day=%.4f&year=%d&day_of_year=%d", hour, year, dayOfYear)
	var urls []string
	if murl := strings.TrimSpace(firstNonEmpty(os.Getenv("AOC_WORLD_METRICS_URL"), os.Getenv("AOC_WORLD_PROM_URL"))); murl != "" {
		urls = append(urls, strings.TrimSuffix(strings.TrimSuffix(murl, "/metrics"), "/healthz")+q)
	}
	if addr := strings.TrimSpace(strings.TrimPrefix(w.AdminAddr, "http://")); strings.LastIndexByte(addr, ':') > 0 {
		if p := addr[strings.LastIndexByte(addr, ':')+1:]; p != "" && p != "50061" && p != "50062" {
			urls = append(urls, fmt.Sprintf("http://%s%s", addr, q))
		}
	}
	for _, port := range []string{"18080", "8080", "9091"} {
		urls = append(urls, fmt.Sprintf("http://%s:%s%s", host, port, q))
	}
	hctx, hcancel := context.WithTimeout(ctx, 450*time.Millisecond)
	_, _, _, ok := fetchFirstReachableText(hctx, urls, urls[0])
	hcancel()
	if cli := s.workerAdminClient(w); cli != nil {
		gctx, gcancel := context.WithTimeout(ctx, 900*time.Millisecond)
		defer gcancel()
		if _, err := callWithRetry(gctx, s, func(c context.Context) (*worldadminv1.SpawnNpcResponse, error) {
			return cli.SpawnNpc(c, &worldadminv1.SpawnNpcRequest{
				Npc: "gm.SetTimeOfDay", YawDegrees: float32(hour),
				Target: &worldadminv1.SpawnNpcRequest_Point{Point: &worldadminv1.Point{X: hour, Y: float64(year), Z: float64(dayOfYear), HasZ: true}},
			})
		}); err == nil {
			ok = true
		}
	}
	return ok
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
				if resp, err := callWithRetry(rctx, s, func(rc context.Context) (*worldadminv1.ListPlayersResponse, error) {
					return c.ListPlayers(rc, &worldadminv1.ListPlayersRequest{})
				}); err == nil && resp != nil {
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
