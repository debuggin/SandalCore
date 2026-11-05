// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

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
	"encoding/pem"
	"io"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	gatewayv1 "aocrop/services/gen/aoc/gateway/v1"
	icscommon "aocrop/services/gen/ics/common"
	icsxclient "aocrop/services/gen/ics/xclient"
	"aocrop/services/internal/gatewaygrpc"
	"aocrop/services/internal/ics"
	"aocrop/services/internal/svcauth"
	"aocrop/services/internal/telemetry"
	"aocrop/services/internal/tickets"
)

// CoA captures, C-Y-B code

// selfSigned writes a certificate and key for 127.0.0.1 and returns a trusting CertPool.
func selfSigned(t *testing.T) (certFile, keyFile string, pool *x509.CertPool) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	tmpl := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: "test gateway"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(time.Hour),
		IPAddresses:           []net.IP{net.ParseIP("127.0.0.1")},
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		IsCA:                  true,
		BasicConstraintsValid: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	certFile, keyFile = filepath.Join(dir, "server.pem"), filepath.Join(dir, "server.key")
	if err := os.WriteFile(certFile, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyFile, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER}), 0o600); err != nil {
		t.Fatal(err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	pool = x509.NewCertPool()
	pool.AddCert(cert)
	return certFile, keyFile, pool
}

// startICS serves an ICS server with certFile and keyFile on a loopback port.
func startICS(t *testing.T, certFile, keyFile string) (*grpc.Server, string) {
	t.Helper()
	creds, err := icsCredentials(certFile, keyFile)
	if err != nil {
		t.Fatal(err)
	}
	srv := newICSServer(creds, ics.New(ics.Deps{}, ics.DefaultConfig()))
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	return srv, lis.Addr().String()
}

// keepAlive opens an ICS stream to addr and verifies one keep-alive round-trip.
func keepAlive(t *testing.T, addr string, pool *x509.CertPool) icsxclient.XClientService_ProcessXClientMessageClient {
	t.Helper()
	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(credentials.NewTLS(&tls.Config{RootCAs: pool})))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	t.Cleanup(cancel)
	stream, err := icsxclient.NewXClientServiceClient(conn).ProcessXClientMessage(ctx)
	if err != nil {
		t.Fatal(err)
	}
	data, err := proto.Marshal(&icscommon.KeepAliveMessage{TimestampMsec: 1})
	if err != nil {
		t.Fatal(err)
	}
	if err := stream.Send(&icscommon.MessageWrapper{MessageTypeName: "ics_common.KeepAliveMessage", MessageData: data}); err != nil {
		t.Fatal(err)
	}
	m, err := stream.Recv()
	if err != nil || m.GetMessageTypeName() != "ics_common.KeepAliveMessage" {
		t.Fatalf("Recv = %v %v, want a KeepAliveMessage", m, err)
	}
	return stream
}

// The client reaches the stream over TLS (ALPN h2) on an IP-literal address.
func TestICSServerSpeaksTLSAndStreams(t *testing.T) {
	certFile, keyFile, pool := selfSigned(t)
	_, addr := startICS(t, certFile, keyFile)
	keepAlive(t, addr, pool)
}

func TestICSServerNeedsItsCertificate(t *testing.T) {
	dir := t.TempDir()
	_, err := icsCredentials(filepath.Join(dir, "missing.pem"), filepath.Join(dir, "missing.key"))
	if err == nil || !strings.Contains(err.Error(), "certificate") {
		t.Errorf("icsCredentials with missing files = %v, want an error about the certificate", err)
	}
}

// The key is missing (as in every worktree): a clear error naming it.
func TestICSServerNeedsItsKey(t *testing.T) {
	certFile, _, _ := selfSigned(t)
	creds, err := icsCredentials(certFile, filepath.Join(t.TempDir(), "server.key"))
	if creds != nil || err == nil || !strings.Contains(err.Error(), "server.key") {
		t.Errorf("icsCredentials without the key = %v %v", creds, err)
	}
}

