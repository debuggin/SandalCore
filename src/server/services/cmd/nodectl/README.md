# Node CLI

**Related Guides**: [Root Overview](../../../../../README.md) | [CityNode Service](../citynode/README.md) | [Server Workers](../../../../../docs/SERVER_WORKERS.md) | [Operational Runbook](../../../../../docs/RUNBOOK.md)

`src/server/services/cmd/nodectl` (`nodectl`) is the command-line tool for inspecting and updating City Node progression stages (`0..6`), node experience, and settlement clocks in `aoc-citynode` (`:50053`).

---

## Usage Examples

```powershell
# List all City Nodes and their current stages (0..6) and XP
docker exec aoc-citynode nodectl --addr 127.0.0.1:50053 list

# Promote Winstead to Stage 4 (Town) or Stage 6 (Metropolis)
docker exec aoc-citynode nodectl --addr 127.0.0.1:50053 set-level Verra_RVR_Node_Winstead 4
docker exec aoc-citynode nodectl --addr 127.0.0.1:50053 set-level Verra_RVR_Node_Winstead 6

# Add 10,000 XP to a settlement
docker exec aoc-citynode nodectl --addr 127.0.0.1:50053 add-xp Verra_RVR_Node_Winstead 10000
```

---

## Subcommands

| Subcommand | Syntax | Description |
|:---|:---|:---|
| `list` | `nodectl list` | Prints all 85 City Nodes with their current stage (`0..6`), XP, and mayor. |
| `get` | `nodectl get <node_id_or_guid>` | Prints detailed state for a single settlement. |
| `set-level` | `nodectl set-level <node_id> <0..6>` | Sets a settlement's stage (`0 = Wilderness` .. `6 = Metropolis`) and notifies `world` workers to swap its NPC and station population. |
| `add-xp` | `nodectl add-xp <node_id> <amount>` | Adds node experience and triggers stage promotion when thresholds are crossed. |
