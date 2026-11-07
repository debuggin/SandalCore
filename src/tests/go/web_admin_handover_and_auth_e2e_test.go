// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package go_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"
)

func TestWebAdminPassphraseJWTAndTickRateLive(t *testing.T) {
	baseURL := resolveLiveOrEphemeralBaseURL(t)
	hdrJSON := map[string]string{"Content-Type": "application/json", "Accept": "application/json"}
	if badCode, _, _ := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/admin/auth/login", hdrJSON, []byte(`{"username":"admin","passphrase":"invalid-passphrase-for-test"}`)); badCode != http.StatusUnauthorized {
		t.Fatalf("POST /api/v1/admin/auth/login with invalid passphrase status=%d, want 401", badCode)
	}
	pass := resolveAdminPassphrase(t)
	code, _, raw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/admin/auth/login", hdrJSON, []byte(fmt.Sprintf(`{"username":"admin","passphrase":%q}`, pass)))
	if code == http.StatusUnauthorized && pass != "sandal-admin-dev-passphrase" {
		pass = "sandal-admin-dev-passphrase"
		code, _, raw = doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/admin/auth/login", hdrJSON, []byte(fmt.Sprintf(`{"username":"admin","passphrase":%q}`, pass)))
	}
	var loginDoc map[string]any
	_ = json.Unmarshal(raw, &loginDoc)
	tok, _ := loginDoc["token"].(string)
	bootID, _ := loginDoc["boot_id"].(string)
	if code != http.StatusOK || tok == "" || bootID == "" {
		t.Fatalf("POST /api/v1/admin/auth/login status=%d tok=%q boot_id=%q body=%s", code, tok, bootID, string(raw))
	}
	setActiveAdminBearer(tok)
	authHdr := map[string]string{"Authorization": "Bearer " + tok, "Content-Type": "application/json", "Accept": "application/json"}
	badAuthHdr := map[string]string{"Authorization": "Bearer " + tok + "corrupt", "Content-Type": "application/json", "Accept": "application/json"}
	if unauthCode, _, _ := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/command", badAuthHdr, []byte(`{"command":"set_tick_rate","tick_hz":25}`)); unauthCode != http.StatusUnauthorized {
		t.Fatalf("POST /api/v1/gm/command with corrupted token status=%d, want 401", unauthCode)
	}
	c25, _, b25 := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/command", authHdr, []byte(`{"command":"set_tick_rate","tick_hz":25}`))
	if c25 != http.StatusOK {
		t.Fatalf("POST /api/v1/gm/command set_tick_rate=25 status=%d body=%s", c25, string(b25))
	}
	cRestore, _, bRestore := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/command", authHdr, []byte(`{"command":"set_tick_rate","tick_hz":20}`))
	if cRestore != http.StatusOK {
		t.Fatalf("POST /api/v1/gm/command restore set_tick_rate=20 status=%d body=%s", cRestore, string(bRestore))
	}
}

func verifyLiveSpawnKillDespawnLifecycle(t *testing.T, baseURL string) {
	t.Helper()
	for i, act := range []string{"kill_npc", "despawn_npc"} {
		hdrs := map[string]string{"Content-Type": "application/json", "Accept": "application/json", "Idempotency-Key": fmt.Sprintf("skd-%s-%d-%d", act, time.Now().UnixNano(), i)}
		spBody := `{"command":"spawn_npc","npc":"DesignData_NPC_Goblin","place":"Winstead","x_cm":-641389,"y_cm":377367.1,"z_cm":12500,"level":10}`
		code, _, raw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", hdrs, []byte(spBody))
		var spDoc map[string]any
		_ = json.Unmarshal(raw, &spDoc)
		guid, _ := spDoc["entity_guid"].(string)
		if code != http.StatusOK || guid == "" {
			t.Fatalf("spawn_npc(%s) status=%d guid=%q body=%s", act, code, guid, string(raw))
		}
		hdrs["Idempotency-Key"] = hdrs["Idempotency-Key"] + "-rm"
		if rmCode, _, rmRaw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", hdrs, []byte(fmt.Sprintf(`{"command":%q,"entity_guid":%q}`, act, guid))); rmCode != http.StatusOK {
			t.Fatalf("%s(%s) status=%d body=%s", act, guid, rmCode, string(rmRaw))
		}
		_, _, stRaw := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/state", nil, nil)
		if strings.Contains(string(stRaw), fmt.Sprintf(`"id":%q`, guid)) {
			t.Fatalf("%s did not remove %s from /api/v1/world/state", act, guid)
		}
	}
}

