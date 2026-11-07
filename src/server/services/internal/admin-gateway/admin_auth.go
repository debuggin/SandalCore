// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"bytes"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"reflect"
	"strconv"
	"strings"
	"time"

	admincatalog "aocrop/services/internal/admin-catalog"
	worldstream "aocrop/services/internal/world-stream"
)

const (
	MinDynamicWorkersPerScope = 1
	MaxDynamicWorkersPerScope = 26
)

var NatoPhoneticAlphabet = [MaxDynamicWorkersPerScope]string{
	"alpha", "beta", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india",
	"juliett", "kilo", "lima", "mike", "november", "oscar", "papa", "quebec", "romeo",
	"sierra", "tango", "uniform", "victor", "whiskey", "xray", "yankee", "zulu",
}

var AllAdminScopes = []string{
	"admin:read", "admin:write", "gm:commands", "gm:spawn", "gm:teleport",
	"gm:node_governance", "gm:sql_hotfix", "metrics:read", "containers:manage",
}

type AdminAuthConfig struct {
	Username, Passphrase, BootID       string
	PassphraseEnvSet, CustomRoleTokens bool
	JWTSecret                          []byte
}

type AdminJWTClaims struct {
	Subject  string   `json:"sub"`
	Username string   `json:"username"`
	Role     Role     `json:"role"`
	Scopes   []string `json:"scopes"`
	BootID   string   `json:"boot_id"`
	Iat      int64    `json:"iat"`
	Exp      int64    `json:"exp"`
}

type WorldWorkerStatus struct {
	WorldWorkerEndpoint
	WorkerIndex        int          `json:"worker_index"`
	CanonicalName      string       `json:"canonical_name"`
	TopologyMode       string       `json:"topology_mode,omitempty"`
	PolygonCutout      [][2]float64 `json:"polygon_cutout,omitempty"`
	DynamicScalingStub bool         `json:"dynamic_scaling_stub"`
}

type workerTopologyMeta struct {
	WorkerIndex                 int
	CanonicalName, TopologyMode string
	PolygonCutout               [][2]float64
	DynamicScalingStub          bool
	ScrapedTickHz               float64
	SyncedTimeAt                time.Time
}

func (s *Server) TimeOfDayAnchor() (float64, int64) {
	s.timeOfDayMu.RLock()
	defer s.timeOfDayMu.RUnlock()
	if s.timeOfDayUpdatedAt.IsZero() && s.timeOfDayHours == 0 {
		return 12.0, s.startedAt.UnixMilli()
	}
	return s.timeOfDayHours, s.timeOfDayUpdatedAt.UnixMilli()
}

func (s *Server) TimeOfDayHoursAt(at time.Time) float64 {
	s.timeOfDayMu.RLock()
	base, updatedAt := s.timeOfDayHours, s.timeOfDayUpdatedAt
	s.timeOfDayMu.RUnlock()
	if updatedAt.IsZero() && base == 0 {
		base, updatedAt = 12.0, s.startedAt
	}
	return wrapHours24(base + max(at.Sub(updatedAt).Seconds(), 0)*(9.0/3600.0))
}

func (s *Server) TimeOfDayHours() float64 { return s.TimeOfDayHoursAt(s.now()) }

func formatTimeOfDayLabel(hours float64) string {
	mins := int(math.Round(wrapHours24(hours)*60.0)) % (24 * 60)
	return fmt.Sprintf("%02d:%02d", mins/60, mins%60)
}

func wrapHours24(h float64) float64 {
	if h = math.Mod(h, 24.0); h < 0 {
		h += 24.0
	}
	return math.Round(h*10000.0) / 10000.0
}

func parseClockOrFloatHours(raw string) (float64, bool) {
	str := strings.TrimSpace(raw)
	if str == "" {
		return 0, false
	}
	if parts := strings.Split(str, ":"); len(parts) == 2 || len(parts) == 3 {
		hh, errH := strconv.ParseFloat(strings.TrimSpace(parts[0]), 64)
		mm, errM := strconv.ParseFloat(strings.TrimSpace(parts[1]), 64)
		var ss float64
		if len(parts) == 3 {
			ss, _ = strconv.ParseFloat(strings.TrimSpace(parts[2]), 64)
		}
		if errH == nil && errM == nil && !math.IsNaN(hh) && !math.IsNaN(mm) {
			return wrapHours24(hh + mm/60.0 + ss/3600.0), true
		}
	}
	if f, err := strconv.ParseFloat(str, 64); err == nil && !math.IsNaN(f) && !math.IsInf(f, 0) {
		return wrapHours24(f), true
	}
	return 0, false
}

