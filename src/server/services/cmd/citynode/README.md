# CityNode Service

**Related Guides**: [Root Overview](../../../../../README.md) | [Server Workers](../../../../../docs/SERVER_WORKERS.md) | [Control-Plane Services](../../../../../docs/SERVICES.md) | [Node CLI (`nodectl`)](../nodectl/README.md)

`src/server/services/cmd/citynode` (`aoc-citynode`, listening on `:50053` gRPC) manages settlement progression, node experience, mayoral state, tax rates, and building plots for all 85 City Nodes in Verra.

---

## Core Responsibilities

- **Settlement Stage Progression (`0..6`)**: Tracks each node's stage (`0 = Wilderness`, `1 = Crossroads`, `2 = Encampment`, `3 = Village`, `4 = Town`, `5 = City`, `6 = Metropolis`) and experience in PostgreSQL (`city_node_state`).
- **Live Worker Notification**: Broadcasts stage transitions and building changes to `aoc-world` workers and `aoc-mesh-gateway` so town NPCs, guards, vendors, and crafting stations update in real time (with Tier 5 and Tier 6 settlements automatically using the Tier 4 populated layout in `content.db`).
- **Civic Governance**: Stores settlement mayors, tax rates, treasury balances, and plot assignments.

---

## Configuration

| Environment Variable | Default | Description |
|:---|:---|:---|
| `AOC_CITYNODE_GRPC_ADDR` | `:50053` | Internal gRPC listen address for `CityNodeService`. |
| `AOC_CITYNODE_METRICS_ADDR` | `:9093` | HTTP listen address for `/livez`, `/readyz`, `/healthz`, and `/metrics`. |
| `DATABASE_URL` | `postgres://aoc:aoc@postgres:5432/aoc?sslmode=disable` | PostgreSQL connection string. |

---

## Testing

```powershell
.\sandal.ps1 test
```