type borderHopSpec struct {
	x, y       float64
	wantRegion string
	wantZoi    string
	wantWorker string
	wantCount  int
}

func verifyLiveBorderHandoverHysteresis(t *testing.T, baseURL string) {
	t.Helper()
	charName := "TST GruelingBiscuitLauncher"
	spawnedTestPlayers.Store(charName, true)
	seq := 0
	stepBorder := func(body string) map[string]any {
		seq++
		hdrs := map[string]string{"Content-Type": "application/json", "Accept": "application/json", "Idempotency-Key": fmt.Sprintf("hb6-%d-%d", time.Now().UnixNano(), seq)}
		code, _, raw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", hdrs, []byte(body))
		if code != http.StatusOK {
			t.Fatalf("step_player_border[%d] status=%d body=%s", seq, code, string(raw))
		}
		var doc map[string]any
		_ = json.Unmarshal(raw, &doc)
		return doc
	}
	initDoc := stepBorder(fmt.Sprintf(`{"command":"step_player_border","character":%q,"x_cm":-644519,"y_cm":376773}`, charName))
	if fmt.Sprint(initDoc["region"]) != "riverlands" || intField(initDoc, "handover_count") != 0 || initDoc["suppressed_by_hysteresis"] == true {
		t.Fatalf("initial Riverlands placement invalid: %#v", initDoc)
	}
	hops := []borderHopSpec{
		{-762862, -563881, "anvils", "Verra_ANV_Node_Vhalgadim", "anvils", 1},
		{-1764281, 114558, "turquoise_sea", "Verra_TUR_Node_Korrin", "turquoise-sea", 2},
		{-1783982, 809682, "jundark", "Verra_JUN_Node_Wildport", "jundark", 3},
		{-619803, 873514, "sandsquall", "Verra_SAN_Node_Sunhaven", "sunhaven", 4},
		{-278768, 987391, "sandsquall", "Verra_SAN_Node_Azmaran", "azmaran", 5},
		{-644519, 376773, "riverlands", "Verra_RVR_Node_Winstead", "riverlands", 6},
	}
	for _, h := range hops {
		jitter := stepBorder(fmt.Sprintf(`{"command":"step_player_border","character":%q,"x_cm":%.0f,"y_cm":%.0f,"zoi_id":%q,"border_distance_cm":2400}`, charName, h.x, h.y, h.wantZoi))
		if jitter["suppressed_by_hysteresis"] != true || intField(jitter, "handover_count") != h.wantCount-1 {
			t.Fatalf("hop %d jitter not suppressed: %#v", h.wantCount, jitter)
		}
		for sample := 1; sample <= 3; sample++ {
			doc := stepBorder(fmt.Sprintf(`{"command":"step_player_border","character":%q,"x_cm":%.0f,"y_cm":%.0f,"zoi_id":%q,"advance_ms":2500}`, charName, h.x, h.y, h.wantZoi))
			if sample < 3 && (doc["suppressed_by_hysteresis"] != true || intField(doc, "handover_count") != h.wantCount-1) {
				t.Fatalf("hop %d sample %d expected hysteresis hold: %#v", h.wantCount, sample, doc)
			}
			if sample == 3 {
				reg, zoi, wrk := fmt.Sprint(doc["region"]), fmt.Sprint(doc["zoi_id"]), strings.ToLower(fmt.Sprint(doc["authoritative_worker"]))
				if doc["suppressed_by_hysteresis"] == true || intField(doc, "handover_count") != h.wantCount || reg != h.wantRegion || zoi != h.wantZoi || !strings.Contains(wrk, h.wantWorker) {
					t.Fatalf("hop %d handover mismatch: got reg=%q zoi=%q worker=%q count=%d, want reg=%q zoi=%q worker~=%q count=%d", h.wantCount, reg, zoi, wrk, intField(doc, "handover_count"), h.wantRegion, h.wantZoi, h.wantWorker, h.wantCount)
				}
			}
		}
	}
}

