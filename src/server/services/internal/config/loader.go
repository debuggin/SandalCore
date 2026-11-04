// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package config reads service configuration from AOC_* environment
// variables, with defaults for a single-machine development setup.
package config

import (
	"errors"
	"flag"
	"fmt"
	"net"
	"os"
	"strings"
	"time"
)

type Config struct {
	Env               string        // SANDAL_ENV / AOC_ENV
	MeshMode          string        // SANDAL_MESH_MODE / AOC_MESH_MODE
	DatabaseURL       string        // AOC_DATABASE_URL
	AuthHTTPAddr      string        // AOC_AUTH_HTTP_ADDR: the launcher's login API
	AuthGRPCAddr      string        // AOC_AUTH_GRPC_ADDR: internal token validation
	CharacterGRPCAddr string        // AOC_CHARACTER_GRPC_ADDR
	CityNodeGRPCAddr  string        // AOC_CITYNODE_GRPC_ADDR: the city node service (Nodes 1)
	CityNodeSeed      string        // AOC_CITYNODE_SEED: the node seed file citynode applies once (default config/seeds/citynode_seed.json)
	QuestsGRPCAddr    string        // AOC_QUESTS_GRPC_ADDR: the quest service (quests, slice 1), default 127.0.0.1:50058
	WorldAdminAddr    string        // AOC_WORLD_ADMIN_ADDR: world's admin API (worldctl, the admin app to come); world listens here, default 127.0.0.1:50056
	WorldStreamAddr   string        // AOC_WORLD_STREAM_ADDR: world's world stream (worldctl watch, admin tools); world listens here, default 127.0.0.1:50057
	TokenTTL          time.Duration // AOC_TOKEN_TTL, e.g. "24h"
	ServiceTokenFile  string        // AOC_SERVICE_TOKEN_FILE: the internal services' shared token (internal/svcauth)
	ContentDB         string        // AOC_CONTENT_DB: the content.db contentc builds; the character service checks new characters against it
	GMStarterSet      bool          // AOC_GM_STARTER_SET=1: new characters get the GM starter set, existing ones once at a start (plan D1), and a wallet starts with 21 gold (testing only)

	MeshGRPCAddr     string        // AOC_MESH_GRPC_ADDR
	MeshStaticWorld  string        // AOC_MESH_STATIC_WORLD: a world server that never registers; "" for none
	MeshHeartbeatTTL time.Duration // AOC_MESH_HEARTBEAT_TTL: a world server silent this long is not placed

	GatewayICSAddr    string        // AOC_GATEWAY_ICS_ADDR: the game client's ICS gRPC, over TLS
	GatewayGRPCAddr   string        // AOC_GATEWAY_GRPC_ADDR: internal (RedeemTicket), on loopback unless GatewayGRPCPublic
	GatewayGRPCPublic bool          // AOC_GATEWAY_GRPC_PUBLIC=1: allow any GatewayGRPCAddr (the ticket crosses the network in clear text)
	GatewayTLSCert    string        // AOC_GATEWAY_TLS_CERT: PEM certificate for the ICS listener
	GatewayTLSKey     string        // AOC_GATEWAY_TLS_KEY: its private key
	TicketTTL         time.Duration // AOC_TICKET_TTL: how long a join ticket stays redeemable
}

func Load() Config {
	cfg, _ := LoadWithEnvFile(os.Getenv("SANDAL_CONFIG_FILE"))
	return cfg
}

