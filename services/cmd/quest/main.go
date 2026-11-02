// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Command quest serves aoc.quest.v1.QuestService over gRPC.

package main

import (
	"context"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	"google.golang.org/grpc"

	questv1 "aocrop/services/gen/aoc/quest/v1"
	"aocrop/services/internal/config"
	"aocrop/services/internal/db"
	"aocrop/services/internal/quest"
	"aocrop/services/internal/questsgrpc"
	"aocrop/services/internal/svcauth"
)

func main() {
	if err := run(); err != nil {
		slog.Error("quests stopped", "err", err)
		os.Exit(1)
	}
}

func run() error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	cfg := config.Load()
	token, err := svcauth.Load(cfg.ServiceTokenFile)
	if err != nil {
		return err
	}

	// Ctrl+C while the database is being reached is a stop, not a failure.
	pool, err := db.Open(ctx, cfg.DatabaseURL)
	if err != nil {
		return startupErr(ctx, err)
	}
	defer pool.Close()
	if err := db.Migrate(ctx, pool); err != nil {
		return startupErr(ctx, err)
	}

	lis, err := net.Listen("tcp", cfg.QuestsGRPCAddr)
	if err != nil {
		return err
	}
	srv := newServer(token, quest.New(pool))

	errc := make(chan error, 1)
	go func() { errc <- srv.Serve(lis) }()
	slog.Info("quests listening", "grpc", lis.Addr().String())

	select {
	case <-ctx.Done():
	case err = <-errc:
	}
	stop() // from here a second Ctrl+C ends the process at once
	force := time.AfterFunc(10*time.Second, srv.Stop)
	srv.GracefulStop()
	force.Stop()
	return err
}

func newServer(token svcauth.Token, svc *quest.Service) *grpc.Server {
	s := grpc.NewServer(token.ServerOptions()...)
	questv1.RegisterQuestServiceServer(s, questsgrpc.NewServer(svc))
	return s
}

func startupErr(ctx context.Context, err error) error {
	if ctx.Err() != nil {
		return nil
	}
	return err
}