// ~ C:Y:B & C.O.A ~

// Failed TLS handshakes (non-TLS, missing ALPN, bad cert) are logged at WARN.
func TestTLSHandshakeFailuresAreLogged(t *testing.T) {
	logs := captureLog(t)
	certFile, keyFile, pool := selfSigned(t)
	_, addr := startICS(t, certFile, keyFile)

	// h2 is fine, and not logged.
	keepAlive(t, addr, pool)
	if strings.Contains(logs.String(), "handshake failed") {
		t.Errorf("a good handshake was logged:\n%s", logs.String())
	}

	// waitFor waits up to 2 s for the log to contain want.
	waitFor := func(want string) string {
		deadline := time.Now().Add(2 * time.Second)
		for !strings.Contains(logs.String(), want) && time.Now().Before(deadline) {
			time.Sleep(10 * time.Millisecond)
		}
		return logs.String()
	}
	const failed = `level=WARN msg="ics: TLS handshake failed" remote=127.0.0.1:`

	// A client that does not speak TLS at all.
	raw, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	raw.Write([]byte("GET / HTTP/1.1\r\n\r\n"))
	raw.SetReadDeadline(time.Now().Add(2 * time.Second))
	raw.Read(make([]byte, 1)) // until the server closes the connection
	raw.Close()
	if got := waitFor(failed); !strings.Contains(got, failed) {
		t.Errorf("log after a client that does not speak TLS:\n%s", got)
	}

	// A client that offers no ALPN unless GRPC_ENFORCE_ALPN_ENABLED=false.
	if strings.EqualFold(os.Getenv("GRPC_ENFORCE_ALPN_ENABLED"), "false") {
		t.Log("GRPC_ENFORCE_ALPN_ENABLED=false: a client without ALPN is accepted")
		return
	}
	c, err := tls.DialWithDialer(&net.Dialer{Timeout: 2 * time.Second}, "tcp", addr, &tls.Config{RootCAs: pool})
	if err == nil {
		c.SetReadDeadline(time.Now().Add(2 * time.Second))
		c.Read(make([]byte, 1))
		c.Close()
	}
	if got := waitFor("ALPN"); !strings.Contains(got, failed) || !strings.Contains(got, "ALPN") {
		t.Errorf("log after a handshake without ALPN:\n%s", got)
	}
}

// hangingRedeemer holds each RedeemTicket until its call ends.
type hangingRedeemer struct{ started chan struct{} }

func (h hangingRedeemer) Redeem(ctx context.Context, _, _ string, _ uint32) (int64, error) {
	h.started <- struct{}{}
	<-ctx.Done()
	return 0, ctx.Err()
}

// startInternal serves newInternalServer(store) on a loopback port.
func startInternal(t *testing.T, store gatewaygrpc.Tickets) (*grpc.Server, string) {
	t.Helper()
	srv := newInternalServer(store, testToken)
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	return srv, lis.Addr().String()
}

// refuses reports whether nothing accepts connections at addr any more.
func refuses(addr string) bool {
	c, err := net.DialTimeout("tcp", addr, time.Second)
	if err == nil {
		c.Close()
	}
	return err != nil
}

// Shutdown cuts live ICS streams and stuck RedeemTicket calls after the grace period.
func TestShutdownCutsLiveStreams(t *testing.T) {
	certFile, keyFile, pool := selfSigned(t)
	icsSrv, addr := startICS(t, certFile, keyFile)
	started := make(chan struct{}, 1)
	internal, internalAddr := startInternal(t, hangingRedeemer{started})
	stream := keepAlive(t, addr, pool)

	cc, err := grpc.NewClient(internalAddr, grpc.WithTransportCredentials(insecure.NewCredentials()), testToken.DialOption())
	if err != nil {
		t.Fatal(err)
	}
	defer cc.Close()
	redeemed := make(chan error, 1)
	go func() {
		_, err := gatewayv1.NewGatewayServiceClient(cc).RedeemTicket(context.Background(), &gatewayv1.RedeemTicketRequest{})
		redeemed <- err
	}()
	<-started

	done := make(chan struct{})
	go func() { shutdown(200*time.Millisecond, icsSrv, internal); close(done) }()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("shutdown still waiting for live calls after 3s")
	}
	if _, err := stream.Recv(); err == nil {
		t.Error("the ICS stream outlived shutdown")
	}
	select {
	case err := <-redeemed:
		if err == nil {
			t.Error("the stuck RedeemTicket succeeded")
		}
	case <-time.After(2 * time.Second):
		t.Error("the stuck RedeemTicket outlived shutdown")
	}
	if !refuses(addr) || !refuses(internalAddr) {
		t.Error("a server still accepts connections after shutdown")
	}
}

