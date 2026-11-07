# Container Stack and Deployment

**Related Guides**: [Root Overview](../../README.md) | [Configuration Reference](../../docs/CONFIGURATION.md) | [Operational Runbook](../../docs/RUNBOOK.md) | [Control-Plane Services](../../docs/SERVICES.md) | [TLS Certificates](../config/certs/README.md)

`src/deploy` contains the Docker Compose topology (`src/deploy/compose.yml`), multi-stage Linux Dockerfiles (`src/deploy/docker/*.Dockerfile`), Envoy/NGINX ingress configuration, and Kubernetes / systemd deployment manifests for SandalCore.

---

## Default 11-Container Linux Stack

When started in the default single-worker Riverlands mode (`AOC_WORLD_PARTITION_MODE=single_riverlands`), `src/deploy/compose.yml` runs **11 Linux containers**:

| Container | Dockerfile / Image | Exposed Ports | Role |
|:---|:---|:---:|:---|
| `aoc-postgres` | `postgres:16-alpine` | `:5432` (TCP) | PostgreSQL relational database for accounts, characters, quests, simulation state, and City Nodes. |
| `aoc-auth` | `docker/services.Dockerfile` (`auth`) | `:50051` (gRPC) | Account authentication, Argon2id password hashing, and session tickets. |
| `aoc-character` | `docker/services.Dockerfile` (`character`) | `:50052` (gRPC), `:50061` (TLS 1.3) | UE5 `IntrepidLobbyService` (`:50061` ICS TLS 1.3) and character persistence. |
| `aoc-citynode` | `docker/services.Dockerfile` (`citynode`) | `:50053` (gRPC) | Settlement stage (`0..6`), XP, mayor, and tax state for all 85 City Nodes. |
| `aoc-quest` | `docker/services.Dockerfile` (`quest`) | `:50054` (gRPC) | Character quest logs, objective counters, and GM quest commands. |
| `aoc-mesh` | `docker/services.Dockerfile` (`mesh`) | `:50055` (gRPC) | Sequential `worker_index` (`1..N`) allocator and spatial coordinate directory. |
| `aoc-mesh-gateway` | `docker/services.Dockerfile` (`mesh-gateway`) | `:50056` (gRPC), `:8082` (HTTP) | Cross-worker player handovers, simulation state persistence, and `30 Hz` stream coalescing. |
| `aoc-gateway` | `docker/services.Dockerfile` (`gateway`) | `:8080` (HTTP/WS) | Public HTTP/WebSocket gateway for launcher login, tether tickets, and `/api/v1/admin/*` routes. |
| `aoc-web-admin` | `docker/web-admin.Dockerfile` | `:8090` (HTTP) | Serves the Web Admin frontend bundle and HTTP `206` `verra.pmtiles` map tiles. |
| `aoc-ingress-proxy` | Envoy / NGINX | `:8443` (HTTPS), `:8088` (HTTP) | TLS 1.3 edge proxy terminating `dist/certs/server.pem` + `server.key`. |
| `aoc-world` (`aoc-riverlands`) | `docker/world.Dockerfile` | `:7777+` (UDP), `:50061+` (gRPC) | Authoritative zone simulation worker (`2560M` RAM limit). |

> [!NOTE]
> The UDP launcher tether (`127.0.0.1:19021/udp`) runs in-process inside `SandalLauncher.exe` on the Windows host, never as a Docker container.

---

## Container Hardening and Zero Disk Log Churn

Every runtime container in `src/deploy/compose.yml` enforces:

- **Read-Only Root Filesystem (`read_only: true`)**: Containers cannot modify their image filesystem at runtime.
- **RAM-Backed Scratch Mounts (`tmpfs`)**: `/tmp` and `/app/logs` are mounted in RAM (`tmpfs`), and logs are emitted to `stdout`/`stderr` with capped memory buffers so running the server produces zero log file churn on your SSD.
- **Bounded Worker Memory (`2560M`)**: Each `world` worker container is capped at `2560M` (`2.5 GiB`, configurable via `AOC_WORLD_WORKER_MEM_LIMIT`).

---

## Managing the Stack

```powershell
# Build all container images and desktop binaries
.\build.bat

# Start the container stack and open SandalLauncher
.\start.bat

# Stop all running containers
.\stop.bat
```