func randomHexToken(numBytes int) string {
	buf := make([]byte, numBytes)
	if _, err := rand.Read(buf); err != nil {
		return fmt.Sprintf("%d", time.Now().UnixNano())
	}
	return hex.EncodeToString(buf)
}

func (s *Server) initAdminAuth(customRoleTokens bool) {
	passEnv := strings.TrimSpace(os.Getenv("AOC_ADMIN_PASSPHRASE"))
	s.authCfg = AdminAuthConfig{
		Username: firstNonEmpty(os.Getenv("AOC_ADMIN_USERNAME"), "admin"), Passphrase: firstNonEmpty(passEnv, "sandal-admin-dev-passphrase"),
		PassphraseEnvSet: passEnv != "", JWTSecret: []byte(firstNonEmpty(os.Getenv("AOC_ADMIN_JWT_SECRET"), "ephemeral-secret-"+randomHexToken(16))),
		BootID: firstNonEmpty(os.Getenv("AOC_ADMIN_BOOT_ID"), "boot-"+randomHexToken(8)), CustomRoleTokens: customRoleTokens,
	}
	s.timeOfDayHours, s.timeOfDayUpdatedAt = 12.0, s.startedAt
	if h, ok := parseClockOrFloatHours(os.Getenv("AOC_TIME_OF_DAY_HOURS")); ok {
		s.timeOfDayHours = h
	}
	if s.workerMeta == nil {
		s.workerMeta = make(map[string]workerTopologyMeta)
	}
}

func scopesForRole(role Role) []string {
	switch role {
	case RoleArchitect:
		return append([]string(nil), AllAdminScopes...)
	case RoleOperator:
		return []string{"admin:read", "metrics:read", "admin:write", "gm:commands", "gm:spawn", "gm:teleport", "gm:node_governance", "containers:manage"}
	default:
		return []string{"admin:read", "metrics:read"}
	}
}

func parseRoleString(raw string) Role {
	switch strings.ToUpper(strings.TrimSpace(raw)) {
	case "GM_OBSERVER", "OBSERVER", "READONLY":
		return RoleObserver
	case "GM_OPERATOR", "OPERATOR":
		return RoleOperator
	default:
		return RoleArchitect
	}
}

func constantTimeEqualStrings(a, b string) bool {
	ha, hb := sha256.Sum256([]byte(a)), sha256.Sum256([]byte(b))
	return len(a) == len(b) && subtle.ConstantTimeCompare(ha[:], hb[:]) == 1
}

func (s *Server) mintAdminJWT(claims AdminJWTClaims) (string, error) {
	payloadJSON, err := json.Marshal(claims)
	if err != nil {
		return "", err
	}
	signingInput := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`)) + "." + base64.RawURLEncoding.EncodeToString(payloadJSON)
	mac := hmac.New(sha256.New, s.authCfg.JWTSecret)
	_, _ = mac.Write([]byte(signingInput))
	return signingInput + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil)), nil
}

func (s *Server) verifyAdminJWT(token string) (AdminJWTClaims, error) {
	parts := strings.Split(strings.TrimSpace(token), ".")
	if len(parts) != 3 {
		return AdminJWTClaims{}, errors.New("malformed jwt")
	}
	mac := hmac.New(sha256.New, s.authCfg.JWTSecret)
	_, _ = mac.Write([]byte(parts[0] + "." + parts[1]))
	gotSig, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil || subtle.ConstantTimeCompare(gotSig, mac.Sum(nil)) != 1 {
		return AdminJWTClaims{}, errors.New("invalid jwt signature")
	}
	payloadBytes, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return AdminJWTClaims{}, errors.New("invalid jwt payload")
	}
	var claims AdminJWTClaims
	if err := json.Unmarshal(payloadBytes, &claims); err != nil {
		return AdminJWTClaims{}, err
	}
	if claims.Exp > 0 && s.now().Unix() > claims.Exp {
		return AdminJWTClaims{}, errors.New("jwt expired")
	}
	if claims.BootID == "" || !constantTimeEqualStrings(claims.BootID, s.authCfg.BootID) {
		return AdminJWTClaims{}, errors.New("boot_id mismatch")
	}
	return claims, nil
}

func (s *Server) handleAdminLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		s.methodNotAllowed("POST")(w, r)
		return
	}
	var raw map[string]any
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10)).Decode(&raw); err != nil {
		writeAuthProblem(w, r, http.StatusBadRequest, "invalid-json-body", "invalid_request", "Malformed login JSON payload.")
		return
	}
	username, passphrase := firstNonEmpty(extractStringParam(raw, "username", "user"), s.authCfg.Username), extractStringParam(raw, "passphrase", "password", "secret")
	if !constantTimeEqualStrings(username, s.authCfg.Username) || passphrase == "" || !constantTimeEqualStrings(passphrase, s.authCfg.Passphrase) {
		w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway", error="invalid_credentials"`)
		writeAuthProblem(w, r, http.StatusUnauthorized, "invalid-credentials", "invalid_credentials", "Invalid admin username or startup passphrase.")
		return
	}
	role := parseRoleString(extractStringParam(raw, "requested_role", "role"))
	scopes, now := extractRequestedScopes(raw, role), s.now()
	exp := now.Add(12 * time.Hour)
	jwtTok, err := s.mintAdminJWT(AdminJWTClaims{Subject: s.authCfg.Username, Username: s.authCfg.Username, Role: role, Scopes: scopes, BootID: s.authCfg.BootID, Iat: now.Unix(), Exp: exp.Unix()})
	if err != nil {
		writeAuthProblem(w, r, http.StatusInternalServerError, "token-mint-failed", "internal_error", "Failed to sign admin JWT.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"token": jwtTok, "access_token": jwtTok, "token_type": "Bearer", "username": s.authCfg.Username,
		"role": role, "scopes": scopes, "boot_id": s.authCfg.BootID, "expires_at": exp.Unix(), "expires_at_iso": exp.UTC().Format(time.RFC3339),
	})
}

