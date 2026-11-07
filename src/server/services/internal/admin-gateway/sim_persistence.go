// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	admincatalog "aocrop/services/internal/admin-catalog"
	worldstream "aocrop/services/internal/world-stream"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

var verraSeasonNames = [4]string{
	"Spring (Greenwake)",
	"Summer (Suncrest)",
	"Autumn (Harvestfall)",
	"Winter (Frostveil)",
}

var verraMonthNames = [12]string{
	"Greenwake", "Bloomtide", "Sunseed",
	"Suncrest", "Highflare", "Emberwane",
	"Harvestfall", "Duskreap", "Mistveil",
	"Frostveil", "Deepcold", "Starthaw",
}

type VerraCalendar struct {
	Year       int    `json:"year"`
	DayOfYear  int    `json:"day_of_year"`
	Season     string `json:"season"`
	Month      string `json:"month"`
	DayOfMonth int    `json:"day_of_month"`
}

func (s *Server) initSimulationDefaults() {
	s.timeOfDayMu.Lock()
	if s.calendarAnchorYear <= 0 {
		s.calendarAnchorYear = 1
	}
	if s.calendarAnchorDay < 1 || s.calendarAnchorDay > 360 {
		s.calendarAnchorDay = 1
	}
	if s.timeScale <= 0 {
		s.timeScale = 9.0
	}
	if s.workerTickMode == "" {
		s.workerTickMode = "dynamic"
	}
	if s.workerTickHzMap == nil {
		s.workerTickHzMap = make(map[string]float64)
	}
	s.simulateAllNpcs = true
	s.timeOfDayMu.Unlock()
}

func (s *Server) TimeScale() float64 {
	s.timeOfDayMu.RLock()
	defer s.timeOfDayMu.RUnlock()
	if s.timeScale <= 0 {
		return 9.0
	}
	return s.timeScale
}

func (s *Server) WorkerTickMode() string {
	s.timeOfDayMu.RLock()
	defer s.timeOfDayMu.RUnlock()
	if s.workerTickMode == "" {
		return "dynamic"
	}
	return s.workerTickMode
}

func (s *Server) CalendarAt(at time.Time) VerraCalendar {
	s.timeOfDayMu.RLock()
	baseHours, updatedAt := s.timeOfDayHours, s.timeOfDayUpdatedAt
	anchorYear, anchorDay, scale := max(s.calendarAnchorYear, 1), min(max(s.calendarAnchorDay, 1), 360), s.timeScale
	s.timeOfDayMu.RUnlock()
	if updatedAt.IsZero() && baseHours == 0 {
		baseHours, updatedAt = 12.0, s.startedAt
	}
	if scale <= 0 {
		scale = 9.0
	}
	totalHours := baseHours + max(at.Sub(updatedAt).Seconds(), 0)*(scale/3600.0)
	elapsedDays := int(math.Floor(totalHours / 24.0))
	if elapsedDays < 0 {
		elapsedDays = 0
	}
	absDays := (anchorYear-1)*360 + (anchorDay - 1) + elapsedDays
	year := absDays/360 + 1
	dayIdx := absDays % 360
	seasonIdx := min(max(dayIdx/90, 0), 3)
	monthIdx := min(max(dayIdx/30, 0), 11)
	return VerraCalendar{
		Year:       year,
		DayOfYear:  dayIdx + 1,
		Season:     verraSeasonNames[seasonIdx],
		Month:      verraMonthNames[monthIdx],
		DayOfMonth: (dayIdx % 30) + 1,
	}
}

func (s *Server) enrichPayloadWithTimeAndCalendar(payload map[string]any, at time.Time) {
	todHours := s.TimeOfDayHoursAt(at)
	anchorHours, anchorMs := s.TimeOfDayAnchor()
	scale := s.TimeScale()
	cal := s.CalendarAt(at)
	s.timeOfDayMu.RLock()
	simAll, tickMode := s.simulateAllNpcs, firstNonEmpty(s.workerTickMode, "dynamic")
	s.timeOfDayMu.RUnlock()
	payload["time_of_day_hours"] = todHours
	payload["hours"] = todHours
	payload["time_of_day_label"] = formatTimeOfDayLabel(todHours)
	payload["time_of_day_anchor_hours"] = anchorHours
	payload["time_of_day_anchor_ms"] = anchorMs
	payload["time_of_day_rate"] = scale
	payload["time_scale"] = scale
	payload["calendar_year"] = cal.Year
	payload["year"] = cal.Year
	payload["calendar_day_of_year"] = cal.DayOfYear
	payload["day_of_year"] = cal.DayOfYear
	payload["calendar_season"] = cal.Season
	payload["season"] = cal.Season
	payload["calendar_month"] = cal.Month
	payload["month"] = cal.Month
	payload["calendar_day_of_month"] = cal.DayOfMonth
	payload["day_of_month"] = cal.DayOfMonth
	payload["calendar"] = cal
	payload["tick_mode"] = tickMode
	payload["simulate_all_npcs"] = simAll
}

