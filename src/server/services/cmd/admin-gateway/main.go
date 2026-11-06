// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"context"
	"errors"
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
	"aocrop/services/internal/telemetry"
	worldstream "aocrop/services/internal/world-stream"
)

const (
	defaultAdminAddr = ":8090"
	shutdownGrace    = 10 * time.Second
)

func main() {
	config.HandleHealthcheck("8090", "AOC_ADMIN_HTTP_BIND_ADDR", "AOC_ADMIN_HTTP_ADDR", "AOC_ADMIN_ADDR")
	config.BindContainerFlags("admin")
	if err := run(context.Background()); err != nil {
		slog.Error("admin-gateway stopped", "err", err)
		os.Exit(1)
	}
}

func run(parent context.Context) error {
	ctx, stop := signal.NotifyContext(parent, os.Interrupt, syscall.SIGTERM)
	defer stop()

	addr := resolveAddr()
	catalog := admincatalog.NewDefaultCatalog()
	coalescer := worldstream.NewCoalescer(worldstream.DefaultTickInterval)
	router := admingateway.NewRouter(admingateway.Config{
		Catalog:    catalog,
		Coalescer:  coalescer,
		RoleTokens: loadRoleTokens(),
	})

	lis, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}

	httpSrv := telemetry.NewCleartextHTTPServer(addr, telemetry.ETagMiddleware(router))

	go coalescer.Run(ctx)

	errCh := make(chan error, 1)
	go func() {
		if serveErr := httpSrv.Serve(lis); serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
			errCh <- serveErr
			return
		}
		errCh <- nil
	}()

	slog.Info("admin-gateway listening", "addr", lis.Addr().String())

	select {
	case <-ctx.Done():
	case serveErr := <-errCh:
		return serveErr
	}

	stop()
	router.SetReady(false)
	shutdownCtx, cancel := context.WithTimeout(context.Background(), shutdownGrace)
	defer cancel()
	return httpSrv.Shutdown(shutdownCtx)
}

func resolveAddr() string {
	for _, k := range []string{"AOC_ADMIN_HTTP_BIND_ADDR", "AOC_ADMIN_HTTP_ADDR", "AOC_ADMIN_ADDR", "SANDAL_ADMIN_HTTP_BIND_ADDR", "SANDAL_ADMIN_ADDR"} {
		if v := strings.TrimSpace(os.Getenv(k)); v != "" {
			return v
		}
	}
	return defaultAdminAddr
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
