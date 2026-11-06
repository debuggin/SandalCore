# SandalCore Configuration Reference

This document specifies every configuration file in [`src/config/`](../src/config/), local override directories (`$HOME/.aoc/certs/*` and `$HOME/.aoc/usmap/*`), and every environment variable in [`src/config/sandal.env.example`](../src/config/sandal.env.example).

---

## 1. Configuration Files & Local Overrides (`src/config/` & `$HOME/.aoc/`)

At build, deployment, and runtime, configuration resolves from lowest to highest precedence:

`src/config/sandal.env.example` -> `deploy/.env` (and `src/config/sandal.local.env`) -> container environment (`src/deploy/compose.yml`) -> process environment / CLI flags.

| File / Directory | Local Override Path | Purpose |
|:---|:---|:---|
| [`src/config/sandal.env.example`](../src/config/sandal.env.example) | `deploy/.env`, `src/config/sandal.local.env` | Canonical environment variable template for the 11-container Linux stack and host scripts. |
| [`src/config/profiles/docker-single.env.example`](../src/config/profiles/docker-single.env.example) | `deploy/.env` | Single-host Docker Compose profile defaults. |
| [`src/config/profiles/local-dev.env.example`](../src/config/profiles/local-dev.env.example) | `src/config/sandal.local.env` | Native loopback host-process profile defaults (`127.0.0.1` ports). |
| [`src/config/profiles/prod-mesh.env.example`](../src/config/profiles/prod-mesh.env.example) | Kubernetes `ConfigMap` / `Secret` | Multi-node Kubernetes production cluster profile defaults. |
| [`src/config/launcher.json`](../src/config/launcher.json) | `src/config/launcher.local.json` | `SandalLauncher` default gateway/ICS addresses, tether UDP port (`19021`), and UI settings. |
| [`src/config/paths.json`](../src/config/paths.json) | `src/config/paths.local.json`, `dist/config/paths.json` | Game client executable path (`game_executable`), extracted content directory, and log paths. |
| [`src/config/rates.json`](../src/config/rates.json) | `src/config/rates.local.json` | Server XP, drop, gathering, and progression rate multipliers. |
| [`src/config/realm.json`](../src/config/realm.json) | `src/config/realm.local.json` | Realm ID, display name, and population cap settings. |
| [`src/config/server.json`](../src/config/server.json) | `src/config/server.local.json` | Server bind addresses, tick rate, and service connection settings. |
| [`src/config/seeds/citynode_seed.json`](../src/config/seeds/citynode_seed.json) | `--citynode-seed` / `AOC_CITYNODE_SEED` | Starting settlement node levels for the 5 Riverlands towns (Joeva `4` Town, Halcyon `3` Village, Winstead/New Aela/Miraleth `0` Wilderness). |
| [`src/config/hotfixes/0001_initial_overlay.sql`](../src/config/hotfixes/0001_initial_overlay.sql) | `AOC_HOTFIX_DIR` (`config/hotfixes`) | SQL hotfix overlay scripts applied on top of extracted design data. |
| [`src/config/certs/`](../src/config/certs/README.md) | `$HOME/.aoc/certs/*` (or `SANDAL_CERTS_OVERRIDE_DIR` / `SANDAL_TLS_CERT_PATH` + `SANDAL_TLS_KEY_PATH`) | TLS certificate directory (`server.pem` + `server.key` or `fullchain.pem` + `privkey.pem`). Reuses existing or `$HOME/.aoc/certs/*` certificates automatically, falling back to local Root CA generation when neither exists. |
| [`src/config/layouts/aoc.usmap`](../src/config/layouts/aoc.usmap) | `$HOME/.aoc/usmap/*` (or `SANDAL_USMAP_PATH` / `AOC_USMAP` / `SANDAL_USMAP_OVERRIDE_DIR`) | Unreal Engine reflection mappings file (`aoc.usmap` / `*.usmap`). Automatically prefers `$HOME/.aoc/usmap/*` (or the env override) during extraction and staging without modifying `src/config/layouts/aoc.usmap`. |

---

## 2. Environment Variables Reference (`src/config/sandal.env.example`)

### 2.1 Core Runtime, Logging & Client Paths