func (s *Server) registerSimulationRoutes() {
	s.mux.HandleFunc("GET /api/v1/admin/commands", s.requireRole(RoleObserver, s.wrapHandler(s.HandleCommandList)))
	s.mux.HandleFunc("HEAD /api/v1/admin/commands", s.requireRole(RoleObserver, s.wrapHandler(s.HandleCommandList)))
	s.mux.HandleFunc("POST /api/v1/admin/commands", s.guardGMCommandsAuth(s.handleGMCommandsEndpoint))
	s.mux.HandleFunc("POST /api/v1/admin/command", s.optionalRole(RoleArchitect, s.handleGMCommand))
	for _, base := range []string{"/api/v1/gm/world/time-of-day", "/api/v1/world/time-of-day"} {
		s.mux.HandleFunc("GET "+base, s.optionalRole(RoleObserver, s.handleGetTimeOfDay))
		s.mux.HandleFunc("HEAD "+base, s.optionalRole(RoleObserver, s.handleGetTimeOfDay))
		s.mux.HandleFunc("PUT "+base, s.optionalRole(RoleOperator, s.handleSetTimeOfDayHTTP))
		s.mux.HandleFunc("POST "+base, s.optionalRole(RoleOperator, s.handleSetTimeOfDayHTTP))
	}
}

func (s *Server) handleGetTimeOfDay(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	now := s.now()
	payload := map[string]any{"schema_version": "1.0", "status": "ok", "serverTimeMs": now.UnixMilli()}
	s.enrichPayloadWithTimeAndCalendar(payload, now)
	writeJSON(w, http.StatusOK, payload)
}

func (s *Server) handleSetTimeOfDayHTTP(w http.ResponseWriter, r *http.Request, role Role) {
	if !checkJSONContentType(w, r) || !checkAcceptHeader(w, r) {
		return
	}
	var body map[string]any
	if !decodeBoundedJSON(w, r, &body) {
		return
	}
	res, err := s.applyTimeAndCalendarParams(r.Context(), body, false)
	if err != nil {
		writeGRPCProblem(w, r, err, "set-time-of-day-failed")
		return
	}
	s.recordGMAuditWithContext(r, role, CommandRequest{Command: "set_time_of_day", Params: body}, res)
	writeJSON(w, http.StatusOK, res)
}

func (s *Server) execSetWorldCalendarCommand(ctx context.Context, p map[string]any) (map[string]any, error) {
	res, err := s.applyTimeAndCalendarParams(ctx, p, false)
	if err != nil {
		return nil, err
	}
	res["action"], res["command"] = "set_world_calendar", "set_world_calendar"
	return res, nil
}

func (s *Server) applyTimeAndCalendarParams(ctx context.Context, p map[string]any, requireHour bool) (map[string]any, error) {
	now := s.now()
	hour, hasHour := extractTimeOfDayHours(p)
	dayOfYear, hasDay := extractIntParam(p, "day_of_year", "calendar_day_of_year", "day")
	year, hasYear := extractIntParam(p, "year", "calendar_year")
	scale, hasScale := extractFloatParam(p, "time_scale", "time_of_day_rate", "rate")
	if requireHour && !hasHour {
		return nil, status.Error(codes.InvalidArgument, "hour or time_of_day (0..24 or HH:MM) is required")
	}
	if !hasHour && !hasDay && !hasYear && !hasScale {
		return nil, status.Error(codes.InvalidArgument, "hours, day_of_year, year, or time_scale is required")
	}
	curCal := s.CalendarAt(now)
	if !hasHour {
		hour = s.TimeOfDayHoursAt(now)
	}
	if !hasDay || dayOfYear < 1 || dayOfYear > 360 {
		dayOfYear = curCal.DayOfYear
	}
	if !hasYear || year < 1 {
		year = curCal.Year
	}
	s.timeOfDayMu.Lock()
	s.timeOfDayHours, s.timeOfDayUpdatedAt = hour, now
	s.calendarAnchorDay, s.calendarAnchorYear = dayOfYear, year
	if hasScale && scale >= 0.1 && scale <= 3600.0 {
		s.timeScale = scale
	}
	s.timeOfDayMu.Unlock()
	s.recordMutation()
	s.persistSimulationState(ctx)
	s.broadcastTimeOfDayToWorkers(ctx, now, hour)
	return s.buildTimeCommandResult(now, hour), nil
}

