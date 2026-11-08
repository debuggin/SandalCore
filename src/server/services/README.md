# Control-Plane Services

**Related Guides**: [Root Overview](../../../README.md) | [Control-Plane Services Guide](../../../docs/SERVICES.md) | [Configuration Reference](../../../docs/CONFIGURATION.md) | [Operational Runbook](../../../docs/RUNBOOK.md) | [World Server](../world/README.md)

`src/server/services` contains the control-plane services, shared internal packages, PostgreSQL database migrations, and operator command-line tools for SandalCore.

---

## Services and Command-Line Tools

### Control-Plane Services (`cmd/*`)

| Binary | Container | Ports | Guide | Purpose |
|:---|:---|:---:|:---|:---|
| `cmd/auth` | `aoc-auth` | `:50051` (gRPC) | [Auth Service](cmd/auth/README.md) | Account registration, Argon2id password verification, session tickets, and temporary accounts. |
| `cmd/character` | `aoc-character` | `:50052` (gRPC), `:50061` (TLS 1.3) | [Character Service](cmd/character/README.md) | UE5 `IntrepidLobbyService` (`:50061` ICS TLS 1.3) and PostgreSQL character state persistence. |
| `cmd/citynode` | `aoc-citynode` | `:50053` (gRPC) | [CityNode Service](cmd/citynode/README.md) | Settlement stage (`0..6`), XP, mayor, tax, and building progression for all 85 City Nodes. |
| `cmd/quest` | `aoc-quest` | `:50054` (gRPC) | [Quest Service](cmd/quest/README.md) | Per-character quest journals, objective counters, narrative variables, and GM quest commands. |
| `cmd/mesh` | `aoc-mesh` | `:50055` (gRPC) | [Mesh Service](cmd/mesh/README.md) | Sequential `worker_index` (`1..N`) registration, heartbeats, and spatial coordinate routing. |
| `cmd/mesh-gateway` | `aoc-mesh-gateway` | `:50056` (gRPC), `:8082` (HTTP) | [Mesh Gateway](cmd/mesh-gateway/README.md) | Cross-worker player handovers (`lease_epoch` fencing), PostgreSQL simulation state persistence (`world_simulation_state`, `city_node_state`), and `30 Hz` stream coalescing. |
| `cmd/gateway` | `aoc-gateway` | `:8080` (HTTP/WS) | [Gateway Service](cmd/gateway/README.md)<br/>[Admin Gateway](cmd/admin-gateway/README.md) | Public HTTP/WebSocket gateway for launcher login, tether tickets, and `/api/v1/admin/*` routes. |
| `cmd/web-admin` | `aoc-web-admin` | `:8090` (HTTP) | [Web Admin Server](cmd/web-admin/README.md) | Serves the Web Admin frontend bundle and HTTP `206` range requests for `verra.pmtiles`. |

### Operator CLIs and Shared Packages

| Tool or Package | Guide | Purpose |
|:---|:---|:---|
| `cmd/contentc` | [Content Compiler](cmd/contentc/README.md) | Compiles extracted client DesignData into `dist/content/content.db` (schema `v41`), creates 13 covering indexes, runs `ANALYZE`, and writes `roads.tsv`. |
| `cmd/worldctl` | [World CLI](cmd/worldctl/README.md) | Operator CLI for controlling live `world` workers (time of day, tick rates, NPC simulation, player teleports, item grants, and NPC spawning). |
| `cmd/nodectl` | [Node CLI](cmd/nodectl/README.md) | Operator CLI for inspecting and changing City Node stages (`0..6`) and node XP. |
| `cmd/questctl` | [Quest CLI](cmd/questctl/README.md) | Operator CLI for inspecting and modifying character quest states. |
| `cmd/charimport` | [Character Import](cmd/charimport/README.md) | Imports `characters.json` snapshots into PostgreSQL. |
| `internal/statprog` | [Stat Bytecode Engine](internal/statprog/README.md) | Compiles stat curves and ability formulas into `AOSP` bytecode shared with `libaocstats`. |

---

## Building and Testing

```powershell
# Run unit and migration tests via the repository test runner
.\sandal.ps1 test

# Run the live end-to-end API, ICS TLS 1.3, and fake::Client verification suite
pwsh -NoProfile -File scripts/test_functional_apis.ps1
```

---

## Database Migration Rollback Reference

The following deterministic PostgreSQL rollback blocks reverse individual `sql/migrations` schema versions (`0001` through `0025` forward migrations; rollback reference blocks for `0003` through `0024` below) and are verified by the `go test ./internal/db` migration rollback suite:

```sql
-- rollback of migration 0003
BEGIN;
UPDATE characters SET race_id = COALESCE(class_id, 0), gender_id = COALESCE(gender, 0) + 1
WHERE race_id = 0 AND gender_id = 0;
ALTER TABLE characters ALTER COLUMN race_id DROP DEFAULT, ALTER COLUMN gender_id DROP DEFAULT;
ALTER TABLE characters DROP COLUMN race, DROP COLUMN gender, DROP COLUMN class_id;
DELETE FROM schema_migrations WHERE version = 3;
COMMIT;
```

