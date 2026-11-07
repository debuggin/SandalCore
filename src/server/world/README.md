# SandalCore Authoritative C++23 Zone Server Worker (`src/server/world`)

**Primary Diataxis Quadrant**: `Technical Reference` (`DOC-01`)
**Related Guides**: [Server Workers & Spatial Authority Guide](../../../docs/SERVER_WORKERS.md) | [Configuration Reference](../../../docs/CONFIGURATION.md) | [Control-Plane Services Reference](../../../docs/SERVICES.md) | [Web Admin & GM Studio Reference](../../../docs/WEB_ADMIN.md) | [Wire Protocol Specifications](../../../docs/reference/wire-specs/README.md) | [`libaocnav` Navigation Reference](../../libs/aocnav/README.md) | [`libaocnet` Wire Protocol Reference](../../libs/aocnet/README.md) | [`libaocstats` Combat & Stat Reference](../../libs/aocstats/README.md)

`src/server/world` compiles the authoritative C++23 Linux zone server binary (`world`). Depending on the configured [`AOC_WORLD_SHARD_MODE`](../../../docs/SERVER_WORKERS.md), one or more `world` worker containers simulate the continent of Verra (`Verra_01`), terminating the Unreal Engine 5.6 UDP bit-stream protocol (`:7777+`), stepping movement and combat on a deterministic fixed-timestep loop (`10 Hz` / `20 Hz` / `30 Hz`), streaming tiled Recast/Detour navigation meshes (`libaocnav`), and coordinating cross-boundary player handovers via `aoc-mesh` (`:50055`) and `aoc-mesh-gateway` (`:50056`).

---

## 1. Server Worker Topologies, Canonical Naming & Mesh Index (`worker_index`)

A single `world` binary supports all **6 spatial topology presets**, additive **Convex Polygon Cutout** workers, and **Mix-and-Match** deployments configured via CLI flags (`--area-tier`, `--area-id`, `--area-polygon`, `--area-exclude-node-plug`, `--tick-hz`) or root `.env` variables:

| Spatial Tier (`--area-tier`) | Canonical Worker Naming | Dynamic Scaling & Capacity | Default Tick Rate | Spatial Authority Rule |
|:---|:---|:---|:---:|:---|
| **`world`** (`world_only` / `single`) | `aoc-world` (or single-region fallback `aoc-riverlands` / `aoc-world-riverlands`) | `1` worker (`11` containers total in default Riverlands fallback) | `10 Hz` (`AOC_WORLD_TICK_HZ_WORLD=10`) | Simulates the full continent (or `riverlands` only when root `.env` is absent and `AOC_WORLD_REGION=riverlands` is active). |
| **`region`** (`per_region`) | `aoc-<region-name>` (`aoc-riverlands`, `aoc-sandsquall`, `aoc-jundark`, `aoc-anvils`, `aoc-turquoise-sea`) | `1` worker per biome region (`5` regions) | `20 Hz` (`AOC_WORLD_TICK_HZ_REGION=20`) | Simulates a single biome region polygon. |
| **`zoi`** (`per_zoi` / `dynamic_zoi`) | Static: `aoc-<zoi-name>`<br/>Dynamic stub: `aoc-<zoi-name>-alpha` .. `aoc-<zoi-name>-zulu` | Static: `1` per ZOI (`27` ZOIs)<br/>Dynamic stub: `1..26` per ZOI (NATO alphabet startup order, `min = 1` `-alpha`, never `0`; currently a stub) | `30 Hz` (`AOC_WORLD_TICK_HZ_ZOI=30`) | Simulates a single Voronoi Zone of Influence polygon (`zoi_polygons.tsv`). When paired with a Node Plug worker, passes `--area-exclude-node-plug=true`. |
| **`node_plug`** (`node_plug_and_zoi` / `dynamic_node_plug_zoi`) | Static: `aoc-<city-name>`<br/>Dynamic stub: `aoc-<city-name>-alpha` .. `aoc-<city-name>-zulu` | Static: `1` per city plug<br/>Dynamic stub: `1..26` per city plug (NATO alphabet startup order, `min = 1` `-alpha`, never `0`; currently a stub) | `30 Hz` (`AOC_WORLD_TICK_HZ_ZOI=30`) | Simulates the rotated `1 km x 1 km` (`100,000 cm x 100,000 cm`) CityNode Oriented Bounding Box (`node_plug_bounds.tsv`). |
| **`polygon_cutout`** (additive) | `aoc-<cutout-name>` | `1+` additive workers | `30 Hz` (`AOC_WORLD_TICK_HZ_ZOI=30`) | Simulates an arbitrary counter-clockwise convex polygon (`--area-polygon="x1,y1;x2,y2;..."`) carved out of the surrounding ZOI or region. |