func (s *Server) broadcastTimeOfDayToWorkers(ctx context.Context, now time.Time, hour float64) {
	s.mu.Lock()
	workers := append([]*WorldWorkerEndpoint(nil), s.worldWorkers...)
	s.mu.Unlock()
	var wg sync.WaitGroup
	for _, w := range workers {
		if w != nil {
			s.markWorkerTimeSyncedAt(w.WorkerID, now)
			wg.Add(1)
			go func(worker *WorldWorkerEndpoint) { defer wg.Done(); s.pushTimeOfDayToWorker(ctx, worker, hour) }(w)
		}
	}
	wg.Wait()
}

func (s *Server) buildTimeCommandResult(now time.Time, hour float64) map[string]any {
	s.mu.Lock()
	updatedCount := max(len(s.worldWorkers), 1)
	s.mu.Unlock()
	label := formatTimeOfDayLabel(hour)
	cal := s.CalendarAt(now)
	msg := fmt.Sprintf("Set server time of day to %s (%.2fh, Year %d Day %d %s) across %d world workers", label, hour, cal.Year, cal.DayOfYear, cal.Season, updatedCount)
	res := map[string]any{
		"status": "executed", "action": "set_time_of_day", "command": "set_time_of_day",
		"workers_updated": updatedCount, "message": msg, "serverTimeMs": now.UnixMilli(),
	}
	s.enrichPayloadWithTimeAndCalendar(res, now)
	return res
}

func workerBatchScopeKey(w *WorldWorkerEndpoint) string {
	if w == nil {
		return "riverlands"
	}
	wid := strings.ToLower(strings.TrimSpace(w.WorkerID))
	skey := strings.ToLower(strings.TrimSpace(w.ScopeKey))
	if wid != "" {
		trimmed := strings.TrimPrefix(strings.TrimPrefix(wid, "aoc-world-"), "world-")
		if trimmed == "verra" || trimmed == "all" || strings.EqualFold(skey, "all") || strings.EqualFold(skey, "verra") {
			return "verra"
		}
		if trimmed != "" {
			return trimmed
		}
	}
	if skey == "all" || skey == "verra" {
		return "verra"
	}
	return firstNonEmpty(skey, "riverlands")
}

func resolveCatalogCityNode(idOrName string) (int64, string, string, bool) {
	trimmed := strings.TrimSpace(idOrName)
	if trimmed == "" {
		return 0, "", "", false
	}
	if sp, ok := findCanonicalNodeSpec(trimmed); ok {
		rosterName := strings.Replace(sp.Name, "_Node_", "_", 1)
		catalogName := sp.Name
		if !strings.Contains(catalogName, "_Node_") {
			for _, code := range []string{"_RVR_", "_SAN_", "_JUN_", "_ANV_", "_TUR_"} {
				if strings.Contains(catalogName, code) {
					catalogName = strings.Replace(catalogName, code, code+"Node_", 1)
					break
				}
			}
		}
		return sp.GUID, rosterName, catalogName, true
	}
	if g, err := strconv.ParseInt(trimmed, 10, 64); err == nil && g != 0 {
		return g, trimmed, trimmed, true
	}
	rosterName := strings.Replace(trimmed, "_Node_", "_", 1)
	return 0, rosterName, trimmed, false
}