| Environment Variable | Default | Description |
|:---|:---|:---|
| `AOC_ENV` | `development` | Runtime environment profile name (`development`, `staging`, `production`). |
| `SANDAL_CONFIG_FILE` | `config/sandal.env` | Path to the base environment configuration file loaded by scripts. |
| `SANDAL_ADVERTISE_HOST` | `ingress-proxy` | Default hostname advertised between containers (`127.0.0.1` on host loopback). |
| `SANDAL_LOG_LEVEL` | `INFO` | Minimum log severity (`DEBUG`, `INFO`, `WARN`, `ERROR`). |
| `SANDAL_LOG_FORMAT` | `json` | Log output format (`json` or `text`). |
| `GAME_ROOT` / `SANDAL_CLIENT_PAK_DIR` | `C:\Program Files\Intrepid Studios\Ashes of Creation\AOC\Content\Paks` (auto-discovered via Steam `libraryfolders.vdf`) | Installed Ashes of Creation client root (`...\Ashes of Creation\Game`) or `AOC\Content\Paks` directory. |
| `SANDAL_CLIENT_EXE_PATH` | `AOC\Binaries\Win64\AoCClient-Win64-Shipping.exe` | Relative path to the game client executable inside the game root. |
| `SANDAL_CLIENT_USE_EOS` | `false` | Controls whether the game client enables Epic Online Services (`-UseEOS=false`). |
| `AOC_CLIENT_CONFIG` | `dist/content/client_config` | Extracted client `AOC\Config` directory (`DefaultCombat.ini`). |
| `AOC_ZOI_CHUNKS` | `dist/content/zoi_chunks` | Extracted Zone-of-Influence chunk `.uasset` directory. |
| `AOC_MIRROR_LOGS_ROOT` | `dist/logs/mirror` | Host directory for mirrored service and world server logs. |

### 2.2 Host TLS Certificate & Unreal Mappings (`.usmap`) Overrides

| Environment Variable | Default | Description |
|:---|:---|:---|
| `SANDAL_CERTS_OVERRIDE_DIR` / `AOC_CERTS_OVERRIDE_DIR` | `$HOME/.aoc/certs` | Host directory searched for user-provisioned `server.pem` + `server.key` (or `fullchain.pem` + `privkey.pem`) before falling back to `src/config/certs/`. |
| `SANDAL_TLS_CERT_PATH` | Unset (uses `SANDAL_CERTS_OVERRIDE_DIR`) | Explicit file path to a custom TLS leaf/chain certificate (`server.pem` / `fullchain.pem`). |
| `SANDAL_TLS_KEY_PATH` | Unset (uses `SANDAL_CERTS_OVERRIDE_DIR`) | Explicit file path to the matching TLS private key (`server.key` / `privkey.pem`). |
| `SANDAL_USMAP_PATH` / `AOC_USMAP` | `$HOME/.aoc/usmap/aoc.usmap` (fallback `src/config/layouts/aoc.usmap`) | Explicit file path to an Unreal Engine `.usmap` mappings file used during extraction and staging. |
| `SANDAL_USMAP_OVERRIDE_DIR` / `AOC_USMAP_OVERRIDE_DIR` | `$HOME/.aoc/usmap` | Host directory searched for `aoc.usmap` (or any non-empty `*.usmap`) before falling back to `src/config/layouts/aoc.usmap`. |

### 2.3 World Server In-Memory File Caching (`AOC_WORLD_CACHE_*`)

By default, `world` (`aoc-world`) preloads `content.db` (`~314 MB`) and `area_nav.bin` (`~1.0 GB`) into RAM at startup so zero disk reads or WSL2 9P bind-mount I/O operations occur during gameplay ticks.

| Environment Variable | Default | Valid Values | Description |
|:---|:---:|:---|:---|
| `AOC_WORLD_CACHE_ALL_CONTENT_IN_MEMORY` | `true` | `true \| false` (`1 \| 0`) | Master toggle to preload all runtime-streamed content files (`content.db` and `area_nav.bin`) into RAM at world server startup so zero disk reads occur during gameplay. |
| `AOC_WORLD_CACHE_CONTENT_DB_IN_MEMORY` | `true` | `true \| false` (`1 \| 0`) | Preloads `dist/content/content.db` (`~314 MB`) into RAM at startup (`true`), or reads from disk on demand when disabled (`false`). |
| `AOC_WORLD_CACHE_AREA_NAV_IN_MEMORY` | `true` | `true \| false` (`1 \| 0`) | Preloads `dist/content/area_nav.bin` (`~1.0 GB`) into memory at startup so the background navmesh chunk loader streams tiles directly from RAM without hitting disk or WSL2 9P mounts. |
| `AOC_WORLD_CACHE_FILES` | `content.db,area_nav.bin` | `content.db`, `area_nav.bin`, `all`, or `none` | Comma-separated list of runtime files to cache in memory. Overrides individual per-file defaults when set. |

On hosts with less than `4 GB` of available RAM, disable `area_nav.bin` preloading or all in-memory file caching in `deploy/.env` or `src/config/sandal.local.env`:

```dotenv
# Option A: Keep content.db (~314 MB) in RAM, stream area_nav.bin (~1.0 GB) from disk on demand
AOC_WORLD_CACHE_AREA_NAV_IN_MEMORY=false
AOC_WORLD_CACHE_FILES=content.db

# Option B: Disable all in-memory file preloading (stream both files from disk on demand)
AOC_WORLD_CACHE_ALL_CONTENT_IN_MEMORY=false
AOC_WORLD_CACHE_CONTENT_DB_IN_MEMORY=false
AOC_WORLD_CACHE_AREA_NAV_IN_MEMORY=false
AOC_WORLD_CACHE_FILES=none
```

### 2.4 World Server Per-Tick Entity & Spatial Streaming Budgets (`AOC_WORLD_*`)

| Environment Variable | Default | Valid Range | Description |
|:---|:---:|:---:|:---|
| `AOC_WORLD_NAV_PUMP_BUDGET_MS` | `4.0` ms | `0.5..50.0` | Per-tick millisecond budget for integrating newly arrived navmesh tiles on the world tick thread. |
| `AOC_WORLD_NPC_GROUND_PER_TICK` | `32` | `1..4096` | Maximum NPCs grounded onto newly resident navmesh chunks per `30 Hz` tick. |
| `AOC_WORLD_NPC_GROUND_BUDGET_MS` | `2.0` ms | `0.25..50.0` | Per-tick millisecond budget for incremental NPC grounding across chunk boundaries. |
| `AOC_WORLD_NPC_STATS_PER_TICK` | `24` | `1..1024` | Maximum lazy NPC stat block evaluations executed per combat tick. |
| `AOC_WORLD_NPC_PATHFINDS_PER_STEP` | `24` | `1..512` | Maximum pathfinding queries executed per `15 Hz` NPC AI step. |
| `AOC_WORLD_ACTOR_OPEN_PER_TICK` | `24` | `1..512` | Maximum new actor replication channels opened per player per tick. |
| `AOC_WORLD_ACTOR_WAKE_PER_TICK` | `24` | `1..512` | Maximum dormant actor channels woken per player per tick when existing actors re-enter relevancy. |
| `AOC_WORLD_ACTOR_UPDATE_PER_TICK` | `64` | `1..1024` | Maximum active actor transform and state delta updates replicated per player per tick. |
| `AOC_WORLD_NODE_SUMMARIES_PER_TICK` | `4` | `1..128` | Maximum city-node and siege summary packets drained per player per tick. |
| `AOC_WORLD_FOLIAGE_CELLS_PER_TICK` | `2` | `1..64` | Maximum painted foliage cells streamed per player per tick. |

### 2.5 Database, Extraction, Navmesh & Security Tokens

| Environment Variable | Default | Description |
|:---|:---|:---|
| `AOC_SKIP_EXTRACTION` | `0` | Set to `1` to skip automatic game-data extraction checks on startup. |
| `AOC_EXTRACTOR_THREADS` | `0` | Worker thread count for extraction tools (`0` = auto-detect logical CPU cores). |
| `AOC_EXTRACTED_DIR` | `dist/extracted` | Output directory for raw extracted assets. |
| `AOC_MESH_MODE` | `standalone` | Topology mode (`standalone` single-node or `mesh` multi-node). |
| `AOC_DATABASE_URL` | `postgres://aoc:aoc@postgres:5432/aoc?sslmode=disable` | PostgreSQL 17 connection URI (`aoc-postgres`). |
| `POSTGRES_USER` | `aoc` | PostgreSQL role username. |
| `POSTGRES_PASSWORD` | `CHANGE_ME` | PostgreSQL role password (`deploy/.env`). |
| `POSTGRES_DB` | `aoc` | PostgreSQL database name. |
| `POSTGRES_PORT` / `AOC_POSTGRES_HOST_PORT` | `5432` / `5433` | Container PostgreSQL port (`5432`) and optional host port mapping (`5433`). |
| `AOC_DB_MAX_CONNS` / `AOC_DB_MIN_CONNS` | `16` / `2` | Maximum and minimum PostgreSQL connection pool size per Go service. |
| `AOC_DB_MAX_CONN_LIFETIME` / `AOC_DB_MAX_CONN_IDLE_TIME` | `30m` / `5m` | Optional PostgreSQL connection lifetime and idle timeout bounds. |
| `AOC_SQL_MIGRATIONS_DIR` | `services/sql/migrations` | Directory containing the 24 PostgreSQL schema migration files (`0001..0024`). |
| `AOC_CONTENT_DB` / `AOC_CONTENT_DB_PATH` | `dist/content/content.db` | Compiled SQLite `v41` design database file path (`/app/content/content.db` in containers). |
| `AOC_CONTENT_DIR` | `dist/content` | Directory containing all 25 runtime game-data artifacts. |
| `AOC_HOTFIX_DIR` | `config/hotfixes` | Directory containing SQL hotfix overlays. |
| `AOC_PMTILES_PATH` | `dist/content/verra.pmtiles` | Map tile archive served to `web-admin`. |
| `AOC_NAVMESH_DIR` | `dist/nav` | Navigation mesh directory path. |
| `AOC_RIVERLANDS_NAVMESH_PATH` | `dist/nav/riverlands.aocnav` | Riverlands navmesh file path. |
| `AOC_AREA_NAV` / `AOC_NAV_MESH` | `dist/content/area_nav.bin` | Tiled Recast/Detour navigation mesh and terrain heightfield path. |
| `AOC_VMAP_PATH` | `dist/nav/area_vmap.bin` | Visibility/collision map binary path. |
| `AOC_SERVICE_TOKEN_FILE` / `AOC_SERVICE_TOKEN` | `dist/services/service.token` | Shared internal gRPC Bearer authentication token file or literal value. |
| `AOC_TOKEN_TTL` | `24h` | Session access token lifetime issued by `auth`. |
| `AOC_TICKET_TTL` | `2m` | Single-use world handoff ticket lifetime redeemed by `world` via `gateway`. |