// Idle servers stop immediately without waiting for the grace period.
func TestShutdownOfIdleServersIsPrompt(t *testing.T) {
	certFile, keyFile, _ := selfSigned(t)
	icsSrv, addr := startICS(t, certFile, keyFile)
	internal, internalAddr := startInternal(t, tickets.NewStore(nil, 2*time.Minute))
	done := make(chan struct{})
	go func() { shutdown(time.Minute, icsSrv, internal); close(done) }()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("shutdown of idle servers still running after 3s")
	}
	if !refuses(addr) || !refuses(internalAddr) {
		t.Error("a server still accepts connections after shutdown")
	}
}

// ~ C | Y | B :: CoA ~

// World servers reach RedeemTicket on the internal server with the service token.
func TestInternalServerServesRedeemTicket(t *testing.T) {
	srv := newInternalServer(tickets.NewStore(nil, 2*time.Minute), testToken)
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	redeem := func(opts ...grpc.DialOption) error {
		opts = append(opts, grpc.WithTransportCredentials(insecure.NewCredentials()))
		cc, err := grpc.NewClient(lis.Addr().String(), opts...)
		if err != nil {
			t.Fatal(err)
		}
		defer cc.Close()
		_, err = gatewayv1.NewGatewayServiceClient(cc).RedeemTicket(context.Background(), &gatewayv1.RedeemTicketRequest{})
		return err
	}
	if err := redeem(testToken.DialOption()); status.Code(err) != codes.PermissionDenied {
		t.Errorf("RedeemTicket = %v, want PermissionDenied from the ticket store", err)
	}
	if err := redeem(); status.Code(err) != codes.Unauthenticated {
		t.Errorf("RedeemTicket without the service token = %v, want Unauthenticated", err)
	}
}

// testToken stands in for dist/services/service.token.
const testToken = svcauth.Token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

// Internal server binds loopback only unless AOC_GATEWAY_GRPC_PUBLIC=1.
func TestInternalBindMustBeLoopback(t *testing.T) {
	for _, addr := range []string{"127.0.0.1:50054", "127.1.2.3:1", "[::1]:50054", "localhost:50054", "LocalHost:1"} {
		if err := checkInternalBind(addr, false); err != nil {
			t.Errorf("checkInternalBind(%q) = %v, want nil", addr, err)
		}
	}
	public := []string{"0.0.0.0:50054", ":50054", "[::]:50054", "192.0.2.5:50054", "198.51.100.2:1", "gateway.lan:50054", "[fe80::1%eth0]:1"}
	for _, addr := range public {
		err := checkInternalBind(addr, false)
		if err == nil || !strings.Contains(err.Error(), "AOC_GATEWAY_GRPC_PUBLIC=1") {
			t.Errorf("checkInternalBind(%q) = %v, want a refusal naming AOC_GATEWAY_GRPC_PUBLIC=1", addr, err)
		}
	}
	if err := checkInternalBind("no-port", true); err == nil {
		t.Error("checkInternalBind(no-port) = nil, want an error")
	}

	logs := captureLog(t)
	if err := checkInternalBind("127.0.0.1:50054", true); err != nil || logs.String() != "" {
		t.Errorf("loopback with the flag = %v, log %q; want nil and no warning", err, logs.String())
	}
	for _, addr := range public {
		if err := checkInternalBind(addr, true); err != nil {
			t.Errorf("checkInternalBind(%q) with the flag = %v, want nil", addr, err)
		}
	}
	if n := strings.Count(logs.String(), "level=WARN"); n != len(public) || !strings.Contains(logs.String(), "no TLS") {
		t.Errorf("%d warnings for %d public addresses:\n%s", n, len(public), logs.String())
	}
}

