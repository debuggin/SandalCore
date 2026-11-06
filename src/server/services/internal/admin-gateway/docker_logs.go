// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

var canonicalContainers = []string{
	"aoc-postgres", "aoc-auth", "aoc-character", "aoc-citynode", "aoc-quest",
	"aoc-mesh", "aoc-gateway", "aoc-mesh-gateway", "aoc-web-admin", "aoc-world-riverlands",
	"aoc-world-sandsquall", "aoc-world-jundark", "aoc-world-anvils", "aoc-world-turquoise-sea",
	"aoc-ingress-proxy",
}

var allowedContainers = map[string]bool{
	"aoc-postgres": true, "aoc-auth": true, "aoc-character": true, "aoc-citynode": true,
	"aoc-quest": true, "aoc-mesh": true, "aoc-gateway": true, "aoc-mesh-gateway": true,
	"aoc-web-admin": true, "aoc-world": true, "aoc-world-riverlands": true, "aoc-world-sandsquall": true,
	"aoc-world-jundark": true, "aoc-world-anvils": true, "aoc-world-turquoise-sea": true, "aoc-ingress-proxy": true,
}

type ContainerMetric struct {
	ID                string  `json:"id"`
	Name              string  `json:"name"`
	Image             string  `json:"image"`
	State             string  `json:"state"`
	Status            string  `json:"status"`
	CPUPct            float64 `json:"cpu_pct"`
	CPUPercent        float64 `json:"cpu_percent"`
	MemMB             float64 `json:"mem_mb"`
	MemoryMB          float64 `json:"memory_mb"`
	MemLimitMB        float64 `json:"mem_limit_mb"`
	MemoryLimitMB     float64 `json:"memory_limit_mb"`
	MemPct            float64 `json:"mem_pct"`
	MemoryPct         float64 `json:"memory_pct"`
	RxBytes           uint64  `json:"rx_bytes"`
	NetRxBytes        uint64  `json:"net_rx_bytes"`
	TxBytes           uint64  `json:"tx_bytes"`
	NetTxBytes        uint64  `json:"net_tx_bytes"`
	Pids              uint64  `json:"pids"`
	ScrapeTarget      string  `json:"scrape_target,omitempty"`
	ScrapeStatus      string  `json:"scrape_status,omitempty"`
	ScrapeLatencyMs   float64 `json:"scrape_latency_ms,omitempty"`
	ExportedMetrics   int     `json:"exported_metrics,omitempty"`
	RequestsTotal     float64 `json:"requests_total,omitempty"`
	ErrorsTotal       float64 `json:"errors_total,omitempty"`
	ActiveConnections float64 `json:"active_connections,omitempty"`
}

type cpuSample struct {
	totalUsage  uint64
	systemUsage uint64
}

var (
	prevCPUMu      sync.Mutex
	prevCPUSamples = make(map[string]cpuSample)
)

type LogEntry struct {
	Container string `json:"container"`
	Stream    string `json:"stream"`
	Timestamp string `json:"timestamp,omitempty"`
	Message   string `json:"message"`
	Raw       string `json:"raw"`
}

func newDockerSocketClient(sockPath string) *http.Client {
	if strings.TrimSpace(sockPath) == "" {
		sockPath = "/var/run/docker.sock"
	}
	dialer := &net.Dialer{Timeout: 1500 * time.Millisecond}
	return &http.Client{
		Timeout: 3 * time.Second,
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return dialer.DialContext(ctx, "unix", sockPath)
			},
		},
	}
}

func (s *Server) getDockerClient() *http.Client {
	if s.dockerClient != nil {
		return s.dockerClient
	}
	return newDockerSocketClient(s.dockerSocketPath)
}

