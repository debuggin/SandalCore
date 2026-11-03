// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingw

import (
	"crypto/sha1"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"

	"aocrop/services/internal/admincatalog"
	"aocrop/services/internal/worldstream"
)

type Role string

const (
	RoleObserver  Role = "GM_OBSERVER"
	RoleOperator  Role = "GM_OPERATOR"
	RoleArchitect Role = "GM_ARCHITECT"

	wsMagicGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
)

func (r Role) rank() int {
	switch r {
	case RoleObserver:
		return 1
	case RoleOperator:
		return 2
	case RoleArchitect:
		return 3
	default:
		return 0
	}
}

func (r Role) AtLeast(min Role) bool {
	return r.rank() >= min.rank() && min.rank() > 0
}

type ProblemDetails struct {
	Type       string `json:"type"`
	Title      string `json:"title"`
	Status     int    `json:"status"`
	Detail     string `json:"detail"`
	Instance   string `json:"instance,omitempty"`
	Code       string `json:"code,omitempty"`
	RetryAfter int    `json:"retryAfter,omitempty"`
}

type CommandRequest struct {
	Command  string         `json:"command"`
	TargetID string         `json:"targetId,omitempty"`
	Zone     string         `json:"zone,omitempty"`
	Params   map[string]any `json:"params,omitempty"`
	Reason   string         `json:"reason,omitempty"`
}

type CommandReceipt struct {
	ReceiptID        string `json:"receiptId"`
	IdempotencyKey   string `json:"idempotencyKey"`
	Command          string `json:"command"`
	TargetID         string `json:"targetId,omitempty"`
	Role             Role   `json:"role"`
	Status           string `json:"status"`
	ExecutedAtMs     int64  `json:"executedAtMs"`
	IdempotentReplay bool   `json:"idempotentReplay"`
}

type idempotencyRecord struct {
	payloadHash [32]byte
	receipt     CommandReceipt
}

type Config struct {
	Catalog    *admincatalog.Catalog
	Coalescer  *worldstream.Coalescer
	RoleTokens map[string]Role
	Now        func() time.Time
}

type Server struct {
	catalog    *admincatalog.Catalog
	coalescer  *worldstream.Coalescer
	roleTokens map[string]Role
	now        func() time.Time
	startedAt  time.Time
	ready      atomic.Bool
	subSeq     atomic.Uint64
	mu         sync.Mutex
	idemStore  map[string]idempotencyRecord
	mux        *http.ServeMux
}

func DefaultRoleTokens() map[string]Role {
	return map[string]Role{
		"gm-observer-dev-token":  RoleObserver,
		"gm-operator-dev-token":  RoleOperator,
		"gm-architect-dev-token": RoleArchitect,
	}
}

func NewRouter(cfg Config) *Server {
	if cfg.Catalog == nil {
		cfg.Catalog = admincatalog.NewDefaultCatalog()
	}
	if cfg.Coalescer == nil {
		cfg.Coalescer = worldstream.NewCoalescer(worldstream.DefaultTickInterval)
	}
	if len(cfg.RoleTokens) == 0 {
		cfg.RoleTokens = DefaultRoleTokens()
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	s := &Server{
		catalog:    cfg.Catalog,
		coalescer:  cfg.Coalescer,
		roleTokens: cfg.RoleTokens,
		now:        cfg.Now,
		startedAt:  cfg.Now(),
		idemStore:  make(map[string]idempotencyRecord),
		mux:        http.NewServeMux(),
	}
	s.ready.Store(true)
	s.registerRoutes()
	return s
}

func (s *Server) SetReady(ready bool) {
	s.ready.Store(ready)
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "no-store")
	s.mux.ServeHTTP(w, r)
}

func (s *Server) registerRoutes() {
	s.mux.HandleFunc("GET /livez", s.handleLivez)
	s.mux.HandleFunc("GET /readyz", s.handleReadyz)
	s.mux.HandleFunc("GET /healthz", s.handleHealthz)
	s.mux.HandleFunc("GET /api/v1/catalog/search", s.requireRole(RoleObserver, s.handleCatalogSearch))
	s.mux.HandleFunc("GET /api/v1/world/entities", s.requireRole(RoleObserver, s.handleWorldEntities))
	s.mux.HandleFunc("GET /api/v1/world/stream", s.requireRole(RoleObserver, s.handleWorldStream))
	s.mux.HandleFunc("POST /api/v1/gm/command", s.requireRole(RoleOperator, s.handleGMCommand))
}

type authedHandler func(w http.ResponseWriter, r *http.Request, role Role)

