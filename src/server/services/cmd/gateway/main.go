// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// gateway serves the game client's ICS XClient stream over TLS (session,
// worlds, characters and Play) and, for world servers,
// aoc.gateway.v1.GatewayService (join-ticket redemption).
package main

import (
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"math/big"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/keepalive"

	authv1 "aocrop/services/gen/aoc/auth/v1"
	characterv1 "aocrop/services/gen/aoc/character/v1"
	gatewayv1 "aocrop/services/gen/aoc/gateway/v1"
	meshv1 "aocrop/services/gen/aoc/mesh/v1"
	icsxclient "aocrop/services/gen/ics/xclient"
	"aocrop/services/internal/character"
	"aocrop/services/internal/config"
	"aocrop/services/internal/db"
	"aocrop/services/internal/gatewaygrpc"
	"aocrop/services/internal/ics"
	"aocrop/services/internal/svcauth"
	"aocrop/services/internal/telemetry"
	"aocrop/services/internal/tickets"
)

const shutdownGrace = 10 * time.Second

var (
	gatewayH2CClient   = telemetry.NewH2CClient(5 * time.Second)
	gatewayHTTP1Client = &http.Client{Timeout: 5 * time.Second}
	gatewayLiveDeps    *ics.Deps
	gatewayLiveChars   *character.Store
)

// -- C | O | A . C_Y_B --

func main() {
	config.HandleHealthcheck("8080", "AOC_GATEWAY_HTTP_BIND_ADDR", "AOC_GATEWAY_HTTP_ADDR", "AOC_GATEWAY_ICS_ADDR", "AOC_GATEWAY_GRPC_BIND_ADDR", "AOC_GATEWAY_GRPC_ADDR", "50050", "443")
	origICS := strings.TrimSpace(os.Getenv("AOC_GATEWAY_ICS_ADDR"))
	tlsAddr := strings.TrimSpace(os.Getenv("AOC_GATEWAY_TLS_ADDR"))
	config.BindContainerFlags("gateway")
	if flagHTTP := strings.TrimSpace(os.Getenv("AOC_GATEWAY_ICS_ADDR")); flagHTTP != "" && strings.TrimSpace(os.Getenv("AOC_GATEWAY_HTTP_ADDR")) == "" {
		_ = os.Setenv("AOC_GATEWAY_HTTP_ADDR", flagHTTP)
	}
	if tlsAddr != "" {
		_ = os.Setenv("AOC_GATEWAY_ICS_ADDR", tlsAddr)
	} else if origICS != "" {
		_ = os.Setenv("AOC_GATEWAY_ICS_ADDR", origICS)
	}
	if err := run(); err != nil {
		slog.Error("gateway stopped", "err", err)
		os.Exit(1)
	}
}

func run() error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	cfg := config.Load()

	// What needs no database first: a gateway that cannot serve never runs
	// the migrations.
	if err := checkInternalBind(cfg.GatewayGRPCAddr, cfg.GatewayGRPCPublic); err != nil {
		return err
	}
	creds, err := icsCredentialsOrEphemeral(cfg.GatewayTLSCert, cfg.GatewayTLSKey)
	if err != nil {
		return err
	}
	token, err := svcauth.LoadOrEnv(cfg.ServiceTokenFile)
	if err != nil {
		return err
	}

	// Ctrl+C while the database is being reached is a stop, not a failure.
	pool, err := db.Open(ctx, cfg.DatabaseURL)
	if err != nil {
		return startupErr(ctx, err)
	}
	defer pool.Close()
	if err := db.Migrate(ctx, pool); err != nil {
		return startupErr(ctx, err)
	}
	ticketStore := tickets.NewStore(pool, cfg.TicketTTL)

	conns, err := dialServices(cfg, token)
	if err != nil {
		return err
	}
	defer conns.close()

	deps := ics.Deps{
		Accounts:   accounts{authv1.NewAuthServiceClient(conns.auth)},
		Characters: characters{characterv1.NewCharacterServiceClient(conns.character)},
		Placer:     placer{meshv1.NewMeshServiceClient(conns.mesh)},
		Tickets:    issuer{ticketStore},
	}
	gatewayLiveDeps = &deps
	gatewayLiveChars = character.NewStore(pool)
	icsSrv := newICSServer(creds, ics.New(deps, ics.DefaultConfig()))
	internal := newInternalServer(ticketStore, token)
	go purgeTickets(ctx, ticketStore)
	return serveGateway(ctx, stop, cfg, icsSrv, internal)
}