func (s *Server) resolveAndExecSetNodeLevel(ctx context.Context, node string, level int) (map[string]any, error) {
	node = strings.TrimSpace(node)
	if node == "" {
		return nil, status.Error(codes.InvalidArgument, "node identifier is required")
	}
	guidInt64, rosterName, catalogName, _ := resolveCatalogCityNode(node)
	s.storeLocalNodeLevel(node, guidInt64, rosterName, catalogName, level)
	s.recordMutation()
	s.persistCityNodeState(ctx, guidInt64, firstNonEmpty(catalogName, node))
	if cli := s.getCityNodeClient(); cli != nil {
		resp, err := s.callCityNodeSetLevel(ctx, cli, guidInt64, firstNonEmpty(rosterName, node), level)
		if err != nil {
			if st, ok := status.FromError(err); !ok || (st.Code() != codes.Unavailable && st.Code() != codes.DeadlineExceeded) {
				return nil, err
			}
		} else if resp != nil && resp.GetNode() != nil {
			n := resp.GetNode()
			lv := max(int(n.GetLevel()), level)
			s.storeLocalNodeLevel(node, n.GetGuid(), n.GetName(), catalogName, lv)
			s.refreshStreamAfterNodeTierChange(ctx)
			return map[string]any{
				"guid": strconv.FormatInt(n.GetGuid(), 10), "name": firstNonEmpty(n.GetName(), rosterName),
				"display_name": firstNonEmpty(n.GetDisplayName(), node), "level": lv,
				"plug_tier": nodePlugTierForLevel(lv), "stage_name": admincatalog.StageNameForLevel(lv), "status": "UPDATED",
			}, nil
		}
	}
	s.refreshStreamAfterNodeTierChange(ctx)
	return map[string]any{
		"guid": strconv.FormatInt(guidInt64, 10), "node": firstNonEmpty(rosterName, node), "name": firstNonEmpty(rosterName, node),
		"level": level, "plug_tier": nodePlugTierForLevel(level), "stage_name": admincatalog.StageNameForLevel(level), "status": "UPDATED",
	}, nil
}

func (s *Server) storeLocalNodeLevel(rawNode string, guid int64, rosterName, catalogName string, level int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, k := range []string{rawNode, rosterName, catalogName, normalizeNodeKey(rawNode)} {
		if trimmed := strings.ToLower(strings.TrimSpace(k)); trimmed != "" {
			s.localNodeLevels[trimmed] = level
		}
	}
	if guid > 0 {
		s.localNodeLevels[strconv.FormatInt(guid, 10)] = level
	}
	if sp, ok := findCanonicalNodeSpec(firstNonEmpty(rosterName, rawNode)); ok {
		guidStr := strconv.FormatInt(sp.GUID, 10)
		s.localNodeLevels[guidStr] = level
		s.localNodeLevels[strings.ToLower(sp.Name)] = level
		s.localNodeLevels[strings.ToLower(strings.Replace(sp.Name, "_RVR_", "_RVR_Node_", 1))] = level
		s.localNodeLevels[strings.ToLower(sp.DisplayName)] = level
	}
}

func (s *Server) callCityNodeSetLevel(ctx context.Context, cli citynodev1.CityNodeServiceClient, guid int64, name string, level int) (*citynodev1.SetNodeLevelResponse, error) {
	ref := &citynodev1.NodeRef{Guid: guid, Name: name}
	rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	resp, err := callWithRetry(rctx, s, func(c context.Context) (*citynodev1.SetNodeLevelResponse, error) {
		return cli.SetNodeLevel(c, &citynodev1.SetNodeLevelRequest{Node: ref, Level: int32(level), DisableVassalship: true})
	})
	if err != nil && level > 4 && status.Code(err) == codes.InvalidArgument {
		resp, err = callWithRetry(rctx, s, func(c context.Context) (*citynodev1.SetNodeLevelResponse, error) {
			return cli.SetNodeLevel(c, &citynodev1.SetNodeLevelRequest{Node: ref, Level: 4, DisableVassalship: true})
		})
	}
	return resp, err
}

func parseTickRateParams(p map[string]any) (int, string, error) {
	mode := strings.ToLower(strings.TrimSpace(extractStringParam(p, "tick_mode", "mode")))
	rawStr := strings.ToLower(strings.TrimSpace(extractStringParam(p, "tick_hz", "hz", "rate", "value")))
	if mode == "dynamic" || rawStr == "dynamic" || rawStr == "auto" {
		return 60, "dynamic", nil
	}
	tickHz, ok := extractIntParam(p, "tick_hz", "hz", "rate", "amount", "value")
	if !ok {
		return 0, "", status.Error(codes.InvalidArgument, "tick_hz must be 'dynamic' (-1), 0 (pause worker NPCs), or 1..120")
	}
	if tickHz == -1 {
		return 60, "dynamic", nil
	}
	if tickHz == 0 {
		return 0, "manual", nil
	}
	if tickHz < 1 || tickHz > 120 {
		return 0, "", status.Error(codes.InvalidArgument, "tick_hz must be between 1 and 120, 0 (pause worker NPCs), or -1 ('dynamic')")
	}
	return tickHz, "manual", nil
}

