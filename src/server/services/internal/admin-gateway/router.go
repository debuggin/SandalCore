// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingateway

import (
	"crypto/subtle"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"

	citynodev1 "aocrop/services/gen/aoc/citynode/v1"
	worldadminv1 "aocrop/services/gen/aoc/worldadmin/v1"
	admincatalog "aocrop/services/internal/admin-catalog"
	"aocrop/services/internal/telemetry"
	worldstream "aocrop/services/internal/world-stream"
	"google.golang.org/grpc"
)

type Role string

const (
	RoleObserver  Role = "GM_OBSERVER"
	RoleOperator  Role = "GM_OPERATOR"
	RoleArchitect Role = "GM_ARCHITECT"
	wsMagicGUID        = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
)

var roleRanks = map[Role]int{RoleObserver: 1, RoleOperator: 2, RoleArchitect: 3}

func (r Role) rank() int           { return roleRanks[r] }
func (r Role) AtLeast(m Role) bool { return r.rank() >= m.rank() && m.rank() > 0 }

type ProblemDetails struct {
	Type          string `json:"type"`
	Title         string `json:"title"`
	Status        int    `json:"status"`
	Detail        string `json:"detail"`
	Instance      string `json:"instance,omitempty"`
	Code          string `json:"code,omitempty"`
	CorrelationID string `json:"correlation_id,omitempty"`
	RetryAfter    int    `json:"retryAfter,omitempty"`
}

type CommandRequest struct {
	Command  string         `json:"command"`
	TargetID string         `json:"targetId,omitempty"`
	Zone     string         `json:"zone,omitempty"`
	Params   map[string]any `json:"params,omitempty"`
	Reason   string         `json:"reason,omitempty"`
}

type CommandReceipt struct {
	ReceiptID        string         `json:"receiptId"`
	IdempotencyKey   string         `json:"idempotencyKey"`
	Command          string         `json:"command"`
	TargetID         string         `json:"targetId,omitempty"`
	Role             Role           `json:"role"`
	Status           string         `json:"status"`
	ExecutedAtMs     int64          `json:"executedAtMs"`
	IdempotentReplay bool           `json:"idempotentReplay"`
	Result           map[string]any `json:"result,omitempty"`
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
	Catalog                                                    *admincatalog.Catalog
	Coalescer                                                  *worldstream.Coalescer
	RoleTokens                                                 map[string]Role
	Now                                                        func() time.Time
	PmTilesPath, WorldAdminAddr, WorldStreamAddr, CityNodeAddr string
	ServiceToken, DockerSocketPath, WorldWorkersSpec           string
	PmTilesArchive                                             *PmTilesArchive
	DockerClient                                               *http.Client
	WorldAdminClient                                           worldadminv1.WorldAdminServiceClient
	WorldStreamClient                                          worldadminv1.WorldStreamServiceClient
	CityNodeClient                                             citynodev1.CityNodeServiceClient
	OfflinePlayers                                             []PlayerInfo
	WorldWorkers                                               []*WorldWorkerEndpoint
}