func serveGateway(ctx context.Context, stop context.CancelFunc, cfg config.Config, icsSrv, internal *grpc.Server) error {
	icsLis, err := telemetry.ListenDualStackTCP(ctx, cfg.GatewayICSAddr)
	if err != nil {
		return err
	}
	grpcLis, err := telemetry.ListenDualStackTCP(ctx, cfg.GatewayGRPCAddr)
	if err != nil {
		icsLis.Close()
		return err
	}
	errc := make(chan error, 3)
	var httpSrv *http.Server
	if httpAddr := resolveGatewayHTTPAddr(cfg.GatewayICSAddr); httpAddr != "" {
		httpLis, lerr := telemetry.ListenDualStackTCP(ctx, httpAddr)
		if lerr != nil {
			icsLis.Close()
			grpcLis.Close()
			return lerr
		}
		httpSrv = newGatewayHTTPServer(httpAddr, telemetry.NewMemoryIdempotencyLedger(0), nil)
		go func() { errc <- httpSrv.Serve(httpLis) }()
	}
	go func() { errc <- icsSrv.Serve(icsLis) }()
	go func() { errc <- internal.Serve(grpcLis) }()
	slog.Info("gateway listening", "ics", icsLis.Addr().String(), "grpc", grpcLis.Addr().String())

	select {
	case <-ctx.Done():
	case err = <-errc:
	}
	stop()
	if httpSrv != nil {
		sctx, cancel := context.WithTimeout(context.Background(), shutdownGrace)
		_ = httpSrv.Shutdown(sctx)
		cancel()
	}
	shutdown(shutdownGrace, icsSrv, internal)
	return err
}

// newInternalServer is the gRPC server world servers call (RedeemTicket). It
// requires the service token and enforces non-strict UUIDv7 idempotency replay.
func newInternalServer(store gatewaygrpc.Tickets, token svcauth.Token) *grpc.Server {
	ledger := telemetry.NewMemoryIdempotencyLedger(0)
	opts := append(token.ServerOptions(), grpc.ChainUnaryInterceptor(telemetry.UnaryIdempotencyInterceptor(ledger, false)))
	s := grpc.NewServer(opts...)
	gatewayv1.RegisterGatewayServiceServer(s, gatewaygrpc.New(store))
	return s
}

func newGatewayHTTPServer(addr string, ledger telemetry.IdempotencyLedger, next http.Handler) *http.Server {
	if next == nil {
		next = newGatewayHTTPMux(resolveAuthHTTPBaseURL(), resolveWorldEndpoint())
	}
	return telemetry.NewCleartextHTTPServer(addr, telemetry.NewIdempotencyMiddleware(ledger, false)(next))
}

func resolveGatewayHTTPAddr(icsAddr string) string {
	for _, k := range []string{"AOC_GATEWAY_HTTP_ADDR", "AOC_GATEWAY_HTTP_BIND_ADDR"} {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" && v != icsAddr {
			return v
		}
	}
	return ""
}

func resolveAuthHTTPBaseURL() string {
	for _, k := range []string{"AOC_AUTH_HTTP_ADDR", "AOC_AUTH_ADDR"} {
		if v := strings.TrimSpace(os.Getenv(k)); strings.HasPrefix(v, "http://") || strings.HasPrefix(v, "https://") {
			return strings.TrimRight(v, "/")
		}
	}
	return "http://auth:8081"
}

func resolveWorldEndpoint() string {
	host := strings.TrimSpace(os.Getenv("WORLDD_HOST"))
	if host == "" {
		host = "127.0.0.1"
	}
	port := strings.TrimSpace(os.Getenv("WORLDD_PORT"))
	if port == "" {
		port = "7777"
	}
	return net.JoinHostPort(host, port)
}

