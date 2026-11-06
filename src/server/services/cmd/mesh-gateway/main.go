// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"aocrop/services/internal/admincatalog"
	"aocrop/services/internal/admingw"
	"aocrop/services/internal/config"
	"aocrop/services/internal/mesh"
	"aocrop/services/internal/telemetry"
	"aocrop/services/internal/worldstream"
	pkgtelemetry "aocrop/services/pkg/telemetry"
)

type Gateway struct {
	registry  *mesh.Registry
	router    *mesh.ZoneRouter
	readiness *pkgtelemetry.ReadinessGate
	coalescer *worldstream.Coalescer
	admin     *admingw.Server
}

func newGateway(reg *mesh.Registry, router *mesh.ZoneRouter, serverID string) (*Gateway, error) {
	if reg == nil {
		reg = mesh.NewRegistry(nil, 15*time.Second, "")
	}
	if router == nil {
		router = mesh.NewZoneRouter(mesh.DefaultBoundaryHysteresisCm)
	}
	if len(router.Zones()) == 0 {
		if err := seedDefaultZones(router); err != nil {
			return nil, err
		}
	}
	gate := pkgtelemetry.NewReadinessGate(serverID, 60)
	gate.SetReady(true, true)
	catalog := admincatalog.NewDefaultCatalog()
	coalescer := worldstream.NewCoalescer(worldstream.DefaultTickInterval)
	admin := admingw.NewRouter(admingw.Config{
		Catalog:    catalog,
		Coalescer:  coalescer,
		RoleTokens: loadRoleTokens(),
	})
	return &Gateway{
		registry:  reg,
		router:    router,
		readiness: gate,
		coalescer: coalescer,
		admin:     admin,
	}, nil
}

func loadRoleTokens() map[string]admingw.Role {
	tokens := admingw.DefaultRoleTokens()
	if v := strings.TrimSpace(os.Getenv("AOC_GM_OBSERVER_TOKEN")); v != "" {
		tokens[v] = admingw.RoleObserver
	}
	if v := strings.TrimSpace(os.Getenv("AOC_GM_OPERATOR_TOKEN")); v != "" {
		tokens[v] = admingw.RoleOperator
	}
	if v := strings.TrimSpace(os.Getenv("AOC_GM_ARCHITECT_TOKEN")); v != "" {
		tokens[v] = admingw.RoleArchitect
	}
	return tokens
}

func seedDefaultZones(router *mesh.ZoneRouter) error {
	defaults := []mesh.ZoneBounds{
		{
			ZoneID:         1,
			MinX:           -200000.0,
			MaxX:           0.0,
			MinY:           -100000.0,
			MaxY:           100000.0,
			ServerID:       1,
			Address:        "127.0.0.1:7777",
			AuthorityEpoch: 1,
			State:          mesh.HandoverStateAuthority,
		},
		{
			ZoneID:         2,
			MinX:           0.0,
			MaxX:           200000.0,
			MinY:           -100000.0,
			MaxY:           100000.0,
			ServerID:       2,
			Address:        "127.0.0.1:7778",
			AuthorityEpoch: 1,
			State:          mesh.HandoverStateAuthority,
		},
	}
	for _, z := range defaults {
		if err := router.UpsertZone(z); err != nil {
			return err
		}
	}
	return nil
}

func (g *Gateway) Handler() http.Handler {
	probes := telemetry.ETagMiddleware(g.readiness.Handler())
	mux := http.NewServeMux()
	for _, p := range []string{"/livez", "/readyz", "/healthz", "/metrics"} {
		mux.Handle(p, probes)
	}
	if g.admin != nil {
		mux.Handle("/api/", g.admin)
	}
	return mux
}

func (g *Gateway) RouteForwardPacket(
	currentZoneID uint32, x, y float64, packet []byte,
) (mesh.MeshForwardHeader, uint32, []byte, error) {
	hdr, payload, err := mesh.UnpackMeshForwardPacket(packet)
	if err != nil {
		return mesh.MeshForwardHeader{}, 0, nil, err
	}
	if err := g.router.ValidateForwardHeader(hdr); err != nil {
		return mesh.MeshForwardHeader{}, 0, nil, err
	}
	nextZoneID, _, err := g.router.ResolveAuthorityZone(currentZoneID, x, y)
	if err != nil {
		return mesh.MeshForwardHeader{}, 0, nil, err
	}
	return hdr, nextZoneID, payload, nil
}

func main() {
	config.HandleHealthcheck("8086", "AOC_MESH_GATEWAY_HTTP_BIND_ADDR", "AOC_MESH_GATEWAY_ADDR", "19082")
	config.BindContainerFlags("mesh-gateway")
	if err := run(); err != nil {
		slog.Error("mesh-gateway stopped", "err", err)
		os.Exit(1)
	}
}

func run() error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return runWithContext(ctx, resolveProbeAddr())
}

func resolveProbeAddr() string {
	for _, k := range []string{"AOC_MESH_GATEWAY_HTTP_BIND_ADDR", "SANDAL_MESH_GATEWAY_HTTP_BIND_ADDR", "AOC_MESH_GATEWAY_ADDR", "SANDAL_MESH_GATEWAY_ADDR"} {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" {
			return v
		}
	}
	return "127.0.0.1:19082"
}

func checkStaticWorld(addr string) error {
	if addr == "" {
		return nil
	}
	if err := mesh.ValidAddress(addr); err != nil {
		return fmt.Errorf("AOC_MESH_STATIC_WORLD %q is not an address a game client can dial: %w", addr, err)
	}
	return nil
}

// Mark readiness probes 503 immediately on SIGINT/SIGTERM before draining active connections.
func runWithContext(ctx context.Context, probeAddr string) error {
	cfg := config.Load()
	if err := checkStaticWorld(cfg.MeshStaticWorld); err != nil {
		return err
	}
	reg := mesh.NewRegistry(nil, cfg.MeshHeartbeatTTL, cfg.MeshStaticWorld)
	router := mesh.NewZoneRouter(mesh.DefaultBoundaryHysteresisCm)
	gw, err := newGateway(reg, router, "mesh-gateway")
	if err != nil {
		return err
	}
	lis, err := net.Listen("tcp", probeAddr)
	if err != nil {
		return err
	}
	if gw.coalescer != nil {
		go gw.coalescer.Run(ctx)
	}
	httpSrv := telemetry.NewCleartextHTTPServer(probeAddr, gw.Handler())
	errc := make(chan error, 1)
	go func() {
		if serveErr := httpSrv.Serve(lis); serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
			errc <- serveErr
			return
		}
		errc <- nil
	}()
	slog.Info("mesh-gateway listening", "http_probes", lis.Addr().String(), "hysteresis_cm", router.HysteresisCm())
	select {
	case <-ctx.Done():
	case err = <-errc:
		return err
	}
	_, drainErr := gw.readiness.DrainGracefully(context.Background(), 5*time.Second, func(dctx context.Context) error {
		if gw.admin != nil {
			gw.admin.SetReady(false)
		}
		return httpSrv.Shutdown(dctx)
	})
	if err != nil {
		return err
	}
	return drainErr
}