func LoadWithEnvFile(path string) (Config, error) {
	envFile, err := readEnvFile(path)
	str := func(key, def string) string { return resolve(envFile, strings.TrimPrefix(key, "AOC_"), def) }
	firstStr := func(def string, keys ...string) string {
		for _, k := range keys {
			if strings.HasPrefix(k, "AOC_") {
				if v := resolve(envFile, strings.TrimPrefix(k, "AOC_"), ""); v != "" {
					return v
				}
			} else if v := os.Getenv(k); v != "" {
				return v
			} else if v := envFile[k]; v != "" {
				return v
			}
		}
		return def
	}
	duration := func(key string, def time.Duration) time.Duration { return parseDur(str(key, ""), def) }
	grpcPublic := str("AOC_GATEWAY_GRPC_PUBLIC", "") == "1" || str("AOC_GATEWAY_GRPC_BIND_ADDR", "") != ""
	return Config{
		Env:               str("AOC_ENV", "development"),
		MeshMode:          str("AOC_MESH_MODE", "standalone"),
		DatabaseURL:       firstStr("postgres://aoc:aoc@127.0.0.1:5433/aoc?sslmode=disable", "AOC_DATABASE_URL", "AOC_AUTH_DB_DSN", "AOC_CHAR_DB_DSN", "AOC_CITYNODE_DB_DSN", "AOC_QUEST_DB_DSN", "DATABASE_URL"),
		AuthHTTPAddr:      firstStr("127.0.0.1:8081", "AOC_AUTH_HTTP_ADDR", "AOC_AUTH_HTTP_BIND_ADDR"),
		AuthGRPCAddr:      firstStr("127.0.0.1:50051", "AOC_AUTH_GRPC_ADDR", "AOC_AUTH_GRPC_BIND_ADDR", "AUTH_GRPC_ADDR"),
		CharacterGRPCAddr: firstStr("127.0.0.1:50052", "AOC_CHARACTER_GRPC_ADDR", "AOC_CHARACTER_GRPC_BIND_ADDR", "AOC_CHAR_GRPC_ADDR", "CHAR_GRPC_ADDR"),
		CityNodeGRPCAddr:  firstStr("127.0.0.1:50055", "AOC_CITYNODE_GRPC_ADDR", "AOC_CITYNODE_GRPC_BIND_ADDR", "CITYNODE_GRPC_ADDR"),
		CityNodeSeed:      str("AOC_CITYNODE_SEED", "config/seeds/citynode_seed.json"),
		QuestsGRPCAddr:    firstStr("127.0.0.1:50058", "AOC_QUESTS_GRPC_ADDR", "AOC_QUEST_GRPC_BIND_ADDR", "AOC_QUEST_GRPC_ADDR", "QUEST_GRPC_ADDR"),
		WorldAdminAddr:    firstStr("127.0.0.1:50056", "AOC_WORLD_ADMIN_ADDR", "WORLDD_ADMIN_GRPC_ADDR"),
		WorldStreamAddr:   firstStr("127.0.0.1:50057", "AOC_WORLD_STREAM_ADDR", "WORLDD_STREAM_GRPC_ADDR"),
		TokenTTL:          duration("AOC_TOKEN_TTL", 24*time.Hour),
		ServiceTokenFile:  str("AOC_SERVICE_TOKEN_FILE", "dist/services/service.token"),
		ContentDB:         str("AOC_CONTENT_DB", "dist/content/content.db"),
		GMStarterSet:      str("AOC_GM_STARTER_SET", "") == "1" || strings.EqualFold(str("AOC_GM_STARTER_SET", ""), "true"),

		MeshGRPCAddr:     firstStr("127.0.0.1:50054", "AOC_MESH_GRPC_ADDR", "AOC_MESH_GRPC_BIND_ADDR", "MESH_GRPC_ADDR"),
		MeshStaticWorld:  str("AOC_MESH_STATIC_WORLD", ""),
		MeshHeartbeatTTL: duration("AOC_MESH_HEARTBEAT_TTL", 15*time.Second),

		GatewayICSAddr:    firstStr("127.0.0.1:443", "AOC_GATEWAY_ICS_ADDR", "AOC_GATEWAY_HTTP_BIND_ADDR", "AOC_GATEWAY_HTTP_ADDR"),
		GatewayGRPCAddr:   firstStr("127.0.0.1:50050", "AOC_GATEWAY_GRPC_ADDR", "AOC_GATEWAY_GRPC_BIND_ADDR"),
		GatewayGRPCPublic: grpcPublic,
		GatewayTLSCert:    str("AOC_GATEWAY_TLS_CERT", "dist/certs/server.pem"),
		GatewayTLSKey:     str("AOC_GATEWAY_TLS_KEY", "dist/certs/server.key"),
		TicketTTL:         duration("AOC_TICKET_TTL", 2*time.Minute),
	}, err
}