func extractRequestedScopes(raw map[string]any, role Role) []string {
	allowed := scopesForRole(role)
	rawScopes, ok := raw["scopes"].([]any)
	if !ok || len(rawScopes) == 0 {
		return allowed
	}
	allowSet := make(map[string]bool, len(allowed))
	for _, sc := range allowed {
		allowSet[sc] = true
	}
	var filtered []string
	for _, item := range rawScopes {
		if sc, ok := item.(string); ok && allowSet[strings.TrimSpace(sc)] {
			filtered = append(filtered, strings.TrimSpace(sc))
		}
	}
	if len(filtered) == 0 {
		return []string{"admin:read"}
	}
	return filtered
}

func (s *Server) handleAdminSession(w http.ResponseWriter, r *http.Request) {
	tok, ok := extractAdminToken(r)
	if !ok {
		w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway"`)
		writeAuthProblem(w, r, http.StatusUnauthorized, "unauthorized", "token_missing_or_boot_id_mismatch", "Missing admin session Bearer token.")
		return
	}
	claims, err := s.verifyAdminJWT(tok)
	if err != nil {
		w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway", error="invalid_token"`)
		writeAuthProblem(w, r, http.StatusUnauthorized, "invalid-token", "token_missing_or_boot_id_mismatch", "Session token is invalid, expired, or from a previous server boot.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"authenticated": true, "username": firstNonEmpty(claims.Username, claims.Subject),
		"role": claims.Role, "scopes": claims.Scopes, "boot_id": s.authCfg.BootID, "expires_at": claims.Exp,
	})
}

func extractAdminToken(r *http.Request) (string, bool) {
	if tok, ok := extractBearerToken(r); ok {
		return tok, true
	}
	if hdr := strings.TrimSpace(r.Header.Get("X-Admin-Token")); hdr != "" {
		return hdr, true
	}
	if r.URL != nil {
		if tok := firstNonEmpty(r.URL.Query().Get("access_token"), r.URL.Query().Get("token")); tok != "" {
			return tok, true
		}
	}
	return "", false
}

func requiredScopeForPath(r *http.Request) string {
	if r == nil || r.URL == nil {
		return "admin:read"
	}
	p := r.URL.Path
	switch {
	case strings.Contains(p, "/hotfix/") || strings.HasSuffix(p, "/audit/rollback"):
		return "gm:sql_hotfix"
	case strings.Contains(p, "/nodes/") || strings.Contains(p, "/node/"):
		return "gm:node_governance"
	case strings.HasSuffix(p, "/spawn_npc") || strings.HasSuffix(p, "/spawn"):
		return "gm:spawn"
	case strings.HasSuffix(p, "/teleport"):
		return "gm:teleport"
	case strings.HasSuffix(p, "/containers") || strings.HasSuffix(p, "/logs") || strings.HasSuffix(p, "/metrics"):
		return "metrics:read"
	case r.Method == http.MethodPost || r.Method == http.MethodPut || r.Method == http.MethodDelete:
		return "gm:commands"
	default:
		return "admin:read"
	}
}

