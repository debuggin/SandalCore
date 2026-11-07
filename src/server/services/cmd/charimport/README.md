# `charimport` -- Offline Character Snapshot Import Utility

`charimport` (`src/server/services/cmd/charimport`) is a deterministic, idempotent migration utility that imports JSON character snapshots (`characters.json`) into the PostgreSQL `characters` and `accounts` tables managed by [`aoc-character`](../character/README.md) and [`aoc-auth`](../auth/README.md).

---

## 1. Architecture & Pre-Flight Validation Pipeline

`charimport` parses and validates the entire JSON snapshot file in memory before opening a database connection. This two-stage design ensures malformed JSON files fail fast with zero database side effects, and re-running `charimport` against the same file and account is safe and idempotent.

```mermaid
flowchart TD
    JSON["characters.json Snapshot File"] --> Validate["Stage 1: Offline Schema Validation (legacyimport.ValidateFile)"]
    Validate -->|"Malformed JSON"| FailFast["Exit 1 (Zero DB Connection Opened)"]
    Validate -->|"Valid Records"| Migrate["Stage 2: Connect PostgreSQL & Run Schema Migrations"]
    Migrate --> Lookup["Lookup Target Account by Username (account.Store)"]
    Lookup --> Import["Idempotent Character Import (legacyimport.ImportFile)"]
    Import --> Summary["Report Total / Imported / Skipped / Conflicts / Invalid"]
```

---

## 2. Idempotency & Conflict Resolution Semantics

| Outcome Counter | Condition | Database Behavior | Exit Code Impact |
|:---|:---|:---|:---|
| `Imported` | Character GUID and name do not exist in PostgreSQL. | Inserts character row, appearance, inventory, equipment, and spawn coordinates under target account. | Exit `0` |
| `Skipped (already present)` | Character already exists under the same target account with matching identity. | Leaves existing database record untouched. | Exit `0` |
| `Conflicts (id/name taken)` | Character GUID or unique name is already owned by a different account or conflicting record. | Logs detailed per-character conflict reason without overwriting existing data. | Exit `1` |
| `Invalid records` | Record violates class, race, coordinate, or field invariants during import. | Rejects the invalid record and logs diagnostic details. | Exit `1` |

---

## 3. Command-Line Flags & Environment Variables

| Flag / Variable | Default | Required | Description |
|:---|:---|:---:|:---|
| `-account` | *(none)* | Yes | Target account username (`accounts.username`) that will own the imported characters (the account must already exist). |
| `-file` | `dist/Release/data/characters.json` | No | Path to the JSON character snapshot file to validate and import. |
| `AOC_DATABASE_URL` | `postgres://aoc:aoc_dev_password@127.0.0.1:5432/aoc?sslmode=disable` | No | PostgreSQL connection string used for schema migration, account lookup, and character persistence. |

---

## 4. Operational Examples

```bash
# Import a character snapshot file into an existing account inside the character container
docker compose -f src/deploy/compose.yml exec -T character \
  charimport -account pioneer -file /app/data/characters.json
```

Example output:

```text
charimport summary for account "pioneer" (id=1):
  file:      /app/data/characters.json
  total:     3
  imported:  3
  skipped:   0 (already present)
  conflicts: 0 (id/name taken)
  invalid:   0
```

---

## 5. Related Documentation

- [Character Service (`src/server/services/cmd/character/README.md`)](../character/README.md)
- [Auth Service (`src/server/services/cmd/auth/README.md`)](../auth/README.md)
- [Go Control-Plane Services Overview (`src/server/services/README.md`)](../../README.md)
- [Control-Plane Services Architecture (`docs/SERVICES.md`)](../../../../docs/SERVICES.md)
- [Server Workers & Spatial Mesh Guide (`docs/SERVER_WORKERS.md`)](../../../../docs/SERVER_WORKERS.md)
- [Configuration Reference (`docs/CONFIGURATION.md`)](../../../../docs/CONFIGURATION.md)
- [Web Admin Studio Guide (`docs/WEB_ADMIN.md`)](../../../../docs/WEB_ADMIN.md)