func (c Config) Validate() error {
	if c.Env != "development" && c.Env != "staging" && c.Env != "production" {
		return fmt.Errorf("invalid env %q: must be development, staging, or production", c.Env)
	}
	allowInsecureDB := os.Getenv("AOC_ALLOW_INSECURE_DB") == "1" ||
		strings.EqualFold(os.Getenv("AOC_ALLOW_INSECURE_DB"), "true") ||
		strings.Contains(c.DatabaseURL, "@postgres:") ||
		strings.Contains(c.DatabaseURL, "@postgres/")
	badSSL := strings.Contains(c.DatabaseURL, "sslmode=disable") && !allowInsecureDB
	noToken := strings.TrimSpace(c.ServiceTokenFile) == "" &&
		strings.TrimSpace(os.Getenv("AOC_SERVICE_TOKEN")) == "" &&
		strings.TrimSpace(os.Getenv("SANDAL_SERVICE_TOKEN")) == ""
	if c.Env == "production" && (strings.Contains(c.DatabaseURL, "aoc:aoc@") || badSSL || c.GMStarterSet || noToken) {
		return errors.New("production config forbids default/non-TLS database URL, GMStarterSet, and empty ServiceTokenFile")
	}
	return nil
}

func readEnvFile(path string) (map[string]string, error) {
	defaultPath := false
	if path == "" {
		path = resolve(nil, "CONFIG_FILE", "config/sandal.env")
		defaultPath = (path == "config/sandal.env")
	}
	out := make(map[string]string)
	b, err := os.ReadFile(path)
	if err != nil && defaultPath && errors.Is(err, os.ErrNotExist) {
		for _, alt := range []string{"src/config/sandal.env", "../../config/sandal.env"} {
			if ab, aerr := os.ReadFile(alt); aerr == nil {
				b, err = ab, nil
				break
			}
		}
	}
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return out, nil
		}
		return out, err
	}
	for _, raw := range strings.Split(string(b), "\n") {
		if line := strings.TrimSpace(raw); line != "" && !strings.HasPrefix(line, "#") {
			if k, v, ok := strings.Cut(strings.TrimPrefix(line, "export "), "="); ok {
				out[strings.TrimSpace(k)] = strings.Trim(strings.TrimSpace(v), `"'`)
			}
		}
	}
	return out, nil
}

func resolve(envFile map[string]string, suffix, def string) string {
	for _, v := range [4]string{os.Getenv("SANDAL_" + suffix), os.Getenv("AOC_" + suffix), envFile["SANDAL_"+suffix], envFile["AOC_"+suffix]} {
		if v != "" {
			return v
		}
	}
	return def
}

func parseDur(raw string, def time.Duration) time.Duration {
	if d, err := time.ParseDuration(raw); err == nil && d > 0 {
		return d
	}
	return def
}

// HandleHealthcheck exits with 0 if the local service port is reachable when
// -healthcheck or --healthcheck is present in os.Args, or 1 if unreachable.
func HandleHealthcheck(defaultPort string, envKeys ...string) {
	if !hasHealthcheckFlag(os.Args[1:]) {
		return
	}
	ports := resolveProbePorts(defaultPort, envKeys)
	if probeTCPPorts(ports, 2*time.Second) {
		os.Exit(0)
	}
	os.Exit(1)
}

func hasHealthcheckFlag(args []string) bool {
	for _, a := range args {
		if a == "-healthcheck" || a == "--healthcheck" || strings.HasPrefix(a, "-healthcheck=") || strings.HasPrefix(a, "--healthcheck=") {
			return true
		}
	}
	return false
}