### Mesh Worker Index (`worker_index`) & Collision-Free `FGuid` Allocation

On startup, `main.cpp` invokes `MeshClient::RegisterWorld` against [`aoc-mesh`](../services/cmd/mesh/README.md) (`:50055`):

1. `aoc-mesh` assigns a unique sequential `worker_index` (`uint16_t` `server_id`, `1..N`) to the worker and records its bounding polygon and tick rate in the cluster spatial directory.
2. The worker initializes `ServerGuidAllocator(worker_index, randomizer)` (`src/server/world/session.h`), embedding `worker_index` into the upper bits of `FGuid.D` for every spawned actor (`0x60100001` NPCs, loot bags, projectiles) so dynamic GUIDs never collide across shards.

---

## 2. Internal C++23 Architecture & Simulation Loop

```mermaid
flowchart TB
    subgraph NetworkIO["Non-Blocking UDP & gRPC I/O Layer"]
        UDP["UDP Socket (:7777+) - UE5 StatelessConnect (96B Ext), PacketHeader & Bunch ARQ"]
        GrpcSrv["WorldControlService (:50062+) - PrepareHandover, GM Lifecycle & WorldState Snapshots"]
        GrpcCli["Resilient gRPC Clients (wait_for_ready + 1s DNS Cooldown) - aoc-character, aoc-citynode, aoc-quest, aoc-mesh, aoc-mesh-gateway"]
    end

    subgraph WorldTick["Deterministic Fixed-Timestep Loop (world_loop.cpp: 10 Hz / 20 Hz / 30 Hz)"]
        Inbound["1. Drain Inbound UDP Bunches & RPCs (Move, Combat, Ability, Interact, Chat)"]
        Spatial["2. Spatial Grid, AI & Movement Step (AreaNav::pump, Detour Pathing & TownPlatforms Z-Rescue)"]
        Boundary["3. Multi-Boundary Handover Check (5,000 cm Buffer, 3-Sample Dwell, 2,000 ms Cooldown, lease_epoch)"]
        Populations["4. CityNode Tier (0..6) & Spawner Budget (NodePopulations & reap_placed_stations)"]
        Replicate["5. Interest Management & BitWriter Replication (Actor Channels, Movement Samples & Vitals)"]
    end

    subgraph StaticData["Static Content & Navigation Substrates (deploy/content/)"]
        ContentDB["SQLite content.db (v41) - 13 Covering Indexes + LRU Record Cache"]
        TsvStore["Per-Data-Type .tsv Tables (memory | disk) - zoi_polygons, city_nodes, pois, npcs, items"]
        NavData["libaocnav Substrates - area_nav.bin (4,096 Chunks) + area_geom.bin + town_platforms.bin"]
    end

    UDP --> Inbound
    GrpcSrv --> Inbound
    Inbound --> Spatial
    Spatial --> Boundary
    Boundary --> Populations
    Populations --> Replicate
    Replicate --> UDP
    Boundary -->|"InitiateHandover / CompleteHandover"| GrpcCli
    Spatial -.-> NavData
    Populations -.-> ContentDB
    Populations -.-> TsvStore
```

---

## 3. Source Module Map (`src/server/world/`)