func newGatewayHTTPMux(authBaseURL, worldEndpoint string) http.Handler {
	return newGatewayHTTPMuxWithDeps(authBaseURL, worldEndpoint, gatewayLiveDeps, gatewayLiveChars)
}

func newGatewayHTTPMuxWithDeps(authBaseURL, worldEndpoint string, deps *ics.Deps, charStore *character.Store) http.Handler {
	mux := http.NewServeMux()
	bridge := newCharacterHTTPBridge(worldEndpoint, deps, charStore)
	health := func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	}
	for _, p := range []string{"/healthz", "/livez", "/readyz"} {
		mux.HandleFunc(p, health)
	}
	proxy := func(w http.ResponseWriter, r *http.Request) {
		proxyAuthHTTP(w, r, authBaseURL, bridge)
	}
	for _, p := range []string{
		"/api/v1/auth/register", "/v1/register",
		"/api/v1/auth/login", "/v1/login",
		"/api/v1/auth/account", "/v1/account",
		"/api/v1/auth/account/delete", "/v1/account/delete",
	} {
		mux.HandleFunc(p, proxy)
	}
	bridge.registerRoutes(mux)
	mux.HandleFunc("/api/v1/catalog/realms", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"realms": []map[string]any{
				{"id": "verra-alpha", "name": "Verra Alpha", "status": "online", "world_endpoint": worldEndpoint},
			},
		})
	})
	mux.HandleFunc("/api/v1/world/enter", func(w http.ResponseWriter, r *http.Request) {
		handleGatewayWorldEnter(w, r, worldEndpoint)
	})
	return telemetry.ETagMiddleware(mux)
}

func handleGatewayAuthProxy(w http.ResponseWriter, r *http.Request, authBaseURL string) {
	proxyAuthHTTP(w, r, authBaseURL, nil)
}