func (s *Server) handleGMContainers(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	const cacheKey = "gm:containers"
	if cached, ok := s.containersCache.get(cacheKey, s.now()); ok {
		s.cacheHits.Add(1)
		writeCachedJSONWithModTime(w, r, "private, max-age=2", s.lastMutatedTime(), cached)
		return
	}
	s.cacheMisses.Add(1)
	containers := s.scrapeContainers(r.Context())
	worldStatus := s.fetchWorldStatus(r.Context())
	promReport := s.scrapePrometheusReport(r.Context(), worldStatus)
	enrichContainersWithPrometheus(containers, promReport)
	payload := map[string]any{
		"schema_version": "1.0",
		"count":          len(containers),
		"containers":     containers,
		"items":          containers,
		"workers":        s.snapshotWorldWorkers(),
		"world_status":   worldStatus,
		"prometheus":     promReport,
		"gateway_telemetry": map[string]any{
			"requests_total":          s.requestsTotal.Load(),
			"errors_total":            s.errorsTotal.Load(),
			"cache_hits_total":        s.cacheHits.Load(),
			"cache_misses_total":      s.cacheMisses.Load(),
			"transient_retries_total": s.transientRetries.Load(),
			"uptime_ms":               s.now().Sub(s.startedAt).Milliseconds(),
		},
		"serverTimeMs": s.now().UnixMilli(),
	}
	s.containersCache.set(cacheKey, payload, 2*time.Second, s.now())
	writeCachedJSONWithModTime(w, r, "private, max-age=2", s.lastMutatedTime(), payload)
}

type dockerContainerSummary struct {
	ID     string   `json:"Id"`
	Names  []string `json:"Names"`
	Image  string   `json:"Image"`
	State  string   `json:"State"`
	Status string   `json:"Status"`
}

func (s *Server) scrapeContainers(ctx context.Context) []ContainerMetric {
	cli := s.getDockerClient()
	rctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	req, _ := http.NewRequestWithContext(rctx, http.MethodGet, "http://localhost/containers/json?all=1", nil)
	resp, err := cli.Do(req)
	byName := make(map[string]ContainerMetric, len(canonicalContainers))
	if err == nil && resp != nil {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 2<<20))
		_ = resp.Body.Close()
		if resp.StatusCode == http.StatusOK {
			var list []dockerContainerSummary
			if json.Unmarshal(body, &list) == nil {
				byName = s.collectContainerStats(ctx, cli, list)
			}
		}
	}
	activeList := s.activeCanonicalContainers()
	out := make([]ContainerMetric, 0, len(activeList))
	for _, cname := range activeList {
		if m, ok := byName[cname]; ok {
			out = append(out, m)
		} else {
			out = append(out, ContainerMetric{Name: cname, State: "unknown", Status: "unavailable"})
		}
	}
	return out
}

func (s *Server) collectContainerStats(ctx context.Context, cli *http.Client, list []dockerContainerSummary) map[string]ContainerMetric {
	var matched []ContainerMetric
	for _, c := range list {
		name := cleanContainerName(c.Names)
		if !allowedContainers[name] {
			continue
		}
		shortID := c.ID
		if len(shortID) > 12 {
			shortID = shortID[:12]
		}
		matched = append(matched, ContainerMetric{
			ID: shortID, Name: name, Image: c.Image, State: c.State, Status: c.Status,
		})
	}
	var wg sync.WaitGroup
	for i := range matched {
		if matched[i].State != "running" || matched[i].ID == "" {
			continue
		}
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			populateOneContainerStats(ctx, cli, &matched[idx])
		}(i)
	}
	wg.Wait()
	out := make(map[string]ContainerMetric, len(matched))
	for _, m := range matched {
		key := m.Name
		if key == "aoc-world" {
			key = "aoc-world-riverlands"
			m.Name = key
		}
		out[key] = m
	}
	return out
}

func cleanContainerName(names []string) string {
	for _, n := range names {
		clean := strings.TrimPrefix(strings.TrimSpace(n), "/")
		if allowedContainers[clean] {
			return clean
		}
	}
	if len(names) > 0 {
		return strings.TrimPrefix(strings.TrimSpace(names[0]), "/")
	}
	return ""
}

type dockerStatsPayload struct {
	CPUStats struct {
		CPUUsage struct {
			TotalUsage uint64 `json:"total_usage"`
		} `json:"cpu_usage"`
		SystemCPUUsage uint64 `json:"system_cpu_usage"`
		OnlineCPUs     uint32 `json:"online_cpus"`
	} `json:"cpu_stats"`
	PreCPUStats struct {
		CPUUsage struct {
			TotalUsage uint64 `json:"total_usage"`
		} `json:"cpu_usage"`
		SystemCPUUsage uint64 `json:"system_cpu_usage"`
	} `json:"precpu_stats"`
	MemoryStats struct {
		Usage uint64            `json:"usage"`
		Limit uint64            `json:"limit"`
		Stats map[string]uint64 `json:"stats"`
	} `json:"memory_stats"`
	PidsStats struct {
		Current uint64 `json:"current"`
	} `json:"pids_stats"`
	Networks map[string]struct {
		RxBytes uint64 `json:"rx_bytes"`
		TxBytes uint64 `json:"tx_bytes"`
	} `json:"networks"`
}