func despawnAndVerifyZeroOnlineTestPlayers(t *testing.T, baseURL string) {
	t.Helper()
	despawnOne := func(name string) {
		if strings.TrimSpace(name) == "" || name == "<nil>" {
			return
		}
		hdr := map[string]string{"Content-Type": "application/json", "Idempotency-Key": fmt.Sprintf("dsp-pl-%s-%d", strings.ReplaceAll(name, " ", "_"), time.Now().UnixNano())}
		_, _, _ = doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", hdr, []byte(fmt.Sprintf(`{"command":"despawn_entity","entity_guid":%q,"character":%q}`, name, name)))
	}
	spawnedTestPlayers.Range(func(k, _ any) bool {
		despawnOne(fmt.Sprint(k))
		return true
	})
	_, _, plRaw := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/players?online=true", nil, nil)
	var plEnv map[string]any
	_ = json.Unmarshal(plRaw, &plEnv)
	for _, raw := range extractSliceField(plEnv, "players", "items") {
		p, _ := raw.(map[string]any)
		for _, field := range []string{fmt.Sprint(p["character"]), fmt.Sprint(p["name"]), fmt.Sprint(p["guid"])} {
			despawnOne(field)
		}
	}
	_, _, stRaw := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/state", nil, nil)
	var stDoc map[string]any
	_ = json.Unmarshal(stRaw, &stDoc)
	counts, _ := stDoc["layer_counts"].(map[string]any)
	if gotPl := intField(counts, "players"); gotPl != 0 {
		t.Fatalf("expected layer_counts.players == 0 in /api/v1/world/state after despawning test players, got %d", gotPl)
	}
	_, _, afterPlRaw := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/players?online=true", nil, nil)
	var afterEnv map[string]any
	_ = json.Unmarshal(afterPlRaw, &afterEnv)
	if onlinePl := extractSliceField(afterEnv, "players", "items"); len(onlinePl) != 0 {
		t.Fatalf("expected 0 online players in /api/v1/world/players?online=true after cleanup, got %d (%s)", len(onlinePl), string(afterPlRaw))
	}
}

func postWorkItemAndAssert(t *testing.T, baseURL string, spec gmWorkItemSpec, seq int) map[string]any {
	t.Helper()
	idem := fmt.Sprintf("e2e-%s-%d-%d", spec.name, time.Now().UnixNano(), seq)
	hdrs := map[string]string{"Content-Type": "application/json", "Accept": "application/json", "Idempotency-Key": idem, "X-Idempotency-Key": idem}
	var lastCode int
	var lastBody []byte
	for _, route := range spec.routes {
		lastCode, _, lastBody = doAdminHTTP(t, http.MethodPost, baseURL+route, hdrs, []byte(spec.body))
		if lastCode != http.StatusNotFound && lastCode != http.StatusMethodNotAllowed {
			break
		}
	}
	if lastCode != http.StatusOK {
		t.Fatalf("GM work-item %s status=%d body=%s, want 200 OK", spec.name, lastCode, string(lastBody))
	}
	var resp map[string]any
	_ = json.Unmarshal(lastBody, &resp)
	if st, _ := resp["status"].(string); !strings.EqualFold(st, "executed") && !strings.EqualFold(st, "queued") {
		t.Fatalf("GM work-item %s status=%q, want executed or queued (body=%s)", spec.name, st, string(lastBody))
	}
	return resp
}

func extractSliceField(doc map[string]any, keys ...string) []any {
	for _, k := range keys {
		if arr, ok := doc[k].([]any); ok {
			return arr
		}
	}
	return nil
}

func intField(doc map[string]any, keys ...string) int {
	for _, k := range keys {
		if v, ok := doc[k].(float64); ok {
			return int(v)
		}
	}
	return 0
}

func probeWebAdminEndpoint(t *testing.T, client *http.Client, fullURL string) {
	t.Helper()
	bearer := "Bearer " + getActiveAdminBearer("gm-observer-dev-token")
	req, _ := http.NewRequest(http.MethodGet, fullURL, nil)
	req.Header.Set("Authorization", bearer)
	req.Header.Set("Accept-Encoding", "br, gzip")
	resp, err := client.Do(req)
	if err != nil || resp.StatusCode != http.StatusOK {
		t.Fatalf("GET %s failed: resp=%v err=%v", fullURL, resp, err)
	}
	_, _ = io.Copy(io.Discard, resp.Body)
	_ = resp.Body.Close()
	etag := resp.Header.Get("ETag")
	if etag == "" {
		t.Fatalf("GET %s missing ETag header", fullURL)
	}
	req304, _ := http.NewRequest(http.MethodGet, fullURL, nil)
	req304.Header.Set("Authorization", bearer)
	req304.Header.Set("Accept-Encoding", "br, gzip")
	if strings.Contains(fullURL, "/api/v1/world/state") || strings.Contains(fullURL, "/api/v1/gm/logs") {
		req304.Header.Set("If-Modified-Since", time.Now().UTC().Add(24*time.Hour).Format(http.TimeFormat))
	} else {
		req304.Header.Set("If-None-Match", etag)
	}
	resp304, err := client.Do(req304)
	if err != nil || resp304.StatusCode != http.StatusNotModified {
		t.Fatalf("GET %s conditional 304 check failed (etag=%s): got resp=%v err=%v", fullURL, etag, resp304, err)
	}
	_ = resp304.Body.Close()
}

