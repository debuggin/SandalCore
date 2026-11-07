# World Server

**Related Guides**: [Root Overview](../../../README.md) | [Server Workers](../../../docs/SERVER_WORKERS.md) | [Configuration Reference](../../../docs/CONFIGURATION.md) | [Operational Runbook](../../../docs/RUNBOOK.md) | [Navigation Library](../../libs/aocnav/README.md) | [Wire Protocol Library](../../libs/aocnet/README.md) | [Stat Evaluation Library](../../libs/aocstats/README.md)

`src/server/world` (`world`, running inside the `aoc-world` / `aoc-<region>` / `aoc-<zoi>` Linux container) is the authoritative zone simulation server for Ashes of Creation (`Verra_01`). It terminates Unreal Engine 5.6 UDP `IpNetDriver` connections (`:7777+`), runs the simulation loop, evaluates combat and NPC AI, manages spatial handovers, and serves Game Master gRPC commands (`:50061+`).

---

## Core Subsystems

- **Spatial Partitioning and Scope-Tuned Tick Rates**: Supports whole-world (`world` at `30 Hz`), regional (`region` at `45 Hz`), Zone of Influence (`zoi` at `60 Hz`), City Node OBB plug (`node_plug` at `100 Hz`), and convex polygon POI cut-out (`poi` at `100 Hz`) authority boundaries. At runtime, operators can switch any worker between `Dynamic (30-60 Hz)`, `0 Hz (Pause NPCs)`, and manual `10..60 Hz`, backed by a `<= 1.0 s` (`>= 1 Hz`) entity service floor so every spawned NPC is stepped at least once per second.
- **Unbounded Authentic Spawns and Resource Provenance**: Loads the complete authentic set of NPCs, monsters, and gatherable resource nodes inside the worker's spatial boundary without artificial `AOC_WORLD_MAX_*` or `max_gatherables` caps. Gatherable foliage and resource nodes come strictly from extracted client data (`region_foliage.tsv` and `content.db`) and retail packet captures (`pcap_resource_spawners.tsv`).
- **In-Memory Content and Chunked Navigation**: Preloads `content.db` (schema `v41`, `:memory:` SQLite backup) and `area_nav.bin` (`libaocnav`) into RAM at startup within a `2560M` (`2.5 GiB`) container limit, using `town_platforms.bin` to keep town NPCs and crafting stations at street height.
- **Combat, Abilities, and Professions**: Executes bytecode stat graphs (`libaocstats`), class abilities, weapon combo finishers, status effects, summoner pets, mounts, gathering, processing, crafting, quests, and City Node stage (`0..6`) population swaps (with Tier 5 and Tier 6 settlements automatically using the Tier 4 populated layout).
- **Seamless Cross-Worker Handovers**: Coordinates border crossings through `aoc-mesh-gateway` (`5,000 cm` inward buffer, `3` dwell samples, `2,000 ms` cooldown, and monotonic `lease_epoch` fencing).

---

## Directory Structure

| Path | Purpose |
|:---|:---|
| `main.cpp` / `world_loop.{h,cpp}` | Startup CLI parsing, area filter initialization, fixed-timestep simulation loop, and dynamic tick-rate scaling. |
| `session/player_session*.{h,cpp}` | Bounded-context player session modules (`movement`, `combat`, `inventory`, `crafting`, `gathering`, `quests`, `nodes`, `mounts`, `pets`, `social`, `gm`). |
| `npc/` & `spawns/` | Ambient and combat NPC AI, behavior trees, City Node stage `0..6` population tables (`node_population.cpp`), and client/PCAP foliage spawners. |
| `control/` | `WorldControlService` gRPC server (`:50061+`) handling `worldctl` and Web Admin GM commands (`SpawnNpc`, `KillNpc`, `MoveNpc`, `TeleportPlayer`, tick rates, and `30 Hz` snapshots). |
| `tests/` | Hermetic unit test binaries, the 26 modular `fake::Client` bot plans (`src/tests/cpp/bot/`), and the live UDP `:7777` `test_world_live_e2e` runner. |

---

## Building and Running Tests

`world` compiles inside the Linux container build stage (`src/deploy/docker/world.Dockerfile`):

```powershell
# Compile the world container and run all unit tests
.\sandal.ps1 build
.\sandal.ps1 test

# Run the live UDP :7777 8-class Lv 50 fake::Client play-flow suite
pwsh -NoProfile -File scripts/test_functional_apis.ps1
```