```sql
-- rollback of migration 0004
BEGIN;
DROP TABLE character_items;
DELETE FROM schema_migrations WHERE version = 4;
COMMIT;
```

```sql
-- rollback of migration 0005
BEGIN;
DROP TABLE character_experience;
ALTER TABLE characters DROP COLUMN gm_starter_set_granted_at;
DELETE FROM schema_migrations WHERE version = 5;
COMMIT;
```

```sql
-- rollback of migration 0006
BEGIN;
DROP TABLE character_skill_ranks;
DROP TABLE character_action_bar;
DELETE FROM schema_migrations WHERE version = 6;
COMMIT;
```

```sql
-- rollback of migration 0007
BEGIN;
ALTER TABLE character_items
    DROP CONSTRAINT character_items_persisted_health,
    DROP CONSTRAINT character_items_persisted_mana,
    DROP CONSTRAINT character_items_lock_expiration,
    DROP COLUMN persisted_health,
    DROP COLUMN persisted_mana,
    DROP COLUMN lock_expiration;
DELETE FROM schema_migrations WHERE version = 7;
COMMIT;
```

```sql
-- rollback of migration 0008
BEGIN;
ALTER TABLE characters DROP COLUMN gm_starter_mounts_granted_at;
DELETE FROM schema_migrations WHERE version = 8;
COMMIT;
```

```sql
-- rollback of migration 0009
BEGIN;
DROP TABLE city_node_state;
DROP TABLE city_node_clock;
DROP TABLE city_node_seed;
DELETE FROM schema_migrations WHERE version = 9;
COMMIT;
```

```sql
-- rollback of migration 0010
BEGIN;
DROP TABLE character_professions;
DELETE FROM schema_migrations WHERE version = 10;
COMMIT;
```

```sql
-- rollback of migration 0011
BEGIN;
DROP TABLE character_resource_items;
DELETE FROM schema_migrations WHERE version = 11;
COMMIT;
```

```sql
-- rollback of migration 0012
BEGIN;
DROP TABLE character_wallet;
DELETE FROM schema_migrations WHERE version = 12;
COMMIT;
```

```sql
-- rollback of migration 0013
BEGIN;
DROP TABLE character_processing_jobs;
DELETE FROM schema_migrations WHERE version = 13;
COMMIT;
```

```sql
-- rollback of migration 0014
BEGIN;
DROP TABLE character_known_recipes;
DELETE FROM schema_migrations WHERE version = 14;
COMMIT;
```

```sql
-- rollback of migration 0015
BEGIN;
DROP TABLE character_completed_ftues;
DELETE FROM schema_migrations WHERE version = 15;
COMMIT;
```

```sql
-- rollback of migration 0016
BEGIN;
DROP TABLE character_quest_commands;
DROP TABLE character_quest_variables;
DROP TABLE character_quests_completed;
DROP TABLE character_quests;
DELETE FROM schema_migrations WHERE version = 16;
COMMIT;
```

```sql
-- rollback of migration 0017
BEGIN;
DROP TABLE character_warehouse_items;
DELETE FROM schema_migrations WHERE version = 17;
COMMIT;
```

```sql
-- rollback of migration 0018
BEGIN;
DROP TABLE character_home_attunement;
DROP TABLE character_attunements;
DELETE FROM schema_migrations WHERE version = 18;
COMMIT;
```

```sql
-- rollback of migration 0019
BEGIN;
ALTER TABLE character_processing_jobs
    DROP CONSTRAINT character_processing_jobs_rating_xp,
    DROP CONSTRAINT character_processing_jobs_rating_quantity,
    DROP CONSTRAINT character_processing_jobs_rating_rarity,
    DROP COLUMN rating_xp,
    DROP COLUMN rating_quantity,
    DROP COLUMN rating_rarity;
DELETE FROM schema_migrations WHERE version = 19;
COMMIT;
```

```sql
-- rollback of migration 0024
BEGIN;
DROP INDEX IF EXISTS idx_gm_audit_log_command_created;
DROP INDEX IF EXISTS idx_gm_audit_log_created_at;
DROP INDEX IF EXISTS idx_hotfix_audit_log_recorded_at;
DROP INDEX IF EXISTS idx_hotfix_overlays_active_order;
DROP INDEX IF EXISTS idx_character_known_recipes_taught;
DROP INDEX IF EXISTS idx_character_skill_ranks_tree;
DROP INDEX IF EXISTS idx_character_resource_items_char_order_id;
DROP INDEX IF EXISTS idx_character_items_char_order_id;
DROP INDEX IF EXISTS idx_world_servers_heartbeat_server_id;
DROP INDEX IF EXISTS idx_characters_ungranted_starter_mounts;
DROP INDEX IF EXISTS idx_characters_ungranted_starter_set;
DROP INDEX IF EXISTS idx_characters_account_created_id;
ALTER TABLE characters DROP COLUMN IF EXISTS fencing_token;
DELETE FROM schema_migrations WHERE version = 24;
COMMIT;
```