func (s *Server) requireRole(min Role, next authedHandler) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		token, ok := extractBearerToken(r)
		if !ok {
			w.Header().Set("WWW-Authenticate", `Bearer realm="admingw"`)
			writeProblem(w, r, http.StatusUnauthorized, "unauthorized", "Missing or malformed Bearer token.")
			return
		}
		role, valid := s.resolveRole(token)
		if !valid {
			w.Header().Set("WWW-Authenticate", `Bearer realm="admingw", error="invalid_token"`)
			writeProblem(w, r, http.StatusUnauthorized, "invalid-token", "Provided Bearer token is not recognized.")
			return
		}
		if !role.AtLeast(min) {
			detail := fmt.Sprintf("Role %s does not satisfy required minimum role %s.", role, min)
			writeProblem(w, r, http.StatusForbidden, "insufficient-role", detail)
			return
		}
		next(w, r, role)
	}
}

func extractBearerToken(r *http.Request) (string, bool) {
	auth := strings.TrimSpace(r.Header.Get("Authorization"))
	if !strings.HasPrefix(auth, "Bearer ") {
		return "", false
	}
	tok := strings.TrimSpace(strings.TrimPrefix(auth, "Bearer "))
	return tok, tok != ""
}

func (s *Server) resolveRole(candidate string) (Role, bool) {
	var matched Role
	found := false
	candBytes := []byte(candidate)
	for tok, role := range s.roleTokens {
		if len(tok) == len(candBytes) && subtle.ConstantTimeCompare([]byte(tok), candBytes) == 1 {
			matched = role
			found = true
		}
	}
	return matched, found
}

func (s *Server) handleLivez(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"status":       "ok",
		"probe":        "livez",
		"serverTimeMs": s.now().UnixMilli(),
	})
}

func (s *Server) handleReadyz(w http.ResponseWriter, r *http.Request) {
	if !s.ready.Load() || s.catalog == nil || s.coalescer == nil {
		writeProblem(w, r, http.StatusServiceUnavailable, "not-ready", "Admin gateway dependencies are not ready.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"status":         "ready",
		"probe":          "readyz",
		"catalogEntries": s.catalog.Count(),
		"serverTimeMs":   s.now().UnixMilli(),
	})
}

func (s *Server) handleHealthz(w http.ResponseWriter, r *http.Request) {
	if !s.ready.Load() {
		writeProblem(w, r, http.StatusServiceUnavailable, "unhealthy", "Admin gateway is shutting down.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"status":            "healthy",
		"probe":             "healthz",
		"uptimeMs":          s.now().Sub(s.startedAt).Milliseconds(),
		"catalogEntries":    s.catalog.Count(),
		"worldEntities":     s.coalescer.EntityCount(),
		"activeSubscribers": s.coalescer.SubscriberCount(),
		"serverTimeMs":      s.now().UnixMilli(),
	})
}

func (s *Server) handleCatalogSearch(w http.ResponseWriter, r *http.Request, _ Role) {
	q := strings.TrimSpace(r.URL.Query().Get("q"))
	if utf8.RuneCountInString(q) > 100 {
		writeProblem(w, r, http.StatusBadRequest, "invalid-query", "Query parameter 'q' exceeds 100 characters.")
		return
	}
	kind := strings.TrimSpace(r.URL.Query().Get("kind"))
	limit := 25
	if rawLimit := strings.TrimSpace(r.URL.Query().Get("limit")); rawLimit != "" {
		parsed, err := strconv.Atoi(rawLimit)
		if err != nil || parsed < 1 || parsed > 100 {
			writeProblem(w, r, http.StatusBadRequest, "invalid-limit", "Query parameter 'limit' must be an integer between 1 and 100.")
			return
		}
		limit = parsed
	}
	items := s.catalog.Search(q, kind, limit)
	writeJSON(w, http.StatusOK, map[string]any{
		"query": q,
		"kind":  kind,
		"count": len(items),
		"items": items,
	})
}

func (s *Server) handleWorldEntities(w http.ResponseWriter, r *http.Request, _ Role) {
	filter, err := parseViewportFilter(r)
	if err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-viewport", err.Error())
		return
	}
	entities := s.coalescer.Snapshot(filter)
	writeJSON(w, http.StatusOK, map[string]any{
		"zone":         filter.Zone,
		"count":        len(entities),
		"entities":     entities,
		"serverTimeMs": s.now().UnixMilli(),
	})
}

