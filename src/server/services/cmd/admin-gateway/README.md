# Admin Gateway

**Related Guides**: [Root Overview](../../../../../README.md) | [Web Admin Guide](../../../../../docs/WEB_ADMIN.md) | [Web Admin Architecture](../../../../../docs/WEB_ADMIN_ARCHITECTURE.md) | [Gateway Service](../gateway/README.md) | [Services Overview](../../README.md)

`src/server/services/cmd/admin-gateway` (along with `internal/admin-gateway`, `internal/admin-catalog`, and `internal/world-stream`) implements the backend routes, authentication middleware, catalog indexers, and `30 Hz` world-state stream for the SandalCore **Web Admin and GM Studio**.

---

## Core Responsibilities

- **Single-Boot Passphrase Authentication**: Verifies `AOC_ADMIN_USERNAME` (`Admin`) and the startup passphrase (`AOC_ADMIN_PASSPHRASE` in `deploy/.admin_passphrase`) in constant time (`POST /api/v1/admin/auth/login`), returning a scoped `HS256` JWT bound to `AOC_ADMIN_BOOT_ID`.
- **Constant `30 Hz` Viewport Entity Streaming**: Polls registered `world` workers at `AOC_SERVICE_TICK_HZ` (`30 Hz`, `33 ms`) and serves viewport-filtered (`minX`, `minY`, `maxX`, `maxY`) entity snapshots on `GET /api/v1/world/state`.
- **`.tsv` Catalog Indexing (`memory` or `disk`)**: Serves searchable NPC (`npcs.tsv`), item (`items.tsv`), POI (`pois.tsv`), City Node (`city_nodes.tsv`), and ZOI (`zoi_polygons.tsv`) catalogs according to `AOC_DATA_MODE_*`.
- **Game Master Command Dispatch and Audit Logging**: Validates admin JWT scopes (`gm:commands`, `gm:spawn`, `gm:teleport`, `gm:node_governance`, `gm:sql_hotfix`) and records every action in PostgreSQL `gm_audit_log`.

---

## Key HTTP Routes

| Endpoint | Method | Required Scope | Description |
|:---|:---:|:---|:---|
| `/api/v1/admin/auth/status` | `GET` | Public | Returns `auth_required` and the current `boot_id` so the browser knows when to prompt for login. |
| `/api/v1/admin/auth/login` | `POST` | Public | Exchanges `username` (`Admin`) and `passphrase` for a scoped `HS256` Bearer JWT. |
| `/api/v1/world/state` | `GET` | `admin:read` | Returns viewport-bounded players, NPCs, monsters, mounts, resource nodes, worker shards, and the `9x` Verra clock at `30 Hz`. |
| `/api/v1/admin/gm/*` | `POST` | `gm:*` | Dispatches NPC spawn/lifecycle actions, player teleports, City Node stage changes, time updates, and per-worker tick-rate changes. |
| `/api/v1/admin/containers` & `/logs` | `GET` | `metrics:read` | Streams container USE/RED metrics and structured JSON logs for the **Observability** tab. |

---

## Testing

```powershell
.\sandal.ps1 test
```
