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
	"aocrop/services/internal/telemetry"
	"aocrop/services/internal/worldstream"
)

type Role string

const (
	RoleObserver  Role = "GM_OBSERVER"
	RoleOperator  Role = "GM_OPERATOR"
	RoleArchitect Role = "GM_ARCHITECT"
	wsMagicGUID        = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
)

var roleRanks = map[Role]int{RoleObserver: 1, RoleOperator: 2, RoleArchitect: 3}

func (r Role) rank() int          { return roleRanks[r] }
func (r Role) AtLeast(m Role) bool { return r.rank() >= m.rank() && m.rank() > 0 }

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
type AuditEntry struct {
	AuditID           string `json:"audit_id"`
	ActorSub          string `json:"actor_sub"`
	ActorRole         Role   `json:"actor_role"`
	CommandDomain     string `json:"command_domain"`
	ActionName        string `json:"action_name"`
	TargetEntityID    string `json:"target_entity_id,omitempty"`
	SQLText           string `json:"sql_text,omitempty"`
	UndoSQL           string `json:"undo_sql"`
	DiffSummary       string `json:"diff_summary"`
	SnapshotEpoch     uint64 `json:"snapshot_epoch"`
	AppliedAt         string `json:"applied_at"`
	RolledBack        bool   `json:"rolled_back"`
	RolledBackAuditID string `json:"rolled_back_audit_id,omitempty"`
}
type idempotencyRecord struct {
	payloadHash [32]byte
	receipt     CommandReceipt
}
type hotfixIdemRecord struct {
	payloadHash     [32]byte
	applyReceipt    *HotfixApplyReceipt
	rollbackReceipt *AuditRollbackReceipt
}
type Config struct {
	Catalog    *admincatalog.Catalog
	Coalescer  *worldstream.Coalescer
	RoleTokens map[string]Role
	Now        func() time.Time
}
type Server struct {
	catalog                                     *admincatalog.Catalog
	coalescer                                   *worldstream.Coalescer
	roleTokens                                  map[string]Role
	now                                         func() time.Time
	startedAt                                   time.Time
	ready                                       atomic.Bool
	subSeq, snapshotEpoch, auditSeq, reloadCount atomic.Uint64
	mu                                          sync.Mutex
	idemStore                                   map[string]idempotencyRecord
	hotfixIdem                                  map[string]hotfixIdemRecord
	overlayState                                map[string]HotfixRowDiff
	auditLog                                    []AuditEntry
	mux                                         *http.ServeMux
	etagHandler                                 http.Handler
}
type authedHandler func(w http.ResponseWriter, r *http.Request, role Role)

func DefaultRoleTokens() map[string]Role {
	return map[string]Role{"gm-observer-dev-token": RoleObserver, "gm-operator-dev-token": RoleOperator, "gm-architect-dev-token": RoleArchitect}
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
		catalog: cfg.Catalog, coalescer: cfg.Coalescer, roleTokens: cfg.RoleTokens, now: cfg.Now, startedAt: cfg.Now(),
		idemStore: make(map[string]idempotencyRecord), hotfixIdem: make(map[string]hotfixIdemRecord),
		overlayState: make(map[string]HotfixRowDiff), mux: http.NewServeMux(),
	}
	s.ready.Store(true)
	s.snapshotEpoch.Store(1)
	s.registerRoutes()
	s.etagHandler = telemetry.ETagMiddleware(s.mux)
	return s
}
func (s *Server) SetReady(ready bool)   { s.ready.Store(ready) }
func (s *Server) SnapshotEpoch() uint64 { return s.snapshotEpoch.Load() }
func (s *Server) ReloadCount() uint64   { return s.reloadCount.Load() }
func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Alt-Svc", `h3=":8443"; ma=86400`)
	if r.URL.Path == "/api/v1/world/stream" {
		s.mux.ServeHTTP(w, r)
		return
	}
	if !s.checkMutatingIfMatch(w, r) {
		return
	}
	s.etagHandler.ServeHTTP(canonicalHeaderWriter{w}, r)
}

type canonicalHeaderWriter struct{ http.ResponseWriter }

