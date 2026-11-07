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

	admincatalog "aocrop/services/internal/admin-catalog"
	admingateway "aocrop/services/internal/admin-gateway"
	"aocrop/services/internal/config"
	"aocrop/services/internal/db"
	"aocrop/services/internal/mesh"
	"aocrop/services/internal/telemetry"
	worldstream "aocrop/services/internal/world-stream"
	pkgtelemetry "aocrop/services/pkg/telemetry"
	"github.com/jackc/pgx/v5/pgxpool"
)

type Gateway struct {
	registry  *mesh.Registry
	router    *mesh.ZoneRouter
	readiness *pkgtelemetry.ReadinessGate
	coalescer *worldstream.Coalescer
	admin     *admingateway.Server
}

func newGateway(reg *mesh.Registry, router *mesh.ZoneRouter, serverID string) (*Gateway, error) {
	return newGatewayWithPool(reg, router, serverID, nil)
}

func newGatewayWithPool(reg *mesh.Registry, router *mesh.ZoneRouter, serverID string, pool *pgxpool.Pool) (*Gateway, error) {
	if reg == nil {
		reg = mesh.NewRegistry(pool, 15*time.Second, "")
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
	if dbPath := resolveCatalogDBPath(); dbPath != "" {
		if loaded, err := admincatalog.NewCatalogFromContentDB(dbPath); err == nil {
			catalog = loaded
		} else {
			slog.Warn("mesh-gateway failed to load content.db catalog, falling back to default", "path", dbPath, "err", err)
		}
	}
	coalescer := worldstream.NewCoalescer(worldstream.DefaultTickInterval)
	admin := admingateway.NewRouter(admingateway.Config{
		Catalog:         catalog,
		Coalescer:       coalescer,
		RoleTokens:      loadRoleTokens(),
		PmTilesPath:     firstEnv("AOC_PMTILES_PATH", "SANDAL_PMTILES_PATH"),
		WorldAdminAddr:  firstEnvDefault("world:50061", "AOC_WORLD_ADMIN_ADDR", "SANDAL_WORLD_ADMIN_ADDR"),
		WorldStreamAddr: firstEnvDefault("world:50062", "AOC_WORLD_STREAM_ADDR", "SANDAL_WORLD_STREAM_ADDR"),
		CityNodeAddr:    firstEnvDefault("citynode:50056", "AOC_CITYNODE_GRPC_ADDR", "SANDAL_CITYNODE_GRPC_ADDR"),
		ServiceToken:    resolveServiceToken(),
		Pool:            pool,
	})
	return &Gateway{
		registry:  reg,
		router:    router,
		readiness: gate,
		coalescer: coalescer,
		admin:     admin,
	}, nil
}

func resolveCatalogDBPath() string {
	for i, arg := range os.Args {
		if strings.HasPrefix(arg, "-catalog-db=") {
			return strings.TrimSpace(strings.TrimPrefix(arg, "-catalog-db="))
		}
		if arg == "-catalog-db" && i+1 < len(os.Args) {
			return strings.TrimSpace(os.Args[i+1])
		}
	}
	for _, cand := range []string{
		os.Getenv("AOC_CONTENT_DB"),
		os.Getenv("SANDAL_CONTENT_DB"),
		"/content/content.db",
		"dist/content/content.db",
		"deploy/content/content.db",
	} {
		if c := strings.TrimSpace(cand); c != "" {
			if st, err := os.Stat(c); err == nil && !st.IsDir() {
				return c
			}
		}
	}
	return ""
}

func resolveServiceToken() string {
	if tok := firstEnv("AOC_SERVICE_TOKEN", "SANDAL_SERVICE_TOKEN"); tok != "" {
		return tok
	}
	if file := firstEnv("AOC_SERVICE_TOKEN_FILE", "SANDAL_SERVICE_TOKEN_FILE"); file != "" {
		if b, err := os.ReadFile(file); err == nil {
			return strings.TrimSpace(string(b))
		}
	}
	return ""
}

func firstEnv(keys ...string) string {
	for _, k := range keys {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" {
			return v
		}
	}
	return ""
}

func firstEnvDefault(def string, keys ...string) string {
	if v := firstEnv(keys...); v != "" {
		return v
	}
	return def
}

func firstNonEmptyString(vals ...string) string {
	for _, v := range vals {
		if s := strings.TrimSpace(v); s != "" {
			return s
		}
	}
	return ""
}

func loadRoleTokens() map[string]admingateway.Role {
	tokens := admingateway.DefaultRoleTokens()
	for _, k := range []string{"AOC_GM_OBSERVER_TOKEN", "AOC_ADMIN_GATEWAY_OBSERVER_TOKEN"} {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" {
			tokens[v] = admingateway.RoleObserver
		}
	}
	for _, k := range []string{"AOC_GM_OPERATOR_TOKEN", "AOC_ADMIN_GATEWAY_OPERATOR_TOKEN"} {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" {
			tokens[v] = admingateway.RoleOperator
		}
	}
	for _, k := range []string{"AOC_GM_ARCHITECT_TOKEN", "AOC_ADMIN_GATEWAY_ARCHITECT_TOKEN"} {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" {
			tokens[v] = admingateway.RoleArchitect
		}
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
	var pool *pgxpool.Pool
	if dsn := firstEnv("AOC_DATABASE_URL", "DATABASE_URL", "SANDAL_DATABASE_URL"); dsn != "" || cfg.DatabaseURL != "" {
		if p, err := db.Open(ctx, firstNonEmptyString(cfg.DatabaseURL, dsn)); err == nil {
			defer p.Close()
			if migErr := db.Migrate(ctx, p); migErr != nil {
				slog.Warn("mesh-gateway database migrate warning", "err", migErr)
			}
			pool = p
		} else {
			slog.Warn("mesh-gateway failed to open postgres pool, continuing in memory mode", "err", err)
		}
	}
	reg := mesh.NewRegistry(pool, cfg.MeshHeartbeatTTL, cfg.MeshStaticWorld)
	router := mesh.NewZoneRouter(mesh.DefaultBoundaryHysteresisCm)
	gateway, err := newGatewayWithPool(reg, router, "mesh-gateway", pool)
	if err != nil {
		return err
	}
	lis, err := net.Listen("tcp", probeAddr)
	if err != nil {
		return err
	}
	if gateway.coalescer != nil {
		go gateway.coalescer.Run(ctx)
	}
	httpSrv := telemetry.NewCleartextHTTPServer(probeAddr, gateway.Handler())
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
	_, drainErr := gateway.readiness.DrainGracefully(context.Background(), 5*time.Second, func(dctx context.Context) error {
		if gateway.admin != nil {
			gateway.admin.SetReady(false)
		}
		return httpSrv.Shutdown(dctx)
	})
	if err != nil {
		return err
	}
	return drainErr
}