type Server struct {
	catalog                                                    *admincatalog.Catalog
	coalescer                                                  *worldstream.Coalescer
	roleTokens                                                 map[string]Role
	now                                                        func() time.Time
	startedAt                                                  time.Time
	lastMutatedUnixNano                                        atomic.Int64
	ready                                                      atomic.Bool
	subSeq, snapshotEpoch, auditSeq, reloadCount               atomic.Uint64
	requestsTotal, errorsTotal, cacheHits, cacheMisses         atomic.Uint64
	transientRetries                                           atomic.Uint64
	mu, pmtilesMu, grpcMu                                      sync.Mutex
	idemStore                                                  map[string]idempotencyRecord
	hotfixIdem                                                 map[string]hotfixIdemRecord
	overlayState                                               map[string]HotfixRowDiff
	localNodeLevels                                            map[string]int
	nodeGovernance                                             map[string]nodeGovernanceState
	lastStreamAt                                               map[uint32]time.Time
	auditLog                                                   []AuditEntry
	catalogCache, metaCache, containersCache                   *ttlCache
	pmtilesPath, worldAdminAddr, worldStreamAddr, cityNodeAddr string
	serviceToken, dockerSocketPath, worldWorkersSpec           string
	pmtilesArch                                                *PmTilesArchive
	worldAdmin                                                 worldadminv1.WorldAdminServiceClient
	worldStream                                                worldadminv1.WorldStreamServiceClient
	cityNode                                                   citynodev1.CityNodeServiceClient
	worldAdminConn, worldStreamConn, cityNodeConn              *grpc.ClientConn
	dockerClient                                               *http.Client
	offlinePlayers                                             []PlayerInfo
	worldWorkers                                               []*WorldWorkerEndpoint
	mux                                                        *http.ServeMux
	etagHandler                                                http.Handler
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
	nowTime := cfg.Now()
	s := &Server{
		catalog: cfg.Catalog, coalescer: cfg.Coalescer, roleTokens: cfg.RoleTokens, now: cfg.Now, startedAt: nowTime,
		idemStore: make(map[string]idempotencyRecord), hotfixIdem: make(map[string]hotfixIdemRecord),
		overlayState: make(map[string]HotfixRowDiff), localNodeLevels: make(map[string]int),
		nodeGovernance: make(map[string]nodeGovernanceState), lastStreamAt: make(map[uint32]time.Time),
		catalogCache: newTTLCache(), metaCache: newTTLCache(), containersCache: newTTLCache(),
		pmtilesPath: cfg.PmTilesPath, pmtilesArch: cfg.PmTilesArchive,
		worldAdminAddr:  firstNonEmpty(cfg.WorldAdminAddr, os.Getenv("AOC_WORLD_ADMIN_ADDR")),
		worldStreamAddr: firstNonEmpty(cfg.WorldStreamAddr, os.Getenv("AOC_WORLD_STREAM_ADDR")),
		cityNodeAddr:    firstNonEmpty(cfg.CityNodeAddr, os.Getenv("AOC_CITYNODE_GRPC_ADDR")),
		serviceToken:     firstNonEmpty(cfg.ServiceToken, os.Getenv("AOC_SERVICE_TOKEN")),
		worldWorkersSpec: firstNonEmpty(cfg.WorldWorkersSpec, os.Getenv("AOC_WORLD_WORKERS")),
		worldAdmin: cfg.WorldAdminClient, worldStream: cfg.WorldStreamClient,
		cityNode: cfg.CityNodeClient, dockerSocketPath: cfg.DockerSocketPath, dockerClient: cfg.DockerClient,
		offlinePlayers: cfg.OfflinePlayers, worldWorkers: cfg.WorldWorkers, mux: http.NewServeMux(),
	}
	s.initWorldWorkers()
	s.lastMutatedUnixNano.Store(nowTime.UnixNano())
	s.ready.Store(true)
	s.snapshotEpoch.Store(1)
	s.registerRoutes()
	s.etagHandler = telemetry.ETagMiddleware(s.mux)
	return s
}

func (s *Server) SetReady(ready bool)   { s.ready.Store(ready) }
func (s *Server) SnapshotEpoch() uint64 { return s.snapshotEpoch.Load() }
func (s *Server) ReloadCount() uint64   { return s.reloadCount.Load() }

func (s *Server) Close() error {
	s.pmtilesMu.Lock()
	arch := s.pmtilesArch
	s.pmtilesArch = nil
	s.pmtilesMu.Unlock()
	if arch != nil {
		_ = arch.Close()
	}
	return nil
}

func (s *Server) lastMutatedTime() time.Time {
	ns := s.lastMutatedUnixNano.Load()
	if ns <= 0 {
		return s.startedAt
	}
	return time.Unix(0, ns).UTC()
}

