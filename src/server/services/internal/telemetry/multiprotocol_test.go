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
	"io"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/google/uuid"
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
	h2Client := NewH2CClient(5 * time.Second)
	h2Resp, err := h2Client.Get("http://" + httpAddr + "/healthz")
	if err != nil {
		t.Fatalf("h2c GET: %v", err)
	}
	body, _ := io.ReadAll(h2Resp.Body)
	_ = h2Resp.Body.Close()
	if h2Resp.StatusCode != http.StatusOK || h2Resp.ProtoMajor != 2 || string(body) != "HTTP/2.0" {
		t.Fatalf("h2c unexpected status=%d proto=%s body=%q", h2Resp.StatusCode, h2Resp.Proto, string(body))
	}
}

func assertTLS13AndQUIC(t *testing.T, httpsAddr, quicAddr string) {
	t.Helper()
	clientTLS := &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS13}
	mpClient := NewMultiProtocolClient(clientTLS, 5*time.Second)
	resp, err := mpClient.Get("https://" + httpsAddr + "/readyz")
	if err != nil {
		t.Fatalf("TLS 1.3 h2 GET: %v", err)
	}
	body, _ := io.ReadAll(resp.Body)
	_ = resp.Body.Close()
	if resp.TLS == nil || resp.TLS.Version != tls.VersionTLS13 || resp.ProtoMajor != 2 || string(body) != "HTTP/2.0" {
		t.Fatalf("expected TLS 1.3 HTTP/2.0, got proto=%s body=%q tls=%+v", resp.Proto, string(body), resp.TLS)
	}
	if altSvc := resp.Header.Get("Alt-Svc"); !strings.HasPrefix(altSvc, `h3=":`) {
		t.Fatalf("expected Alt-Svc h3 advertisement, got %q", altSvc)
	}
	h3Req, err := http.NewRequest(http.MethodGet, "https://"+quicAddr+"/readyz", nil)
	if err != nil {
		t.Fatalf("NewRequest h3: %v", err)
	}
	h3Resp, err := H3RoundTrip(context.Background(), clientTLS, h3Req)
	if err != nil {
		t.Fatalf("H3RoundTrip: %v", err)
	}
	h3Body, _ := io.ReadAll(h3Resp.Body)
	_ = h3Resp.Body.Close()
	if h3Resp.StatusCode != http.StatusOK || h3Resp.ProtoMajor != 3 || string(h3Body) != "HTTP/3.0" {
		t.Fatalf("expected HTTP/3.0 over QUIC, got status=%d proto=%s body=%q", h3Resp.StatusCode, h3Resp.Proto, string(h3Body))
	}
	upResp, err := mpClient.Get("https://" + httpsAddr + "/readyz")
	if err != nil {
		t.Fatalf("MultiProtocolClient Alt-Svc upgrade GET: %v", err)
	}
	_ = upResp.Body.Close()
	if upResp.ProtoMajor != 3 {
		t.Fatalf("expected Alt-Svc upgrade to HTTP/3, got %s", upResp.Proto)
	}
}

func TestCleartextHTTPServerAndHPACKQPACK(t *testing.T) {
	ln, err := ListenDualStackTCP(context.Background(), "127.0.0.1:0")
	if err != nil {
		t.Fatalf("ListenDualStackTCP: %v", err)
	}
	srv := NewCleartextHTTPServer(ln.Addr().String(), http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Received-Trace", r.Header.Get("X-Trace-Context"))
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(r.Proto))
	}))
	defer func() { _ = srv.Close() }()
	go func() { _ = srv.Serve(ln) }()

	client := NewH2CClient(5 * time.Second)
	req, _ := http.NewRequest(http.MethodGet, "http://"+ln.Addr().String()+"/east-west", nil)
	req.Header.Set("X-Trace-Context", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01")
	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("NewH2CClient Do: %v", err)
	}
	_ = resp.Body.Close()
	if resp.ProtoMajor != 2 || resp.Header.Get("X-Received-Trace") != req.Header.Get("X-Trace-Context") {
		t.Fatalf("h2c response proto=%s trace=%q", resp.Proto, resp.Header.Get("X-Received-Trace"))
	}
	assertHPACKAndQPACKRoundTrip(t)
}

func assertHPACKAndQPACKRoundTrip(t *testing.T) {
	t.Helper()
	src := http.Header{
		"Content-Type":    []string{"application/problem+json"},
		"Cache-Control":   []string{"max-age=60, must-revalidate"},
		"X-Sandal-Region": []string{"verra-central-1"},
	}
	hpackBlock, err := EncodeHPACKHeaders(src)
	if err != nil || len(hpackBlock) == 0 {
		t.Fatalf("EncodeHPACKHeaders: len=%d err=%v", len(hpackBlock), err)
	}
	hpackOut, err := DecodeHPACKHeaders(hpackBlock)
	if err != nil || hpackOut.Get("X-Sandal-Region") != "verra-central-1" {
		t.Fatalf("DecodeHPACKHeaders: %v, %v", hpackOut, err)
	}
	qpackBlock, err := EncodeQPACKHeaders(src)
	if err != nil || len(qpackBlock) == 0 {
		t.Fatalf("EncodeQPACKHeaders: len=%d err=%v", len(qpackBlock), err)
	}
	qpackOut, err := DecodeQPACKHeaders(qpackBlock)
	if err != nil || qpackOut.Get("Content-Type") != "application/problem+json" {
		t.Fatalf("DecodeQPACKHeaders: %v, %v", qpackOut, err)
	}
}