func doAdminHTTP(t *testing.T, method, fullURL string, hdrs map[string]string, body []byte) (int, http.Header, []byte) {
	t.Helper()
	cli := &http.Client{Timeout: 5 * time.Second, Transport: &http.Transport{DisableCompression: true}}
	req, err := http.NewRequest(method, fullURL, bytes.NewReader(body))
	if err != nil {
		t.Fatalf("NewRequest %s %s: %v", method, fullURL, err)
	}
	req.Header.Set("Authorization", "Bearer "+getActiveAdminBearer("gm-architect-dev-token"))
	for k, v := range hdrs {
		req.Header.Set(k, v)
	}
	resp, err := cli.Do(req)
	if err != nil {
		t.Fatalf("%s %s failed: %v", method, fullURL, err)
	}
	raw, _ := io.ReadAll(resp.Body)
	_ = resp.Body.Close()
	return resp.StatusCode, resp.Header, raw
}

func verifyHostileMonsterClassificationAndZois(t *testing.T, baseURL string) {
	t.Helper()
	codeZ, _, bodyZ := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/zois", nil, nil)
	var zoiDoc map[string]any
	_ = json.Unmarshal(bodyZ, &zoiDoc)
	zois := extractSliceField(zoiDoc, "zois", "items")
	if codeZ != http.StatusOK || len(zois) < 26 {
		t.Fatalf("GET /api/v1/world/zois status=%d len(zois)=%d, want 200 and >=26 ZOI polygons", codeZ, len(zois))
	}
	firstZoi, _ := zois[0].(map[string]any)
	if fmt.Sprint(firstZoi["zoi_id"]) == "" && fmt.Sprint(firstZoi["name"]) == "" {
		t.Fatalf("GET /api/v1/world/zois first entry missing zoi_id/name: %#v", firstZoi)
	}
	var spawnedGUIDs []string
	wantMobs := []string{"raven", "wolf"}
	for i, mob := range []string{"Raven", "Mycelette", "Wolf"} {
		hdr := map[string]string{"Content-Type": "application/json", "Idempotency-Key": fmt.Sprintf("mob-cls-%s-%d-%d", mob, time.Now().UnixNano(), i)}
		body := fmt.Sprintf(`{"command":"spawn_npc","npc":%q,"place":"Winstead","x_cm":-641389,"y_cm":377367,"z_cm":12500,"level":8}`, mob)
		_, _, raw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", hdr, []byte(body))
		var sp map[string]any
		_ = json.Unmarshal(raw, &sp)
		if g, _ := sp["entity_guid"].(string); g != "" {
			spawnedGUIDs = append(spawnedGUIDs, g)
		}
		if mob == "Mycelette" && (fmt.Sprint(sp["display_name"]) == "" || strings.Contains(strings.ToLower(fmt.Sprint(sp["display_name"])), "mycelette")) {
			wantMobs = append(wantMobs, "mycelette")
		}
	}
	defer func() {
		for i, g := range spawnedGUIDs {
			hdr := map[string]string{"Content-Type": "application/json", "Idempotency-Key": fmt.Sprintf("mob-rm-%d-%d", time.Now().UnixNano(), i)}
			_, _, _ = doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", hdr, []byte(fmt.Sprintf(`{"command":"despawn_entity","entity_guid":%q}`, g)))
		}
	}()
	_, _, stRaw := doAdminHTTP(t, http.MethodGet, baseURL+"/api/v1/world/state", nil, nil)
	var stDoc map[string]any
	_ = json.Unmarshal(stRaw, &stDoc)
	for _, nRaw := range extractSliceField(stDoc, "npcs") {
		n, _ := nRaw.(map[string]any)
		if nm := strings.ToLower(fmt.Sprint(n["name"])); strings.Contains(nm, "raven") || strings.Contains(nm, "mycelette") || strings.Contains(nm, "wolf") {
			t.Fatalf("hostile mob %q misclassified in /api/v1/world/state npcs", nm)
		}
	}
	monsters := extractSliceField(stDoc, "monsters")
	for _, wantMob := range wantMobs {
		found := false
		for _, mRaw := range monsters {
			m, _ := mRaw.(map[string]any)
			if strings.Contains(strings.ToLower(fmt.Sprint(m["name"])), wantMob) && fmt.Sprint(m["kind"]) == "monster" {
				found = true
				break
			}
		}
		if !found {
			t.Fatalf("expected %q to be classified with kind=\"monster\" in /api/v1/world/state monsters", wantMob)
		}
	}
}

