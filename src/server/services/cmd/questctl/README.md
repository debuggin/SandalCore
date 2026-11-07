# `questctl` -- Quest State & Asynchronous Inbox Operator CLI

`questctl` (`src/server/services/cmd/questctl`) is the operator CLI for inspecting per-character quest logs and queueing administrative quest mutations (`give`, `finish`, `fail`, `abandon`, `clear`, `set-var`) on [`aoc-quest`](../quest/README.md) (`aoc.quest.v1.QuestService`).

---

## 1. Architecture & Asynchronous Command Inbox Flow

`questctl` operates safely on both online and offline characters. When `questctl` issues a mutation (`QueueCommand`), `aoc-quest` writes the directive into the PostgreSQL `quest_inbox` table. If the character is currently online in `aoc-world`, the zone server drains pending directives via `TakeCommands` and updates the live player session; if the character is offline, `TakeCommands` drains the inbox on their next login.

```mermaid
sequenceDiagram
    autonumber
    participant Op as "Operator (questctl)"
    participant QS as "aoc-quest (:50057)"
    participant DB as "PostgreSQL (quest_inbox & character_quests)"
    participant World as "aoc-world Zone Worker"

    Op->>QS: "QueueCommand (GIVE / COMPLETE / FAIL / ABANDON / RESET / SET_VAR)"
    QS->>DB: "INSERT INTO quest_inbox (character_guid, action, quest_guid)"
    QS-->>Op: "Queued Command ID"
    World->>QS: "TakeCommands (character_guid)"
    QS->>DB: "DELETE FROM quest_inbox ... RETURNING *"
    QS-->>World: "Pending QuestCommand List"
    World->>World: "Apply Quest State Transition & Replicate to Client"
    World->>QS: "SaveCharacterQuests (Updated Active/Completed/Variables)"
```

---

## 2. Subcommand Reference

Character identifiers (`<character>`) accept a character name (`Aventus`, resolved automatically against PostgreSQL via `AOC_DATABASE_URL`) or a numeric `uint64` character GUID. Quest identifiers (`<quest>`) accept a numeric design GUID or a case-insensitive quest slug (`WelcomeToVerra`, `welcome_to_verra`, `Narrative_Commission_...`).

| Subcommand | Arguments | RPC Method | Description |
|:---|:---|:---|:---|
| `log` | `<character>` | `LoadCharacterQuests` | Prints the character's persisted active quests (with objective progress counters), completed quest GUIDs, and narrative variables. |
| `give` | `<character> <quest>` | `QueueCommand` (`QUEST_GIVE`) | Grants a quest to the character, bypassing prerequisite gates. |
| `finish` | `<character> <quest>` | `QueueCommand` (`QUEST_COMPLETE`) | Marks all objectives on the specified quest complete and grants completion rewards. |
| `fail` | `<character> <quest>` | `QueueCommand` (`QUEST_FAIL`) | Transitions an active quest into its failure state. |
| `abandon` | `<character> <quest>` | `QueueCommand` (`QUEST_ABANDON`) | Removes an active quest from the character's quest log without marking it completed. |
| `clear` | `<character> [<quest>]` | `QueueCommand` (`QUEST_RESET`) | Clears completion history and active state for a specific quest, or resets the character's entire quest log when `<quest>` is omitted. |
| `set-var` | `<character> <name> <value>` | `QueueCommand` (`QUEST_SET_VAR`) | Sets an integer narrative variable (`int64`) in the character's quest state dictionary. |

---

## 3. Global Flags & Environment Variables

| Flag / Variable | Default | Description |
|:---|:---|:---|
| `--addr` / `AOC_QUESTS_GRPC_ADDR` | `127.0.0.1:50058` (host) / `quest:50057` (container) | Target `aoc-quest` gRPC endpoint. |
| `--token-file` / `AOC_SERVICE_TOKEN_FILE` | `service.token` / `dist/services/service.token` | Shared service token file sent via `x-aoc-service-token`. |
| `AOC_DATABASE_URL` | `postgres://aoc:aoc_dev_password@127.0.0.1:5432/aoc?sslmode=disable` | PostgreSQL DSN used to resolve character names to numeric GUIDs and load pending inbox items. |

---

## 4. Operational Examples

```bash
# Inspect persisted quests and narrative variables for character GUID 1001
docker compose -f src/deploy/compose.yml exec -T quest \
  questctl --addr 127.0.0.1:50057 log 1001

# Grant and complete WelcomeToVerra by quest slug
docker compose -f src/deploy/compose.yml exec -T quest \
  questctl --addr 127.0.0.1:50057 give Aventus WelcomeToVerra
docker compose -f src/deploy/compose.yml exec -T quest \
  questctl --addr 127.0.0.1:50057 finish Aventus WelcomeToVerra

# Set a custom narrative variable
docker compose -f src/deploy/compose.yml exec -T quest \
  questctl --addr 127.0.0.1:50057 set-var Aventus LionholdIntroSeen 1
```

---

## 5. Related Documentation

- [Quest Service (`src/server/services/cmd/quest/README.md`)](../quest/README.md)
- [Go Control-Plane Services Overview (`src/server/services/README.md`)](../../README.md)
- [Control-Plane Services Architecture (`docs/SERVICES.md`)](../../../../docs/SERVICES.md)
- [Server Workers & Spatial Mesh Guide (`docs/SERVER_WORKERS.md`)](../../../../docs/SERVER_WORKERS.md)
- [Configuration Reference (`docs/CONFIGURATION.md`)](../../../../docs/CONFIGURATION.md)
- [Web Admin Studio Guide (`docs/WEB_ADMIN.md`)](../../../../docs/WEB_ADMIN.md)