func (c canonicalHeaderWriter) WriteHeader(code int) {
	hdr := c.Header()
	for k, vals := range hdr {
		if canon := http.CanonicalHeaderKey(k); canon != k {
			delete(hdr, k)
			hdr[canon] = vals
		}
	}
	c.ResponseWriter.WriteHeader(code)
}
func (s *Server) checkMutatingIfMatch(w http.ResponseWriter, r *http.Request) bool {
	if r.Method != http.MethodPost && r.Method != http.MethodPut && r.Method != http.MethodPatch && r.Method != http.MethodDelete {
		return true
	}
	ifMatch := strings.TrimSpace(r.Header.Get("If-Match"))
	if ifMatch == "" {
		return true
	}
	if ifMatch == "*" || ifMatch == s.currentAuditETag() || ifMatch == s.currentCommandsETag() {
		r.Header.Del("If-Match")
		return true
	}
	writeProblem(w, r, http.StatusPreconditionFailed, "precondition-failed", fmt.Sprintf("If-Match %s did not match current representation ETag.", ifMatch))
	return false
}
func (s *Server) registerRoutes() {
	s.mux.HandleFunc("GET /livez", s.handleLivez)
	s.mux.HandleFunc("GET /readyz", s.handleReadyz)
	s.mux.HandleFunc("GET /healthz", s.handleHealthz)
	s.mux.HandleFunc("GET /api/v1/catalog/search", s.requireRole(RoleObserver, s.handleCatalogSearch))
	s.mux.HandleFunc("GET /api/v1/world/entities", s.requireRole(RoleObserver, s.handleWorldEntities))
	s.mux.HandleFunc("GET /api/v1/world/stream", s.requireRole(RoleObserver, s.handleWorldStream))
	s.mux.HandleFunc("GET /api/v1/gm/commands", s.requireRole(RoleObserver, s.wrapHandler(s.HandleCommandList)))
	s.mux.HandleFunc("POST /api/v1/gm/command", s.requireRole(RoleOperator, s.handleGMCommand))
	s.mux.HandleFunc("POST /api/v1/gm/hotfix/dry-run", s.requireRole(RoleArchitect, s.wrapHandler(s.HandleHotfixDryRun)))
	s.mux.HandleFunc("POST /api/v1/gm/hotfix/apply", s.requireRole(RoleArchitect, s.wrapHandler(s.HandleHotfixApply)))
	s.mux.HandleFunc("GET /api/v1/gm/audit", s.requireRole(RoleObserver, s.wrapHandler(s.HandleAuditList)))
	s.mux.HandleFunc("POST /api/v1/gm/audit/rollback", s.requireRole(RoleArchitect, s.wrapHandler(s.HandleAuditRollback)))
}
func (s *Server) wrapHandler(fn http.HandlerFunc) authedHandler {
	return func(w http.ResponseWriter, r *http.Request, _ Role) { fn(w, r) }
}
func (s *Server) HandleCommandList(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.buildCommandsPayload())
}
func (s *Server) buildCommandsPayload() map[string]any {
	s.mu.Lock()
	executed := len(s.idemStore)
	s.mu.Unlock()
	cmds := []map[string]any{
		{"command": "spawn_npc", "min_role": RoleOperator},
		{"command": "despawn_entity", "min_role": RoleOperator},
		{"command": "teleport_player", "min_role": RoleOperator},
		{"command": "grant_item", "min_role": RoleOperator},
		{"command": "set_node_level", "min_role": RoleArchitect},
		{"command": "world_broadcast", "min_role": RoleArchitect},
		{"command": "reset_encounter", "min_role": RoleArchitect},
		{"command": "shutdown_zone", "min_role": RoleArchitect},
	}
	return map[string]any{"count": len(cmds), "executed_count": executed, "commands": cmds}
}
func (s *Server) currentCommandsETag() string {
	b, _ := json.Marshal(s.buildCommandsPayload())
	return telemetry.ComputeStrongETag(append(b, '\n'))
}
func (s *Server) HandleAuditList(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.buildAuditPayload())
}
func (s *Server) buildAuditPayload() map[string]any {
	s.mu.Lock()
	entries, epoch := append([]AuditEntry(nil), s.auditLog...), s.snapshotEpoch.Load()
	s.mu.Unlock()
	return map[string]any{"snapshot_epoch": epoch, "count": len(entries), "entries": entries}
}
func (s *Server) currentAuditETag() string {
	b, _ := json.Marshal(s.buildAuditPayload())
	return telemetry.ComputeStrongETag(append(b, '\n'))
}
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
			writeProblem(w, r, http.StatusForbidden, "insufficient-role", fmt.Sprintf("Role %s does not satisfy required minimum role %s.", role, min))
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
func extractIdempotencyKey(r *http.Request) (string, bool) {
	key := firstNonEmpty(r.Header.Get("Idempotency-Key"), r.Header.Get("X-Idempotency-Key"))
	return key, len(key) >= 1 && len(key) <= 128
}
func firstNonEmpty(a, b string) string {
	if strings.TrimSpace(a) != "" {
		return strings.TrimSpace(a)
	}
	return strings.TrimSpace(b)
}
func decodeBoundedJSON(w http.ResponseWriter, r *http.Request, dst any) bool {
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-json-body", "Malformed JSON request payload.")
		return false
	}
	return true
}
func (s *Server) resolveRole(candidate string) (Role, bool) {
	var matched Role
	found, candBytes := false, []byte(candidate)
	for tok, role := range s.roleTokens {
		if len(tok) == len(candBytes) && subtle.ConstantTimeCompare([]byte(tok), candBytes) == 1 {
			matched, found = role, true
		}
	}
	return matched, found
}
func (s *Server) handleLivez(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "probe": "livez", "serverTimeMs": s.now().UnixMilli()})
}
func (s *Server) handleReadyz(w http.ResponseWriter, r *http.Request) {
	if !s.ready.Load() || s.catalog == nil || s.coalescer == nil {
		writeProblem(w, r, http.StatusServiceUnavailable, "not-ready", "Admin gateway dependencies are not ready.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"status": "ready", "probe": "readyz", "catalogEntries": s.catalog.Count(), "serverTimeMs": s.now().UnixMilli()})
}
func (s *Server) handleHealthz(w http.ResponseWriter, r *http.Request) {
	if !s.ready.Load() {
		writeProblem(w, r, http.StatusServiceUnavailable, "unhealthy", "Admin gateway is shutting down.")
		return
	}
	w.Header().Set("ETag", telemetry.ComputeStrongETag([]byte(fmt.Sprintf("healthy:%d:%d:%d", s.catalog.Count(), s.coalescer.EntityCount(), s.snapshotEpoch.Load()))))
	writeJSON(w, http.StatusOK, map[string]any{"status": "healthy", "probe": "healthz", "uptimeMs": s.now().Sub(s.startedAt).Milliseconds(), "catalogEntries": s.catalog.Count(), "worldEntities": s.coalescer.EntityCount(), "activeSubscribers": s.coalescer.SubscriberCount(), "serverTimeMs": s.now().UnixMilli()})
}
func (s *Server) handleCatalogSearch(w http.ResponseWriter, r *http.Request, _ Role) {
	q, kind, limit := strings.TrimSpace(r.URL.Query().Get("q")), strings.TrimSpace(r.URL.Query().Get("kind")), 25
	if utf8.RuneCountInString(q) > 100 {
		writeProblem(w, r, http.StatusBadRequest, "invalid-query", "Query parameter 'q' exceeds 100 characters.")
		return
	}
	if rawLimit := strings.TrimSpace(r.URL.Query().Get("limit")); rawLimit != "" {
		parsed, err := strconv.Atoi(rawLimit)
		if err != nil || parsed < 1 || parsed > 100 {
			writeProblem(w, r, http.StatusBadRequest, "invalid-limit", "Query parameter 'limit' must be an integer between 1 and 100.")
			return
		}
		limit = parsed
	}
	items := s.catalog.Search(q, kind, limit)
	writeJSON(w, http.StatusOK, map[string]any{"query": q, "kind": kind, "count": len(items), "items": items})
}
func (s *Server) handleWorldEntities(w http.ResponseWriter, r *http.Request, _ Role) {
	filter, err := parseViewportFilter(r)
	if err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-viewport", err.Error())
		return
	}
	entities := s.coalescer.Snapshot(filter)
	writeJSON(w, http.StatusOK, map[string]any{"zone": filter.Zone, "count": len(entities), "entities": entities, "serverTimeMs": s.now().UnixMilli()})
}
func parseViewportFilter(r *http.Request) (worldstream.ViewportFilter, error) {
	q := r.URL.Query()
	filter := worldstream.ViewportFilter{Zone: strings.TrimSpace(q.Get("zone"))}
	minXStr, minYStr, maxXStr, maxYStr := q.Get("minX"), q.Get("minY"), q.Get("maxX"), q.Get("maxY")
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
	filter.HasBounds, filter.MinX, filter.MinY, filter.MaxX, filter.MaxY = true, minX, minY, maxX, maxY
	return filter, nil
}
func (s *Server) handleWorldStream(w http.ResponseWriter, r *http.Request, _ Role) {
	filter, err := parseViewportFilter(r)
	if err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-viewport", err.Error())
		return
	}
	maxFrames, _ := strconv.Atoi(r.URL.Query().Get("maxFrames"))
	subID := fmt.Sprintf("sub-%d", s.subSeq.Add(1))
	sub := s.coalescer.Subscribe(subID, filter)
	defer s.coalescer.Unsubscribe(subID)
	if strings.EqualFold(r.Header.Get("Upgrade"), "websocket") && r.Header.Get("Sec-WebSocket-Key") != "" {
		s.serveWebSocketStream(w, r, sub, maxFrames)
		return
	}
	s.serveSSEStream(w, r, sub, maxFrames)
}
func (s *Server) serveSSEStream(w http.ResponseWriter, r *http.Request, sub *worldstream.Subscription, maxFrames int) {
	rc := http.NewResponseController(w)
	_ = rc.SetReadDeadline(time.Time{})
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Connection", "keep-alive")
	w.WriteHeader(http.StatusOK)
	_ = rc.Flush()
	s.streamSubscriptionLoop(r, sub, maxFrames, func(payload []byte) error {
		_ = rc.SetWriteDeadline(s.now().Add(5 * time.Second))
		_, err := fmt.Fprintf(w, "event: delta\ndata: %s\n\n", payload)
		if err == nil {
			err = rc.Flush()
		}
		return err
	})
}
func (s *Server) serveWebSocketStream(w http.ResponseWriter, r *http.Request, sub *worldstream.Subscription, maxFrames int) {
	rc := http.NewResponseController(w)
	conn, rw, err := rc.Hijack()
	if err != nil {
		writeProblem(w, r, http.StatusInternalServerError, "hijack-failed", "WebSocket upgrade hijack failed.")
		return
	}
	defer conn.Close()
	sum := sha1.Sum([]byte(strings.TrimSpace(r.Header.Get("Sec-WebSocket-Key")) + wsMagicGUID))
	handshake := "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + base64.StdEncoding.EncodeToString(sum[:]) + "\r\n\r\n"
	if _, err := rw.WriteString(handshake); err != nil || rw.Flush() != nil {
		return
	}
	s.streamSubscriptionLoop(r, sub, maxFrames, func(payload []byte) error {
		_ = conn.SetWriteDeadline(s.now().Add(5 * time.Second))
		return writeWSFrame(conn, payload)
	})
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
func (s *Server) handleGMCommand(w http.ResponseWriter, r *http.Request, role Role) {
	idemKey, ok := extractIdempotencyKey(r)
	if !ok {
		writeProblem(w, r, http.StatusBadRequest, "missing-idempotency-key", "Header 'X-Idempotency-Key' (1..128 chars) is required.")
		return
	}
	var req CommandRequest
	if !decodeBoundedJSON(w, r, &req) {
		return
	}
	if req.Command = strings.ToLower(strings.TrimSpace(req.Command)); req.Command == "" {
		writeProblem(w, r, http.StatusBadRequest, "missing-command", "Field 'command' is required.")
		return
	}
	if required := requiredRoleForCommand(req.Command); !role.AtLeast(required) {
		writeProblem(w, r, http.StatusForbidden, "insufficient-role", fmt.Sprintf("Command %q requires role %s.", req.Command, required))
		return
	}
	s.dispatchIdempotentCommand(w, r, role, idemKey, req)
}
func requiredRoleForCommand(cmd string) Role {
	if cmd == "set_node_level" || cmd == "world_broadcast" || cmd == "reset_encounter" || cmd == "shutdown_zone" {
		return RoleArchitect
	}
	return RoleOperator
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
	receipt := CommandReceipt{ReceiptID: "rcpt-" + hex.EncodeToString(pHash[:8]), IdempotencyKey: idemKey, Command: req.Command, TargetID: req.TargetID, Role: role, Status: "EXECUTED", ExecutedAtMs: s.now().UnixMilli()}
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
	s.coalescer.UpsertEntity(worldstream.EntitySnapshot{ID: req.TargetID, Kind: "npc", Name: req.Command + ":" + req.TargetID, Zone: firstNonEmpty(req.Zone, "riverlands"), HealthPct: 100, Level: 50, State: "active", UpdatedAt: s.now().UnixMilli()})
}
func writeProblem(w http.ResponseWriter, r *http.Request, status int, code, detail string) {
	w.Header().Set("Content-Type", "application/problem+json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(ProblemDetails{Type: "https://sandalcore.dev/problems/" + code, Title: http.StatusText(status), Status: status, Detail: detail, Instance: r.URL.Path, Code: code})
}
func writeJSON(w http.ResponseWriter, status int, payload any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(payload)
}