func requiredScopeForCommand(cmd string) string {
	switch cmd {
	case "spawn_npc", "spawn", "kill_npc", "kill", "despawn_npc", "despawn", "despawn_entity", "heal_npc", "reset_npc", "respawn_npc", "move_npc":
		return "gm:spawn"
	case "teleport_player", "teleport", "step_player_border", "simulate_border_crossing":
		return "gm:teleport"
	case "set_node_level", "set_node_stage", "set_node_mayor", "trigger_node_election", "set_node_election", "add_node_xp", "advance_node_clock":
		return "gm:node_governance"
	case "set_time_of_day", "time_of_day", "set_world_time":
		return "gm:commands"
	default:
		return "gm:commands"
	}
}

func hasScope(scopes []string, required string) bool {
	if required == "" {
		return true
	}
	for _, sc := range scopes {
		if sc == required || sc == "*" || (required == "metrics:read" && sc == "admin:read") {
			return true
		}
	}
	return false
}

func (s *Server) authenticateRequest(w http.ResponseWriter, r *http.Request, minRole Role, reqScope string, tokenRequired bool) (Role, bool) {
	tok, hasTok := extractAdminToken(r)
	if !hasTok {
		if tokenRequired || s.authCfg.PassphraseEnvSet {
			w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway"`)
			writeAuthProblem(w, r, http.StatusUnauthorized, "unauthorized", "token_missing_or_boot_id_mismatch", "Missing or malformed Bearer token.")
			return "", false
		}
		return minRole, true
	}
	if strings.Count(tok, ".") == 2 {
		claims, err := s.verifyAdminJWT(tok)
		if err != nil {
			w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway", error="invalid_token"`)
			writeAuthProblem(w, r, http.StatusUnauthorized, "invalid-token", "token_missing_or_boot_id_mismatch", "JWT token is invalid, expired, or boot_id mismatched.")
			return "", false
		}
		if !claims.Role.AtLeast(minRole) || !hasScope(claims.Scopes, reqScope) {
			writeAuthProblem(w, r, http.StatusForbidden, "insufficient-scope", "insufficient_scope", fmt.Sprintf("Token lacks required role %s or scope %s.", minRole, reqScope))
			return "", false
		}
		return claims.Role, true
	}
	if s.authCfg.PassphraseEnvSet && !s.authCfg.CustomRoleTokens {
		w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway", error="invalid_token"`)
		writeAuthProblem(w, r, http.StatusUnauthorized, "invalid-token", "token_missing_or_boot_id_mismatch", "Startup passphrase mode requires a valid HS256 JWT.")
		return "", false
	}
	role, valid := s.resolveRole(tok)
	if !valid {
		w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway", error="invalid_token"`)
		writeAuthProblem(w, r, http.StatusUnauthorized, "invalid-token", "token_missing_or_boot_id_mismatch", "Provided Bearer token is not recognized.")
		return "", false
	}
	if !role.AtLeast(minRole) {
		writeAuthProblem(w, r, http.StatusForbidden, "insufficient-role", "insufficient_scope", fmt.Sprintf("Role %s does not satisfy required minimum role %s.", role, minRole))
		return "", false
	}
	return role, true
}

func (s *Server) guardGMCommandsAuth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		tok, hasTok := extractAdminToken(r)
		if !hasTok {
			if s.authCfg.PassphraseEnvSet {
				w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway"`)
				writeAuthProblem(w, r, http.StatusUnauthorized, "unauthorized", "token_missing_or_boot_id_mismatch", "Missing or malformed Bearer token.")
				return
			}
			next(w, r)
			return
		}
		r.Header.Set("Authorization", "Bearer "+tok)
		if strings.Count(tok, ".") == 2 {
			claims, err := s.verifyAdminJWT(tok)
			if err != nil {
				w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway", error="invalid_token"`)
				writeAuthProblem(w, r, http.StatusUnauthorized, "invalid-token", "token_missing_or_boot_id_mismatch", "JWT token is invalid, expired, or boot_id mismatched.")
				return
			}
			rawBody, _ := io.ReadAll(http.MaxBytesReader(w, r.Body, 16<<10))
			r.Body = io.NopCloser(bytes.NewReader(rawBody))
			var rawMap map[string]any
			_ = json.Unmarshal(rawBody, &rawMap)
			cmd := strings.ToLower(firstNonEmpty(extractStringParam(rawMap, "command", "action", "type"), inferCommandFromPath(r.URL.Path)))
			reqScope, minRole := requiredScopeForCommand(cmd), requiredRoleForCommand(cmd)
			if !claims.Role.AtLeast(minRole) || !hasScope(claims.Scopes, reqScope) {
				writeAuthProblem(w, r, http.StatusForbidden, "insufficient-scope", "insufficient_scope", fmt.Sprintf("Token lacks required role %s or scope %s.", minRole, reqScope))
				return
			}
		} else if s.authCfg.PassphraseEnvSet && !s.authCfg.CustomRoleTokens {
			w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway", error="invalid_token"`)
			writeAuthProblem(w, r, http.StatusUnauthorized, "invalid-token", "token_missing_or_boot_id_mismatch", "Startup passphrase mode requires a valid HS256 JWT.")
			return
		}
		next(w, r)
	}
}