func fetchOneShotStats(ctx context.Context, cli *http.Client, name string) (dockerStatsPayload, bool) {
	sctx, cancel := context.WithTimeout(ctx, 2500*time.Millisecond)
	defer cancel()
	url := fmt.Sprintf("http://localhost/containers/%s/stats?stream=false&one-shot=true", name)
	req, _ := http.NewRequestWithContext(sctx, http.MethodGet, url, nil)
	resp, err := cli.Do(req)
	if err != nil || resp == nil {
		return dockerStatsPayload{}, false
	}
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	_ = resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return dockerStatsPayload{}, false
	}
	var st dockerStatsPayload
	if json.Unmarshal(body, &st) != nil {
		return dockerStatsPayload{}, false
	}
	return st, true
}

func populateOneContainerStats(ctx context.Context, cli *http.Client, dst *ContainerMetric) {
	st, ok := fetchOneShotStats(ctx, cli, dst.Name)
	if !ok {
		return
	}
	dst.CPUPct = computeOneShotCPUPercent(ctx, cli, dst.Name, &st)
	dst.CPUPercent = dst.CPUPct
	cacheBytes := st.MemoryStats.Stats["inactive_file"]
	if cacheBytes == 0 {
		cacheBytes = st.MemoryStats.Stats["cache"]
	}
	used := st.MemoryStats.Usage
	if used > cacheBytes {
		used -= cacheBytes
	}
	dst.MemMB = math.Round((float64(used)/(1024.0*1024.0))*100.0) / 100.0
	dst.MemoryMB = dst.MemMB
	dst.MemLimitMB = math.Round((float64(st.MemoryStats.Limit)/(1024.0*1024.0))*100.0) / 100.0
	dst.MemoryLimitMB = dst.MemLimitMB
	if dst.MemLimitMB > 0 {
		dst.MemPct = math.Round((dst.MemMB/dst.MemLimitMB)*10000.0) / 100.0
		dst.MemoryPct = dst.MemPct
	}
	dst.Pids = st.PidsStats.Current
	for _, netIf := range st.Networks {
		dst.RxBytes += netIf.RxBytes
		dst.TxBytes += netIf.TxBytes
	}
	dst.NetRxBytes = dst.RxBytes
	dst.NetTxBytes = dst.TxBytes
}

func computeOneShotCPUPercent(ctx context.Context, cli *http.Client, name string, st *dockerStatsPayload) float64 {
	preTotal := st.PreCPUStats.CPUUsage.TotalUsage
	preSys := st.PreCPUStats.SystemCPUUsage
	if preSys == 0 {
		prevCPUMu.Lock()
		prev, hasPrev := prevCPUSamples[name]
		prevCPUMu.Unlock()
		if hasPrev && prev.systemUsage > 0 && st.CPUStats.SystemCPUUsage > prev.systemUsage {
			preTotal, preSys = prev.totalUsage, prev.systemUsage
		} else {
			time.Sleep(25 * time.Millisecond)
			if st2, ok := fetchOneShotStats(ctx, cli, name); ok && st2.CPUStats.SystemCPUUsage > st.CPUStats.SystemCPUUsage {
				preTotal, preSys = st.CPUStats.CPUUsage.TotalUsage, st.CPUStats.SystemCPUUsage
				*st = st2
			}
		}
	}
	prevCPUMu.Lock()
	prevCPUSamples[name] = cpuSample{totalUsage: st.CPUStats.CPUUsage.TotalUsage, systemUsage: st.CPUStats.SystemCPUUsage}
	prevCPUMu.Unlock()
	cpuDelta := float64(st.CPUStats.CPUUsage.TotalUsage) - float64(preTotal)
	sysDelta := float64(st.CPUStats.SystemCPUUsage) - float64(preSys)
	cpus := float64(max(st.CPUStats.OnlineCPUs, 1))
	if sysDelta > 0 && cpuDelta > 0 {
		return math.Round((cpuDelta/sysDelta)*cpus*10000.0) / 100.0
	}
	return 0
}

func (s *Server) fetchWorldStatus(ctx context.Context) map[string]any {
	return s.fetchMultiWorkerWorldStatus(ctx)
}

