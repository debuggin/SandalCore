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
)

var canonicalContainers = []string{
	"aoc-postgres",
	"aoc-auth",
	"aoc-character",
	"aoc-citynode",
	"aoc-quest",
	"aoc-mesh",
	"aoc-gateway",
	"aoc-mesh-gateway",
	"aoc-web-admin",
	"aoc-world",
	"aoc-ingress-proxy",
}

var allowedContainers = map[string]bool{
	"aoc-postgres":      true,
	"aoc-auth":          true,
	"aoc-character":     true,
	"aoc-citynode":      true,
	"aoc-quest":         true,
	"aoc-mesh":          true,
	"aoc-gateway":       true,
	"aoc-mesh-gateway":  true,
	"aoc-web-admin":     true,
	"aoc-world":         true,
	"aoc-ingress-proxy": true,
}

type ContainerMetric struct {
	ID         string  `json:"id"`
	Name       string  `json:"name"`
	Image      string  `json:"image"`
	State      string  `json:"state"`
	Status     string  `json:"status"`
	CPUPct     float64 `json:"cpu_pct"`
	MemMB      float64 `json:"mem_mb"`
	MemLimitMB float64 `json:"mem_limit_mb"`
	RxBytes    uint64  `json:"rx_bytes"`
	TxBytes    uint64  `json:"tx_bytes"`
}

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
	payload := map[string]any{
		"schema_version": "1.0",
		"count":          len(containers),
		"containers":     containers,
		"items":          containers,
		"world_status":   worldStatus,
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
	out := make([]ContainerMetric, 0, len(canonicalContainers))
	for _, cname := range canonicalContainers {
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
		out[m.Name] = m
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
	Networks map[string]struct {
		RxBytes uint64 `json:"rx_bytes"`
		TxBytes uint64 `json:"tx_bytes"`
	} `json:"networks"`
}

func populateOneContainerStats(ctx context.Context, cli *http.Client, dst *ContainerMetric) {
	sctx, cancel := context.WithTimeout(ctx, 1500*time.Millisecond)
	defer cancel()
	url := fmt.Sprintf("http://localhost/containers/%s/stats?stream=false", dst.Name)
	req, _ := http.NewRequestWithContext(sctx, http.MethodGet, url, nil)
	resp, err := cli.Do(req)
	if err != nil || resp == nil {
		return
	}
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	_ = resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return
	}
	var st dockerStatsPayload
	if json.Unmarshal(body, &st) != nil {
		return
	}
	cpuDelta := float64(st.CPUStats.CPUUsage.TotalUsage) - float64(st.PreCPUStats.CPUUsage.TotalUsage)
	sysDelta := float64(st.CPUStats.SystemCPUUsage) - float64(st.PreCPUStats.SystemCPUUsage)
	cpus := float64(max(st.CPUStats.OnlineCPUs, 1))
	if sysDelta > 0 && cpuDelta > 0 {
		dst.CPUPct = math.Round((cpuDelta/sysDelta)*cpus*10000.0) / 100.0
	}
	cacheBytes := st.MemoryStats.Stats["inactive_file"]
	if cacheBytes == 0 {
		cacheBytes = st.MemoryStats.Stats["cache"]
	}
	used := st.MemoryStats.Usage
	if used > cacheBytes {
		used -= cacheBytes
	}
	dst.MemMB = math.Round((float64(used)/(1024.0*1024.0))*100.0) / 100.0
	dst.MemLimitMB = math.Round((float64(st.MemoryStats.Limit)/(1024.0*1024.0))*100.0) / 100.0
	for _, netIf := range st.Networks {
		dst.RxBytes += netIf.RxBytes
		dst.TxBytes += netIf.TxBytes
	}
}

func (s *Server) fetchWorldStatus(ctx context.Context) map[string]any {
	out := map[string]any{
		"connected":      false,
		"tick_ms":        0.0,
		"publish_ms":     0.0,
		"publish_max_ms": 0.0,
		"players":        0,
		"npcs":           0,
		"resource_nodes": 0,
		"mounts":         0,
		"subscribers":    s.coalescer.SubscriberCount(),
	}
	cli := s.getWorldStreamClient()
	if cli == nil {
		return out
	}
	rctx, cancel := context.WithTimeout(ctx, 1500*time.Millisecond)
	defer cancel()
	st, err := callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.GetStatusResponse, error) {
		return cli.GetStatus(c, &worldadminv1.GetStatusRequest{})
	})
	if err != nil || st == nil {
		return out
	}
	out["connected"] = true
	out["subscribers"] = st.GetSubscribers()
	if stats := st.GetStats(); stats != nil {
		out["tick_ms"] = stats.GetTickMs()
		out["publish_ms"] = stats.GetPublishMs()
		out["publish_max_ms"] = stats.GetPublishMaxMs()
		out["players"] = stats.GetPlayers()
		out["npcs"] = stats.GetNpcs()
		out["resource_nodes"] = stats.GetResourceNodes()
		out["mounts"] = stats.GetMounts()
	}
	return out
}

func (s *Server) handleGMLogs(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	q := r.URL.Query()
	container := strings.TrimSpace(q.Get("container"))
	if container == "" {
		container = "aoc-world"
	}
	if !allowedContainers[container] {
		writeProblem(w, r, http.StatusBadRequest, "invalid-container", fmt.Sprintf("Container %q is not in the 11-container allowlist.", container))
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
