// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package telemetry

import (
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/google/uuid"
	"golang.org/x/net/http2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"

	gatewayv1 "aocrop/services/gen/aoc/gateway/v1"
)

func TestResolveDualStackAddrs(t *testing.T) {
	ctx := context.Background()
	v4, err := ResolveDualStackAddrs(ctx, "127.0.0.1:8080")
	if err != nil || len(v4) != 1 || !v4[0].Addr().Is4() || v4[0].Port() != 8080 {
		t.Fatalf("ResolveDualStackAddrs(127.0.0.1:8080) = %v, %v", v4, err)
	}
	v6, err := ResolveDualStackAddrs(ctx, "[::1]:8443")
	if err != nil || len(v6) != 1 || !v6[0].Addr().Is6() || v6[0].Port() != 8443 {
		t.Fatalf("ResolveDualStackAddrs([::1]:8443) = %v, %v", v6, err)
	}
	local, err := ResolveDualStackAddrs(ctx, "localhost:443")
	if err != nil || len(local) == 0 {
		t.Fatalf("ResolveDualStackAddrs(localhost:443) = %v, %v", local, err)
	}
	seenV4 := false
	for _, ap := range local {
		if ap.Addr().Is4() {
			seenV4 = true
		} else if ap.Addr().Is6() && seenV4 {
			t.Fatalf("expected IPv6 addresses ordered before IPv4: %v", local)
		}
	}
	for _, bad := range []string{"localhost", ":8080", "localhost:0", "localhost:99999", "localhost:abc"} {
		if _, err := ResolveDualStackAddrs(ctx, bad); err == nil {
			t.Fatalf("expected error for invalid hostPort %q", bad)
		}
	}
}

func TestMultiProtocolServerLifecycle(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv, err := NewMultiProtocolServer(ctx, MultiProtocolConfig{
		HTTPBindAddr: "127.0.0.1:0", HTTPSBindAddr: "127.0.0.1:0", QUICBindAddr: "127.0.0.1:0",
		TLSConfig: mustSelfSignedTLS(t),
	}, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(r.Proto))
	}))
	if err != nil {
		t.Fatalf("NewMultiProtocolServer: %v", err)
	}
	if srv.TLSConfig().MinVersion != tls.VersionTLS13 {
		t.Fatalf("expected TLS 1.3 MinVersion, got %x", srv.TLSConfig().MinVersion)
	}
	errCh := make(chan error, 1)
	go func() { errCh <- srv.Serve(ctx) }()
	assertCleartextAndH2C(t, srv.HTTPAddr())
	assertTLS13AndQUIC(t, srv.HTTPSAddr(), srv.QUICAddr())
	cancel()
	if serveErr := <-errCh; serveErr != nil {
		t.Fatalf("Serve returned error on shutdown: %v", serveErr)
	}
}

func assertCleartextAndH2C(t *testing.T, httpAddr string) {
	t.Helper()
	resp, err := http.Get("http://" + httpAddr + "/healthz")
	if err != nil {
		t.Fatalf("HTTP/1.1 GET: %v", err)
	}
	_ = resp.Body.Close()
	if resp.StatusCode != http.StatusOK || resp.ProtoMajor != 1 {
		t.Fatalf("HTTP/1.1 unexpected status=%d proto=%s", resp.StatusCode, resp.Proto)
	}
	h2Client := &http.Client{Transport: &http2.Transport{
		AllowHTTP: true,
		DialTLSContext: func(ctx context.Context, network, addr string, _ *tls.Config) (net.Conn, error) {
			return (&net.Dialer{}).DialContext(ctx, network, addr)
		},
	}}
	h2Resp, err := h2Client.Get("http://" + httpAddr + "/healthz")
	if err != nil {
		t.Fatalf("h2c GET: %v", err)
	}
	_ = h2Resp.Body.Close()
	if h2Resp.StatusCode != http.StatusOK || h2Resp.ProtoMajor != 2 {
		t.Fatalf("h2c unexpected status=%d proto=%s", h2Resp.StatusCode, h2Resp.Proto)
	}
}

func assertTLS13AndQUIC(t *testing.T, httpsAddr, quicAddr string) {
	t.Helper()
	tlsClient := &http.Client{Transport: &http.Transport{TLSClientConfig: &tls.Config{
		InsecureSkipVerify: true, MinVersion: tls.VersionTLS13, MaxVersion: tls.VersionTLS13,
	}}}
	resp, err := tlsClient.Get("https://" + httpsAddr + "/readyz")
	if err != nil {
		t.Fatalf("TLS 1.3 GET: %v", err)
	}
	_ = resp.Body.Close()
	if resp.TLS == nil || resp.TLS.Version != tls.VersionTLS13 {
		t.Fatalf("expected TLS 1.3 connection, got %+v", resp.TLS)
	}
	if altSvc := resp.Header.Get("Alt-Svc"); !strings.HasPrefix(altSvc, `h3=":`) {
		t.Fatalf("expected Alt-Svc h3 advertisement, got %q", altSvc)
	}
	udpConn, err := net.DialTimeout("udp", quicAddr, 2*time.Second)
	if err != nil {
		t.Fatalf("UDP QUIC dial: %v", err)
	}
	defer udpConn.Close()
	_ = udpConn.SetDeadline(time.Now().Add(2 * time.Second))
	if _, err := udpConn.Write([]byte("ping")); err != nil {
		t.Fatalf("UDP QUIC write: %v", err)
	}
	buf := make([]byte, 128)
	n, err := udpConn.Read(buf)
	if err != nil || !strings.HasPrefix(string(buf[:n]), "h3:") {
		t.Fatalf("UDP QUIC read = %q, %v", string(buf[:n]), err)
	}
}

