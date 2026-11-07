// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"encoding/json"
	"fmt"
	"math"
	"net/http"
	"testing"
	"time"
)

func TestTimeOfDayAndVerraCalendarSSoTPersistence(t *testing.T) {
	fixedNow := time.Date(2026, 11, 7, 15, 0, 0, 0, time.UTC)
	srv := NewRouter(Config{Now: func() time.Time { return fixedNow }})
	hdrs := map[string]string{
		"Content-Type": "application/json",
		"Accept":       "application/json",
	}

	putBody := []byte(`{"hours": 18.5, "day_of_year": 95, "year": 2}`)
	putRec := serveAuthed(srv, http.MethodPut, "/api/v1/gm/world/time-of-day", "gm-architect-dev-token", hdrs, putBody)
	if putRec.Code != http.StatusOK {
		t.Fatalf("PUT /api/v1/gm/world/time-of-day status = %d, want 200; body = %s", putRec.Code, putRec.Body.String())
	}
	var putPayload map[string]any
	if err := json.Unmarshal(putRec.Body.Bytes(), &putPayload); err != nil {
		t.Fatalf("decode PUT /api/v1/gm/world/time-of-day response: %v", err)
	}
	assertTimeAndCalendarPayload(t, "PUT /api/v1/gm/world/time-of-day", putPayload)

	getTodRec := serveAuthed(srv, http.MethodGet, "/api/v1/gm/world/time-of-day", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
	if getTodRec.Code != http.StatusOK {
		t.Fatalf("GET /api/v1/gm/world/time-of-day status = %d, want 200; body = %s", getTodRec.Code, getTodRec.Body.String())
	}
	var getTodPayload map[string]any
	if err := json.Unmarshal(getTodRec.Body.Bytes(), &getTodPayload); err != nil {
		t.Fatalf("decode GET /api/v1/gm/world/time-of-day response: %v", err)
	}
	assertTimeAndCalendarPayload(t, "GET /api/v1/gm/world/time-of-day", getTodPayload)

	stateRec := serveAuthed(srv, http.MethodGet, "/api/v1/world/state", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
	if stateRec.Code != http.StatusOK {
		t.Fatalf("GET /api/v1/world/state status = %d, want 200; body = %s", stateRec.Code, stateRec.Body.String())
	}
	var statePayload map[string]any
	if err := json.Unmarshal(stateRec.Body.Bytes(), &statePayload); err != nil {
		t.Fatalf("decode GET /api/v1/world/state response: %v", err)
	}
	assertTimeAndCalendarPayload(t, "GET /api/v1/world/state", statePayload)
}

func assertTimeAndCalendarPayload(t *testing.T, source string, payload map[string]any) {
	t.Helper()
	hours, ok := extractFloatParam(payload, "time_of_day_hours", "hours", "hour")
	if !ok || math.Abs(hours-18.5) > 0.05 {
		t.Fatalf("%s time_of_day_hours = %v (ok=%v), want 18.5 (never reset to 12.0); payload=%v", source, hours, ok, payload)
	}
	dayOfYear, hasDay := extractIntParam(payload, "calendar_day_of_year", "day_of_year")
	if !hasDay || dayOfYear != 95 {
		t.Fatalf("%s calendar_day_of_year = %d (ok=%v), want 95; payload=%v", source, dayOfYear, hasDay, payload)
	}
	year, hasYear := extractIntParam(payload, "calendar_year", "year")
	if !hasYear || year != 2 {
		t.Fatalf("%s calendar_year = %d (ok=%v), want 2; payload=%v", source, year, hasYear, payload)
	}
	season := extractStringParam(payload, "calendar_season", "season")
	if season != "Summer (Suncrest)" {
		t.Fatalf("%s calendar_season = %q, want %q; payload=%v", source, season, "Summer (Suncrest)", payload)
	}
}

func TestAdminCommandsSetNodeLevelUpdatesWinsteadTier4InWorldState(t *testing.T) {
	srv := NewRouter(Config{CityNodeClient: &fakeCityNodeClient{}})
	hdrs := map[string]string{
		"Content-Type": "application/json",
		"Accept":       "application/json",
	}
	cmdBody := []byte(`{"command": "set_node_level", "targetId": "Verra_RVR_Node_Winstead", "params": {"node_name": "Verra_RVR_Node_Winstead", "level": 4}}`)
	cmdRec := serveAuthed(srv, http.MethodPost, "/api/v1/admin/commands", "gm-architect-dev-token", hdrs, cmdBody)
	if cmdRec.Code != http.StatusOK {
		t.Fatalf("POST /api/v1/admin/commands set_node_level status = %d, want 200; body = %s", cmdRec.Code, cmdRec.Body.String())
	}

	stateRec := serveAuthed(srv, http.MethodGet, "/api/v1/world/state", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
	if stateRec.Code != http.StatusOK {
		t.Fatalf("GET /api/v1/world/state status = %d, want 200; body = %s", stateRec.Code, stateRec.Body.String())
	}
	var statePayload map[string]any
	if err := json.Unmarshal(stateRec.Body.Bytes(), &statePayload); err != nil {
		t.Fatalf("decode GET /api/v1/world/state: %v", err)
	}
	rawNodes, _ := statePayload["nodes"].([]any)
	if len(rawNodes) == 0 {
		rawNodes, _ = statePayload["city_nodes"].([]any)
	}
	foundWinstead := false
	for _, item := range rawNodes {
		nodeMap, ok := item.(map[string]any)
		if !ok {
			continue
		}
		guid := extractStringParam(nodeMap, "guid")
		name := extractStringParam(nodeMap, "name")
		if guid == "108646108518008" || name == "Verra_RVR_Winstead" || name == "Verra_RVR_Node_Winstead" {
			foundWinstead = true
			lv, hasLv := extractIntParam(nodeMap, "level")
			if !hasLv || lv != 4 {
				t.Fatalf("Winstead (108646108518008) level in GET /api/v1/world/state = %d (has=%v), want 4; node=%v", lv, hasLv, nodeMap)
			}
		}
	}
	if !foundWinstead {
		t.Fatalf("Winstead (108646108518008) not found in GET /api/v1/world/state nodes (%d nodes)", len(rawNodes))
	}
}

func TestSetWorkerTickRateSupportsDynamicZeroHzPauseAndManualPresetsAndRealWorkerCount(t *testing.T) {
	srv := NewRouter(Config{WorldAdminAddr: "world:50061", WorldStreamAddr: "world:50062"})
	hdrs := map[string]string{
		"Content-Type": "application/json",
		"Accept":       "application/json",
	}

	workersRec := serveAuthed(srv, http.MethodGet, "/api/v1/gm/workers", "gm-observer-dev-token", map[string]string{"Accept": "application/json"}, nil)
	if workersRec.Code != http.StatusOK {
		t.Fatalf("GET /api/v1/gm/workers status = %d, want 200", workersRec.Code)
	}
	var workersPayload map[string]any
	if err := json.Unmarshal(workersRec.Body.Bytes(), &workersPayload); err != nil {
		t.Fatalf("decode GET /api/v1/gm/workers: %v", err)
	}
	rawWorkers, _ := workersPayload["workers"].([]any)
	if len(rawWorkers) != 1 {
		t.Fatalf("GET /api/v1/gm/workers count = %d, want 1 configured single-world worker when AOC_WORLD_WORKERS is unset", len(rawWorkers))
	}

	dynBodies := [][]byte{
		[]byte(`{"command":"set_worker_tick_rate","targetId":"aoc-world-riverlands","params":{"worker_id":"aoc-world-riverlands","tick_hz":-1,"tick_mode":"dynamic"}}`),
		[]byte(`{"command":"set_worker_tick_rate","targetId":"aoc-world-riverlands","params":{"worker_id":"aoc-world-riverlands","tick_hz":"dynamic"}}`),
	}
	for i, body := range dynBodies {
		rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "gm-architect-dev-token", hdrs, body)
		if rec.Code != http.StatusOK {
			t.Fatalf("dynamic set_worker_tick_rate case %d status = %d, want 200; body = %s", i, rec.Code, rec.Body.String())
		}
	}

	pauseBody := []byte(`{"command":"set_worker_tick_rate","targetId":"aoc-world-riverlands","params":{"worker_id":"aoc-world-riverlands","tick_hz":0}}`)
	pauseRec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "gm-architect-dev-token", hdrs, pauseBody)
	if pauseRec.Code != http.StatusOK {
		t.Fatalf("0 Hz pause set_worker_tick_rate status = %d, want 200; body = %s", pauseRec.Code, pauseRec.Body.String())
	}
	var pauseResp map[string]any
	if err := json.Unmarshal(pauseRec.Body.Bytes(), &pauseResp); err != nil {
		t.Fatalf("decode 0 Hz pause response: %v", err)
	}
	if simAll, _ := pauseResp["simulate_all_npcs"].(bool); simAll {
		t.Fatalf("0 Hz set_worker_tick_rate expected simulate_all_npcs=false on worker, got %v", pauseResp)
	}

	for _, hz := range []int{10, 15, 20, 25, 30, 40, 50, 60} {
		body := []byte(fmt.Sprintf(`{"command":"set_worker_tick_rate","targetId":"aoc-world-riverlands","params":{"worker_id":"aoc-world-riverlands","tick_hz":%d}}`, hz))
		rec := serveAuthed(srv, http.MethodPost, "/api/v1/gm/commands", "gm-architect-dev-token", hdrs, body)
		if rec.Code != http.StatusOK {
			t.Fatalf("set_worker_tick_rate %d Hz status = %d, want 200; body = %s", hz, rec.Code, rec.Body.String())
		}
		var resp map[string]any
		if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
			t.Fatalf("decode set_worker_tick_rate %d Hz response: %v", hz, err)
		}
		gotHz, ok := extractIntParam(resp, "tick_hz", "hz")
		if !ok || gotHz != hz {
			t.Fatalf("set_worker_tick_rate %d Hz returned tick_hz=%d (ok=%v); resp=%v", hz, gotHz, ok, resp)
		}
	}
}