func parseViewportFilter(r *http.Request) (worldstream.ViewportFilter, error) {
	q := r.URL.Query()
	filter := worldstream.ViewportFilter{Zone: strings.TrimSpace(q.Get("zone"))}
	minXStr, minYStr := q.Get("minX"), q.Get("minY")
	maxXStr, maxYStr := q.Get("maxX"), q.Get("maxY")
	if minXStr == "" && minYStr == "" && maxXStr == "" && maxYStr == "" {
		return filter, nil
	}
	minX, err1 := strconv.ParseFloat(minXStr, 64)
	minY, err2 := strconv.ParseFloat(minYStr, 64)
	maxX, err3 := strconv.ParseFloat(maxXStr, 64)
	maxY, err4 := strconv.ParseFloat(maxYStr, 64)
	if err1 != nil || err2 != nil || err3 != nil || err4 != nil || minX > maxX || minY > maxY {
		return worldstream.ViewportFilter{}, fmt.Errorf("invalid viewport bounding box coordinates")
	}
	filter.HasBounds = true
	filter.MinX, filter.MinY, filter.MaxX, filter.MaxY = minX, minY, maxX, maxY
	return filter, nil
}

func (s *Server) handleWorldStream(w http.ResponseWriter, r *http.Request, _ Role) {
	filter, err := parseViewportFilter(r)
	if err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-viewport", err.Error())
		return
	}
	maxFrames := 0
	if rawMax := r.URL.Query().Get("maxFrames"); rawMax != "" {
		if n, err := strconv.Atoi(rawMax); err == nil && n > 0 {
			maxFrames = n
		}
	}
	subID := fmt.Sprintf("sub-%d", s.subSeq.Add(1))
	sub := s.coalescer.Subscribe(subID, filter)
	defer s.coalescer.Unsubscribe(subID)

	if isWebSocketUpgrade(r) {
		s.serveWebSocketStream(w, r, sub, maxFrames)
		return
	}
	s.serveSSEStream(w, r, sub, maxFrames)
}

func isWebSocketUpgrade(r *http.Request) bool {
	return strings.EqualFold(r.Header.Get("Upgrade"), "websocket") && r.Header.Get("Sec-WebSocket-Key") != ""
}

func (s *Server) serveSSEStream(w http.ResponseWriter, r *http.Request, sub *worldstream.Subscription, maxFrames int) {
	rc := http.NewResponseController(w)
	_ = rc.SetReadDeadline(time.Time{})
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Connection", "keep-alive")
	w.WriteHeader(http.StatusOK)
	_ = rc.Flush()

	s.coalescer.Flush()
	sent := 0
	for {
		select {
		case <-r.Context().Done():
			return
		case delta, ok := <-sub.Updates():
			if !ok {
				return
			}
			payload, _ := json.Marshal(delta)
			_ = rc.SetWriteDeadline(s.now().Add(5 * time.Second))
			if _, err := fmt.Fprintf(w, "event: delta\ndata: %s\n\n", payload); err != nil {
				return
			}
			_ = rc.Flush()
			sent++
			if maxFrames > 0 && sent >= maxFrames {
				return
			}
		}
	}
}

func (s *Server) serveWebSocketStream(w http.ResponseWriter, r *http.Request, sub *worldstream.Subscription, maxFrames int) {
	key := strings.TrimSpace(r.Header.Get("Sec-WebSocket-Key"))
	rc := http.NewResponseController(w)
	conn, rw, err := rc.Hijack()
	if err != nil {
		writeProblem(w, r, http.StatusInternalServerError, "hijack-failed", "WebSocket upgrade hijack failed.")
		return
	}
	defer conn.Close()

	sum := sha1.Sum([]byte(key + wsMagicGUID))
	accept := base64.StdEncoding.EncodeToString(sum[:])
	handshake := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + accept + "\r\n\r\n"
	if _, err := rw.WriteString(handshake); err != nil || rw.Flush() != nil {
		return
	}
	s.coalescer.Flush()
	s.loopWebSocketFrames(r, conn, sub, maxFrames)
}

func (s *Server) loopWebSocketFrames(r *http.Request, conn net.Conn, sub *worldstream.Subscription, maxFrames int) {
	sent := 0
	for {
		select {
		case <-r.Context().Done():
			return
		case delta, ok := <-sub.Updates():
			if !ok {
				return
			}
			payload, _ := json.Marshal(delta)
			_ = conn.SetWriteDeadline(s.now().Add(5 * time.Second))
			if err := writeWSFrame(conn, payload); err != nil {
				return
			}
			sent++
			if maxFrames > 0 && sent >= maxFrames {
				return
			}
		}
	}
}