func (s *Server) handleGMLogs(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	q := r.URL.Query()
	container := strings.TrimSpace(q.Get("container"))
	if container == "" || container == "aoc-world" {
		container = "aoc-world-riverlands"
	}
	if !allowedContainers[container] {
		writeProblem(w, r, http.StatusBadRequest, "invalid-container", fmt.Sprintf("Container %q is not in the 15-container allowlist.", container))
		return
	}
	params, ok := parsePageParams(w, r, 200, 1000)
	if !ok {
		return
	}
	tail := params.Limit
	if rawTail := strings.TrimSpace(q.Get("tail")); rawTail != "" {
		parsed, err := strconv.Atoi(rawTail)
		if err != nil || parsed < 1 || parsed > 1000 {
			writeProblem(w, r, http.StatusBadRequest, "invalid-tail", "Query parameter 'tail' must be an integer between 1 and 1000.")
			return
		}
		tail = parsed
	}
	entries := s.fetchContainerLogs(r.Context(), container, tail)
	filtered := filterLogEntries(entries, params.Query, strings.TrimSpace(q.Get("stream")))
	total := len(filtered)
	start := min(max(params.Offset, 0), total)
	end := min(start+params.Limit, total)
	paged := filtered[start:end]
	rawLines := make([]string, len(paged))
	for i, e := range paged {
		rawLines[i] = e.Raw
	}
	env, xPag := buildPaginatedEnvelope(r, paged, "entries", total, params.Limit, params.Offset, params.Fields)
	env["container"] = container
	env["lines"] = rawLines
	w.Header().Set("X-Pagination", xPag)
	writeCachedJSONWithModTime(w, r, "private, no-cache", s.lastMutatedTime(), env)
}

func (s *Server) fetchContainerLogs(ctx context.Context, container string, tail int) []LogEntry {
	cli := s.getDockerClient()
	rctx, cancel := context.WithTimeout(ctx, 2500*time.Millisecond)
	defer cancel()
	url := fmt.Sprintf("http://localhost/containers/%s/logs?stdout=1&stderr=1&tail=%d&timestamps=1", container, tail)
	req, _ := http.NewRequestWithContext(rctx, http.MethodGet, url, nil)
	resp, err := cli.Do(req)
	if err != nil || resp == nil {
		return nil
	}
	raw, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	_ = resp.Body.Close()
	if resp.StatusCode != http.StatusOK || len(raw) == 0 {
		return nil
	}
	return demuxDockerLogStream(container, raw)
}

func demuxDockerLogStream(container string, raw []byte) []LogEntry {
	var out []LogEntry
	pos := 0
	for pos+8 <= len(raw) {
		streamType := raw[pos]
		if (streamType != 1 && streamType != 2) || raw[pos+1] != 0 || raw[pos+2] != 0 || raw[pos+3] != 0 {
			out = nil
			break
		}
		frameLen := int(binary.BigEndian.Uint32(raw[pos+4 : pos+8]))
		pos += 8
		if frameLen < 0 || pos+frameLen > len(raw) {
			out = nil
			break
		}
		streamName := "stdout"
		if streamType == 2 {
			streamName = "stderr"
		}
		chunk := string(raw[pos : pos+frameLen])
		pos += frameLen
		out = append(out, splitChunkIntoLogEntries(container, streamName, chunk)...)
	}
	if len(out) == 0 && len(bytes.TrimSpace(raw)) > 0 {
		out = splitChunkIntoLogEntries(container, "stdout", string(raw))
	}
	return out
}

func splitChunkIntoLogEntries(container, streamName, chunk string) []LogEntry {
	var entries []LogEntry
	for _, line := range strings.Split(strings.ReplaceAll(chunk, "\r\n", "\n"), "\n") {
		trimmed := strings.TrimRight(line, "\r\n")
		if trimmed == "" {
			continue
		}
		ts, msg := "", trimmed
		if sp := strings.IndexByte(trimmed, ' '); sp > 18 && sp < 40 && strings.Contains(trimmed[:sp], "T") {
			ts, msg = trimmed[:sp], strings.TrimSpace(trimmed[sp+1:])
		}
		entries = append(entries, LogEntry{
			Container: container, Stream: streamName, Timestamp: ts, Message: msg, Raw: trimmed,
		})
	}
	return entries
}