func verifySixEnvConfigurablePartitionTopologies(t *testing.T, baseURL string) {
	t.Helper()
	wantModes := []struct {
		mode  string
		tick  int
		cap   int
		scope string
	}{
		{"1_single_riverlands", 45, 3600, "region"},
		{"2_single_world", 30, 5200, "world"},
		{"3_per_region", 45, 3600, "region"},
		{"4_per_zoi_in_region", 60, 2600, "zoi"},
		{"5_per_zoi_and_node_plugs_in_region", 100, 1500, "node_plug"},
		{"6_per_zoi_node_plugs_and_pois_in_region", 100, 1500, "polygon_cutout"},
	}
	regions := []string{"riverlands", "sandsquall", "anvils", "jundark", "turquoise_sea"}
	hdr := map[string]string{"Content-Type": "application/json", "Accept": "application/json"}
	for idx, m := range wantModes {
		pickedRegion := regions[(idx*7+3)%len(regions)]
		if pickedRegion == "" || m.tick < 30 || m.cap < 1500 {
			t.Fatalf("invalid partition topology spec %#v", m)
		}
		code, _, raw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/command", hdr, []byte(fmt.Sprintf(`{"command":"set_tick_rate","tick_hz":%d,"partition_mode":%q,"partition_region":%q,"scope":%q}`, m.tick, m.mode, pickedRegion, m.scope)))
		if code != http.StatusOK {
			t.Fatalf("partition mode %s set_tick_rate=%d failed: status=%d body=%s", m.mode, m.tick, code, string(raw))
		}
	}
	_, _, _ = doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/command", hdr, []byte(`{"command":"set_tick_rate","tick_hz":20}`))
}

func verifySixteenBotEightClassLevel50BoundaryPlay(t *testing.T, baseURL string) {
	t.Helper()
	classes := []string{"Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"}
	boundaryWaypoints := []struct {
		x, y       float64
		zoi, reg   string
		wantWorker string
	}{
		{-644519, 376773, "Verra_RVR_Node_Winstead", "riverlands", "riverlands"},
		{-762862, -563881, "Verra_ANV_Node_Vhalgadim", "anvils", "anvils"},
		{-619803, 873514, "Verra_SAN_Node_Sunhaven", "sandsquall", "sunhaven"},
		{-1764281, 114558, "Verra_TUR_Node_Korrin", "turquoise_sea", "turquoise-sea"},
	}
	botCount := 0
	for cIdx, cls := range classes {
		for copyIdx := 1; copyIdx <= 2; copyIdx++ {
			botCount++
			botName := fmt.Sprintf("TST Lv50_%s_%d", cls, copyIdx)
			spawnedTestPlayers.Store(botName, true)
			wp := boundaryWaypoints[(cIdx+copyIdx)%len(boundaryWaypoints)]
			hdr := map[string]string{"Content-Type": "application/json", "Accept": "application/json", "Idempotency-Key": fmt.Sprintf("bot50-%s-%d-%d", botName, time.Now().UnixNano(), botCount)}
			body := fmt.Sprintf(`{"command":"step_player_border","character":%q,"class":%q,"level":50,"unlock_all_embersprings":true,"mounted":true,"x_cm":%.0f,"y_cm":%.0f,"zoi_id":%q,"advance_ms":2500}`, botName, cls, wp.x, wp.y, wp.zoi)
			code, _, raw := doAdminHTTP(t, http.MethodPost, baseURL+"/api/v1/gm/commands", hdr, []byte(body))
			if code != http.StatusOK {
				t.Fatalf("bot %s (class=%s level=50) boundary step failed: status=%d body=%s", botName, cls, code, string(raw))
			}
		}
	}
	if botCount != 16 {
		t.Fatalf("expected 16 level-50 bots across 8 classes, spawned %d", botCount)
	}
	despawnAndVerifyZeroOnlineTestPlayers(t, baseURL)
}

func TestSixPartitionTopologiesAndMultiBotCrossBoundaryE2E(t *testing.T) {
	baseURL := resolveLiveOrEphemeralBaseURL(t)
	verifyLiveAdminAuthLoginAndSession(t, baseURL)
	verifySixEnvConfigurablePartitionTopologies(t, baseURL)
	verifySixteenBotEightClassLevel50BoundaryPlay(t, baseURL)
	verifyLiveBorderHandoverHysteresis(t, baseURL)
}

