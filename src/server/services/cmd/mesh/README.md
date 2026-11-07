# Mesh Service

**Related Guides**: [Root Overview](../../../../../README.md) | [Server Workers](../../../../../docs/SERVER_WORKERS.md) | [Control-Plane Services](../../../../../docs/SERVICES.md) | [Mesh Gateway](../mesh-gateway/README.md) | [World Server](../../../world/README.md)

`src/server/services/cmd/mesh` (`aoc-mesh`, listening on `:50055` gRPC) is the spatial worker directory and index allocator for SandalCore.

---

## Core Responsibilities

- **Sequential `worker_index` Allocation (`1..N`)**: Assigns each registering `world` worker a unique `worker_index` via `RegisterWorld`. Workers embed this index into their `ServerGuidAllocator` so dynamically spawned 128-bit actor `FGuid`s never collide across workers.
- **Spatial Coordinate Resolution**: Evaluates `(x, y)` world coordinates against registered worker boundaries using the 5-tier containment hierarchy (`poi` > `node_plug` > `zoi` > `region` > `world`).
- **Heartbeat Monitoring**: Tracks worker liveness and updates the cluster directory consumed by `aoc-character`, `aoc-mesh-gateway`, and `aoc-gateway`.

---

## Configuration

| Environment Variable | Default | Description |
|:---|:---|:---|
| `AOC_MESH_GRPC_ADDR` | `:50055` | Internal gRPC listen address for `MeshService`. |
| `AOC_MESH_METRICS_ADDR` | `:9095` | HTTP listen address for `/livez`, `/readyz`, `/healthz`, and `/metrics`. |

---

## Testing

```powershell
.\sandal.ps1 test
```
