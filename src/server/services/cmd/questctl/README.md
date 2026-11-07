# Quest CLI

**Related Guides**: [Root Overview](../../../../../README.md) | [Quest Service](../quest/README.md) | [Operational Runbook](../../../../../docs/RUNBOOK.md) | [Known Issues](../../../../../docs/KNOWN_ISSUES.md)

`src/server/services/cmd/questctl` (`questctl`) is the operator command-line utility for inspecting character quest journals and queueing Game Master quest commands (`give`, `finish`, `fail`, `abandon`, `clear`, `set-var`).

---

## Usage Examples

```powershell
# Inspect a character's active and completed quests
docker exec aoc-quest questctl list <character_name_or_id>

# Grant or immediately complete a quest for a character
docker exec aoc-quest questctl give <character_name_or_id> <quest_name_or_guid>
docker exec aoc-quest questctl finish <character_name_or_id> <quest_name_or_guid>

# Clear all active and completed quest records for a character
docker exec aoc-quest questctl clear <character_name_or_id>
```

---

## Subcommands

| Subcommand | Syntax | Description |
|:---|:---|:---|
| `list` | `questctl list <character>` | Lists active quests, objective progress, completed quests, and narrative variables for a character. |
| `give` | `questctl give <character> <quest>` | Starts a quest for the character. |
| `finish` | `questctl finish <character> <quest>` | Marks a quest completed and triggers its completion rewards. |
| `fail` | `questctl fail <character> <quest>` | Marks an active quest failed. |
| `abandon` | `questctl abandon <character> <quest>` | Removes an active quest from the character's journal. |
| `clear` | `questctl clear <character>` | Clears all quest records and variables for the character. |
| `set-var` | `questctl set-var <character> <key> <value>` | Sets a quest narrative variable for the character. |