func (s *Server) pushWorkerTickAndNpcSim(ctx context.Context, host string, w *WorldWorkerEndpoint, adminHz int, tickMode string, simEnabled bool, simFlag int) {
	rctx, cancel := context.WithTimeout(ctx, 900*time.Millisecond)
	defer cancel()
	simURLs := []string{fmt.Sprintf("http://%s:8080/admin/simulate_all_npcs?simulate_all_npcs=%d", host, simFlag)}
	tickURLs := []string{fmt.Sprintf("http://%s:8080/admin/tick_hz?tick_hz=%d&tick_mode=%s", host, adminHz, tickMode)}
	if murl := strings.TrimSpace(firstNonEmpty(os.Getenv("AOC_WORLD_METRICS_URL"), os.Getenv("AOC_WORLD_PROM_URL"))); murl != "" {
		base := strings.TrimSuffix(strings.TrimSuffix(murl, "/metrics"), "/healthz")
		simURLs = append([]string{fmt.Sprintf("%s/admin/simulate_all_npcs?simulate_all_npcs=%d", base, simFlag)}, simURLs...)
		tickURLs = append([]string{fmt.Sprintf("%s/admin/tick_hz?tick_hz=%d&tick_mode=%s", base, adminHz, tickMode)}, tickURLs...)
	}
	_, _, _, _ = fetchFirstReachableText(rctx, simURLs, simURLs[0])
	_, _, _, _ = fetchFirstReachableText(rctx, tickURLs, tickURLs[0])
	if cli := s.workerAdminClient(w); cli != nil {
		_, _ = callWithRetry(rctx, s, func(c context.Context) (*worldadminv1.SpawnNpcResponse, error) {
			return cli.SpawnNpc(c, &worldadminv1.SpawnNpcRequest{Npc: "gm.SetSimulateAllNpcs", Level: int32(simFlag), Target: &worldadminv1.SpawnNpcRequest_Place{Place: fmt.Sprintf("%t", simEnabled)}})
		})
	}
}

func (s *Server) registerWorkerAndTelemetryRoutes() {
	s.registerReadRoute("/api/v1/gm/containers", s.optionalRole(RoleObserver, s.handleGMContainers))
	s.registerReadRoute("/api/v1/gm/workers", s.optionalRole(RoleObserver, s.handleWorldWorkers))
	s.registerReadRoute("/api/v1/world/workers", s.optionalRole(RoleObserver, s.handleWorldWorkers))
	for _, p := range []string{"/api/v1/mesh/workers/register", "/api/v1/world/workers", "/api/v1/gm/workers"} {
		s.mux.HandleFunc("POST "+p, s.optionalRole(RoleOperator, s.handleRegisterMeshWorker))
	}
	for _, p := range []string{"POST /api/v1/gm/workers/{id}/tick-rate", "PUT /api/v1/gm/workers/{id}/tick-rate", "POST /api/v1/world/workers/{id}/tick-rate", "PUT /api/v1/world/workers/{id}/tick-rate"} {
		s.mux.HandleFunc(p, s.optionalRole(RoleOperator, s.handleSetWorkerTickRate))
	}
	s.registerReadRoute("/api/v1/gm/metrics", s.optionalRole(RoleObserver, s.handlePrometheusMetrics))
	s.registerReadRoute("/api/v1/metrics", s.optionalRole(RoleObserver, s.handlePrometheusMetrics))
	s.registerReadRoute("/api/v1/gm/logs", s.optionalRole(RoleObserver, s.handleGMLogs))
	s.registerReadRoute("/api/v1/gm/openapi.json", s.optionalRole(RoleObserver, s.handleOpenAPI))
	s.registerReadRoute("/api/v1/world/openapi.json", s.optionalRole(RoleObserver, s.handleOpenAPI))
}

func (s *Server) handleSetWorkerTickRate(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	var p map[string]any
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10)).Decode(&p); err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-json-body", "Malformed JSON body.")
		return
	}
	targetID := strings.TrimSpace(r.PathValue("id"))
	res, err := s.execSetTickRateCommand(r.Context(), targetID, p)
	if err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-argument", err.Error())
		return
	}
	s.recordMutation()
	writeJSON(w, http.StatusOK, res)
}