func writeWSFrame(conn net.Conn, payload []byte) error {
	length := len(payload)
	header := make([]byte, 0, 10)
	header = append(header, 0x81)
	switch {
	case length <= 125:
		header = append(header, byte(length))
	case length <= 65535:
		header = append(header, 126, 0, 0)
		binary.BigEndian.PutUint16(header[2:4], uint16(length))
	default:
		header = append(header, 127, 0, 0, 0, 0, 0, 0, 0, 0)
		binary.BigEndian.PutUint64(header[2:10], uint64(length))
	}
	if _, err := conn.Write(header); err != nil {
		return err
	}
	_, err := conn.Write(payload)
	return err
}

func (s *Server) handleGMCommand(w http.ResponseWriter, r *http.Request, role Role) {
	idemKey := strings.TrimSpace(r.Header.Get("X-Idempotency-Key"))
	if len(idemKey) == 0 || len(idemKey) > 128 {
		writeProblem(w, r, http.StatusBadRequest, "missing-idempotency-key", "Header 'X-Idempotency-Key' (1..128 chars) is required.")
		return
	}
	var req CommandRequest
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-command-body", "Malformed JSON command payload.")
		return
	}
	req.Command = strings.ToLower(strings.TrimSpace(req.Command))
	if req.Command == "" {
		writeProblem(w, r, http.StatusBadRequest, "missing-command", "Field 'command' is required.")
		return
	}
	required := requiredRoleForCommand(req.Command)
	if !role.AtLeast(required) {
		detail := fmt.Sprintf("Command %q requires role %s.", req.Command, required)
		writeProblem(w, r, http.StatusForbidden, "insufficient-role", detail)
		return
	}
	s.dispatchIdempotentCommand(w, r, role, idemKey, req)
}

func requiredRoleForCommand(cmd string) Role {
	switch cmd {
	case "set_node_level", "world_broadcast", "reset_encounter", "shutdown_zone":
		return RoleArchitect
	default:
		return RoleOperator
	}
}

func (s *Server) dispatchIdempotentCommand(w http.ResponseWriter, r *http.Request, role Role, idemKey string, req CommandRequest) {
	 canonical, _ := json.Marshal(req)
	pHash := sha256.Sum256(canonical)

	s.mu.Lock()
	if existing, ok := s.idemStore[idemKey]; ok {
		s.mu.Unlock()
		if existing.payloadHash != pHash {
			writeProblem(w, r, http.StatusConflict, "idempotency-key-conflict", "X-Idempotency-Key was already used with a different command payload.")
			return
		}
		replay := existing.receipt
		replay.IdempotentReplay = true
		writeJSON(w, http.StatusOK, replay)
		return
	}
	receipt := CommandReceipt{
		ReceiptID:        "rcpt-" + hex.EncodeToString(pHash[:8]),
		IdempotencyKey:   idemKey,
		Command:          req.Command,
		TargetID:         req.TargetID,
		Role:             role,
		Status:           "EXECUTED",
		ExecutedAtMs:     s.now().UnixMilli(),
		IdempotentReplay: false,
	}
	s.idemStore[idemKey] = idempotencyRecord{payloadHash: pHash, receipt: receipt}
	s.mu.Unlock()

	s.applyCommandSideEffect(req)
	writeJSON(w, http.StatusOK, receipt)
}

func (s *Server) applyCommandSideEffect(req CommandRequest) {
	if req.TargetID == "" {
		return
	}
	if req.Command == "despawn_entity" {
		s.coalescer.RemoveEntity(req.TargetID)
		return
	}
	zone := req.Zone
	if zone == "" {
		zone = "riverlands"
	}
	s.coalescer.UpsertEntity(worldstream.EntitySnapshot{
		ID:        req.TargetID,
		Kind:      "npc",
		Name:      req.Command + ":" + req.TargetID,
		Zone:      zone,
		HealthPct: 100,
		Level:     50,
		State:     "active",
		UpdatedAt: s.now().UnixMilli(),
	})
}

func writeProblem(w http.ResponseWriter, r *http.Request, status int, code, detail string) {
	w.Header().Set("Content-Type", "application/problem+json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(ProblemDetails{
		Type:     "https://sandalcore.dev/problems/" + code,
		Title:    http.StatusText(status),
		Status:   status,
		Detail:   detail,
		Instance: r.URL.Path,
		Code:     code,
	})
}

func writeJSON(w http.ResponseWriter, status int, payload any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(payload)
}