func (s *Server) recordMutation() {
	s.lastMutatedUnixNano.Store(s.now().UnixNano())
	s.metaCache.invalidatePrefix("")
	s.catalogCache.invalidatePrefix("catalog:places:")
	s.containersCache.invalidatePrefix("")
}

func (s *Server) getPmTilesArchive() *PmTilesArchive {
	s.pmtilesMu.Lock()
	defer s.pmtilesMu.Unlock()
	if s.pmtilesArch != nil {
		return s.pmtilesArch
	}
	for _, cand := range []string{s.pmtilesPath, os.Getenv("AOC_PMTILES_PATH"), "/content/verra.pmtiles", "dist/content/verra.pmtiles", "deploy/content/verra.pmtiles"} {
		if strings.TrimSpace(cand) == "" {
			continue
		}
		if arch, err := OpenPmTilesArchive(cand); err == nil {
			s.pmtilesArch = arch
			return arch
		}
	}
	return nil
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	reqNum := s.requestsTotal.Add(1)
	start := time.Now()
	corrID := firstNonEmpty(r.Header.Get("X-Correlation-ID"), r.Header.Get("X-Request-ID"))
	if corrID == "" {
		corrID = fmt.Sprintf("req-%d", reqNum)
	}
	w.Header().Set("X-Correlation-ID", corrID)
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Alt-Svc", `h3=":8443"; ma=86400`)
	cw := &canonicalHeaderWriter{ResponseWriter: w, start: start, server: s}
	if r.URL.Path == "/api/v1/world/stream" || r.URL.Path == "/api/v1/world/pmtiles" ||
		(strings.HasPrefix(r.URL.Path, "/api/v1/world/tiles/") && !strings.HasSuffix(r.URL.Path, "/metadata.json")) ||
		strings.HasPrefix(r.URL.Path, "/api/v1/world/icons/") ||
		strings.HasPrefix(r.URL.Path, "/api/v1/gm/world/icons/") {
		s.mux.ServeHTTP(cw, r)
		return
	}
	if !s.checkMutatingIfMatch(cw, r) {
		return
	}
	s.etagHandler.ServeHTTP(cw, r)
}

type canonicalHeaderWriter struct {
	http.ResponseWriter
	start       time.Time
	server      *Server
	wroteHeader bool
}

func (c *canonicalHeaderWriter) WriteHeader(code int) {
	if c.wroteHeader {
		return
	}
	c.wroteHeader = true
	if code >= 400 && c.server != nil {
		c.server.errorsTotal.Add(1)
	}
	hdr := c.Header()
	if hdr.Get("Server-Timing") == "" && !c.start.IsZero() {
		durMs := float64(time.Since(c.start).Microseconds()) / 1000.0
		hdr.Set("Server-Timing", fmt.Sprintf("gateway;dur=%.2f", durMs))
	}
	for k, vals := range hdr {
		if canon := http.CanonicalHeaderKey(k); canon != k {
			delete(hdr, k)
			hdr[canon] = vals
		}
	}
	c.ResponseWriter.WriteHeader(code)
}