func (s *Server) hydrateFromPostgres(ctx context.Context) {
	if s.pool == nil {
		return
	}
	qctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	var anchorHours, scale float64
	var anchorAt time.Time
	var year, day int
	var simAll bool
	var tickMode string
	var tickHzRaw []byte
	err := s.pool.QueryRow(qctx, `
		SELECT time_of_day_anchor_hours, time_of_day_anchor_at, calendar_anchor_year, calendar_anchor_day,
		       time_scale, simulate_all_npcs, worker_tick_mode, worker_tick_hz
		FROM world_simulation_state WHERE id = TRUE`).Scan(&anchorHours, &anchorAt, &year, &day, &scale, &simAll, &tickMode, &tickHzRaw)
	if err == nil {
		s.timeOfDayMu.Lock()
		s.timeOfDayHours, s.timeOfDayUpdatedAt = wrapHours24(anchorHours), anchorAt
		s.calendarAnchorYear, s.calendarAnchorDay = max(year, 1), min(max(day, 1), 360)
		if scale > 0 {
			s.timeScale = scale
		}
		s.simulateAllNpcs = simAll
		s.workerTickMode = firstNonEmpty(tickMode, "dynamic")
		_ = json.Unmarshal(tickHzRaw, &s.workerTickHzMap)
		playerHandoversMu.Lock()
		for k, hz := range s.workerTickHzMap {
			workerTickHzOverrides[serverPlayerKey{srv: s, player: strings.ToLower(strings.TrimSpace(k))}] = hz
		}
		playerHandoversMu.Unlock()
		s.timeOfDayMu.Unlock()
	}
	s.hydrateCityNodesFromPostgres(qctx)
}

func (s *Server) hydrateCityNodesFromPostgres(ctx context.Context) {
	rows, err := s.pool.Query(ctx, `SELECT node_guid, node_id, level, mayor_character_id, mayor_name FROM city_node_state`)
	if err != nil {
		return
	}
	defer rows.Close()
	for rows.Next() {
		var guid int64
		var nodeID, mayorID, mayorName string
		var lv int
		if rows.Scan(&guid, &nodeID, &lv, &mayorID, &mayorName) == nil {
			_, rosterName, catalogName, _ := resolveCatalogCityNode(firstNonEmpty(nodeID, strconv.FormatInt(guid, 10)))
			s.storeLocalNodeLevel(nodeID, guid, rosterName, catalogName, lv)
			if mayorID != "" || mayorName != "" {
				gov := nodeGovernanceState{MayorGuid: mayorID, MayorName: mayorName, ElectionStatus: "active_term"}
				s.mu.Lock()
				s.nodeGovernance[strconv.FormatInt(guid, 10)] = gov
				s.nodeGovernance[normalizeNodeKey(nodeID)] = gov
				s.mu.Unlock()
			}
		}
	}
}

func (s *Server) persistSimulationState(ctx context.Context) {
	if s.pool == nil {
		return
	}
	s.timeOfDayMu.RLock()
	anchorHours, anchorAt := s.timeOfDayHours, s.timeOfDayUpdatedAt
	year, day, scale := max(s.calendarAnchorYear, 1), min(max(s.calendarAnchorDay, 1), 360), max(s.timeScale, 9.0)
	simAll, tickMode := s.simulateAllNpcs, firstNonEmpty(s.workerTickMode, "dynamic")
	hzJSON, _ := json.Marshal(s.workerTickHzMap)
	s.timeOfDayMu.RUnlock()
	if len(hzJSON) == 0 {
		hzJSON = []byte("{}")
	}
	pctx, cancel := context.WithTimeout(ctx, 1500*time.Millisecond)
	defer cancel()
	_, _ = s.pool.Exec(pctx, `
		INSERT INTO world_simulation_state (
			id, time_of_day_anchor_hours, time_of_day_anchor_at, calendar_anchor_year, calendar_anchor_day,
			time_scale, simulate_all_npcs, worker_tick_mode, worker_tick_hz, updated_at
		) VALUES (TRUE, $1, $2, $3, $4, $5, $6, $7, $8::jsonb, NOW())
		ON CONFLICT (id) DO UPDATE SET
			time_of_day_anchor_hours = EXCLUDED.time_of_day_anchor_hours,
			time_of_day_anchor_at = EXCLUDED.time_of_day_anchor_at,
			calendar_anchor_year = EXCLUDED.calendar_anchor_year,
			calendar_anchor_day = EXCLUDED.calendar_anchor_day,
			time_scale = EXCLUDED.time_scale,
			simulate_all_npcs = EXCLUDED.simulate_all_npcs,
			worker_tick_mode = EXCLUDED.worker_tick_mode,
			worker_tick_hz = EXCLUDED.worker_tick_hz,
			updated_at = NOW()`,
		anchorHours, anchorAt, year, day, scale, simAll, tickMode, string(hzJSON))
}

