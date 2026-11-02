package telemetry

import (
	"context"
	"fmt"
	"net/http"
	"sync/atomic"
	"time"
)

type ReadinessGate struct {
	id                    string
	hz                    uint32
	t0                    time.Time
	loaded, meshed, drain atomic.Bool
	sessions              atomic.Uint32
	pktIn, pktOut         atomic.Uint64
}

func NewReadinessGate(id string, hz uint32) *ReadinessGate {
	return &ReadinessGate{id: id, hz: hz, t0: time.Now()}
}
func (g *ReadinessGate) SetReady(c, m bool) { g.loaded.Store(c); g.meshed.Store(m) }
func (g *ReadinessGate) BeginDrain()        { g.drain.Store(true) }
func (g *ReadinessGate) IsDraining() bool   { return g.drain.Load() }
func (g *ReadinessGate) IsReady() bool      { return g.loaded.Load() && g.meshed.Load() && !g.drain.Load() }
func (g *ReadinessGate) SetCounters(s uint32, i, o uint64) {
	g.sessions.Store(s)
	g.pktIn.Store(i)
	g.pktOut.Store(o)
}
func (g *ReadinessGate) Handler() http.Handler {
	b2i, st := map[bool]int{true: 1}, map[bool]string{true: "ok", false: "degraded"}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/livez":
			_, _ = w.Write([]byte(`{"status":"alive"}`))
		case "/readyz":
			if !g.IsReady() {
				w.WriteHeader(http.StatusServiceUnavailable)
			}
			_, _ = fmt.Fprint(w, map[bool]string{true: `{"status":"ready"}`, false: fmt.Sprintf(`{"status":"not_ready","draining":%t}`, g.IsDraining())}[g.IsReady()])
		case "/healthz":
			if !g.IsReady() {
				w.WriteHeader(http.StatusServiceUnavailable)
			}
			_, _ = fmt.Fprintf(w, `{"status":%q,"server_id":%q,"ready":%t,"draining":%t,"active_sessions":%d,"tick_hz":%d,"uptime_ms":%d}`,
				st[g.IsReady()], g.id, g.IsReady(), g.IsDraining(), g.sessions.Load(), g.hz, time.Since(g.t0).Milliseconds())
		case "/metrics":
			w.Header().Set("Content-Type", "application/openmetrics-text; version=1.0.0; charset=utf-8")
			_, _ = fmt.Fprintf(w, "aoc_world_ready %d\naoc_world_draining %d\naoc_world_active_sessions %d\naoc_world_tick_hz %d\naoc_world_packets_in_total %d\naoc_world_packets_out_total %d\n# EOF\n",
				b2i[g.IsReady()], b2i[g.IsDraining()], g.sessions.Load(), g.hz, g.pktIn.Load(), g.pktOut.Load())
		default:
			http.NotFound(w, r)
		}
	})
}
func (g *ReadinessGate) DrainGracefully(ctx context.Context, budget time.Duration, steps ...func(context.Context) error) (time.Duration, error) {
	t0 := time.Now()
	g.BeginDrain()
	if budget <= 0 || budget > 5*time.Second {
		return time.Since(t0), fmt.Errorf("invalid drain budget: %v", budget)
	}
	dctx, cancel := context.WithTimeout(ctx, budget)
	defer cancel()
	for _, step := range steps {
		if step != nil && dctx.Err() == nil {
			if err := step(dctx); err != nil {
				return time.Since(t0), err
			}
		}
	}
	return time.Since(t0), dctx.Err()
}