func (c *canonicalHeaderWriter) Unwrap() http.ResponseWriter { return c.ResponseWriter }

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
	s.mux.HandleFunc("GET /metrics", s.handleOpenMetrics)
	s.mux.HandleFunc("HEAD /metrics", s.handleOpenMetrics)
	s.mux.HandleFunc("GET /api/v1/catalog/search", s.requireRole(RoleObserver, s.handleCatalogSearch))
	s.registerReadRoute("/api/v1/catalog/items", s.optionalRole(RoleObserver, s.handleCatalogItems))
	s.registerReadRoute("/api/v1/catalog/npcs", s.optionalRole(RoleObserver, s.handleCatalogNpcs))
	s.registerReadRoute("/api/v1/catalog/places", s.optionalRole(RoleObserver, s.handleCatalogPlaces))
	s.mux.HandleFunc("GET /api/v1/world/entities", s.requireRole(RoleObserver, s.handleWorldEntities))
	s.mux.HandleFunc("GET /api/v1/world/stream", s.requireRole(RoleObserver, s.handleWorldStream))
	s.registerReadRoute("/api/v1/world/state", s.optionalRole(RoleObserver, s.handleWorldState))
	s.registerReadRoute("/api/v1/gm/world/state", s.optionalRole(RoleObserver, s.handleWorldState))
	s.registerReadRoute("/api/v1/world/players", s.optionalRole(RoleObserver, s.handleWorldPlayers))
	s.registerReadRoute("/api/v1/gm/world/players", s.optionalRole(RoleObserver, s.handleWorldPlayers))
	s.registerReadRoute("/api/v1/world/map-meta", s.optionalRole(RoleObserver, s.handleWorldMapMetaWithWorkers))
	s.registerReadRoute("/api/v1/world/tiles/metadata.json", s.optionalRole(RoleObserver, s.handleWorldMapMetaWithWorkers))
	s.registerReadRoute("/api/v1/gm/world/tiles/metadata.json", s.optionalRole(RoleObserver, s.handleWorldMapMetaWithWorkers))
	s.registerReadRoute("/api/v1/world/pmtiles", s.optionalRole(RoleObserver, s.handleWorldPmTiles))
	s.registerReadRoute("/api/v1/world/tiles/{z}/{x}/{y}", s.optionalRole(RoleObserver, s.handleWorldTile))
	s.registerReadRoute("/api/v1/world/icons/{icon}", s.optionalRole(RoleObserver, s.handleWorldIcon))
	s.registerReadRoute("/api/v1/gm/world/icons/{icon}", s.optionalRole(RoleObserver, s.handleWorldIcon))
	s.mux.HandleFunc("PUT /api/v1/world/nodes/{node}", s.optionalRole(RoleArchitect, s.handlePutWorldNode))
	s.mux.HandleFunc("POST /api/v1/world/nodes/{node}/mayor", s.optionalRole(RoleOperator, s.handleSetNodeMayor))
	s.mux.HandleFunc("POST /api/v1/world/nodes/{node}/election", s.optionalRole(RoleOperator, s.handleTriggerNodeElection))
	s.mux.HandleFunc("POST /api/v1/gm/node/mayor", s.optionalRole(RoleOperator, s.handleSetNodeMayor))
	s.mux.HandleFunc("POST /api/v1/gm/node/election", s.optionalRole(RoleOperator, s.handleTriggerNodeElection))
	s.mux.HandleFunc("/api/v1/world/nodes/{node}", s.methodNotAllowed("PUT"))
	s.mux.HandleFunc("GET /api/v1/gm/commands", s.requireRole(RoleObserver, s.wrapHandler(s.HandleCommandList)))
	s.mux.HandleFunc("HEAD /api/v1/gm/commands", s.requireRole(RoleObserver, s.wrapHandler(s.HandleCommandList)))
	s.mux.HandleFunc("POST /api/v1/gm/commands", s.handleGMCommandsEndpoint)
	s.mux.HandleFunc("/api/v1/gm/commands", s.methodNotAllowed("GET, HEAD, POST"))
	s.mux.HandleFunc("POST /api/v1/gm/command", s.optionalRole(RoleArchitect, s.handleGMCommand))
	for _, p := range []string{"/api/v1/gm/give_currency", "/api/v1/gm/currency", "/api/v1/gm/give_item", "/api/v1/gm/item", "/api/v1/gm/teleport", "/api/v1/gm/spawn_npc", "/api/v1/gm/spawn"} {
		s.mux.HandleFunc("POST "+p, s.handleGMCommandsEndpoint)
	}
	s.mux.HandleFunc("POST /api/v1/gm/hotfix/dry-run", s.requireRole(RoleArchitect, s.wrapHandler(s.HandleHotfixDryRun)))
	s.mux.HandleFunc("POST /api/v1/gm/hotfix/apply", s.requireRole(RoleArchitect, s.wrapHandler(s.HandleHotfixApply)))
	s.mux.HandleFunc("GET /api/v1/gm/audit", s.requireRole(RoleObserver, s.wrapHandler(s.HandleAuditList)))
	s.mux.HandleFunc("POST /api/v1/gm/audit/rollback", s.requireRole(RoleArchitect, s.wrapHandler(s.HandleAuditRollback)))
	s.registerReadRoute("/api/v1/gm/containers", s.optionalRole(RoleObserver, s.handleGMContainers))
	s.registerReadRoute("/api/v1/gm/workers", s.optionalRole(RoleObserver, s.handleWorldWorkers))
	s.registerReadRoute("/api/v1/world/workers", s.optionalRole(RoleObserver, s.handleWorldWorkers))
	s.registerReadRoute("/api/v1/gm/metrics", s.optionalRole(RoleObserver, s.handlePrometheusMetrics))
	s.registerReadRoute("/api/v1/metrics", s.optionalRole(RoleObserver, s.handlePrometheusMetrics))
	s.registerReadRoute("/api/v1/gm/logs", s.optionalRole(RoleObserver, s.handleGMLogs))
	s.registerReadRoute("/api/v1/gm/openapi.json", s.optionalRole(RoleObserver, s.handleOpenAPI))
	s.registerReadRoute("/api/v1/world/openapi.json", s.optionalRole(RoleObserver, s.handleOpenAPI))
}

