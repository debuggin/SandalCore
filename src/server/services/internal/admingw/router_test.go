// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package admingw

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"aocrop/services/internal/telemetry"
)

func TestAdminGatewayETagAndConditionalGet(t *testing.T) {
	fixedNow := time.Date(2026, 11, 5, 0, 0, 0, 0, time.UTC)
	srv := NewRouter(Config{Now: func() time.Time { return fixedNow }})

	for _, path := range []string{"/api/v1/gm/commands", "/api/v1/gm/audit", "/healthz"} {
		rec1 := serveAuthed(srv, http.MethodGet, path, "gm-observer-dev-token", nil, nil)
		if rec1.Code != http.StatusOK {
			t.Fatalf("GET %s status = %d, want 200", path, rec1.Code)
		}
		etag := rec1.Header().Get("ETag")
		if !strings.HasPrefix(etag, `"sha256-`) || rec1.Header().Get("Alt-Svc") != `h3=":8443"; ma=86400` {
			t.Fatalf("GET %s ETag=%q Alt-Svc=%q", path, etag, rec1.Header().Get("Alt-Svc"))
		}
		rec304 := serveAuthed(srv, http.MethodGet, path, "gm-observer-dev-token", map[string]string{"If-None-Match": etag}, nil)
		if rec304.Code != http.StatusNotModified || rec304.Body.Len() != 0 || rec304.Header().Get("ETag") != etag {
			t.Fatalf("GET %s If-None-Match status=%d bodyLen=%d etag=%q", path, rec304.Code, rec304.Body.Len(), rec304.Header().Get("ETag"))
		}
		recWeak := serveAuthed(srv, http.MethodGet, path, "gm-observer-dev-token", map[string]string{"If-None-Match": "W/" + etag}, nil)
		if recWeak.Code != http.StatusNotModified {
			t.Fatalf("GET %s weak If-None-Match status=%d, want 304", path, recWeak.Code)
		}
		recMiss := serveAuthed(srv, http.MethodGet, path, "gm-observer-dev-token", map[string]string{"If-None-Match": `"sha256-0000000000000000"`}, nil)
		if recMiss.Code != http.StatusOK {
			t.Fatalf("GET %s If-None-Match miss status=%d, want 200", path, recMiss.Code)
		}
	}
}

func TestAdminGatewayHotfixIfMatchPrecondition(t *testing.T) {
	srv := NewRouter(Config{})
	auditRec := serveAuthed(srv, http.MethodGet, "/api/v1/gm/audit", "gm-observer-dev-token", nil, nil)
	auditETag := auditRec.Header().Get("ETag")
	if !strings.HasPrefix(auditETag, `"sha256-`) {
		t.Fatalf("expected strong audit ETag, got %q", auditETag)
	}

	payload := []byte(`{"sql":"UPDATE spawns SET overlay_value = '2' WHERE record_guid = 'sp-1'","reason":"tune"}`)
	staleHdrs := map[string]string{"Idempotency-Key": "idem-hotfix-1", "If-Match": `"stale-etag"`}
	rec412 := serveAuthed(srv, http.MethodPost, "/api/v1/gm/hotfix/apply", "gm-architect-dev-token", staleHdrs, payload)
	if rec412.Code != http.StatusPreconditionFailed || !strings.HasPrefix(rec412.Header().Get("Content-Type"), "application/problem+json") {
		t.Fatalf("stale If-Match status=%d ct=%q, want 412 problem+json", rec412.Code, rec412.Header().Get("Content-Type"))
	}
	if srv.SnapshotEpoch() != 1 || srv.ReloadCount() != 0 {
		t.Fatalf("expected zero state mutation on 412, got epoch=%d reloads=%d", srv.SnapshotEpoch(), srv.ReloadCount())
	}

	validHdrs := map[string]string{"Idempotency-Key": "idem-hotfix-2", "If-Match": auditETag}
	rec200 := serveAuthed(srv, http.MethodPost, "/api/v1/gm/hotfix/apply", "gm-architect-dev-token", validHdrs, payload)
	if rec200.Code != http.StatusOK || srv.SnapshotEpoch() != 2 || srv.ReloadCount() != 1 {
		t.Fatalf("matching If-Match status=%d epoch=%d reloads=%d", rec200.Code, srv.SnapshotEpoch(), srv.ReloadCount())
	}

	replayOldHdrs := map[string]string{"Idempotency-Key": "idem-hotfix-3", "If-Match": auditETag}
	recStaleAfter := serveAuthed(srv, http.MethodPost, "/api/v1/gm/hotfix/apply", "gm-architect-dev-token", replayOldHdrs, payload)
	if recStaleAfter.Code != http.StatusPreconditionFailed {
		t.Fatalf("expected 412 when reusing outdated audit ETag after mutation, got %d", recStaleAfter.Code)
	}
}

func TestAdminGatewayH2CNegotiation(t *testing.T) {
	ln, err := telemetry.ListenDualStackTCP(context.Background(), "127.0.0.1:0")
	if err != nil {
		t.Fatalf("ListenDualStackTCP: %v", err)
	}
	router := NewRouter(Config{})
	httpSrv := telemetry.NewCleartextHTTPServer(ln.Addr().String(), telemetry.ETagMiddleware(router))
	defer func() { _ = httpSrv.Close() }()
	go func() { _ = httpSrv.Serve(ln) }()

	h2cClient := telemetry.NewETagCachingClient(telemetry.NewH2CClient(5 * time.Second))
	url := "http://" + ln.Addr().String() + "/api/v1/gm/commands"
	req1, _ := http.NewRequest(http.MethodGet, url, nil)
	req1.Header.Set("Authorization", "Bearer gm-observer-dev-token")
	resp1, err := h2cClient.Do(req1)
	if err != nil {
		t.Fatalf("h2c GET 1: %v", err)
	}
	body1, _ := io.ReadAll(resp1.Body)
	_ = resp1.Body.Close()
	if resp1.StatusCode != http.StatusOK || resp1.ProtoMajor != 2 || !strings.HasPrefix(resp1.Header.Get("ETag"), `"sha256-`) {
		t.Fatalf("h2c GET 1 status=%d proto=%s etag=%q", resp1.StatusCode, resp1.Proto, resp1.Header.Get("ETag"))
	}

	req2, _ := http.NewRequest(http.MethodGet, url, nil)
	req2.Header.Set("Authorization", "Bearer gm-observer-dev-token")
	resp2, err := h2cClient.Do(req2)
	if err != nil {
		t.Fatalf("h2c GET 2: %v", err)
	}
	body2, _ := io.ReadAll(resp2.Body)
	_ = resp2.Body.Close()
	if resp2.StatusCode != http.StatusOK || string(body2) != string(body1) || resp2.Header.Get(telemetry.ETagRehydratedHeader) != "true" {
		t.Fatalf("h2c rehydrated GET 2 status=%d rehydrated=%q", resp2.StatusCode, resp2.Header.Get(telemetry.ETagRehydratedHeader))
	}
}

func serveAuthed(h http.Handler, method, path, token string, hdrs map[string]string, body []byte) *httptest.ResponseRecorder {
	req := httptest.NewRequest(method, path, bytes.NewReader(body))
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	for k, v := range hdrs {
		req.Header.Set(k, v)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}