func writeAuthProblem(w http.ResponseWriter, r *http.Request, status int, code, reason, detail string) {
	corrID, instance := w.Header().Get("X-Correlation-ID"), ""
	if r != nil && r.URL != nil {
		instance = r.URL.Path
	}
	w.Header().Set("Content-Type", "application/problem+json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(ProblemDetails{
		Type: "https://sandalcore.local/problems/" + code, Title: http.StatusText(status),
		Status: status, Detail: detail, Instance: instance, Code: code, Reason: reason, CorrelationID: corrID,
	})
}

func canonicalWorkerName(w *WorldWorkerEndpoint) string {
	return canonicalWorkerNameWithOrdinal(w, 0)
}

func canonicalWorkerNameWithOrdinal(w *WorldWorkerEndpoint, dynOrdinal int) string {
	kind := strings.ToLower(strings.TrimSpace(w.ScopeKind))
	keySlug := strings.ReplaceAll(strings.ToLower(strings.TrimSpace(w.ScopeKey)), "_", "-")
	baseSlug := firstNonEmpty(keySlug, strings.TrimPrefix(strings.ToLower(w.WorkerID), "world-"))
	var base string
	switch kind {
	case "world":
		return "aoc-world"
	case "node_plug", "city_node", "plug", "dynamic_node_plug":
		base = "aoc-" + firstNonEmpty(stripVerraPrefix(baseSlug), baseSlug)
	case "zoi", "dynamic_zoi", "node_plug_zoi", "dynamic_node_plug_zoi":
		if strings.Contains(kind, "node_plug") && !strings.Contains(baseSlug, "node") {
			base = "aoc-" + firstNonEmpty(stripVerraPrefix(baseSlug), baseSlug)
		} else {
			base = "aoc-" + baseSlug
		}
	case "polygon_cutout":
		return "aoc-poly-" + baseSlug
	default:
		return "aoc-" + firstNonEmpty(baseSlug, "riverlands")
	}
	if isDynamicStubMode(kind) {
		for _, nato := range NatoPhoneticAlphabet {
			if strings.HasSuffix(base, "-"+nato) {
				return base
			}
		}
		ord := min(max(dynOrdinal, 0), MaxDynamicWorkersPerScope-1)
		return base + "-" + NatoPhoneticAlphabet[ord]
	}
	return base
}

func stripVerraPrefix(slug string) string {
	for _, p := range []string{"verra-rvr-node-", "verra-san-node-", "verra-snd-node-", "verra-jun-node-", "verra-anv-node-", "verra-tur-node-", "verra-rvr-", "verra-san-", "verra-snd-", "verra-jun-", "verra-anv-", "verra-tur-"} {
		if strings.HasPrefix(slug, p) {
			return strings.TrimPrefix(slug, p)
		}
	}
	return slug
}

func defaultWorkerTierTickHz(scopeKind string) float64 {
	switch strings.ToLower(strings.TrimSpace(scopeKind)) {
	case "node", "node_plug", "city_node", "plug", "node_plug_zoi", "dynamic_node_plug_zoi", "dynamic_node_plug", "polygon_cutout":
		return tierTickHzEnv("AOC_WORLD_TICK_HZ_NODE", 100.0)
	case "poi":
		return tierTickHzEnv("AOC_WORLD_TICK_HZ_POI", 100.0)
	case "zoi", "dynamic_zoi":
		return tierTickHzEnv("AOC_WORLD_TICK_HZ_ZOI", 60.0)
	case "world":
		return tierTickHzEnv("AOC_WORLD_TICK_HZ_WORLD", 30.0)
	default:
		return tierTickHzEnv("AOC_WORLD_TICK_HZ_REGION", 45.0)
	}
}

func isDynamicStubMode(scopeKind string) bool {
	k := strings.ToLower(strings.TrimSpace(scopeKind))
	return strings.HasPrefix(k, "dynamic_") || strings.HasPrefix(strings.ToLower(strings.TrimSpace(os.Getenv("AOC_WORLD_SHARD_MODE"))), "dynamic_")
}

