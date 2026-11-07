# Character Service

**Related Guides**: [Root Overview](../../../../../README.md) | [Control-Plane Services](../../../../../docs/SERVICES.md) | [World Server](../../../world/README.md) | [SandalLauncher](../../../../clients/launcher/README.md) | [Character Import CLI](../charimport/README.md)

`src/server/services/cmd/character` (`aoc-character`) serves both the external Unreal Engine 5.6 `IntrepidLobbyService` over TLS 1.3 (`:50061`) and the internal `CharacterService` gRPC API (`:50052`) used by `world` workers to load and save character state.

---

## Core Responsibilities

- **External ICS TLS 1.3 Lobby (`:50061`)**: Terminates TLS 1.3 connections from `AOCClient-Win64-Shipping.exe` and handles `RealmList`, native in-game `CharacterCreate`, `CharacterList`, `CharacterDelete`, and `CharacterEnterWorld`.
- **Spatial Worker Routing on World Entry**: When a player enters the world, `aoc-character` queries `aoc-mesh` (`ResolveWorkerForPosition`) using the character's saved `(X, Y, Z)` coordinates and directs the client to the owning `world` worker's UDP `:7777+` port.
- **PostgreSQL Persistence and Lease Fencing**: Loads and saves character transforms, attributes, inventories (`character_items`, `character_resource_items`, `character_warehouse_items`), wallets, skill trees, professions, and Emberspring attunements, enforcing monotonic `lease_epoch` (`fencing_token`) checks so stale worker saves are rejected.

---

## Configuration

| Environment Variable | Default | Description |
|:---|:---|:---|
| `AOC_CHARACTER_GRPC_ADDR` | `:50052` | Internal gRPC listen address for `world` workers and CLIs. |
| `AOC_ICS_ADDR` | `:50061` | External TLS 1.3 listen address for the UE5 `IntrepidLobbyService`. |
| `AOC_TLS_CERT` / `AOC_TLS_KEY` | `/app/certs/server.pem` / `server.key` | TLS 1.3 certificate and private key paths mounted from `dist/certs/`. |
| `DATABASE_URL` | `postgres://aoc:aoc@postgres:5432/aoc?sslmode=disable` | PostgreSQL connection string. |

---

## Testing

```powershell
.\sandal.ps1 test
pwsh -NoProfile -File scripts/test_functional_apis.ps1
```
