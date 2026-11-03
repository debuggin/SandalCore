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

	"aocrop/services/internal/admincatalog"
	"aocrop/services/internal/admingw"
	"aocrop/services/internal/worldstream"
)

const (
	defaultAdminAddr = ":8090"
	shutdownGrace    = 10 * time.Second
)

func main() {
	if err := run(context.Background()); err != nil {
		slog.Error("admingw stopped", "err", err)
		os.Exit(1)
	}
}

func run(parent context.Context) error {
	ctx, stop := signal.NotifyContext(parent, os.Interrupt, syscall.SIGTERM)
	defer stop()

	addr := resolveAddr()
	catalog := admincatalog.NewDefaultCatalog()
	coalescer := worldstream.NewCoalescer(worldstream.DefaultTickInterval)
	router := admingw.NewRouter(admingw.Config{
		Catalog:    catalog,
		Coalescer:  coalescer,
		RoleTokens: loadRoleTokens(),
	})

	lis, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}

	httpSrv := &http.Server{
		Handler:           router,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	go coalescer.Run(ctx)

	errCh := make(chan error, 1)
	go func() {
		if serveErr := httpSrv.Serve(lis); serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
			errCh <- serveErr
			return
		}
		errCh <- nil
	}()

	slog.Info("admingw listening", "addr", lis.Addr().String())

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
	if v := strings.TrimSpace(os.Getenv("AOC_ADMIN_ADDR")); v != "" {
		return v
	}
	return defaultAdminAddr
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