func parsePolygonCutoutSpec(raw string) [][2]float64 {
	var out [][2]float64
	for _, pair := range strings.FieldsFunc(strings.TrimSpace(raw), func(r rune) bool { return r == ';' || r == ' ' }) {
		if coords := strings.Split(strings.TrimSpace(pair), ":"); len(coords) == 2 {
			x, errX := strconv.ParseFloat(strings.TrimSpace(coords[0]), 64)
			y, errY := strconv.ParseFloat(strings.TrimSpace(coords[1]), 64)
			if errX == nil && errY == nil {
				out = append(out, [2]float64{x, y})
			}
		}
	}
	return out
}

func pointInConvexPolygon(x, y float64, poly [][2]float64) bool {
	if len(poly) < 3 {
		return false
	}
	var pos, neg bool
	for i := 0; i < len(poly); i++ {
		a, b := poly[i], poly[(i+1)%len(poly)]
		cross := (b[0]-a[0])*(y-a[1]) - (b[1]-a[1])*(x-a[0])
		if cross > 1e-6 {
			pos = true
		} else if cross < -1e-6 {
			neg = true
		}
		if pos && neg {
			return false
		}
	}
	return true
}

func (s *Server) snapshotWorldWorkerStatuses() []WorldWorkerStatus {
	s.mu.Lock()
	defer s.mu.Unlock()
	out, dynCounts := make([]WorldWorkerStatus, len(s.worldWorkers)), make(map[string]int)
	for i, w := range s.worldWorkers {
		meta, ep := s.workerMeta[w.WorkerID], *w
		if ep.TickHz <= 0 {
			ep.TickHz = defaultWorkerTierTickHz(ep.ScopeKind)
			if meta.ScrapedTickHz > 0 {
				ep.TickHz = meta.ScrapedTickHz
			}
		}
		if ep.CPUPct <= 0 {
			ep.CPUPct = computeWorkerAdaptiveCPUPct(ep.TickP50Ms, ep.ScopeKind)
		}
		scopeKey := strings.ToLower(strings.TrimSpace(ep.ScopeKey))
		ord := dynCounts[scopeKey]
		if isDynamicStubMode(ep.ScopeKind) {
			dynCounts[scopeKey]++
		}
		out[i] = WorldWorkerStatus{
			WorldWorkerEndpoint: ep, WorkerIndex: i, CanonicalName: firstNonEmpty(meta.CanonicalName, canonicalWorkerNameWithOrdinal(w, ord)),
			TopologyMode: firstNonEmpty(meta.TopologyMode, ep.ScopeKind), PolygonCutout: meta.PolygonCutout, DynamicScalingStub: meta.DynamicScalingStub || isDynamicStubMode(ep.ScopeKind),
		}
	}
	return s.appendSubPartitionWorkerStatusesLocked(out)
}

func (s *Server) handleRegisterMeshWorker(w http.ResponseWriter, r *http.Request, _ Role) {
	var raw map[string]any
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64<<10)).Decode(&raw); err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-json-body", "Malformed worker registration JSON.")
		return
	}
	wid := firstNonEmpty(extractStringParam(raw, "worker_id", "id", "name"), fmt.Sprintf("world-%d", len(s.worldWorkers)))
	scopeKey, scopeKind := firstNonEmpty(extractStringParam(raw, "scope_key", "region", "zoi_id", "city"), strings.TrimPrefix(wid, "world-")), firstNonEmpty(extractStringParam(raw, "scope_kind", "topology_mode", "mode"), "region")
	ep := &WorldWorkerEndpoint{
		WorkerID: wid, ScopeKey: scopeKey, ScopeKind: scopeKind, ContainerName: extractStringParam(raw, "container_name", "container"),
		AdminAddr: extractStringParam(raw, "admin_addr", "grpc_addr"), StreamAddr: extractStringParam(raw, "stream_addr"),
		Status: "ONLINE", Connected: true, TickHz: defaultWorkerTierTickHz(scopeKind), LastSeenMs: s.now().UnixMilli(),
	}
	idx, err := s.upsertRegisteredWorker(ep, extractPolygonFromMap(raw))
	if err != nil {
		writeProblem(w, r, http.StatusBadRequest, "worker-limit-exceeded", err.Error())
		return
	}
	statuses := s.snapshotWorldWorkerStatuses()
	writeJSON(w, http.StatusOK, map[string]any{"status": "registered", "worker_index": idx, "worker": statuses[idx], "workers": statuses})
}

