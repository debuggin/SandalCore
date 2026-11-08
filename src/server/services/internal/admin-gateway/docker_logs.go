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
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	worldstream "aocrop/services/internal/world-stream"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

var canonicalContainers = []string{"aoc-postgres", "aoc-auth", "aoc-character", "aoc-citynode", "aoc-quest", "aoc-mesh", "aoc-gateway", "aoc-mesh-gateway", "aoc-web-admin", "aoc-world-riverlands", "aoc-world-sandsquall", "aoc-world-jundark", "aoc-world-anvils", "aoc-world-turquoise-sea", "aoc-ingress-proxy"}

var allowedContainers = map[string]bool{"aoc-postgres": true, "aoc-auth": true, "aoc-character": true, "aoc-citynode": true, "aoc-quest": true, "aoc-mesh": true, "aoc-gateway": true, "aoc-mesh-gateway": true, "aoc-web-admin": true, "aoc-world": true, "aoc-world-riverlands": true, "aoc-world-sandsquall": true, "aoc-world-jundark": true, "aoc-world-anvils": true, "aoc-world-turquoise-sea": true, "aoc-ingress-proxy": true}

func isAllowedContainer(name string) bool {
	return allowedContainers[name] || (strings.HasPrefix(name, "aoc-") && len(name) > 4 && !strings.ContainsAny(name, " /\\:"))
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

type cpuSample struct{ totalUsage, systemUsage uint64 }

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
	sockPath = firstNonEmpty(strings.TrimSpace(sockPath), "/var/run/docker.sock")
	dialer := &net.Dialer{Timeout: 1500 * time.Millisecond}
	return &http.Client{Timeout: 3 * time.Second, Transport: &http.Transport{DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
		return dialer.DialContext(ctx, "unix", sockPath)
	}}}
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
	containers, worldStatus := s.scrapeContainers(r.Context()), s.fetchWorldStatus(r.Context())
	promReport := s.scrapePrometheusReport(r.Context(), worldStatus)
	enrichContainersWithPrometheus(containers, promReport)
	payload := map[string]any{
		"schema_version": "1.0", "count": len(containers), "containers": containers, "items": containers,
		"workers": s.snapshotWorldWorkerStatuses(), "world_status": worldStatus, "prometheus": promReport,
		"data_modes": s.resolveDataModes(), "service_tick_hz": worldstream.ServiceTickHz(),
		"gateway_telemetry": map[string]any{"requests_total": s.requestsTotal.Load(), "errors_total": s.errorsTotal.Load(), "cache_hits_total": s.cacheHits.Load(), "cache_misses_total": s.cacheMisses.Load(), "transient_retries_total": s.transientRetries.Load(), "uptime_ms": s.now().Sub(s.startedAt).Milliseconds()},
		"serverTimeMs":      s.now().UnixMilli(),
	}
	s.containersCache.set(cacheKey, payload, 2*time.Second, s.now())
	writeCachedJSONWithModTime(w, r, "private, max-age=2", s.lastMutatedTime(), payload)
}

type dockerContainerSummary struct {
	ID, Image, State, Status string
	Names                    []string
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
		var list []dockerContainerSummary
		if resp.StatusCode == http.StatusOK && json.Unmarshal(body, &list) == nil {
			byName = s.collectContainerStats(ctx, cli, list)
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
		if name := cleanContainerName(c.Names); isAllowedContainer(name) {
			matched = append(matched, ContainerMetric{ID: c.ID[:min(len(c.ID), 12)], Name: name, Image: c.Image, State: c.State, Status: c.Status})
		}
	}
	var wg sync.WaitGroup
	for i := range matched {
		if matched[i].State == "running" && matched[i].ID != "" {
			wg.Add(1)
			go func(idx int) { defer wg.Done(); populateOneContainerStats(ctx, cli, &matched[idx]) }(i)
		}
	}
	wg.Wait()
	out := make(map[string]ContainerMetric, len(matched)+2)
	for _, m := range matched {
		out[m.Name] = m
		if m.Name == "aoc-world" || m.Name == "aoc-world-riverlands" {
			alias := m
			alias.Name = map[string]string{"aoc-world": "aoc-world-riverlands", "aoc-world-riverlands": "aoc-world"}[m.Name]
			out[alias.Name] = alias
		}
	}
	return out
}

