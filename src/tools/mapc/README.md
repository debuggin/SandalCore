# Map Compiler

**Related Guides**: [Root Overview](../../../README.md) | [Navigation Library (`libaocnav`)](../../libs/aocnav/README.md) | [World Server](../../server/world/README.md) | [Game-Data Extractor](../aoc-extractor/README.md)

`src/tools/mapc` (`mapc`) is the offline geometry and navigation compiler for SandalCore. It turns extracted Unreal Engine 5.6 terrain heightfields, water planes, road splines, and town building footprints into the binary navigation and elevation files loaded by `world` at startup.

---

## Generated Runtime Files

`mapc` builds the spatial binary files stored in `dist/content/` and mounted at `/app/content/` inside `aoc-world`:

| Output File | Magic Header | Purpose |
|:---|:---:|:---|
| `area_nav.bin` | `AOCNAV01` | Chunked Recast/Detour navigation mesh (`25,600 cm` chunks, `6,400 cm` tiles) used for NPC pathfinding, line-of-sight raycasts, and ground height queries. |
| `town_platforms.bin` | `AOCTPF01` | Walkable town plaza, bridge, and elevated deck height grid (`100 cm` cells) used to snap town NPCs and crafting stations to street level. |
| `town_layouts.bin` | `AOCTLY01` | Settlement building footprints and blocker volumes across City Node stages. |
| `terrain_grid.bin` | `AOCTER01` | Continent-wide terrain heightfield and water elevation lookup grid. |

---

## Source Files

| File | Purpose |
|:---|:---|
| `main.cpp` | CLI entrypoint for building `area_nav.bin`, `town_platforms.bin`, `town_layouts.bin`, and `terrain_grid.bin`. |
| `nav_builder.{h,cpp}` | Rasterizes terrain heightfields, water planes, and building blockers into Recast/Detour navmesh tiles. |
| `town_platforms.{h,cpp}` | Extracts walkable deck and plaza surfaces for the five major towns. |

---

## Building and Running

`mapc` runs automatically during `.\sandal.ps1 extract` (and `.\start.bat` when navigation files are missing). To run the navigation unit tests directly:

```powershell
.\sandal.ps1 test
```