func extractPolygonFromMap(raw map[string]any) [][2]float64 {
	if spec := extractStringParam(raw, "polygon_spec"); spec != "" {
		return parsePolygonCutoutSpec(spec)
	}
	arr, _ := raw["polygon_cutout"].([]any)
	var out [][2]float64
	for _, ptRaw := range arr {
		if pair, ok := ptRaw.([]any); ok && len(pair) >= 2 {
			x, _ := strconv.ParseFloat(fmt.Sprint(pair[0]), 64)
			y, _ := strconv.ParseFloat(fmt.Sprint(pair[1]), 64)
			out = append(out, [2]float64{x, y})
		}
	}
	return out
}

func (s *Server) upsertRegisteredWorker(ep *WorldWorkerEndpoint, poly [][2]float64) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.workerMeta == nil {
		s.workerMeta = make(map[string]workerTopologyMeta)
	}
	idx, dynOrd := -1, 0
	for i, existing := range s.worldWorkers {
		if existing.WorkerID == ep.WorkerID {
			idx = i
			break
		}
		if isDynamicStubMode(ep.ScopeKind) && strings.EqualFold(existing.ScopeKey, ep.ScopeKey) {
			dynOrd++
		}
	}
	if isDynamicStubMode(ep.ScopeKind) && dynOrd >= MaxDynamicWorkersPerScope {
		return -1, fmt.Errorf("dynamic worker limit (%d NATO phonetic suffixes) exceeded for scope %s", MaxDynamicWorkersPerScope, ep.ScopeKey)
	}
	cname := canonicalWorkerNameWithOrdinal(ep, dynOrd)
	if ep.ContainerName == "" {
		ep.ContainerName = cname
	}
	if idx >= 0 {
		s.worldWorkers[idx] = ep
	} else {
		idx, s.worldWorkers = len(s.worldWorkers), append(s.worldWorkers, ep)
	}
	s.workerMeta[ep.WorkerID] = workerTopologyMeta{WorkerIndex: idx, CanonicalName: cname, TopologyMode: ep.ScopeKind, PolygonCutout: poly, DynamicScalingStub: isDynamicStubMode(ep.ScopeKind)}
	return idx, nil
}

func (s *Server) resolveDataModes() map[string]string {
	defMode := firstNonEmpty(os.Getenv("AOC_DATA_MODE"), "extracted")
	modes := map[string]string{
		"zois": firstNonEmpty(os.Getenv("AOC_DATA_MODE_ZOIS"), defMode), "city_nodes": firstNonEmpty(os.Getenv("AOC_DATA_MODE_CITY_NODES"), defMode),
		"pois": firstNonEmpty(os.Getenv("AOC_DATA_MODE_POIS"), defMode), "npcs": firstNonEmpty(os.Getenv("AOC_DATA_MODE_NPCS"), defMode), "items": firstNonEmpty(os.Getenv("AOC_DATA_MODE_ITEMS"), defMode),
	}
	if s.catalog != nil {
		if m := reflect.ValueOf(s.catalog).MethodByName("DataModes"); m.IsValid() && m.Type().NumIn() == 0 && m.Type().NumOut() == 1 {
			if cm, ok := m.Call(nil)[0].Interface().(map[string]string); ok {
				for k, v := range cm {
					if strings.TrimSpace(os.Getenv("AOC_DATA_MODE_"+strings.ToUpper(k))) == "" && v != "" {
						modes[k] = v
					}
				}
			}
		}
	}
	return modes
}

func (s *Server) handleWorldZois(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	zois, modes := s.collectCatalogZois(), s.resolveDataModes()
	writeCachedJSONWithModTime(w, r, "private, max-age=5", s.lastMutatedTime(), map[string]any{
		"schema_version": "1.0", "count": len(zois), "zois": zois, "items": zois,
		"data_mode": modes["zois"], "data_modes": modes, "service_tick_hz": worldstream.ServiceTickHz(), "serverTimeMs": s.now().UnixMilli(),
	})
}