func filterLogEntries(entries []LogEntry, query, streamFilter string) []LogEntry {
	qLower := strings.ToLower(strings.TrimSpace(query))
	sLower := strings.ToLower(strings.TrimSpace(streamFilter))
	out := make([]LogEntry, 0, len(entries))
	for _, e := range entries {
		if sLower != "" && strings.ToLower(e.Stream) != sLower {
			continue
		}
		if qLower != "" && !strings.Contains(strings.ToLower(e.Raw), qLower) {
			continue
		}
		out = append(out, e)
	}
	return out
}

func writeGRPCProblem(w http.ResponseWriter, r *http.Request, err error, defaultCode string) {
	st, ok := status.FromError(err)
	if !ok {
		writeProblem(w, r, http.StatusBadGateway, defaultCode, err.Error())
		return
	}
	httpStatus := http.StatusBadGateway
	switch st.Code() {
	case codes.InvalidArgument, codes.OutOfRange:
		httpStatus = http.StatusBadRequest
	case codes.NotFound:
		httpStatus = http.StatusNotFound
	case codes.AlreadyExists, codes.Aborted, codes.FailedPrecondition:
		httpStatus = http.StatusConflict
	case codes.PermissionDenied:
		httpStatus = http.StatusForbidden
	case codes.Unauthenticated:
		httpStatus = http.StatusUnauthorized
	case codes.Unavailable, codes.DeadlineExceeded:
		httpStatus = http.StatusServiceUnavailable
	}
	writeProblem(w, r, httpStatus, defaultCode, st.Message())
}

func extractStringParam(m map[string]any, keys ...string) string {
	for _, k := range keys {
		if v, ok := m[k]; ok && v != nil {
			switch tv := v.(type) {
			case string:
				if s := strings.TrimSpace(tv); s != "" {
					return s
				}
			case json.Number:
				return tv.String()
			case float64:
				return strconv.FormatInt(int64(tv), 10)
			}
		}
	}
	return ""
}

func extractIntParam(m map[string]any, keys ...string) (int, bool) {
	v, ok := extractInt64Param(m, keys...)
	return int(v), ok
}

func extractInt64Param(m map[string]any, keys ...string) (int64, bool) {
	for _, k := range keys {
		v, ok := m[k]
		if !ok || v == nil {
			continue
		}
		switch tv := v.(type) {
		case json.Number:
			if i, err := tv.Int64(); err == nil {
				return i, true
			}
			if f, err := tv.Float64(); err == nil {
				return int64(f), true
			}
		case float64:
			return int64(tv), true
		case int:
			return int64(tv), true
		case int64:
			return tv, true
		case string:
			if i, err := strconv.ParseInt(strings.TrimSpace(tv), 10, 64); err == nil {
				return i, true
			}
		}
	}
	return 0, false
}

func extractFloatParam(m map[string]any, keys ...string) (float64, bool) {
	for _, k := range keys {
		v, ok := m[k]
		if !ok || v == nil {
			continue
		}
		switch tv := v.(type) {
		case json.Number:
			if f, err := tv.Float64(); err == nil {
				return f, true
			}
		case float64:
			return tv, true
		case int:
			return float64(tv), true
		case string:
			if f, err := strconv.ParseFloat(strings.TrimSpace(tv), 64); err == nil {
				return f, true
			}
		}
	}
	return 0, false
}

func (s *Server) HandleSetNodeMayor(w http.ResponseWriter, r *http.Request) {
	s.handleSetNodeMayor(w, r, RoleArchitect)
}

func (s *Server) handleSetNodeMayor(w http.ResponseWriter, r *http.Request, role Role) {
	if !checkJSONContentType(w, r) || !checkAcceptHeader(w, r) {
		return
	}
	var body map[string]any
	if !decodeBoundedJSON(w, r, &body) {
		return
	}
	nodeIdent := firstNonEmpty(r.PathValue("node"), extractStringParam(body, "node_name", "node", "node_guid", "guid", "name"))
	res, err := s.execSetNodeMayorCommand(r.Context(), nodeIdent, body)
	if err != nil {
		writeGRPCProblem(w, r, err, "set-node-mayor-failed")
		return
	}
	s.recordGMAudit(role, "citynode", "set_node_mayor", fmt.Sprint(res["node_name"]), fmt.Sprintf("mayor=%v (%v)", res["mayor_name"], res["mayor_guid"]))
	writeJSON(w, http.StatusOK, res)
}