func TestETagMiddlewareAndCachingClient(t *testing.T) {
	var bodyState atomic.Value
	bodyState.Store(`{"realm":"verra","epoch":1}`)
	var mutCalls atomic.Int32
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/realm", func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPut {
			mutCalls.Add(1)
			bodyState.Store(`{"realm":"verra","epoch":2}`)
		}
		w.Header().Set("Cache-Control", "private, max-age=0, must-revalidate")
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(bodyState.Load().(string)))
	})
	ts := httptest.NewServer(ETagMiddleware(mux))
	defer ts.Close()

	expectedETag := ComputeStrongETag([]byte(`{"realm":"verra","epoch":1}`))
	assertETagConditionalReadAndWrite(t, ts.URL+"/v1/realm", expectedETag, &mutCalls)
	assertETagCachingClientRehydration(t, ts.URL+"/v1/realm")
}

func assertETagConditionalReadAndWrite(t *testing.T, url, expectedETag string, mutCalls *atomic.Int32) {
	t.Helper()
	r1, err := http.Get(url)
	if err != nil {
		t.Fatalf("GET 1: %v", err)
	}
	b1, _ := io.ReadAll(r1.Body)
	_ = r1.Body.Close()
	if r1.StatusCode != http.StatusOK || r1.Header.Get("ETag") != expectedETag || len(b1) == 0 {
		t.Fatalf("GET 1 status=%d etag=%q expected=%q", r1.StatusCode, r1.Header.Get("ETag"), expectedETag)
	}
	req304, _ := http.NewRequest(http.MethodGet, url, nil)
	req304.Header.Set("If-None-Match", "W/"+expectedETag)
	r304, err := http.DefaultClient.Do(req304)
	if err != nil {
		t.Fatalf("GET If-None-Match: %v", err)
	}
	b304, _ := io.ReadAll(r304.Body)
	_ = r304.Body.Close()
	if r304.StatusCode != http.StatusNotModified || len(b304) != 0 || r304.Header.Get("ETag") != expectedETag {
		t.Fatalf("expected 304 empty body, got status=%d len=%d etag=%q", r304.StatusCode, len(b304), r304.Header.Get("ETag"))
	}
	reqMiss, _ := http.NewRequest(http.MethodGet, url, nil)
	reqMiss.Header.Set("If-None-Match", `"sha256-0000000000000000"`)
	rMiss, err := http.DefaultClient.Do(reqMiss)
	if err != nil {
		t.Fatalf("GET If-None-Match miss: %v", err)
	}
	_ = rMiss.Body.Close()
	if rMiss.StatusCode != http.StatusOK {
		t.Fatalf("expected 200 OK on If-None-Match miss, got %d", rMiss.StatusCode)
	}
	req412, _ := http.NewRequest(http.MethodPut, url, strings.NewReader(`{"epoch":2}`))
	req412.Header.Set("If-Match", `"stale-etag"`)
	r412, err := http.DefaultClient.Do(req412)
	if err != nil {
		t.Fatalf("PUT If-Match stale: %v", err)
	}
	_ = r412.Body.Close()
	if r412.StatusCode != http.StatusPreconditionFailed || mutCalls.Load() != 0 || !strings.HasPrefix(r412.Header.Get("Content-Type"), ProblemMediaType) {
		t.Fatalf("expected 412 problem+json with 0 mutCalls, got status=%d calls=%d ct=%q", r412.StatusCode, mutCalls.Load(), r412.Header.Get("Content-Type"))
	}
}

func assertETagCachingClientRehydration(t *testing.T, url string) {
	t.Helper()
	var wireStatuses []int
	var wireBytes []int
	base := &http.Client{Transport: roundTripInspector{base: http.DefaultTransport, onResp: func(r *http.Response) {
		wireStatuses = append(wireStatuses, r.StatusCode)
		if r.StatusCode == http.StatusNotModified {
			wireBytes = append(wireBytes, 0)
		}
	}}}
	cachingClient := NewETagCachingClient(base)
	resp1, err := cachingClient.Get(url)
	if err != nil {
		t.Fatalf("cachingClient GET 1: %v", err)
	}
	body1, _ := io.ReadAll(resp1.Body)
	_ = resp1.Body.Close()
	resp2, err := cachingClient.Get(url)
	if err != nil {
		t.Fatalf("cachingClient GET 2: %v", err)
	}
	body2, _ := io.ReadAll(resp2.Body)
	_ = resp2.Body.Close()
	if resp2.StatusCode != http.StatusOK || string(body2) != string(body1) || resp2.Header.Get(ETagRehydratedHeader) != "true" {
		t.Fatalf("rehydrated GET 2 status=%d body=%q rehydrated=%q", resp2.StatusCode, string(body2), resp2.Header.Get(ETagRehydratedHeader))
	}
	if len(wireStatuses) != 2 || wireStatuses[0] != http.StatusOK || wireStatuses[1] != http.StatusNotModified || len(wireBytes) != 1 {
		t.Fatalf("expected wire statuses [200, 304], got %v", wireStatuses)
	}
}

type roundTripInspector struct {
	base   http.RoundTripper
	onResp func(*http.Response)
}

func (i roundTripInspector) RoundTrip(req *http.Request) (*http.Response, error) {
	resp, err := i.base.RoundTrip(req)
	if err == nil && i.onResp != nil {
		i.onResp(resp)
	}
	return resp, err
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