func proxyAuthHTTP(w http.ResponseWriter, r *http.Request, authBaseURL string, bridge *characterHTTPBridge) {
	if r.Method != http.MethodPost && r.Method != http.MethodDelete {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	var rawBody []byte
	if r.Body != nil {
		var err error
		rawBody, err = io.ReadAll(io.LimitReader(r.Body, 64*1024))
		if err != nil {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
	}
	bodyToSend, username := normalizeAuthRequestBody(rawBody)
	resp, err := doAuthProxyRequest(r.Context(), r.Method, authBaseURL+r.URL.Path, bodyToSend, r.Header)
	if err != nil {
		w.WriteHeader(http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()
	respBytes, _ := io.ReadAll(io.LimitReader(resp.Body, 64*1024))
	outBytes := enrichAuthResponseBody(resp.StatusCode, respBytes, username)
	syncBridgeAuthState(bridge, r, resp.StatusCode, outBytes, username)
	for _, hdr := range []string{"ETag", "Alt-Svc", "Cache-Control", "Idempotent-Replayed"} {
		if v := resp.Header.Get(hdr); v != "" {
			w.Header().Set(hdr, v)
		}
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(resp.StatusCode)
	_, _ = w.Write(outBytes)
}

func doAuthProxyRequest(ctx context.Context, method, targetURL string, body []byte, srcHeader http.Header) (*http.Response, error) {
	buildReq := func() (*http.Request, error) {
		req, err := http.NewRequestWithContext(ctx, method, targetURL, bytes.NewReader(body))
		if err != nil {
			return nil, err
		}
		req.Header.Set("Content-Type", "application/json")
		for _, hdr := range []string{"Authorization", "If-None-Match", "If-Match", "Idempotency-Key"} {
			if v := srcHeader.Get(hdr); v != "" {
				req.Header.Set(hdr, v)
			}
		}
		return req, nil
	}
	req, err := buildReq()
	if err != nil {
		return nil, err
	}
	if resp, derr := gatewayH2CClient.Do(req); derr == nil {
		return resp, nil
	} else if reqFallback, ferr := buildReq(); ferr == nil {
		return gatewayHTTP1Client.Do(reqFallback)
	} else {
		return nil, derr
	}
}

func syncBridgeAuthState(bridge *characterHTTPBridge, r *http.Request, status int, outBytes []byte, username string) {
	if bridge == nil || status < 200 || status >= 300 {
		return
	}
	path := r.URL.Path
	if r.Method == http.MethodDelete || strings.HasSuffix(path, "/account") || strings.HasSuffix(path, "/account/delete") {
		bridge.revokeAccountOrToken(r.Header.Get("Authorization"), username)
		return
	}
	var payload map[string]any
	if err := json.Unmarshal(outBytes, &payload); err != nil {
		return
	}
	for _, k := range []string{"access_token", "token"} {
		if tok, ok := payload[k].(string); ok && strings.TrimSpace(tok) != "" {
			bridge.recordIssuedToken(tok, username)
		}
	}
}

func normalizeAuthRequestBody(raw []byte) ([]byte, string) {
	var payload map[string]any
	if err := json.Unmarshal(raw, &payload); err != nil || payload == nil {
		return raw, ""
	}
	username, _ := payload["username"].(string)
	email, _ := payload["email"].(string)
	if strings.TrimSpace(email) == "" && strings.TrimSpace(username) != "" {
		if strings.Contains(username, "@") {
			payload["email"] = strings.TrimSpace(username)
		} else {
			payload["email"] = strings.TrimSpace(username) + "@sandalcore.local"
		}
		if out, err := json.Marshal(payload); err == nil {
			return out, username
		}
	}
	return raw, username
}

func enrichAuthResponseBody(status int, raw []byte, username string) []byte {
	if status < 200 || status >= 300 {
		return raw
	}
	var payload map[string]any
	if err := json.Unmarshal(raw, &payload); err != nil || payload == nil {
		return raw
	}
	if tok, ok := payload["access_token"].(string); ok && tok != "" && payload["token"] == nil {
		payload["token"] = tok
	}
	if payload["account_id"] == nil {
		payload["account_id"] = 1
	}
	if username != "" && payload["username"] == nil {
		payload["username"] = username
	}
	if out, err := json.Marshal(payload); err == nil {
		return out
	}
	return raw
}

func handleGatewayWorldEnter(w http.ResponseWriter, r *http.Request, worldEndpoint string) {
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	tok := strings.TrimSpace(strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer"))
	if tok == "" {
		var body map[string]any
		_ = json.NewDecoder(io.LimitReader(r.Body, 16*1024)).Decode(&body)
		for _, k := range []string{"join_ticket", "ticket", "token", "access_token"} {
			if v, ok := body[k].(string); ok && strings.TrimSpace(v) != "" {
				tok = strings.TrimSpace(v)
				break
			}
		}
	}
	w.Header().Set("Content-Type", "application/json")
	if tok == "" {
		w.WriteHeader(http.StatusUnauthorized)
		_, _ = w.Write([]byte(`{"status":"DENY","error":"missing ticket or bearer token"}`))
		return
	}
	_ = json.NewEncoder(w).Encode(map[string]any{
		"status":         "ALLOW",
		"world_endpoint": worldEndpoint,
		"zone_key":       "verra-01",
	})
}

// shutdown drains the servers within one budget, then cuts what is left: the
// client's ICS stream stays open for as long as the game runs.
func shutdown(grace time.Duration, servers ...*grpc.Server) {
	force := time.AfterFunc(grace, func() {
		for _, s := range servers {
			s.Stop()
		}
	})
	defer force.Stop()
	var wg sync.WaitGroup
	for _, s := range servers {
		wg.Add(1)
		go func() { defer wg.Done(); s.GracefulStop() }()
	}
	wg.Wait()
}

// built with C-O-A and Cyb

// checkInternalBind refuses an internal gRPC address beyond loopback unless
// public (AOC_GATEWAY_GRPC_PUBLIC=1) allows it: internal gRPC has no TLS, so
// the service token RedeemTicket requires would cross the network in clear
// text, as the join ticket already does in the client's UDP handshake.
func checkInternalBind(addr string, public bool) error {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return fmt.Errorf("AOC_GATEWAY_GRPC_ADDR %q: %w", addr, err)
	}
	if ip := net.ParseIP(host); (ip != nil && ip.IsLoopback()) || strings.EqualFold(host, "localhost") {
		return nil
	}
	if !public {
		return fmt.Errorf("AOC_GATEWAY_GRPC_ADDR %q is not a loopback address, and internal gRPC has no TLS "+
			"(set AOC_GATEWAY_GRPC_PUBLIC=1 to serve it there anyway)", addr)
	}
	slog.Warn("gateway: RedeemTicket served beyond loopback (AOC_GATEWAY_GRPC_PUBLIC=1); internal gRPC has no TLS, the service token travels in clear text", "addr", addr)
	return nil
}

// icsCredentials loads the ICS listener's certificate and key. Failed
// handshakes are logged (see loggedCreds).
func icsCredentials(certFile, keyFile string) (credentials.TransportCredentials, error) {
	creds, err := credentials.NewServerTLSFromFile(certFile, keyFile)
	if err != nil {
		return nil, fmt.Errorf("ICS TLS certificate: %w", err)
	}
	return loggedCreds{creds}, nil
}

func icsCredentialsOrEphemeral(certFile, keyFile string) (credentials.TransportCredentials, error) {
	creds, err := icsCredentials(certFile, keyFile)
	if err == nil {
		return creds, nil
	}
	if _, cerr := os.Stat(certFile); errors.Is(cerr, os.ErrNotExist) {
		if _, kerr := os.Stat(keyFile); errors.Is(kerr, os.ErrNotExist) {
			if altCreds, aerr := icsCredentials("/app/certs/server.pem", "/app/certs/server.key"); aerr == nil {
				return altCreds, nil
			}
			return ephemeralICSCredentials()
		}
	}
	return nil, err
}

func ephemeralICSCredentials() (credentials.TransportCredentials, error) {
	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, fmt.Errorf("ICS ephemeral key: %w", err)
	}
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "sandalcore-gateway"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(365 * 24 * time.Hour),
		KeyUsage:     x509.KeyUsageKeyEncipherment | x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames:     []string{"localhost", "gateway"},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &priv.PublicKey, priv)
	if err != nil {
		return nil, fmt.Errorf("ICS ephemeral certificate: %w", err)
	}
	slog.Info("gateway using ephemeral in-memory self-signed TLS certificate for ICS listener")
	return loggedCreds{credentials.NewTLS(&tls.Config{
		Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: priv}},
		MinVersion:   tls.VersionTLS12,
	})}, nil
}

// newICSServer is the TLS gRPC server for the game client, which speaks
// grpc-c++ with ALPN h2 (grpc-go negotiates it). The client may ping often:
// grpc-go would otherwise close a connection that pings more than every five
// minutes, and the client's keep-alive settings are not known.
func newICSServer(creds credentials.TransportCredentials, srv icsxclient.XClientServiceServer) *grpc.Server {
	s := grpc.NewServer(
		grpc.Creds(creds),
		grpc.KeepaliveEnforcementPolicy(keepalive.EnforcementPolicy{MinTime: 5 * time.Second, PermitWithoutStream: true}),
	)
	icsxclient.RegisterXClientServiceServer(s, srv)
	return s
}

// purgeTickets deletes expired join tickets now and then hourly until ctx ends.
func purgeTickets(ctx context.Context, store *tickets.Store) {
	tick := time.NewTicker(time.Hour)
	defer tick.Stop()
	for {
		n, err := store.Purge(ctx)
		switch {
		case err != nil && ctx.Err() == nil:
			slog.Warn("join ticket purge failed", "err", err)
		case n > 0:
			slog.Info("purged expired join tickets", "count", n)
		}
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
	}
}

// startupErr is nil when the stop signal is what ended startup.
func startupErr(ctx context.Context, err error) error {
	if ctx.Err() != nil {
		return nil
	}
	return err
}