func (s *Server) HandleTriggerNodeElection(w http.ResponseWriter, r *http.Request) {
	s.handleTriggerNodeElection(w, r, RoleArchitect)
}

func (s *Server) handleTriggerNodeElection(w http.ResponseWriter, r *http.Request, role Role) {
	if !checkJSONContentType(w, r) || !checkAcceptHeader(w, r) {
		return
	}
	var body map[string]any
	if !decodeBoundedJSON(w, r, &body) {
		return
	}
	nodeIdent := firstNonEmpty(r.PathValue("node"), extractStringParam(body, "node_name", "node", "node_guid", "guid", "name"))
	res, err := s.execTriggerNodeElectionCommand(nodeIdent, body)
	if err != nil {
		writeGRPCProblem(w, r, err, "trigger-node-election-failed")
		return
	}
	s.recordGMAudit(role, "citynode", "trigger_node_election", fmt.Sprint(res["node_name"]), fmt.Sprintf("election_status=%v", res["election_status"]))
	writeJSON(w, http.StatusOK, res)
}

func (s *Server) aggregateWorkerStatusResults(
	results []*worldadminv1.GetStatusResponse, monstersByScope map[string]int, monstersCount, poisCount int,
) map[string]any {
	nowMs := s.now().UnixMilli()
	var connected bool
	var tickMs, pubMs, pubMaxMs float64
	var players, npcs, resNodes, mounts, subs uint32
	s.mu.Lock()
	for i, w := range s.worldWorkers {
		w.MonsterCount = monstersByScope[strings.ToLower(w.ScopeKey)]
		if len(s.worldWorkers) == 1 || w.ScopeKind == "world" {
			w.MonsterCount = monstersCount
		}
		st := results[i]
		if st == nil {
			w.Connected, w.Status = false, "OFFLINE"
			continue
		}
		connected = true
		w.Connected, w.Status, w.LastSeenMs = true, "ONLINE", nowMs
		subs += st.GetSubscribers()
		if stats := st.GetStats(); stats != nil {
			tMs, pMs, pMax := float64(stats.GetTickMs()), float64(stats.GetPublishMs()), float64(stats.GetPublishMaxMs())
			tickMs, pubMs, pubMaxMs = max(tickMs, tMs), max(pubMs, pMs), max(pubMaxMs, pMax)
			players += stats.GetPlayers()
			npcs += stats.GetNpcs()
			resNodes += stats.GetResourceNodes()
			mounts += stats.GetMounts()
			w.PlayerCount = int(stats.GetPlayers())
			w.NPCCount = int(stats.GetNpcs())
			w.ResourceNodeCount = int(stats.GetResourceNodes())
			w.TickP50Ms = tMs
			w.TickP99Ms = max(tMs, pMax)
			w.TickHz = 30.0
			if tMs > 0 {
				w.TickHz = math.Min(30.0, math.Round((1000.0/tMs)*100.0)/100.0)
			}
		}
	}
	s.mu.Unlock()
	if subs == 0 {
		subs = uint32(s.coalescer.SubscriberCount())
	}
	return map[string]any{
		"connected": connected, "tick_ms": tickMs, "publish_ms": pubMs, "publish_max_ms": pubMaxMs,
		"players": players, "players_online": players, "npcs": npcs, "npcs_total": npcs,
		"monsters": monstersCount, "resource_nodes": resNodes, "mounts": mounts, "pois": poisCount,
		"subscribers": subs, "workers": s.snapshotWorldWorkers(),
	}
}

func (s *Server) collectMultiWorkerOnlinePlayers(ctx context.Context, seen map[string]bool) []PlayerInfo {
	results := make([][]*worldadminv1.Player, len(s.worldWorkers))
	var wg sync.WaitGroup
	for i, w := range s.worldWorkers {
		cli := s.workerAdminClient(w)
		if cli == nil {
			continue
		}
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
				ID: uint64(len(out) + 1), Character: p.GetName(), CharacterID: p.GetCharacterId(),
				Level: 1, X: x, Y: y, Z: z, YawDegrees: p.GetYawDegrees(),
				Health: hp, MaxHealth: 100, Mounted: p.GetMounted(), Dead: p.GetDead(),
				Online: true, Zone: zone,
			}))
		}
	}
	return out
}


