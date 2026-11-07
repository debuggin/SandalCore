# Quest Service

**Related Guides**: [Root Overview](../../../../../README.md) | [Control-Plane Services](../../../../../docs/SERVICES.md) | [Quest CLI (`questctl`)](../questctl/README.md) | [World Server](../../../world/README.md)

`src/server/services/cmd/quest` (`aoc-quest`, listening on `:50054` gRPC) persists character quest journals, objective counters, narrative variables, and queued Game Master quest commands in PostgreSQL (`aoc-postgres`).

---

## Core Responsibilities

- **Quest Journal Persistence**: Stores active quests (`character_quests`), completed quests (`character_quests_completed`), and quest variables (`character_quest_variables`) for every character.
- **Objective Progress and Completion**: Records kill, gather, talk, and location objective updates sent by `world` workers as players progress through quest steps.
- **GM Command Queue (`character_quest_commands`)**: Queues operator commands issued via `questctl` (`give`, `finish`, `fail`, `abandon`, `clear`, `set-var`) so live `world` workers apply them cleanly on the next session tick.

---

## Configuration

| Environment Variable | Default | Description |
|:---|:---|:---|
| `AOC_QUEST_GRPC_ADDR` | `:50054` | Internal gRPC listen address for `QuestService`. |
| `AOC_QUEST_METRICS_ADDR` | `:9094` | HTTP listen address for `/livez`, `/readyz`, `/healthz`, and `/metrics`. |
| `DATABASE_URL` | `postgres://aoc:aoc@postgres:5432/aoc?sslmode=disable` | PostgreSQL connection string. |

---

## Testing

```powershell
.\sandal.ps1 test
```