| File / Subsystem | Responsibility |
|:---|:---|
| [`main.cpp`](main.cpp) | CLI flag and environment parser (`AreaFilter`, `--area-tier`, `--area-polygon`, `--tick-hz`, `.tsv` `memory|disk` modes), SQLite `content.db` (`v41`) startup validation, `aoc-mesh` registration (`worker_index`), and signal handling. |
| [`world_loop.h`](world_loop.h) / [`world_loop.cpp`](world_loop.cpp) | Fixed-timestep simulation loop (`10 Hz` / `20 Hz` / `30 Hz`), spatial hash grid, multi-boundary player handover evaluation (`kBoundaryBufferCm = 5000.0f`, `kHandoverDwellSamples = 3`, `kHandoverCooldownMs = 2000`), and periodic character persistence. |
| [`session.h`](session.h) / [`session.cpp`](session.cpp) | Per-connection UE5 `IpNetDriver` state machine (`StatelessConnect` 96-byte `Ext`, `NMT_Hello`, `NMT_Login`, `NMT_Welcome`, `NMT_Join`), reliable bunch reassembly, channel table, and `ServerGuidAllocator(worker_index)`. |
| [`node_populations.h`](node_populations.h) / [`node_populations.cpp`](node_populations.cpp) | Live CityNode progression stage (`0..6`: `Wilderness` -> `Metropolis`) population scaling, obsolete station reaping (`reap_placed_stations`), and `TownPlatforms` (`AOCTPF01`) vertical $Z$-floor rescue. |
| [`npc_ai.h`](npc_ai.h) / [`npc_ai.cpp`](npc_ai.cpp) | NPC state machine (`Idle`, `Patrol`, `Chase`, `Combat`, `Evade`, `Dead`), threat/aggro tables, `AreaNav::find_path` steering, and GM lifecycle handlers (`SpawnNpc`, `KillNpc`, `DespawnNpc`, `HealNpc`, `ResetNpc`, `RespawnNpc`, `MoveNpc`). |
| [`combat.h`](combat.h) / [`combat.cpp`](combat.cpp) | Ability execution, cooldowns, projectile/cone hit-registration, `libaocstats` mitigation curves, status effect ticks, and loot/XP distribution. |
| [`control_service.h`](control_service.h) / [`control_service.cpp`](control_service.cpp) | Internal gRPC `WorldControlService` (`:50062+`) servicing `PrepareHandover`, `GetWorldState` (consumed by `world-stream`), `SetNodeStage`, and synchronous GM actor/player manipulation RPCs. |

---

## 4. Seamless Multi-Boundary Handover, CityNode (`0..6`) Scaling & GM Lifecycle

1. **Multi-Boundary Handover & Spatial Anti-Hysteresis**:
   - Supports live transitions across `region <-> region`, `region <-> zoi`, `zoi <-> zoi`, `zoi <-> node_plug`, and `any <-> polygon_cutout` boundaries.
   - Enforces a **`5,000 cm` (`50 m`)** inward border buffer, **`3` consecutive dwell samples**, a **`2,000 ms`** post-handover cooldown, and monotonic **`lease_epoch`** fencing via [`aoc-mesh-gateway`](../services/cmd/mesh-gateway/README.md).
2. **Live CityNode Stage (`0..6`) Population Scaling**:
   - Transitions any of the 85 settlements across stages `0..6` at runtime without restarting the worker, reaping outdated crafting stations (`reap_placed_stations`), spawning new tier NPCs/guards, and resolving multi-story deck heights via `town_platforms.bin` ([`src/libs/aocnav/README.md`](../../libs/aocnav/README.md)).
3. **End-to-End GM Actor Lifecycle**:
   - Exposes `SpawnNpc`, `KillNpc`, `DespawnNpc`, `HealNpc`, `ResetNpc`, `RespawnNpc`, `MoveNpc`, and `TeleportPlayer` over gRPC (`WorldControlService`), immediately replicating state deltas to connected UE5 clients and the [Web Admin & GM Studio](../../../docs/WEB_ADMIN.md).

---

## 5. Build, Unit Tests & Live 10-Stage E2E Verification

`src/server/world` compiles exclusively inside the Linux container toolchain (`src/deploy/docker/world.Dockerfile`, CMake + Ninja + Clang 18 / GCC 14):

```powershell
# Compile the world worker image and all services
.\build.bat

# Run C++23 ctest unit suites (bitstream, session, handover hysteresis, node populations, navmesh)
.\sandal.ps1 test

# Run the live 10-stage fake::Client UDP :7777 play-flow & functional API gate
pwsh -NoProfile -File scripts/test_functional_apis.ps1
```