func cleanContainerName(names []string) string {
	for _, n := range names {
		if clean := strings.TrimPrefix(strings.TrimSpace(n), "/"); isAllowedContainer(clean) {
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
		Usage, Limit uint64
		Stats        map[string]uint64 `json:"stats"`
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
	req, _ := http.NewRequestWithContext(sctx, http.MethodGet, fmt.Sprintf("http://localhost/containers/%s/stats?stream=false&one-shot=true", name), nil)
	resp, err := cli.Do(req)
	if err != nil || resp == nil {
		return dockerStatsPayload{}, false
	}
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	_ = resp.Body.Close()
	var st dockerStatsPayload
	return st, resp.StatusCode == http.StatusOK && json.Unmarshal(body, &st) == nil
}

func populateOneContainerStats(ctx context.Context, cli *http.Client, dst *ContainerMetric) {
	st, ok := fetchOneShotStats(ctx, cli, dst.Name)
	if !ok {
		return
	}
	dst.CPUPct = computeOneShotCPUPercent(ctx, cli, dst.Name, &st)
	cacheBytes := max(st.MemoryStats.Stats["inactive_file"], st.MemoryStats.Stats["cache"])
	used := st.MemoryStats.Usage - min(st.MemoryStats.Usage, cacheBytes)
	dst.MemMB, dst.MemLimitMB = math.Round((float64(used)/(1024.0*1024.0))*100.0)/100.0, math.Round((float64(st.MemoryStats.Limit)/(1024.0*1024.0))*100.0)/100.0
	if dst.MemLimitMB > 0 {
		dst.MemPct = math.Round((dst.MemMB/dst.MemLimitMB)*10000.0) / 100.0
	}
	dst.CPUPercent, dst.MemoryMB, dst.MemoryLimitMB, dst.MemoryPct, dst.Pids = dst.CPUPct, dst.MemMB, dst.MemLimitMB, dst.MemPct, st.PidsStats.Current
	for _, netIf := range st.Networks {
		dst.RxBytes, dst.TxBytes = dst.RxBytes+netIf.RxBytes, dst.TxBytes+netIf.TxBytes
	}
	dst.NetRxBytes, dst.NetTxBytes = dst.RxBytes, dst.TxBytes
}

func computeOneShotCPUPercent(ctx context.Context, cli *http.Client, name string, st *dockerStatsPayload) float64 {
	preTotal, preSys := st.PreCPUStats.CPUUsage.TotalUsage, st.PreCPUStats.SystemCPUUsage
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
	cpuDelta, sysDelta := float64(st.CPUStats.CPUUsage.TotalUsage)-float64(preTotal), float64(st.CPUStats.SystemCPUUsage)-float64(preSys)
	if sysDelta > 0 && cpuDelta > 0 {
		return math.Round((cpuDelta/sysDelta)*float64(max(st.CPUStats.OnlineCPUs, 1))*10000.0) / 100.0
	}
	return 0
}

func (s *Server) fetchWorldStatus(ctx context.Context) map[string]any {
	return s.fetchMultiWorkerWorldStatus(ctx)
}

func (s *Server) handleWorldStatus(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	now, status := s.now(), s.fetchWorldStatus(r.Context())
	payload := make(map[string]any, len(status)+6)
	for k, v := range status {
		payload[k] = v
	}
	payload["schema_version"], payload["world_status"], payload["serverTimeMs"] = "1.0", status, now.UnixMilli()
	s.enrichPayloadWithTimeAndCalendar(payload, now)
	writeCachedJSONWithModTime(w, r, "private, max-age=2", s.lastMutatedTime(), payload)
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
	if !isAllowedContainer(container) {
		writeProblem(w, r, http.StatusBadRequest, "invalid-container", fmt.Sprintf("Container %q is not in the aoc-* container allowlist.", container))
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
	allFiltered := filterLogEntries(s.fetchContainerLogs(r.Context(), container, tail), params.Query, strings.TrimSpace(q.Get("stream")))
	total, window := len(allFiltered), allFiltered
	if tail > 0 && len(window) > tail {
		window = window[len(window)-tail:]
	}
	start := min(max(params.Offset, 0), len(window))
	paged := window[start:min(start+params.Limit, len(window))]
	rawLines := make([]string, len(paged))
	for i, e := range paged {
		rawLines[i] = e.Raw
	}
	env, xPag := buildPaginatedEnvelope(r, paged, "entries", total, params.Limit, params.Offset, params.Fields)
	env["container"], env["lines"] = container, rawLines
	w.Header().Set("X-Pagination", xPag)
	writeCachedJSONWithModTime(w, r, "private, no-cache", s.lastMutatedTime(), env)
}

var adminSecretPattern = regexp.MustCompile(`(?i)(AOC_ADMIN_PASSPHRASE\s*=\s*|AOC_ADMIN_JWT_SECRET\s*=\s*|Passphrase:\s*)[^\s"']+`)

func redactAdminSecrets(line string, passphrase string) string {
	out := line
	for _, secret := range []string{strings.TrimSpace(passphrase), strings.TrimSpace(os.Getenv("AOC_ADMIN_PASSPHRASE")), strings.TrimSpace(os.Getenv("AOC_ADMIN_JWT_SECRET"))} {
		if secret != "" {
			out = strings.ReplaceAll(out, secret, "[REDACTED]")
		}
	}
	return adminSecretPattern.ReplaceAllString(out, "${1}[REDACTED]")
}

var containerLogAliasTable = map[string][]string{
	"aoc-world": {"aoc-world", "aoc-world-riverlands", "aoc-riverlands"}, "aoc-world-riverlands": {"aoc-world-riverlands", "aoc-world", "aoc-riverlands"},
	"aoc-riverlands": {"aoc-riverlands", "aoc-world", "aoc-world-riverlands"}, "aoc-jundark": {"aoc-jundark", "aoc-world-jundark"},
	"aoc-world-jundark": {"aoc-world-jundark", "aoc-jundark"}, "aoc-anvils": {"aoc-anvils", "aoc-world-anvils"},
	"aoc-world-anvils": {"aoc-world-anvils", "aoc-anvils"}, "aoc-turquoise-sea": {"aoc-turquoise-sea", "aoc-world-turquoise-sea"},
	"aoc-world-turquoise-sea":       {"aoc-world-turquoise-sea", "aoc-turquoise-sea"},
	"aoc-verra-san-node-sunhaven":   {"aoc-verra-san-node-sunhaven", "aoc-world-sandsquall", "aoc-world-sandsquall-sunhaven", "aoc-sandsquall"},
	"aoc-world-sandsquall":          {"aoc-world-sandsquall", "aoc-verra-san-node-sunhaven", "aoc-world-sandsquall-sunhaven", "aoc-sandsquall"},
	"aoc-world-sandsquall-sunhaven": {"aoc-world-sandsquall-sunhaven", "aoc-verra-san-node-sunhaven", "aoc-world-sandsquall", "aoc-sandsquall"},
	"aoc-sandsquall":                {"aoc-sandsquall", "aoc-verra-san-node-sunhaven", "aoc-world-sandsquall", "aoc-world-sandsquall-sunhaven"},
	"aoc-verra-san-node-azmaran":    {"aoc-verra-san-node-azmaran", "aoc-world-sandsquall-azmaran"}, "aoc-world-sandsquall-azmaran": {"aoc-world-sandsquall-azmaran", "aoc-verra-san-node-azmaran"},
}

func containerLogAliases(container string) []string {
	if al, ok := containerLogAliasTable[container]; ok {
		return al
	}
	return []string{container}
}

func (s *Server) fetchContainerLogs(ctx context.Context, container string, _ int) []LogEntry {
	var entries []LogEntry
	cli := s.getDockerClient()
	for _, cand := range containerLogAliases(container) {
		rctx, cancel := context.WithTimeout(ctx, 2500*time.Millisecond)
		req, _ := http.NewRequestWithContext(rctx, http.MethodGet, fmt.Sprintf("http://localhost/containers/%s/logs?stdout=1&stderr=1&tail=all&timestamps=1", cand), nil)
		resp, err := cli.Do(req)
		if err != nil || resp == nil {
			cancel()
			continue
		}
		raw, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
		_ = resp.Body.Close()
		cancel()
		if resp.StatusCode != http.StatusOK || len(raw) == 0 {
			continue
		}
		entries = demuxDockerLogStream(container, raw)
		for i := range entries {
			entries[i].Message, entries[i].Raw = redactAdminSecrets(entries[i].Message, s.authCfg.Passphrase), redactAdminSecrets(entries[i].Raw, s.authCfg.Passphrase)
		}
		if len(entries) > 0 {
			break
		}
	}
	return s.appendAuditLogsForContainer(container, entries)
}

func (s *Server) appendAuditLogsForContainer(container string, entries []LogEntry) []LogEntry {
	s.mu.Lock()
	audits := append([]AuditEntry(nil), s.auditLog...)
	s.mu.Unlock()
	aliases := containerLogAliases(container)
	for _, a := range audits {
		tc, match := firstNonEmpty(a.TargetContainer, "aoc-world"), container == "aoc-mesh-gateway"
		for _, al := range aliases {
			if strings.EqualFold(al, tc) {
				match = true
				break
			}
		}
		if !match {
			continue
		}
		who, what, where, when := firstNonEmpty(a.Who, fmt.Sprintf("%s (%s)", a.ActorSub, a.ActorRole)), firstNonEmpty(a.What, a.DiffSummary), firstNonEmpty(a.Where, a.CommandDomain), firstNonEmpty(a.When, a.AppliedAt)
		msg := redactAdminSecrets(fmt.Sprintf("[gm-audit] who=%q what=%q where=%q when=%q", who, what, where, when), s.authCfg.Passphrase)
		entries = append(entries, LogEntry{Container: container, Stream: "stdout", Timestamp: when, Message: msg, Raw: fmt.Sprintf("%s %s", when, msg)})
	}
	return entries
}

func demuxDockerLogStream(container string, raw []byte) []LogEntry {
	var out []LogEntry
	for pos := 0; pos+8 <= len(raw); {
		streamType, frameLen := raw[pos], int(binary.BigEndian.Uint32(raw[pos+4:pos+8]))
		if (streamType != 1 && streamType != 2) || raw[pos+1] != 0 || raw[pos+2] != 0 || raw[pos+3] != 0 || frameLen < 0 || pos+8+frameLen > len(raw) {
			if pos == 0 {
				out = nil
			}
			break
		}
		streamName := map[byte]string{1: "stdout", 2: "stderr"}[streamType]
		out = append(out, splitChunkIntoLogEntries(container, streamName, string(raw[pos+8:pos+8+frameLen]))...)
		pos += 8 + frameLen
	}
	if len(out) == 0 && len(bytes.TrimSpace(raw)) > 0 {
		out = splitChunkIntoLogEntries(container, "stdout", string(raw))
	}
	return out
}

func splitChunkIntoLogEntries(container, streamName, chunk string) []LogEntry {
	var entries []LogEntry
	for _, line := range strings.Split(strings.ReplaceAll(chunk, "\r\n", "\n"), "\n") {
		if trimmed := strings.TrimRight(line, "\r\n"); trimmed != "" {
			ts, msg := "", trimmed
			if sp := strings.IndexByte(trimmed, ' '); sp > 18 && sp < 40 && strings.Contains(trimmed[:sp], "T") {
				ts, msg = trimmed[:sp], strings.TrimSpace(trimmed[sp+1:])
			}
			entries = append(entries, LogEntry{Container: container, Stream: streamName, Timestamp: ts, Message: msg, Raw: trimmed})
		}
	}
	return entries
}

func filterLogEntries(entries []LogEntry, query, streamFilter string) []LogEntry {
	qLower, sLower, out := strings.ToLower(strings.TrimSpace(query)), strings.ToLower(strings.TrimSpace(streamFilter)), make([]LogEntry, 0, len(entries))
	for _, e := range entries {
		if (sLower == "" || strings.ToLower(e.Stream) == sLower) && (qLower == "" || strings.Contains(strings.ToLower(e.Raw), qLower)) {
			out = append(out, e)
		}
	}
	return out
}

var grpcCodeToHTTPStatus = map[codes.Code]int{
	codes.InvalidArgument: http.StatusBadRequest, codes.OutOfRange: http.StatusBadRequest,
	codes.NotFound: http.StatusNotFound, codes.AlreadyExists: http.StatusConflict,
	codes.Aborted: http.StatusConflict, codes.FailedPrecondition: http.StatusConflict,
	codes.PermissionDenied: http.StatusForbidden, codes.Unauthenticated: http.StatusUnauthorized,
	codes.Unavailable: http.StatusServiceUnavailable, codes.DeadlineExceeded: http.StatusServiceUnavailable,
}

func writeGRPCProblem(w http.ResponseWriter, r *http.Request, err error, defaultCode string) {
	st, ok := status.FromError(err)
	if !ok {
		writeProblem(w, r, http.StatusBadGateway, defaultCode, err.Error())
		return
	}
	if hs, mapped := grpcCodeToHTTPStatus[st.Code()]; mapped {
		writeProblem(w, r, hs, defaultCode, st.Message())
		return
	}
	writeProblem(w, r, http.StatusBadGateway, defaultCode, st.Message())
}

func extractStringParam(m map[string]any, keys ...string) string {
	for _, k := range keys {
		switch tv := m[k].(type) {
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
	return ""
}

func extractIntParam(m map[string]any, keys ...string) (int, bool) {
	v, ok := extractInt64Param(m, keys...)
	return int(v), ok
}

func extractInt64Param(m map[string]any, keys ...string) (int64, bool) {
	for _, k := range keys {
		switch tv := m[k].(type) {
		case json.Number:
			if i, err := tv.Int64(); err == nil {
				return i, true
			}
		case int64:
			return tv, true
		case string:
			if i, err := strconv.ParseInt(strings.TrimSpace(tv), 10, 64); err == nil {
				return i, true
			}
		}
		if f, ok := extractFloatParam(m, k); ok {
			return int64(f), true
		}
	}
	return 0, false
}

func extractFloatParam(m map[string]any, keys ...string) (float64, bool) {
	for _, k := range keys {
		switch tv := m[k].(type) {
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
	var body map[string]any
	if !checkJSONContentType(w, r) || !checkAcceptHeader(w, r) || !decodeBoundedJSON(w, r, &body) {
		return
	}
	nodeIdent := firstNonEmpty(r.PathValue("node"), extractStringParam(body, "node_name", "node", "node_guid", "guid", "name"))
	res, err := s.execSetNodeMayorCommand(r.Context(), nodeIdent, body)
	if err != nil {
		writeGRPCProblem(w, r, err, "set-node-mayor-failed")
		return
	}
	s.recordGMAuditWithContext(r, role, CommandRequest{Command: "set_node_mayor", TargetID: fmt.Sprint(res["node_name"]), Params: body}, res)
	writeJSON(w, http.StatusOK, res)
}

func (s *Server) HandleTriggerNodeElection(w http.ResponseWriter, r *http.Request) {
	s.handleTriggerNodeElection(w, r, RoleArchitect)
}

func (s *Server) handleTriggerNodeElection(w http.ResponseWriter, r *http.Request, role Role) {
	var body map[string]any
	if !checkJSONContentType(w, r) || !checkAcceptHeader(w, r) || !decodeBoundedJSON(w, r, &body) {
		return
	}
	nodeIdent := firstNonEmpty(r.PathValue("node"), extractStringParam(body, "node_name", "node", "node_guid", "guid", "name"))
	res, err := s.execTriggerNodeElectionCommand(nodeIdent, body)
	if err != nil {
		writeGRPCProblem(w, r, err, "trigger-node-election-failed")
		return
	}
	s.recordGMAuditWithContext(r, role, CommandRequest{Command: "trigger_node_election", TargetID: fmt.Sprint(res["node_name"]), Params: body}, res)
	writeJSON(w, http.StatusOK, res)
}

func (s *Server) resolveActorSub(r *http.Request) string {
	if r != nil {
		if tok, ok := extractAdminToken(r); ok {
			if claims, err := s.verifyAdminJWT(tok); err == nil {
				return firstNonEmpty(claims.Username, claims.Subject, s.authCfg.Username, "admin")
			}
		}
	}
	return firstNonEmpty(s.authCfg.Username, "admin")
}

func (s *Server) recordGMAuditWithContext(r *http.Request, role Role, req CommandRequest, res map[string]any) AuditEntry {
	whatProse, whereLabel, targetContainer := formatGMCommandProseSummary(req, res)
	if res != nil {
		res["message"] = whatProse
	}
	seq, actorSub, appliedAt := s.auditSeq.Add(1), s.resolveActorSub(r), s.now().UTC().Format(time.RFC3339)
	who := fmt.Sprintf("%s (%s)", actorSub, role)
	entry := AuditEntry{AuditID: fmt.Sprintf("aud-%d", seq), ActorSub: actorSub, ActorRole: role, Who: who, What: whatProse, Where: whereLabel, When: appliedAt, TargetContainer: targetContainer, CommandDomain: whereLabel, ActionName: req.Command, TargetEntityID: req.TargetID, DiffSummary: whatProse, SnapshotEpoch: s.snapshotEpoch.Load(), AppliedAt: appliedAt}
	s.mu.Lock()
	s.auditLog = append(s.auditLog, entry)
	s.mu.Unlock()
	fmt.Printf("{\"time\":%q,\"level\":\"INFO\",\"component\":\"gm-audit\",\"who\":%q,\"what\":%q,\"where\":%q,\"when\":%q,\"target_container\":%q,\"msg\":%q}\n", appliedAt, who, whatProse, whereLabel, appliedAt, targetContainer, whatProse+" ["+whereLabel+"]")
	s.recordMutation()
	return entry
}

func formatGMActionWhatProse(req CommandRequest, res map[string]any) string {
	x, _ := extractFloatParam(res, "x", "x_cm")
	y, _ := extractFloatParam(res, "y", "y_cm")
	z, _ := extractFloatParam(res, "z", "z_cm")
	p, charName := req.Params, firstNonEmpty(extractStringParam(res, "character", "player"), extractStringParam(req.Params, "character", "player"), req.TargetID, "player")
	charGuid, charLabel := firstNonEmpty(extractStringParam(res, "player_guid", "guid"), extractStringParam(p, "player_guid", "guid", "targetId", "target_id"), charName), charName
	if charGuid != "" && charGuid != charName {
		charLabel = charName + " (" + charGuid + ")"
	}
	targetID, npcName := firstNonEmpty(extractStringParam(res, "entity_guid", "target_id"), req.TargetID, "npc"), firstNonEmpty(extractStringParam(res, "display_name", "name", "npc"), extractStringParam(p, "display_name", "name", "npc"), firstNonEmpty(extractStringParam(res, "entity_guid", "target_id"), req.TargetID, "npc"))
	switch req.Command {
	case "teleport_player", "teleport":
		if tp := firstNonEmpty(extractStringParam(p, "target_player", "to_player", "target_player_guid"), extractStringParam(res, "target_player", "to_player")); tp != "" {
			return fmt.Sprintf("%s teleported to %s at (%.0f, %.0f, %.0f)", charLabel, tp, x, y, z)
		} else if pl := firstNonEmpty(extractStringParam(res, "place"), extractStringParam(p, "place", "destination")); pl != "" {
			return fmt.Sprintf("%s teleported to %s (%.0f, %.0f, %.0f)", charLabel, pl, x, y, z)
		}
		return fmt.Sprintf("%s teleported to (%.0f, %.0f, %.0f)", charLabel, x, y, z)
	case "step_player_border", "simulate_border_crossing":
		fromW, toW := firstNonEmpty(extractStringParam(res, "from_worker", "authoritative_worker", "worker_id"), "aoc-world"), firstNonEmpty(extractStringParam(res, "to_worker", "authoritative_worker", "worker_id"), "aoc-world")
		if fromW == toW {
			return fmt.Sprintf("%s stepped in %s at (%.0f, %.0f, %.0f)", charLabel, toW, x, y, z)
		}
		return fmt.Sprintf("%s crossed border %s -> %s at (%.0f, %.0f, %.0f)", charLabel, fromW, toW, x, y, z)
	case "nudge_npc", "move_npc":
		return fmt.Sprintf("Nudged %s (%s) %s to (%.0f, %.0f, %.0f)", npcName, targetID, firstNonEmpty(extractStringParam(p, "axis_delta", "delta", "direction"), fmt.Sprintf("(%.0f, %.0f, %.0f)", x, y, z)), x, y, z)
	case "heal_npc":
		return fmt.Sprintf("Healed %s (%s) to full health", npcName, targetID)
	case "kill_npc", "kill", "reset_npc", "respawn_npc", "despawn_npc", "despawn", "despawn_entity":
		verb := map[string]string{"kill_npc": "Killed", "kill": "Killed", "reset_npc": "Reset", "respawn_npc": "Respawned"}[req.Command]
		return fmt.Sprintf("%s %s (%s)", firstNonEmpty(verb, "Despawned"), npcName, targetID)
	case "spawn_npc", "spawn":
		cnt, _ := extractIntParam(p, "count", "quantity")
		lv, _ := extractIntParam(res, "level")
		return fmt.Sprintf("Spawned %dx %s (%s, Lv %d) at (%.0f, %.0f, %.0f)", max(cnt, 1), npcName, firstNonEmpty(extractStringParam(res, "npc"), extractStringParam(p, "npc"), npcName), max(lv, 1), x, y, z)
	default:
		return formatGMProgressionWhatProse(req, res, charName, charGuid)
	}
}

func formatGMProgressionWhatProse(req CommandRequest, res map[string]any, charName, charGuid string) string {
	p, node := req.Params, firstNonEmpty(extractStringParam(res, "display_name", "node_name", "name", "node"), extractStringParam(req.Params, "node_name", "node", "name"), req.TargetID, "node")
	switch req.Command {
	case "grant_item", "give_item", "item":
		cnt, _ := extractIntParam(res, "count", "quantity")
		item := firstNonEmpty(extractStringParam(res, "item", "item_guid"), extractStringParam(p, "item", "item_name", "item_guid", "guid", "name"), "item")
		return fmt.Sprintf("Granted %dx %s (%s) to %s", max(cnt, 1), item, firstNonEmpty(extractStringParam(res, "rarity"), extractRarityString(p), "Common"), charName)
	case "give_money", "give_gold", "give_currency", "currency":
		tot, _ := extractInt64Param(res, "added", "copper")
		return fmt.Sprintf("Granted %dg %ds %dc to %s", tot/10000, (tot%10000)/100, tot%100, charName)
	case "set_node_level", "set_node_stage":
		lv, _ := extractIntParam(res, "level", "stage")
		return fmt.Sprintf("Set %s stage to Stage %d", node, lv)
	case "set_node_mayor":
		return fmt.Sprintf("Assigned %s as Mayor of %s", firstNonEmpty(extractStringParam(res, "mayor_name", "mayor_guid"), extractStringParam(p, "mayor_name", "mayor_guid"), "none"), node)
	case "trigger_node_election", "set_node_election":
		return fmt.Sprintf("Triggered %s election on %s", firstNonEmpty(extractStringParam(res, "election_status"), "registration"), node)
	case "add_node_xp", "advance_node_clock", "set_weapon_xp":
		val, _ := extractInt64Param(res, "xp_amount", "xp", "seconds", "delta_seconds")
		if req.Command == "add_node_xp" {
			return fmt.Sprintf("Added %d XP to %s", val, node)
		} else if req.Command == "advance_node_clock" {
			return fmt.Sprintf("Advanced clock on %s by %ds", node, val)
		}
		return fmt.Sprintf("Set %s XP of %s to %d", firstNonEmpty(extractStringParam(res, "weapon"), "sword"), charGuid, val)
	case "set_move_speed":
		mult, _ := extractFloatParam(res, "multiplier", "speed")
		return fmt.Sprintf("Set movement speed of %s to %.1fx", charGuid, mult)
	case "reset_tutorials":
		return fmt.Sprintf("Reset tutorials for %s", charGuid)
	case "set_tick_rate", "set_worker_tick_rate":
		hz, _ := extractFloatParam(res, "tick_hz", "hz")
		return fmt.Sprintf("Set tick rate of %s to %.0f Hz", firstNonEmpty(extractStringParam(res, "worker_id"), req.TargetID, "aoc-world"), hz)
	case "set_time_of_day", "time_of_day", "set_world_time":
		return firstNonEmpty(extractStringParam(res, "message"), fmt.Sprintf("Set server time of day to %s", firstNonEmpty(extractStringParam(res, "time_of_day_label"), "12:00")))
	default:
		return fmt.Sprintf("Executed %s on %s", req.Command, firstNonEmpty(req.TargetID, "world"))
	}
}

type worldPopulationCounts struct {
	Players, Npcs, Monsters, Huntables, Trees, Rocks, OreGems, Plants, Mounts, ResourceNodes, TotalEntities int
}

func (c *worldPopulationCounts) add(d worldPopulationCounts) {
	c.Players, c.Npcs, c.Monsters, c.Huntables, c.Mounts = c.Players+d.Players, c.Npcs+d.Npcs, c.Monsters+d.Monsters, c.Huntables+d.Huntables, c.Mounts+d.Mounts
	c.Trees, c.Rocks, c.OreGems, c.Plants = c.Trees+d.Trees, c.Rocks+d.Rocks, c.OreGems+d.OreGems, c.Plants+d.Plants
}

func canonicalScopeKey(raw string) string {
	k := strings.ReplaceAll(strings.ToLower(strings.TrimSpace(raw)), "_", "-")
	if r, ok := map[string]string{"verra-san-node-sunhaven": "sandsquall-sunhaven", "verra-san-sunhaven": "sandsquall-sunhaven", "verra-san-node-azmaran": "sandsquall-azmaran", "verra-san-azmaran": "sandsquall-azmaran"}[k]; ok {
		return r
	}
	return k
}

func (s *Server) collectSnapshotPopulationCounts() (worldPopulationCounts, map[string]worldPopulationCounts) {
	var total worldPopulationCounts
	byScope := make(map[string]worldPopulationCounts, 8)
	for _, snap := range s.coalescer.Snapshot(worldstream.ViewportFilter{}) {
		if d, ok := classifySnapshotPopulation(snap); ok {
			k, sc := canonicalScopeKey(snap.Zone), byScope[canonicalScopeKey(snap.Zone)]
			total.add(d)
			sc.add(d)
			byScope[k] = sc
		}
	}
	return total, byScope
}

func classifySnapshotPopulation(snap worldstream.EntitySnapshot) (worldPopulationCounts, bool) {
	switch strings.ToLower(strings.TrimSpace(snap.Kind)) {
	case "loot_pile", "loot", "corpse":
		return worldPopulationCounts{}, false
	case "player":
		return worldPopulationCounts{Players: 1}, true
	case "mount":
		return worldPopulationCounts{Mounts: 1}, true
	case "resource_node", "gatherable", "resource":
		if isHuntableEntitySnapshot(snap) {
			return worldPopulationCounts{Huntables: 1}, true
		}
		return classifyResourceNodeCounts(snap), true
	}
	if enrichNpcSnapshot(&snap); isHuntableEntitySnapshot(snap) {
		return worldPopulationCounts{Huntables: 1}, true
	} else if isMonsterSnapshot(snap) {
		return worldPopulationCounts{Monsters: 1}, true
	} else if snap.ResourceType != "" || snap.ResourceTypeCamel != "" {
		return classifyResourceNodeCounts(snap), true
	}
	return worldPopulationCounts{Npcs: 1}, true
}

func (s *Server) aggregateWorkerStatusResults(results []*worldadminv1.GetStatusResponse, totalSnap worldPopulationCounts, byScope map[string]worldPopulationCounts, poisCount int) map[string]any {
	nowMs, connected, tickMs, pubMs, pubMaxMs, subs := s.now().UnixMilli(), false, 0.0, 0.0, 0.0, uint32(0)
	var gPlayers, gNpcs, gRes, gMounts int
	s.mu.Lock()
	for i, w := range s.worldWorkers {
		wSnap := byScope[canonicalScopeKey(w.ScopeKey)]
		if len(s.worldWorkers) == 1 || w.ScopeKind == "world" {
			wSnap = totalSnap
		}
		if results[i] == nil {
			w.Connected, w.Status = false, "OFFLINE"
			applyWorkerPopulationCounts(w, reconcilePopulationCounts(wSnap, 0, 0, 0, 0))
			continue
		}
		connected, w.Connected, w.Status, w.LastSeenMs = true, true, "ONLINE", nowMs
		subs += results[i].GetSubscribers()
		var wp, wn, wr, wm int
		if stats := results[i].GetStats(); stats != nil {
			tMs, pMs, pMax := float64(stats.GetTickMs()), float64(stats.GetPublishMs()), float64(stats.GetPublishMaxMs())
			tickMs, pubMs, pubMaxMs = max(tickMs, tMs), max(pubMs, pMs), max(pubMaxMs, pMax)
			wp, wn, wr, wm = int(stats.GetPlayers()), int(stats.GetNpcs()), int(stats.GetResourceNodes()), int(stats.GetMounts())
			gPlayers, gNpcs, gRes, gMounts = gPlayers+wp, gNpcs+wn, gRes+wr, gMounts+wm
			w.TickP50Ms, w.TickP99Ms = tMs, max(pMax, math.Round(tMs*1.25*100.0)/100.0)
			tierHz, rawHz := defaultWorkerTierTickHz(w.ScopeKind), defaultWorkerTierTickHz(w.ScopeKind)
			if meta := s.workerMeta[w.WorkerID]; meta.ScrapedTickHz > 0 {
				rawHz = meta.ScrapedTickHz
			} else if tMs > 1000.0/tierHz {
				rawHz = math.Round((1000.0/tMs)*100.0) / 100.0
			}
			w.TickHz, w.CurrentTickHz, w.TargetTickHz, w.TickMode = s.resolveWorkerTickFieldsLocked(w, rawHz)
			w.CPUPct = computeWorkerAdaptiveCPUPct(tMs, w.ScopeKind)
		}
		applyWorkerPopulationCounts(w, reconcilePopulationCounts(wSnap, wp, wn, wr, wm))
	}
	s.mu.Unlock()
	if subs == 0 {
		subs = uint32(s.coalescer.SubscriberCount())
	}
	tot, tod := reconcilePopulationCounts(totalSnap, gPlayers, gNpcs, gRes, gMounts), s.TimeOfDayHours()
	return map[string]any{
		"connected": connected, "tick_ms": tickMs, "publish_ms": pubMs, "publish_max_ms": pubMaxMs,
		"players": tot.Players, "players_online": tot.Players, "npcs": tot.Npcs, "npcs_total": tot.Npcs,
		"monsters": tot.Monsters, "monsters_total": tot.Monsters, "huntables": tot.Huntables, "huntables_total": tot.Huntables,
		"trees": tot.Trees, "trees_total": tot.Trees, "rocks": tot.Rocks, "rocks_total": tot.Rocks,
		"ore_gems": tot.OreGems, "ore_gems_total": tot.OreGems, "plants": tot.Plants, "plants_total": tot.Plants,
		"mounts": tot.Mounts, "mounts_total": tot.Mounts, "resource_nodes": tot.ResourceNodes, "resource_nodes_total": tot.ResourceNodes,
		"total_entities": tot.TotalEntities, "pois": poisCount, "pois_total": poisCount,
		"subscribers": subs, "workers": s.snapshotWorldWorkerStatuses(),
		"data_modes": s.resolveDataModes(), "service_tick_hz": worldstream.ServiceTickHz(),
		"time_of_day_hours": tod, "time_of_day_label": formatTimeOfDayLabel(tod),
	}
}
