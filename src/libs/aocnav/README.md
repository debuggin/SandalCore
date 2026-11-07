# Navigation Library

**Related Guides**: [Root Overview](../../../README.md) | [Map Compiler](../../tools/mapc/README.md) | [World Server](../../server/world/README.md)

`src/libs/aocnav` (`libaocnav`) is the spatial navigation and terrain query library linked into `world` and `mapc`. It wraps Recast and Detour to provide chunked navmesh loading, pathfinding, ground-height snapping, and town platform elevation queries.

---

## Key Capabilities

- **Chunked `AOCNAV01` Navmesh Streaming (`AreaNav`)**: Loads `25,600 cm x 25,600 cm` navmesh chunks from `area_nav.bin` on demand within a `2 ms` per-tick budget (`AOC_WORLD_NAV_PUMP_BUDGET_MS`), or preloads the worker's spatial territory into RAM at startup (`AOC_WORLD_CACHE_ALL_CONTENT_IN_MEMORY=true`).
- **Town Platform Elevation (`TownPlatforms`)**: Queries `town_platforms.bin` (`AOCTPF01`) so NPCs, players, and crafting stations placed on elevated town plazas, stairs, and bridges snap to the correct deck height instead of falling to the underlying terrain.
- **Pathfinding and Line-of-Sight**: Provides multi-hop corridor pathfinding, straight-line movement raycasts, and navigable point projection for ambient NPC wandering, combat pursuit, and `140 m` leash return paths.

---

## Source Files

| File | Purpose |
|:---|:---|
| `include/aocnav/area_nav.h` / `src/area_nav.cpp` | Chunked `area_nav.bin` loader, Detour `dtNavMeshQuery` wrapper, pathfinding, and ground Z queries. |
| `include/aocnav/town_platforms.h` / `src/town_platforms.cpp` | `town_platforms.bin` (`AOCTPF01`) height lookup for elevated settlement decks and plazas. |
| `include/aocnav/terrain_grid.h` / `src/terrain_grid.cpp` | Continent-wide terrain and water height lookup from `terrain_grid.bin`. |

---

## Running Tests

```powershell
.\sandal.ps1 test
```