func (s *Server) registerReadRoute(pattern string, h http.HandlerFunc) {
	s.mux.HandleFunc("GET "+pattern, h)
	s.mux.HandleFunc("HEAD "+pattern, h)
	s.mux.HandleFunc(pattern, s.methodNotAllowed("GET, HEAD"))
}

func (s *Server) methodNotAllowed(allow string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Allow", allow)
		writeProblem(w, r, http.StatusMethodNotAllowed, "method-not-allowed", fmt.Sprintf("HTTP method %s is not allowed on %s; allowed: %s.", r.Method, r.URL.Path, allow))
	}
}

func (s *Server) wrapHandler(fn http.HandlerFunc) authedHandler {
	return func(w http.ResponseWriter, r *http.Request, _ Role) { fn(w, r) }
}

func (s *Server) HandleCommandList(w http.ResponseWriter, r *http.Request) {
	writeCachedJSONWithModTime(w, r, "no-store", s.lastMutatedTime(), s.buildCommandsPayload())
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
		{"command": "give_money", "min_role": RoleOperator},
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

func (s *Server) HandleAuditList(w http.ResponseWriter, r *http.Request) {
	writeCachedJSONWithModTime(w, r, "no-store", s.lastMutatedTime(), s.buildAuditPayload())
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
			w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway"`)
			writeProblem(w, r, http.StatusUnauthorized, "unauthorized", "Missing or malformed Bearer token.")
			return
		}
		role, valid := s.resolveRole(token)
		if !valid {
			w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway", error="invalid_token"`)
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

func (s *Server) optionalRole(defaultRole Role, next authedHandler) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		role := defaultRole
		if token, ok := extractBearerToken(r); ok {
			resolved, valid := s.resolveRole(token)
			if !valid {
				w.Header().Set("WWW-Authenticate", `Bearer realm="admingateway", error="invalid_token"`)
				writeProblem(w, r, http.StatusUnauthorized, "invalid-token", "Provided Bearer token is not recognized.")
				return
			}
			role = resolved
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
	payload := map[string]any{
		"status": "healthy", "probe": "healthz", "uptimeMs": s.now().Sub(s.startedAt).Milliseconds(),
		"catalogEntries": s.catalog.Count(), "worldEntities": s.coalescer.EntityCount(),
		"activeSubscribers": s.coalescer.SubscriberCount(), "serverTimeMs": s.now().UnixMilli(),
	}
	writeCachedJSONWithModTime(w, r, "no-store", s.lastMutatedTime(), payload)
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

func (s *Server) handleCatalogItems(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	params, ok := parsePageParams(w, r, 50, 200)
	if !ok {
		return
	}
	q := r.URL.Query()
	rarity := firstNonEmpty(q.Get("rarity"), q.Get("rarity_min"))
	equipSlot := firstNonEmpty(q.Get("equip_slot"), q.Get("slot"))
	cacheKey := fmt.Sprintf("catalog:items:%s:%s:%s:%s:%d:%d:%s", params.Query, rarity, equipSlot, params.Sort, params.Limit, params.Offset, strings.Join(params.Fields, ","))
	if cached, hit := s.catalogCache.get(cacheKey, s.now()); hit {
		s.cacheHits.Add(1)
		writeCachedJSONWithModTime(w, r, "public, max-age=30", s.lastMutatedTime(), cached)
		return
	}
	s.cacheMisses.Add(1)
	items, total := s.catalog.SearchItemsFiltered(r.Context(), admincatalog.ItemFilter{
		Query: params.Query, Slot: equipSlot, Rarity: rarity, Sort: params.Sort, Limit: params.Limit, Offset: params.Offset,
	})
	env, xPag := buildPaginatedEnvelope(r, items, "items", total, params.Limit, params.Offset, params.Fields)
	s.catalogCache.set(cacheKey, env, 30*time.Second, s.now())
	w.Header().Set("X-Pagination", xPag)
	writeCachedJSONWithModTime(w, r, "public, max-age=30", s.lastMutatedTime(), env)
}

func (s *Server) handleCatalogNpcs(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	params, ok := parsePageParams(w, r, 50, 200)
	if !ok {
		return
	}
	q := r.URL.Query()
	minLv, _ := strconv.Atoi(strings.TrimSpace(q.Get("min_level")))
	maxLv, _ := strconv.Atoi(strings.TrimSpace(q.Get("max_level")))
	cacheKey := fmt.Sprintf("catalog:npcs:%s:%d:%d:%s:%d:%d:%s", params.Query, minLv, maxLv, params.Sort, params.Limit, params.Offset, strings.Join(params.Fields, ","))
	if cached, hit := s.catalogCache.get(cacheKey, s.now()); hit {
		s.cacheHits.Add(1)
		writeCachedJSONWithModTime(w, r, "public, max-age=30", s.lastMutatedTime(), cached)
		return
	}
	s.cacheMisses.Add(1)
	npcs, total := s.catalog.SearchNpcsFiltered(r.Context(), admincatalog.NpcFilter{
		Query: params.Query, MinLevel: minLv, MaxLevel: maxLv, Sort: params.Sort, Limit: params.Limit, Offset: params.Offset,
	})
	env, xPag := buildPaginatedEnvelope(r, npcs, "npcs", total, params.Limit, params.Offset, params.Fields)
	s.catalogCache.set(cacheKey, env, 30*time.Second, s.now())
	w.Header().Set("X-Pagination", xPag)
	writeCachedJSONWithModTime(w, r, "public, max-age=30", s.lastMutatedTime(), env)
}

func (s *Server) handleCatalogPlaces(w http.ResponseWriter, r *http.Request, _ Role) {
	if !checkAcceptHeader(w, r) {
		return
	}
	params, ok := parsePageParams(w, r, 50, 2000)
	if !ok {
		return
	}
	category := firstNonEmpty(r.URL.Query().Get("category"), r.URL.Query().Get("kind"))
	cacheKey := fmt.Sprintf("catalog:places:%s:%s:%s:%d:%d:%s", params.Query, category, params.Sort, params.Limit, params.Offset, strings.Join(params.Fields, ","))
	if cached, hit := s.catalogCache.get(cacheKey, s.now()); hit {
		s.cacheHits.Add(1)
		writeCachedJSONWithModTime(w, r, "private, max-age=5", s.lastMutatedTime(), cached)
		return
	}
	s.cacheMisses.Add(1)
	worldPlaces := s.fetchWorldPlaces(r.Context())
	liveLevels := s.fetchCityNodeLevels(r.Context())
	places, total := s.catalog.SearchPlacesFiltered(r.Context(), admincatalog.PlaceFilter{
		Query: params.Query, Kind: category, Sort: params.Sort, Limit: params.Limit, Offset: params.Offset,
		ExtraPlaces: worldPlaces, NodeLevels: liveLevels,
	})
	env, xPag := buildPaginatedEnvelope(r, places, "places", total, params.Limit, params.Offset, params.Fields)
	s.catalogCache.set(cacheKey, env, 5*time.Second, s.now())
	w.Header().Set("X-Pagination", xPag)
	writeCachedJSONWithModTime(w, r, "private, max-age=5", s.lastMutatedTime(), env)
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
	z := strings.TrimSpace(q.Get("zone"))
	if strings.EqualFold(z, "verra") || strings.EqualFold(z, "all") || z == "*" {
		z = ""
	}
	filter := worldstream.ViewportFilter{Zone: z}
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
	sel := parseLayerSelection(r)
	if sel.StreamLayers != 0 {
		s.pullWorldStreamSnapshot(r.Context(), filter, sel.StreamLayers)
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

func (s *Server) handleGMCommand(w http.ResponseWriter, r *http.Request, role Role) {
	if !checkJSONContentType(w, r) || !checkAcceptHeader(w, r) {
		return
	}
	rawBody, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 16<<10))
	if err != nil {
		writeProblem(w, r, http.StatusBadRequest, "invalid-json-body", "Request payload exceeds 16 KiB.")
		return
	}
	req, ok := parseFlexibleCommandRequest(w, r, rawBody)
	if !ok {
		return
	}
	if required := requiredRoleForCommand(req.Command); !role.AtLeast(required) {
		writeProblem(w, r, http.StatusForbidden, "insufficient-role", fmt.Sprintf("Command %q requires role %s.", req.Command, required))
		return
	}
	idemKey, hasIdem := extractIdempotencyKey(r)
	if !hasIdem {
		idemKey = fmt.Sprintf("auto-%d", s.subSeq.Add(1))
	}
	s.dispatchIdempotentCommandWithGRPC(w, r, role, idemKey, req)
}

func requiredRoleForCommand(cmd string) Role {
	if cmd == "set_node_level" || cmd == "set_node_stage" || cmd == "world_broadcast" || cmd == "reset_encounter" || cmd == "shutdown_zone" {
		return RoleArchitect
	}
	return RoleOperator
}

func (s *Server) applyCommandSideEffect(req CommandRequest) {
	if req.TargetID == "" {
		return
	}
	if req.Command == "despawn_entity" || req.Command == "despawn_npc" || req.Command == "kill_npc" {
		s.RemoveEntity(req.TargetID)
		return
	}
	if req.Command == "spawn_npc" {
		s.coalescer.UpsertEntity(worldstream.EntitySnapshot{
			ID: req.TargetID, Kind: "npc", Name: req.Command + ":" + req.TargetID,
			Zone: firstNonEmpty(req.Zone, "riverlands"), HealthPct: 100, Level: 50,
			State: "active", UpdatedAt: s.now().UnixMilli(),
		})
	}
}

func writeProblem(w http.ResponseWriter, r *http.Request, status int, code, detail string) {
	corrID := w.Header().Get("X-Correlation-ID")
	w.Header().Set("Content-Type", "application/problem+json; charset=utf-8")
	w.WriteHeader(status)
	if r != nil && r.Method == http.MethodHead {
		return
	}
	inst := ""
	if r != nil && r.URL != nil {
		inst = r.URL.Path
	}
	_ = json.NewEncoder(w).Encode(ProblemDetails{
		Type:          "https://sandalcore.dev/problems/" + code,
		Title:         http.StatusText(status),
		Status:        status,
		Detail:        detail,
		Instance:      inst,
		Code:          code,
		CorrelationID: corrID,
	})
}

func writeJSON(w http.ResponseWriter, status int, payload any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(payload)
}
