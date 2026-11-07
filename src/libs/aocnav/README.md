# SandalCore Navigation & Spatial Geometry Library (`src/libs/aocnav`)

**Primary Diataxis Quadrant**: `Technical Reference` (`DOC-01`)
**Related Guides**: [Server Workers & Spatial Authority](../../../docs/SERVER_WORKERS.md) | [C++23 `world` Worker Component Reference](../../server/world/README.md) | [Game-Data Extraction Pipeline](../../tools/aoc-extractor/README.md)

`src/libs/aocnav` (`libaocnav`) is the static C++23 spatial geometry, heightfield, and Recast/Detour v1.6.0 navigation mesh library shared by the authoritative [`world`](../../server/world/README.md) server worker (`src/server/world`) and the offline navmesh compiler (`src/tools/navbuild`). It provides thread-safe terrain and building-platform $Z$ height resolution, 3D pathfinding (`find_path`), line-of-sight navmesh raycasting (`raycast`), detail-edge height repair (`TileRepair`), and memory-bounded sliding-window chunk streaming across Verra's `4,096`-tile landscape grid.

---

## 1. Public Headers & Module Architecture

| Header Path | Primary Class / Struct | Responsibility |
|:---|:---|:---|
| [`include/nav/area_nav.h`](include/nav/area_nav.h) | `aoc::nav::AreaNav`<br/>`aoc::nav::TerrainGrid`<br/>`aoc::nav::HeightSample` | Runtime navigation façade combining `dtNavMesh` / `dtNavMeshQuery`, 16-bit `TerrainGrid` heightfield bilinear interpolation, and asynchronous `64 x 64` chunk streaming. |
| [`include/nav/nav_file.h`](include/nav/nav_file.h) | `aoc::nav::NavFileHeader`<br/>`aoc::nav::ChunkIndexEntry` | Binary wire layout definitions for `area_nav.bin` and per-chunk compressed Detour tile payloads. |
| [`include/nav/nav_build.h`](include/nav/nav_build.h) | `aoc::nav::NavBuildConfig`<br/>`aoc::nav::build_tile()` | Recast voxelization, walkable slope filtering, heightfield erosion, contour simplification, and `dtNavMeshCreateParams` generation. |
| [`include/nav/tile_repair.h`](include/nav/tile_repair.h) | `aoc::nav::repair_tile_detail_edges()` | Post-process detail-mesh vertex snap pass that eliminates vertical $Z$ seams along adjacent Recast tile borders. |
| [`include/nav/town_geometry.h`](include/nav/town_geometry.h) | `aoc::nav::TownGeometry` | Integrates static settlement meshes and bridge spans into the Recast voxelization input during offline navmesh builds. |
| [`include/world/town_platform.h`](include/world/town_platform.h) | `world::TownPlatforms`<br/>`world::PlatformSample` | Runtime `AOCTPF01` (`town_platforms.bin`) spatial hash and $Z$-floor resolver for multi-story settlements (`winstead`, `joeva`, `new_aela`, `halcyon`, `miraleth`). |

---

## 2. Core Spatial & Navigation Subsystems

### 1. Sliding-Window Detour Chunk Streaming (`AreaNav`)

Verra's playable continent spans a `64 x 64` grid (`4,096` chunks total, where each chunk covers `33,600 cm x 33,600 cm`). Loading all `4,096` Detour navmesh chunks simultaneously in every worker container would waste gigabytes of RAM. `AreaNav` (`include/nav/area_nav.h`) streams chunks on demand around active players and NPCs:

- **Load & Eviction Radii**: `kLoadRadius = 2` (loads a `5 x 5` chunk neighborhood around every active entity) and `kKeepRadius = 3` (`7 x 7` hysteresis retention window) prevent thrashing when a player walks near a chunk border.
- **Background I/O Thread & Main-Tick `pump(budget_ms)`**: A single background reader thread decompresses requested chunks from `area_nav.bin` into a staging queue. During each simulation tick, the main thread calls `AreaNav::pump(budget_ms)` to splice staged `dtMeshTile` entries into `dtNavMesh` and evict out-of-range chunks within a deterministic millisecond budget.

### 2. Multi-Layer Ground & Platform Height Resolution (`ground_height` & `TownPlatforms`)

Resolving an actor's authoritative vertical $Z$ coordinate at `(x, y)` combines three data sources in priority order:

1. **Town Platform $Z$-Floor Rescue (`TownPlatforms::query_height`)**:
   - In major settlements (`winstead`, `joeva`, `new_aela`, `halcyon`, `miraleth`), stone plazas and elevated wooden decks sit above carved-out terrain holes (`TownPlatforms::is_in_hole(x, y)`).
   - `TownPlatforms` (`include/world/town_platform.h`) loads `deploy/content/town_platforms.bin` (`AOCTPF01` magic header) into a `2,000 cm` (`20 m`) 2D spatial hash grid and returns the highest walkable platform surface (`z <= ref_z + 150 cm`, or the topmost deck on initial spawn) so NPCs, crafting stations, and players never fall through town floors.
2. **Detour Detail-Mesh Polygon Query (`AreaNav::ground_height` / `floors_at`)**:
   - Queries `dtNavMeshQuery::findNearestPoly` + `getPolyHeight` across loaded Detour tiles to resolve bridge, stair, and multi-story interior floor heights.
3. **16-Bit Heightfield Fallback (`TerrainGrid::height_at`)**:
   - Performs $O(1)$ bilinear interpolation across the `TerrainGrid` 16-bit landscape heightmap (`area_geom.bin`) when an actor is in open wilderness outside loaded navmesh tiles.

---

## 3. Build & Unit Test Verification

`libaocnav` compiles inside the Linux C++23 container toolchain (`src/deploy/docker/world.Dockerfile`, Clang 18 / GCC 14 + Ninja) and is verified by `ctest` unit suites:

```powershell
# Run the C++23 world & libaocnav unit tests inside the Linux build container
.\sandal.ps1 test
```
