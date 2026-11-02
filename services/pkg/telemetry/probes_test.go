package telemetry

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func probe(t *testing.T, h http.Handler, path string, want int) string {
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, path, nil))
	if rec.Code != want {
		t.Fatalf("GET %s status = %d, want %d", path, rec.Code, want)
	}
	return rec.Body.String()
}

func TestReadinessGateProbesAndDrain(t *testing.T) {
	g, order := NewReadinessGate("gateway-01", 30), ""
	g.SetCounters(4, 10, 20)
	h := g.Handler()
	probe(t, h, "/livez", http.StatusOK)
	probe(t, h, "/readyz", http.StatusServiceUnavailable)
	g.SetReady(true, true)
	probe(t, h, "/readyz", http.StatusOK)
	probe(t, h, "/healthz", http.StatusOK)
	if !strings.HasSuffix(probe(t, h, "/metrics", http.StatusOK), "# EOF\n") {
		t.Fatalf("/metrics missing OpenMetrics EOF")
	}
	probe(t, h, "/unknown", http.StatusNotFound)
	s1, s2 := func(context.Context) error { order += "1"; return nil }, func(context.Context) error { order += "2"; return nil }
	if dur, err := g.DrainGracefully(context.Background(), 2*time.Second, s1, s2); err != nil || !g.IsDraining() || order != "12" || dur >= 5*time.Second {
		t.Fatalf("drain err=%v draining=%v order=%q dur=%v", err, g.IsDraining(), order, dur)
	}
	probe(t, h, "/readyz", http.StatusServiceUnavailable)
	if _, err := g.DrainGracefully(context.Background(), 2*time.Second, func(context.Context) error { return context.DeadlineExceeded }); err == nil {
		t.Fatalf("expected deadline error from failing drain step")
	}
}