func (s *Server) persistCityNodeState(ctx context.Context, guid int64, nodeID string) {
	if s.pool == nil || guid <= 0 {
		return
	}
	guidStr := strconv.FormatInt(guid, 10)
	s.mu.Lock()
	lv := s.localNodeLevels[guidStr]
	gov := s.nodeGovernance[guidStr]
	s.mu.Unlock()
	pctx, cancel := context.WithTimeout(ctx, 1500*time.Millisecond)
	defer cancel()
	_, _ = s.pool.Exec(pctx, `
		INSERT INTO city_node_state (node_guid, node_id, level, mayor_character_id, mayor_name, updated_at)
		VALUES ($1, $2, $3, $4, $5, NOW())
		ON CONFLICT (node_guid) DO UPDATE SET
			node_id = EXCLUDED.node_id,
			level = EXCLUDED.level,
			mayor_character_id = EXCLUDED.mayor_character_id,
			mayor_name = EXCLUDED.mayor_name,
			updated_at = NOW()`,
		guid, firstNonEmpty(nodeID, guidStr), min(max(lv, 0), 6), gov.MayorGuid, gov.MayorName)
}

func (s *Server) markEntityDespawned(entityGuid string, scope string, rawID uint64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if g := strings.ToLower(strings.TrimSpace(entityGuid)); g != "" {
		s.despawnedEntities[g] = struct{}{}
	}
	for _, k := range []string{fmt.Sprintf("%d", rawID), fmt.Sprintf("npc:%d", rawID), fmt.Sprintf("monster:%d", rawID)} {
		if rawID > 0 {
			s.despawnedEntities[k] = struct{}{}
			if scope != "" {
				s.despawnedEntities[strings.ToLower(scope)+":"+k] = struct{}{}
			}
		}
	}
}

func (s *Server) clearEntityDespawned(entityGuid string, rawID uint64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.despawnedEntities, strings.ToLower(strings.TrimSpace(entityGuid)))
	for k := range s.despawnedEntities {
		if rawID > 0 && (k == fmt.Sprintf("%d", rawID) || strings.HasSuffix(k, fmt.Sprintf(":%d", rawID))) {
			delete(s.despawnedEntities, k)
		}
	}
}

func (s *Server) isEntityDespawned(id string) bool {
	k := strings.ToLower(strings.TrimSpace(id))
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.despawnedEntities[k]; ok && k != "" {
		return true
	}
	if idx := strings.IndexByte(k, ':'); idx > 0 {
		_, ok := s.despawnedEntities[k[idx+1:]]
		return ok
	}
	return false
}

func (s *Server) streamSubscriptionLoop(r *http.Request, sub *worldstream.Subscription, maxFrames int, emit func([]byte) error) {
	s.coalescer.Flush()
	for sent := 0; ; {
		select {
		case <-r.Context().Done():
			return
		case delta, ok := <-sub.Updates():
			if !ok {
				return
			}
			payload, _ := json.Marshal(delta)
			if emit(payload) != nil {
				return
			}
			if sent++; maxFrames > 0 && sent >= maxFrames {
				return
			}
		}
	}
}

func writeWSFrame(conn net.Conn, payload []byte) error {
	length, header := len(payload), []byte{0x81}
	if length <= 125 {
		header = append(header, byte(length))
	} else if length <= 65535 {
		header = append(header, 126, byte(length>>8), byte(length))
	} else {
		header = append(header, 127, 0, 0, 0, 0, 0, 0, 0, 0)
		binary.BigEndian.PutUint64(header[2:10], uint64(length))
	}
	if _, err := conn.Write(header); err != nil {
		return err
	}
	_, err := conn.Write(payload)
	return err
}