func TestHTTPIdempotencyMiddleware(t *testing.T) {
	ledger := NewMemoryIdempotencyLedger(time.Minute)
	var calls atomic.Int32
	base := http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		n := calls.Add(1)
		w.Header().Set("X-Call-Count", "1")
		w.WriteHeader(http.StatusCreated)
		_, _ = w.Write([]byte(`{"seq":` + string(rune('0'+n)) + `}`))
	})
	handler := NewIdempotencyMiddleware(ledger, false)(base)
	v7Key, err := uuid.NewV7()
	if err != nil {
		t.Fatalf("uuid.NewV7: %v", err)
	}
	if digest := ComputePayloadSHA256([]byte(`{"op":"create"}`)); len(digest) != 32 {
		t.Fatalf("expected 32-byte SHA-256, got %d", len(digest))
	}

	r1 := sendIdempotentHTTP(handler, http.MethodPost, v7Key.String(), `{"op":"create"}`)
	if r1.Code != http.StatusCreated || calls.Load() != 1 || r1.Header().Get(IdempotentReplayHeader) != "" {
		t.Fatalf("first request status=%d calls=%d", r1.Code, calls.Load())
	}
	r2 := sendIdempotentHTTP(handler, http.MethodPost, v7Key.String(), `{"op":"create"}`)
	if r2.Code != http.StatusCreated || calls.Load() != 1 || r2.Header().Get(IdempotentReplayHeader) != "true" || r2.Body.String() != r1.Body.String() {
		t.Fatalf("replay status=%d calls=%d replay=%q body=%q", r2.Code, calls.Load(), r2.Header().Get(IdempotentReplayHeader), r2.Body.String())
	}
	rConflict := sendIdempotentHTTP(handler, http.MethodPost, v7Key.String(), `{"op":"mutated"}`)
	if rConflict.Code != http.StatusConflict || calls.Load() != 1 || !strings.HasPrefix(rConflict.Header().Get("Content-Type"), ProblemMediaType) {
		t.Fatalf("conflict status=%d content-type=%q", rConflict.Code, rConflict.Header().Get("Content-Type"))
	}
	rV4 := sendIdempotentHTTP(handler, http.MethodPost, uuid.New().String(), `{"op":"create"}`)
	if rV4.Code != http.StatusBadRequest || !strings.HasPrefix(rV4.Header().Get("Content-Type"), ProblemMediaType) {
		t.Fatalf("UUIDv4 expected 400 problem+json, got %d (%q)", rV4.Code, rV4.Header().Get("Content-Type"))
	}
	var prob ProblemDetails
	if err := json.Unmarshal(rV4.Body.Bytes(), &prob); err != nil || prob.Status != http.StatusBadRequest {
		t.Fatalf("decode ProblemDetails: %+v, %v", prob, err)
	}
	if rMissing := sendIdempotentHTTP(NewIdempotencyMiddleware(ledger, true)(base), http.MethodPost, "", `{}`); rMissing.Code != http.StatusBadRequest {
		t.Fatalf("strict mode missing key expected 400, got %d", rMissing.Code)
	}
}

func sendIdempotentHTTP(h http.Handler, method, key, body string) *httptest.ResponseRecorder {
	req := httptest.NewRequest(method, "/v1/commands", bytes.NewBufferString(body))
	if key != "" {
		req.Header.Set(IdempotencyHeaderKey, key)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

type fakeReqID struct{ ID, Body string }

func (f fakeReqID) GetRequestId() string { return f.ID }

func TestGRPCUnaryIdempotencyInterceptor(t *testing.T) {
	interceptor := UnaryIdempotencyInterceptor(NewMemoryIdempotencyLedger(time.Minute), true)
	info := &grpc.UnaryServerInfo{FullMethod: "/aoc.gateway.v1.GatewayService/RedeemTicket"}
	var calls atomic.Int32
	handler := func(_ context.Context, _ any) (any, error) {
		calls.Add(1)
		return &gatewayv1.RedeemTicketResponse{AccountId: 42, CharacterId: "char-1"}, nil
	}
	v7Key, err := uuid.NewV7()
	if err != nil {
		t.Fatalf("uuid.NewV7: %v", err)
	}
	ctx := metadata.NewIncomingContext(context.Background(), metadata.Pairs("idempotency-key", v7Key.String()))
	req1 := &gatewayv1.RedeemTicketRequest{Ticket: "ticket-alpha"}
	if _, err := interceptor(ctx, req1, info, handler); err != nil || calls.Load() != 1 {
		t.Fatalf("first gRPC call err=%v calls=%d", err, calls.Load())
	}
	if _, err := interceptor(ctx, req1, info, handler); err != nil || calls.Load() != 1 {
		t.Fatalf("replay gRPC call err=%v calls=%d", err, calls.Load())
	}
	if _, err := interceptor(ctx, &gatewayv1.RedeemTicketRequest{Ticket: "ticket-beta"}, info, handler); status.Code(err) != codes.AlreadyExists {
		t.Fatalf("expected AlreadyExists on payload mismatch, got %v", err)
	}
	if _, err := interceptor(context.Background(), fakeReqID{ID: uuid.New().String(), Body: "x"}, info, handler); status.Code(err) != codes.InvalidArgument {
		t.Fatalf("expected InvalidArgument on UUIDv4 request_id, got %v", err)
	}
}

func mustSelfSignedTLS(t *testing.T) *tls.Config {
	t.Helper()
	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("ecdsa.GenerateKey: %v", err)
	}
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(1), NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour),
		IPAddresses: []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")}, DNSNames: []string{"localhost"},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &priv.PublicKey, priv)
	if err != nil {
		t.Fatalf("x509.CreateCertificate: %v", err)
	}
	return &tls.Config{Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: priv}}}
}