func (s *Server) collectCatalogZois() []any {
	if s.catalog != nil {
		rv := reflect.ValueOf(s.catalog)
		for _, methodName := range []string{"AllZois", "Zois", "ListZois"} {
			if m := rv.MethodByName(methodName); m.IsValid() && m.Type().NumIn() == 0 && m.Type().NumOut() == 1 {
				if res := m.Call(nil)[0]; res.Kind() == reflect.Slice && res.Len() > 0 {
					out := make([]any, res.Len())
					for i := 0; i < res.Len(); i++ {
						out[i] = res.Index(i).Interface()
					}
					return out
				}
			}
		}
	}
	nodes := admincatalog.DefaultLevelableNodes()
	out := make([]any, 0, len(nodes))
	for _, n := range nodes {
		reg, zoiName := inferRegionFromNodeName(n.Name), n.Name
		for _, code := range []string{"_RVR_", "_SAN_", "_JUN_", "_ANV_", "_TUR_"} {
			zoiName = strings.Replace(zoiName, code, code+"Node_", 1)
		}
		half := float64(max(n.SizeCm, 100000))
		out = append(out, map[string]any{
			"zoi_id": zoiName, "name": zoiName, "display_name": n.DisplayName,
			"node_name": n.Name, "node_guid": strconv.FormatInt(n.GUID, 10), "region": reg,
			"canonical_worker": "aoc-" + strings.ReplaceAll(strings.ToLower(zoiName), "_", "-"),
			"center_x_cm": n.Ox, "center_y_cm": n.Oy, "center_z_cm": n.Oz,
			"min_x_cm": n.Ox - half, "min_y_cm": n.Oy - half, "max_x_cm": n.Ox + half, "max_y_cm": n.Oy + half,
		})
	}
	return out
}

func inferZoiForCoords(region string, x, y float64, p map[string]any) string {
	if explicit := extractStringParam(p, "zoi_id", "target_zoi", "zoi"); explicit != "" {
		return explicit
	}
	if strings.EqualFold(region, "sandsquall") {
		if math.Hypot(x-(-619803.28), y-873514.22) < 180000.0 || (x >= -600000.0 && y < 560000.0 && x < -250000.0) {
			return "Verra_SAN_Node_Sunhaven"
		}
		if y >= 560000.0 || x >= -250000.0 {
			return "Verra_SAN_Node_Azmaran"
		}
		return "Verra_SAN_Node_Sunhaven"
	}
	bestDist, bestName := math.MaxFloat64, "Verra_RVR_Node_Winstead"
	for _, n := range admincatalog.DefaultLevelableNodes() {
		if inferRegionFromNodeName(n.Name) == region {
			if d := math.Hypot(x-n.Ox, y-n.Oy); d < bestDist {
				bestDist, bestName = d, n.Name
			}
		}
	}
	for _, code := range []string{"_RVR_", "_SAN_", "_JUN_", "_ANV_", "_TUR_"} {
		if strings.Contains(bestName, code) {
			return strings.Replace(bestName, code, code+"Node_", 1)
		}
	}
	return bestName
}

func (s *Server) resolveAuthoritativeWorkerForTarget(st *playerRegionalHandoverState, targetReg, targetZoi string, pt [2]float64, p map[string]any) string {
	x, y := pt[0], pt[1]
	prevReg, prevZoi := st.AuthoritativeRegion, st.CurrentZoiID
	if expW := extractStringParam(p, "target_worker", "authoritative_worker"); expW != "" {
		return expW
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, w := range s.worldWorkers {
		if meta := s.workerMeta[w.WorkerID]; len(meta.PolygonCutout) >= 3 && pointInConvexPolygon(x, y, meta.PolygonCutout) {
			return firstNonEmpty(w.ContainerName, w.WorkerID)
		}
	}
	zoiLow := strings.ToLower(targetZoi)
	for _, w := range s.worldWorkers {
		sk := strings.ToLower(w.ScopeKey)
		if (w.ScopeKind == "zoi" || w.ScopeKind == "dynamic_zoi" || w.ScopeKind == "node_plug_zoi" || w.ScopeKind == "dynamic_node_plug_zoi") &&
			(sk == zoiLow || (strings.Contains(zoiLow, "sunhaven") && strings.Contains(sk, "sunhaven")) || (strings.Contains(zoiLow, "azmaran") && strings.Contains(sk, "azmaran"))) {
			return firstNonEmpty(w.ContainerName, w.WorkerID)
		}
	}
	if targetReg == "sandsquall" && ((prevReg == "sandsquall" && prevZoi != "" && prevZoi != targetZoi) || extractStringParam(p, "zoi_id", "target_zoi", "zoi") != "") {
		if strings.Contains(zoiLow, "azmaran") {
			return "aoc-world-sandsquall-azmaran"
		}
		return "aoc-world-sandsquall-sunhaven"
	}
	for _, w := range s.worldWorkers {
		if strings.EqualFold(w.ScopeKey, targetReg) {
			return firstNonEmpty(w.ContainerName, w.WorkerID)
		}
	}
	return "aoc-world-" + strings.ReplaceAll(targetReg, "_", "-")
}