func resolveProbePorts(defaultPort string, envKeys []string) []string {
	var explicit, fallbacks []string
	if p := flagPortFromArgs(os.Args[1:]); p != "" {
		explicit = append(explicit, p)
	}
	for _, k := range envKeys {
		if isDigits(k) {
			fallbacks = append(fallbacks, k)
		} else if v := os.Getenv(k); v != "" {
			if p := extractPort(v); p != "" {
				explicit = append(explicit, p)
			}
		}
	}
	for _, k := range [2]string{"SERVICE_HTTP_PORT", "SERVICE_GRPC_PORT"} {
		if p := extractPort(os.Getenv(k)); p != "" {
			explicit = append(explicit, p)
		}
	}
	if len(explicit) > 0 {
		return append(explicit, fallbacks...)
	}
	return append([]string{ extractPort(defaultPort) }, fallbacks...)
}

func flagPortFromArgs(args []string) string {
	for i := 0; i < len(args); i++ {
		a := strings.TrimLeft(args[i], "-")
		if k, v, ok := strings.Cut(a, "="); ok && (k == "addr" || k == "http" || k == "grpc" || k == "port") {
			return extractPort(v)
		}
		if (a == "addr" || a == "http" || a == "grpc" || a == "port") && i+1 < len(args) {
			return extractPort(args[i+1])
		}
	}
	return ""
}

func extractPort(addr string) string {
	addr = strings.TrimSpace(addr)
	if addr == "" {
		return ""
	}
	if _, port, err := net.SplitHostPort(addr); err == nil && port != "" {
		return port
	}
	if idx := strings.LastIndexByte(addr, ':'); idx >= 0 {
		return addr[idx+1:]
	}
	return addr
}

func isDigits(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return len(s) > 0
}

func probeTCPPorts(ports []string, timeout time.Duration) bool {
	seen := make(map[string]bool, len(ports))
	perTry := timeout
	if len(ports) > 1 {
		perTry = timeout / time.Duration(len(ports))
		if perTry < 400*time.Millisecond {
			perTry = 400 * time.Millisecond
		}
	}
	for _, p := range ports {
		if p == "" || seen[p] {
			continue
		}
		seen[p] = true
		if conn, err := net.DialTimeout("tcp", "127.0.0.1:"+p, perTry); err == nil {
			_ = conn.Close()
			return true
		}
	}
	return false
}

// BindContainerFlags registers common container CLI flags on flag.CommandLine,
// parses os.Args, and propagates non-empty flag overrides into AOC_* env vars.
func BindContainerFlags(service string) {
	ensureStringFlag("addr", "", "listen address")
	ensureStringFlag("http", "", "HTTP listen address")
	ensureStringFlag("grpc", "", "gRPC listen address")
	ensureStringFlag("auth", "", "auth gRPC address")
	ensureStringFlag("char", "", "character gRPC address")
	ensureStringFlag("citynode", "", "citynode gRPC address")
	ensureStringFlag("quest", "", "quest gRPC address")
	ensureStringFlag("mesh", "", "mesh gRPC address")
	ensureStringFlag("worldd-admin", "", "worldd admin gRPC address")
	ensureStringFlag("worldd-stream", "", "worldd stream gRPC address")
	ensureStringFlag("worldd-host", "", "static worldd host")
	ensureStringFlag("worldd-port", "", "static worldd port")
	ensureStringFlag("content-db", "", "content database path")
	ensureStringFlag("seed", "", "citynode seed path")
	if flag.Lookup("no-kit-backfill") == nil {
		flag.Bool("no-kit-backfill", false, "skip starter-kit backfill")
	}
	if flag.Lookup("healthcheck") == nil {
		flag.Bool("healthcheck", false, "run healthcheck probe")
	}
	if !flag.Parsed() {
		flag.Parse()
	}
	applyContainerFlagEnv(service)
}

func ensureStringFlag(name, def, usage string) {
	if flag.Lookup(name) == nil {
		flag.String(name, def, usage)
	}
}

