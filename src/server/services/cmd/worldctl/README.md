# World CLI

**Related Guides**: [Root Overview](../../../../../README.md) | [Operational Runbook](../../../../../docs/RUNBOOK.md) | [Server Workers](../../../../../docs/SERVER_WORKERS.md) | [World Server](../../../world/README.md)

`src/server/services/cmd/worldctl` (`worldctl`) is the operator command-line tool for inspecting and controlling live `world` server workers over gRPC (`:50061+`) and HTTP metrics endpoints (`:18081+`).

---

## Common Commands

```powershell
# Inspect or set the 9x in-game time of day
docker exec aoc-world worldctl --addr 127.0.0.1:50061 time
docker exec aoc-world worldctl --addr 127.0.0.1:50061 time 12:00

# List active workers, adjust worker tick rate, or pause/resume NPC AI
docker exec aoc-world worldctl --addr 127.0.0.1:50061 workers
docker exec aoc-world worldctl tick-rate 60 --metrics-url http://127.0.0.1:18081
docker exec aoc-world worldctl simulate-npcs off --metrics-url http://127.0.0.1:18081

# Teleport a player to a named landmark or world coordinates (cm)
docker exec aoc-world worldctl --addr 127.0.0.1:50061 tp PlayerName winstead
docker exec aoc-world worldctl --addr 127.0.0.1:50061 tp PlayerName -452100 -548900 12400

# Grant currency or items to a character
docker exec aoc-world worldctl --addr 127.0.0.1:50061 give-money PlayerName 10000
docker exec aoc-world worldctl --addr 127.0.0.1:50061 give-item PlayerName "Copper Ore" 20 Legendary

# Spawn or control an NPC or monster
docker exec aoc-world worldctl --addr 127.0.0.1:50061 npc spawn "Goblin Scout" -452100 -548900 --level 50
docker exec aoc-world worldctl --addr 127.0.0.1:50061 npc heal <actor_id>
```

---

## Subcommand Reference

| Subcommand | Syntax | Description |
|:---|:---|:---|
| `time` | `worldctl time [<HH:MM\|hours>]` | Prints or sets the `9x` Verra time of day (`00:00..24:00`). |
| `workers` | `worldctl workers` | Lists registered spatial workers with scope, tick rate, and entity counts. |
| `tick-rate` | `worldctl tick-rate [<hz>] [--metrics-url URL]` | Gets or sets (`1..120 Hz`) the target worker's simulation tick rate. |
| `simulate-npcs` | `worldctl simulate-npcs [on\|off] [--metrics-url URL]` | Queries or toggles ambient NPC AI simulation on the target worker. |
| `players` | `worldctl players` | Lists online players on the worker with level and `(X, Y, Z)` coordinates. |
| `places` | `worldctl places` | Lists named teleport landmarks (`lionhold`, `crossroads`, `samias-hope`, `winstead`, `joeva`, `sunhaven`, `azmaran`). |
| `tp` | `worldctl tp <character> <place> \| <x> <y> [<z>]` | Teleports a player to a landmark or `(x, y[, z])` coordinates in centimeters. |
| `speed` | `worldctl speed <character> <multiplier>` | Sets a character's movement speed multiplier (`0.5..10.0`). |
| `give-money` | `worldctl give-money <character> <copper>` | Grants or removes copper currency (`10,000` copper = `1` gold). |
| `give-item` | `worldctl give-item <character> <item> [<count>] [<rarity>\|q<NN>]` | Grants an item by name or GUID with optional count, rarity, or quality. |
| `npc spawn` | `worldctl npc spawn <def_or_name> <x> <y> [<z>] [--level N]` | Spawns an NPC or monster at `(x, y[, z])` with optional level `1..100`. |
| `npc kill\|respawn\|reset\|heal\|despawn` | `worldctl npc <action> <id>` | Executes a lifecycle command on a live NPC or monster by actor ID. |
| `npc move` | `worldctl npc move <id> <x> <y> [<z>]` | Moves a live NPC to `(x, y[, z])` cm and updates its home anchor. |
| `place-station` / `remove-station` | `worldctl place-station <node> <plot> <station>` | Places or removes a crafting station on settlement plot `0..15`. |
| `craft` | `worldctl craft finish\|teach\|forget\|taught <character> [<arg>]` | Finishes crafting jobs or modifies learned recipes for a character. |
| `weapon-xp` | `worldctl weapon-xp <character> <line> <level\|xp=N>` | Sets weapon skill-tree mastery level (`1..25`) or exact XP. |
| `tutorials reset` | `worldctl tutorials reset <character>` | Resets completed tutorial flags for a character. |
| `watch` | `worldctl watch [--area minx,miny,maxx,maxy] [--density] [--seconds N]` | Streams live `30 Hz` world state updates or density grids to the terminal. |
