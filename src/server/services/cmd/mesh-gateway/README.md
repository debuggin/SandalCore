# Mesh Gateway

**Related Guides**: [Root Overview](../../../../../README.md) | [Server Workers](../../../../../docs/SERVER_WORKERS.md) | [Web Admin Architecture](../../../../../docs/WEB_ADMIN_ARCHITECTURE.md) | [Mesh Service](../mesh/README.md) | [World Server](../../../world/README.md)

`src/server/services/cmd/mesh-gateway` (`aoc-mesh-gateway`, listening on `:50056` gRPC and `:8082` HTTP) coordinates cross-worker player handovers, persists cluster simulation state in PostgreSQL, and coalesces the `30 Hz` world-state stream for the Web Admin Studio.

---

## Core Responsibilities

- **Seamless Cross-Border Player Handovers**: Coordinates `InitiateHandover`, `PrepareHandover`, and `CompleteHandover` between `world` workers, incrementing the character's monotonic `lease_epoch` (`fencing_token`) so delayed saves from an older worker are rejected.
- **PostgreSQL Simulation Persistence (`world_simulation_state` and `city_node_state`)**: Persists the continuous `9x` server clock, the 360-day 4-season Verra Calendar (`Spring`, `Summer`, `Autumn`, `Winter`), per-worker tick-rate settings (`Dynamic`, `0 Hz` NPC pause, or `10..60 Hz`), and City Node progression (`Stage 0..6`, XP, mayors, and taxes).
- **`30 Hz` World-State Stream**: Aggregates entity snapshots, worker shard telemetry, and world clock state every `33 ms` (`AOC_SERVICE_TICK_HZ=30`) for `aoc-gateway` and the Web Admin map canvas.

---

## Configuration

| Environment Variable | Default | Description |
|:---|:---|:---|
| `AOC_MESH_GATEWAY_GRPC_ADDR` | `:50056` | Internal gRPC listen address for worker handover RPCs. |
| `AOC_MESH_GATEWAY_HTTP_ADDR` | `:8082` | Internal HTTP listen address for state snapshots and admin mutations. |
| `AOC_SERVICE_TICK_HZ` | `30` | Tick frequency (`30 Hz`, `33.33 ms`) for snapshot aggregation. |
| `DATABASE_URL` | `postgres://aoc:aoc@postgres:5432/aoc?sslmode=disable` | PostgreSQL connection string. |

---

## Testing

```powershell
.\sandal.ps1 test
pwsh -NoProfile -File scripts/test_functional_apis.ps1
```