func flagVal(name string) string {
	if f := flag.Lookup(name); f != nil {
		return strings.TrimSpace(f.Value.String())
	}
	return ""
}

func applyContainerFlagEnv(service string) {
	addr, httpAddr, grpcAddr := flagVal("addr"), flagVal("http"), flagVal("grpc")
	setIfNonEmpty("AOC_AUTH_GRPC_ADDR", flagVal("auth"))
	setIfNonEmpty("AOC_CHARACTER_GRPC_ADDR", flagVal("char"))
	setIfNonEmpty("AOC_CITYNODE_GRPC_ADDR", flagVal("citynode"))
	setIfNonEmpty("AOC_QUESTS_GRPC_ADDR", flagVal("quest"))
	setIfNonEmpty("AOC_MESH_GRPC_ADDR", flagVal("mesh"))
	setIfNonEmpty("AOC_WORLD_ADMIN_ADDR", flagVal("worldd-admin"))
	setIfNonEmpty("AOC_WORLD_STREAM_ADDR", flagVal("worldd-stream"))
	setIfNonEmpty("AOC_CONTENT_DB", flagVal("content-db"))
	setIfNonEmpty("AOC_CITYNODE_SEED", flagVal("seed"))
	if wh := flagVal("worldd-host"); wh != "" && os.Getenv("AOC_MESH_STATIC_WORLD") == "" {
		wp := flagVal("worldd-port")
		if wp == "" {
			wp = "7777"
		}
		_ = os.Setenv("AOC_MESH_STATIC_WORLD", net.JoinHostPort(wh, wp))
	}
	applyServicePrimaryAddr(service, addr, httpAddr, grpcAddr)
}

func applyServicePrimaryAddr(service, addr, httpAddr, grpcAddr string) {
	primary := firstNonEmpty(httpAddr, addr)
	switch service {
	case "auth":
		setIfNonEmpty("AOC_AUTH_HTTP_ADDR", primary)
		setIfNonEmpty("AOC_AUTH_GRPC_ADDR", grpcAddr)
		if (strings.HasPrefix(primary, ":") || strings.HasPrefix(primary, "0.0.0.0:")) && os.Getenv("AOC_AUTH_GRPC_ADDR") == "" && os.Getenv("AOC_AUTH_GRPC_BIND_ADDR") == "" {
			_ = os.Setenv("AOC_AUTH_GRPC_ADDR", ":"+firstNonEmpty(os.Getenv("SERVICE_GRPC_PORT"), "50051"))
		}
	case "character":
		setIfNonEmpty("AOC_CHARACTER_GRPC_ADDR", firstNonEmpty(grpcAddr, addr))
	case "citynode":
		setIfNonEmpty("AOC_CITYNODE_GRPC_ADDR", firstNonEmpty(grpcAddr, addr))
	case "quest":
		setIfNonEmpty("AOC_QUESTS_GRPC_ADDR", firstNonEmpty(grpcAddr, addr))
	case "mesh":
		setIfNonEmpty("AOC_MESH_GRPC_ADDR", firstNonEmpty(grpcAddr, addr))
	case "gateway":
		setIfNonEmpty("AOC_GATEWAY_ICS_ADDR", primary)
		if grpcAddr != "" {
			_ = os.Setenv("AOC_GATEWAY_GRPC_ADDR", grpcAddr)
			_ = os.Setenv("AOC_GATEWAY_GRPC_PUBLIC", "1")
		}
	case "meshgw":
		setIfNonEmpty("AOC_MESHGW_ADDR", primary)
	case "admin":
		setIfNonEmpty("AOC_ADMIN_ADDR", primary)
	}
}

func setIfNonEmpty(envKey, val string) {
	if val != "" {
		_ = os.Setenv(envKey, val)
	}
}

func firstNonEmpty(vals ...string) string {
	for _, v := range vals {
		if strings.TrimSpace(v) != "" {
			return strings.TrimSpace(v)
		}
	}
	return ""
}