### 2.6 Control-Plane Services, World Simulation & Envoy Ingress Ports/Addresses

| Environment Variable | Default | Description |
|:---|:---|:---|
| `INGRESS_HTTP_PORT` / `ENVOY_HTTP_INGRESS_PORT` | `8080` | Envoy (`aoc-ingress-proxy`) HTTP/1.1 & WebSocket/SSE edge ingress port (`http://127.0.0.1:8080`). |
| `INGRESS_GRPC_PORT` / `ENVOY_GRPC_INGRESS_PORT` | `9090` | Envoy gRPC HTTP/2 edge ingress port (`127.0.0.1:9090`). |
| `ENVOY_ADMIN_PORT` | `9901` | Envoy container-internal admin/readiness interface port (`:9901/ready`). |
| `AOC_GATEWAY_HTTP_BIND_ADDR` / `AOC_GATEWAY_HTTP_ADVERTISE_ADDR` / `GATEWAY_HTTP_PORT` | `[::]:8081` / `gateway:8081` / `8081` | `aoc-gateway` HTTP REST bind address, advertise address, and port. |
| `AOC_GATEWAY_ICS_BIND_ADDR` / `AOC_GATEWAY_ICS_ADVERTISE_ADDR` | `[::]:50061` / `gateway:50061` | `aoc-gateway` ICS TLS 1.3 lobby listener (`127.0.0.1:50061` via Envoy TLS passthrough). |
| `AOC_GATEWAY_GRPC_BIND_ADDR` / `AOC_GATEWAY_GRPC_ADVERTISE_ADDR` / `GATEWAY_GRPC_PORT` | `[::]:50050` / `gateway:50050` / `50050` | `aoc-gateway` internal `GatewayService` gRPC bind address, advertise address, and port. |
| `AOC_GATEWAY_TLS_CERT` / `AOC_GATEWAY_TLS_KEY` / `AOC_TLS_CA_CERT` | `/app/certs/server.pem`, `/app/certs/server.key`, `dist/certs/ca.pem` | TLS certificate, private key, and CA bundle paths mounted into `aoc-gateway` and `aoc-ingress-proxy`. |
| `AOC_ADMIN_HTTP_BIND_ADDR` / `AOC_ADMIN_HTTP_ADVERTISE_ADDR` / `WEB_ADMIN_PORT` / `WEB_PORT` | `[::]:8088` / `web-admin:8088` / `8088` | `aoc-web-admin` HTTP bind address, advertise address, and port. |
| `AOC_ADMIN_HTTPS_BIND_ADDR` / `AOC_ADMIN_HTTPS_ADVERTISE_ADDR` | `[::]:8443` / `web-admin:8443` | `aoc-web-admin` HTTPS bind and advertise addresses. |
| `AOC_ADMIN_METRICS_BIND_ADDR` / `AOC_ADMIN_METRICS_ADVERTISE_ADDR` | `[::]:9099` / `web-admin:9099` | `aoc-web-admin` Prometheus metrics bind and advertise addresses. |
| `AOC_TETHER_UDP_PORT` | `19021` | Windows host `SandalLauncher` in-memory `LocalTetherListener` UDP port (`127.0.0.1:19021/udp`). |
| `AOC_MESH_GATEWAY_UDP_BIND_ADDR` / `AOC_MESH_GATEWAY_UDP_ADVERTISE_ADDR` / `MESH_GATEWAY_HTTP_PORT` | `[::]:8082` / `mesh-gateway:8082` / `8082` | `aoc-mesh-gateway` bind address, advertise address, and `/api/v1/gm` HTTP port. |
| `AOC_AUTH_HTTP_BIND_ADDR` / `AOC_AUTH_HTTP_ADVERTISE_ADDR` | `[::]:8081` / `auth:8081` | `aoc-auth` HTTP bind and advertise addresses. |
| `AOC_AUTH_GRPC_BIND_ADDR` / `AOC_AUTH_GRPC_ADVERTISE_ADDR` / `AUTH_GRPC_PORT` / `AUTH_GRPC_ADDR` | `[::]:50052` / `auth:50052` / `50052` | `aoc-auth` `AuthService` gRPC bind address, advertise address, and port. |
| `AOC_CHARACTER_GRPC_BIND_ADDR` / `AOC_CHARACTER_GRPC_ADVERTISE_ADDR` / `CHARACTER_GRPC_PORT` / `CHAR_GRPC_ADDR` | `[::]:50053` / `character:50053` / `50053` | `aoc-character` `CharacterService` gRPC bind address, advertise address, and port. |
| `AOC_MESH_GRPC_BIND_ADDR` / `AOC_MESH_GRPC_ADVERTISE_ADDR` / `AOC_DIRECTOR_GRPC_BIND_ADDR` / `AOC_DIRECTOR_GRPC_ADVERTISE_ADDR` / `MESH_GRPC_PORT` / `MESH_GRPC_ADDR` | `[::]:50054` / `mesh:50054` / `50054` | `aoc-mesh` `MeshService` gRPC bind address, advertise address, and port. |
| `AOC_MESH_HEARTBEAT_TTL` / `AOC_MESH_STATIC_WORLD` | `15s` / `""` | World server registration lease TTL and optional static world endpoint override in `aoc-mesh`. |
| `AOC_CITYNODE_GRPC_BIND_ADDR` / `AOC_CITYNODE_GRPC_ADVERTISE_ADDR` / `CITYNODE_GRPC_PORT` / `CITYNODE_GRPC_ADDR` | `[::]:50056` / `citynode:50056` / `50056` | `aoc-citynode` `CityNodeService` gRPC bind address, advertise address, and port. |
| `AOC_CITYNODE_SEED` | `config/seeds/citynode_seed.json` | Settlement node level seed file path. |
| `AOC_QUEST_GRPC_BIND_ADDR` / `AOC_QUEST_GRPC_ADVERTISE_ADDR` / `QUEST_GRPC_PORT` / `QUEST_GRPC_ADDR` | `[::]:50057` / `quest:50057` / `50057` | `aoc-quest` `QuestService` gRPC bind address, advertise address, and port. |
| `AOC_REALM_ID` / `AOC_REALM_NAME` | `verra-dev` / `Verra Development` | Realm identifier and display name advertised to clients. |
| `AOC_WORLD_SERVER_ID` | `world-0` | Authoritative `world` zone server instance identifier. |
| `WORLD_UDP_PORT` / `AOC_WORLD_UDP_PORT` / `WORLD_PORT` | `7777` | `world` (`aoc-world`) UE5 UDP replication port (`127.0.0.1:7777/udp`). |
| `AOC_WORLD_UDP_BIND_ADDR` / `AOC_WORLD_UDP_ADVERTISE_ADDR` / `WORLD_PUBLIC_HOST` / `WORLD_HOST` | `[::]:7777` / `world:7777` / `127.0.0.1` | `world` UDP bind address and advertised host/address returned in `CharacterEnterWorld`. |
| `AOC_WORLD_ADMIN_BIND_ADDR` / `WORLD_ADMIN_PORT` / `WORLD_ADMIN_GRPC_ADDR` | `[::]:50061` / `50061` / `world:50061` | `WorldAdminService` gRPC bind address and port (published on host `127.0.0.1:50055` and Envoy `:9090`). |
| `AOC_WORLD_TICK_RATE_HZ` / `AOC_TICK_HZ` | `30` | Authoritative simulation tick rate in Hz (`30 Hz`). |
| `AOC_GM_STARTER_SET` | `true` | Grants newly created characters the starter equipment and consumable kit when enabled. |
| `AOC_AREA_RADIUS_M` | `-1` | Living-area spawn radius in meters (`-1` = full Riverlands, `0` = no non-player actors, `> 0` = circle radius). |