// -- C.O.A & C-Y-B --

// run checks internal bind and TLS files before reaching the database.
func TestRunChecksBeforeTheDatabase(t *testing.T) {
	certFile, keyFile, _ := selfSigned(t)
	missingKey := filepath.Join(t.TempDir(), "server.key")
	for _, tc := range []struct {
		name, grpcAddr, key, want string
	}{
		{"public internal address", "0.0.0.0:50054", missingKey, "AOC_GATEWAY_GRPC_PUBLIC"},
		{"missing key", "127.0.0.1:0", missingKey, "server.key"},
		{"missing service token", "127.0.0.1:0", keyFile, "service token"},
	} {
		t.Setenv("AOC_DATABASE_URL", "postgres://aoc:aoc@"+closedPort(t)+"/aoc?sslmode=disable&connect_timeout=2")
		t.Setenv("AOC_GATEWAY_GRPC_ADDR", tc.grpcAddr)
		t.Setenv("AOC_GATEWAY_GRPC_PUBLIC", "")
		t.Setenv("AOC_GATEWAY_TLS_CERT", certFile)
		t.Setenv("AOC_GATEWAY_TLS_KEY", tc.key)
		t.Setenv("AOC_SERVICE_TOKEN_FILE", filepath.Join(t.TempDir(), "service.token"))
		if err := run(); err == nil || !strings.Contains(err.Error(), tc.want) || strings.Contains(err.Error(), "database") {
			t.Errorf("%s: run() = %v, want an error about %s before the database", tc.name, err, tc.want)
		}
	}
}

func TestGatewayHTTPMuxETagAndPreconditions(t *testing.T) {
	mux := newGatewayHTTPMux("http://127.0.0.1:0", "127.0.0.1:7777")
	for _, path := range []string{"/healthz", "/api/v1/catalog/realms"} {
		req := httptest.NewRequest(http.MethodGet, path, nil)
		rec := httptest.NewRecorder()
		mux.ServeHTTP(rec, req)
		if rec.Code != http.StatusOK {
			t.Fatalf("GET %s = %d, want 200", path, rec.Code)
		}
		etag := rec.Header().Get("ETag")
		if etag == "" || !strings.HasPrefix(etag, `"sha256-`) {
			t.Fatalf("GET %s ETag = %q, want strong sha256 ETag", path, etag)
		}
		req304 := httptest.NewRequest(http.MethodGet, path, nil)
		req304.Header.Set("If-None-Match", etag)
		rec304 := httptest.NewRecorder()
		mux.ServeHTTP(rec304, req304)
		if rec304.Code != http.StatusNotModified {
			t.Fatalf("GET %s If-None-Match = %d, want 304", path, rec304.Code)
		}
		req412 := httptest.NewRequest(http.MethodGet, path, nil)
		req412.Header.Set("If-Match", `"sha256-mismatch"`)
		rec412 := httptest.NewRecorder()
		mux.ServeHTTP(rec412, req412)
		if rec412.Code != http.StatusPreconditionFailed {
			t.Fatalf("GET %s If-Match mismatch = %d, want 412", path, rec412.Code)
		}
	}
}

func TestGatewayH2CClientAndDeleteAccountProxy(t *testing.T) {
	var seenProtoMajor int
	var seenMethod, seenPath, seenAuth, seenIfMatch string
	authMux := http.NewServeMux()
	authMux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		seenProtoMajor, seenMethod, seenPath = r.ProtoMajor, r.Method, r.URL.Path
		seenAuth, seenIfMatch = r.Header.Get("Authorization"), r.Header.Get("If-Match")
		w.Header().Set("ETag", `"sha256-upstream"`)
		w.Header().Set("Alt-Svc", `h3=":8081"; ma=86400, h2c=":8081"; ma=86400`)
		w.Header().Set("Content-Type", "application/json")
		if r.Method == http.MethodDelete || strings.HasSuffix(r.URL.Path, "/delete") {
			_, _ = w.Write([]byte(`{"deleted":true,"status":"deleted"}`))
			return
		}
		_, _ = w.Write([]byte(`{"access_token":"tok-h2c-123456"}`))
	})
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	authSrv := telemetry.NewCleartextHTTPServer(lis.Addr().String(), authMux)
	go func() { _ = authSrv.Serve(lis) }()
	t.Cleanup(func() { _ = authSrv.Close() })

	gwMux := newGatewayHTTPMux("http://"+lis.Addr().String(), "127.0.0.1:7777")
	req := httptest.NewRequest(http.MethodDelete, "/api/v1/auth/account", bytes.NewReader([]byte(`{"username":"temp_user"}`)))
	req.Header.Set("Authorization", "Bearer tok-h2c-123456")
	req.Header.Set("If-Match", "*")
	rec := httptest.NewRecorder()
	gwMux.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK || seenProtoMajor != 2 || seenMethod != http.MethodDelete || seenPath != "/api/v1/auth/account" {
		t.Fatalf("DELETE proxy code=%d proto=%d method=%s path=%s", rec.Code, seenProtoMajor, seenMethod, seenPath)
	}
	if seenAuth != "Bearer tok-h2c-123456" || seenIfMatch != "*" || rec.Header().Get("ETag") == "" || rec.Header().Get("Alt-Svc") == "" {
		t.Fatalf("headers auth=%q ifMatch=%q etag=%q altSvc=%q", seenAuth, seenIfMatch, rec.Header().Get("ETag"), rec.Header().Get("Alt-Svc"))
	}
}

func TestGatewayCharacterSelectHTTPBridge(t *testing.T) {
	mux := newGatewayHTTPMux("http://127.0.0.1:0", "127.0.0.1:7777")
	const bearer = "Bearer valid_session_token_abc123"
	doJSON := func(method, path, body string) (int, map[string]any, http.Header) {
		var rdr io.Reader
		if body != "" {
			rdr = strings.NewReader(body)
		}
		req := httptest.NewRequest(method, path, rdr)
		req.Header.Set("Authorization", bearer)
		req.Header.Set("Content-Type", "application/json")
		rec := httptest.NewRecorder()
		mux.ServeHTTP(rec, req)
		var out map[string]any
		_ = json.Unmarshal(rec.Body.Bytes(), &out)
		return rec.Code, out, rec.Header()
	}
	code, created, _ := doJSON(http.MethodPost, "/api/v1/characters", `{"name":"Kaelen","class_id":1,"race_id":1}`)
	charID, _ := created["id"].(string)
	if code != http.StatusCreated || charID == "" {
		t.Fatalf("POST /api/v1/characters = %d %v", code, created)
	}
	code, listed, hdr := doJSON(http.MethodGet, "/api/v1/characters", "")
	if code != http.StatusOK || hdr.Get("ETag") == "" {
		t.Fatalf("GET /api/v1/characters = %d etag=%q body=%v", code, hdr.Get("ETag"), listed)
	}
	code, sel, _ := doJSON(http.MethodPost, "/api/v1/characters/"+charID+"/select", `{}`)
	joinTicket, _ := sel["join_ticket"].(string)
	if code != http.StatusOK || len(joinTicket) != 32 {
		t.Fatalf("select character = %d ticket=%q body=%v", code, joinTicket, sel)
	}
	code, enter, _ := doJSON(http.MethodPost, "/api/v1/world/enter", `{"join_ticket":"`+joinTicket+`"}`)
	if code != http.StatusOK || enter["status"] != "ALLOW" {
		t.Fatalf("world/enter = %d %v", code, enter)
	}
}

